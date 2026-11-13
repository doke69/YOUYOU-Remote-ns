#include <rtc/rtcpreceivingsession.hpp>

#include <cstdint>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Session-level checks for the SDK REMB path: first-packet gating, local
// identity, compound RR+SDES(CNAME)+REMB on request and on SR repeat, target
// changes, and a fresh session after reconnect. No socket is opened; the send
// callback captures the exact SRTCP payload the libdatachannel transport would
// encrypt and submit.
namespace {
using rtc::binary;
using rtc::message_ptr;
using rtc::message_vector;

void require(bool value, const char *what) {
    if (!value)
        throw std::runtime_error(what);
}
uint32_t u32(const message_ptr &message, size_t offset) {
    return uint32_t(std::to_integer<uint8_t>((*message)[offset])) << 24 |
           uint32_t(std::to_integer<uint8_t>((*message)[offset + 1])) << 16 |
           uint32_t(std::to_integer<uint8_t>((*message)[offset + 2])) << 8 |
           uint32_t(std::to_integer<uint8_t>((*message)[offset + 3]));
}
uint16_t u16(const message_ptr &message, size_t offset) {
    return uint16_t(std::to_integer<uint8_t>((*message)[offset])) << 8 |
           uint16_t(std::to_integer<uint8_t>((*message)[offset + 1]));
}
uint8_t u8(const message_ptr &message, size_t offset) {
    return std::to_integer<uint8_t>((*message)[offset]);
}
uint16_t rembLength(const message_ptr &message, size_t offset) {
    return u16(message, offset + 2);
}
message_ptr rtp(uint32_t ssrc, uint16_t sequence, uint32_t timestamp) {
    auto message = rtc::make_message(12, rtc::Message::Binary);
    auto *data = message->data();
    data[0] = std::byte{0x80};
    data[1] = std::byte{96};
    data[2] = std::byte{uint8_t(sequence >> 8)};
    data[3] = std::byte{uint8_t(sequence)};
    data[4] = std::byte{uint8_t(timestamp >> 24)};
    data[5] = std::byte{uint8_t(timestamp >> 16)};
    data[6] = std::byte{uint8_t(timestamp >> 8)};
    data[7] = std::byte{uint8_t(timestamp)};
    data[8] = std::byte{uint8_t(ssrc >> 24)};
    data[9] = std::byte{uint8_t(ssrc >> 16)};
    data[10] = std::byte{uint8_t(ssrc >> 8)};
    data[11] = std::byte{uint8_t(ssrc)};
    return message;
}
message_ptr sr(uint32_t ssrc) {
    auto message = rtc::make_message(rtc::RtcpSr::Size(0), rtc::Message::Control);
    auto *packet = reinterpret_cast<rtc::RtcpSr *>(message->data());
    packet->preparePacket(ssrc, 0);
    packet->setNtpTimestamp(0x123456789abcdef0ull);
    packet->setRtpTimestamp(90000);
    return message;
}
void feed(rtc::RtcpReceivingSession &session, message_ptr message,
          const rtc::message_callback &send) {
    message_vector messages;
    messages.push_back(std::move(message));
    session.incoming(messages, send);
}
// Returns the offset of the next subpacket using its own length field.
size_t nextOffset(const message_ptr &message, size_t offset) {
    return offset + (size_t(1) + u16(message, offset + 2)) * 4;
}
void verifyRrSdes(const message_ptr &message, uint32_t local, uint32_t media,
                  const std::string &cname) {
    require(message->type == rtc::Message::Control, "RR+SDES is an RTCP control message");
    require(message->size() >= 32 + 20, "RR+SDES minimum size");
    require(u8(message, 0) == 0x81 && u8(message, 1) == 201, "first subpacket is RR");
    require(u16(message, 2) == 7, "RR length");
    require(u32(message, 4) == local, "RR sender is local");
    require(u32(message, 8) == media, "RR report block targets received RTP");
    const size_t sdes = nextOffset(message, 0);
    require(sdes == 32, "RR length locates SDES");
    require(u8(message, sdes) == 0x81 && u8(message, sdes + 1) == 202, "second is SDES");
    require(u32(message, sdes + 4) == local, "SDES chunk is local");
    require(u8(message, sdes + 8) == 1, "SDES item is CNAME");
    require(u8(message, sdes + 9) == cname.size(), "SDES CNAME length");
    for (size_t i = 0; i < cname.size(); ++i)
        require(u8(message, sdes + 10 + i) == uint8_t(cname[i]), "SDES CNAME text");
    require(u8(message, sdes + 10 + cname.size()) == 0, "SDES END");
    require(nextOffset(message, sdes) == message->size(),
            "RR+SDES has no REMB and ends after SDES");
}
void verifyCompound(const message_ptr &message, uint32_t local, uint32_t media,
                    const std::string &cname, unsigned bitrate) {
    require(message->type == rtc::Message::Control, "compound is an RTCP control message");
    require(message->size() >= 32 + 20 + 24, "compound minimum size");
    require(u8(message, 0) == 0x81 && u8(message, 1) == 201, "first subpacket is RR");
    require(u16(message, 2) == 7, "RR length");
    require(u32(message, 4) == local, "RR sender is local");
    require(u32(message, 8) == media, "RR report block targets received RTP");
    const size_t sdes = nextOffset(message, 0);
    require(sdes == 32, "RR length locates SDES");
    require(u8(message, sdes) == 0x81 && u8(message, sdes + 1) == 202, "second is SDES");
    require(u32(message, sdes + 4) == local, "SDES chunk is local");
    require(u8(message, sdes + 8) == 1, "SDES item is CNAME");
    require(u8(message, sdes + 9) == cname.size(), "SDES CNAME length");
    for (size_t i = 0; i < cname.size(); ++i)
        require(u8(message, sdes + 10 + i) == uint8_t(cname[i]), "SDES CNAME text");
    require(u8(message, sdes + 10 + cname.size()) == 0, "SDES END");
    const size_t remb = nextOffset(message, sdes);
    require(remb + 24 == message->size(), "REMB is last and complete");
    require(u8(message, remb) == 0x8f && u8(message, remb + 1) == 206, "third is REMB");
    require(rembLength(message, remb) == 5, "REMB has one target");
    require(u32(message, remb + 4) == local, "REMB sender is local");
    require(u32(message, remb + 8) == 0, "REMB media-source header is zero");
    require(u8(message, remb + 12) == 'R' && u8(message, remb + 13) == 'E' &&
                u8(message, remb + 14) == 'M' && u8(message, remb + 15) == 'B',
            "REMB identifier");
    const uint32_t word = u32(message, remb + 16);
    require((word >> 24) == 1, "one REMB target");
    const uint32_t exponent = (word >> 18) & 0x3f;
    const uint32_t mantissa = word & 0x3ffff;
    const uint32_t decoded = mantissa << exponent;
    require(decoded > 0 && double(decoded) >= double(bitrate) * 0.98 &&
                double(decoded) <= double(bitrate) * 1.02,
            "REMB target bitrate");
    require(u32(message, remb + 20) == media, "REMB FCI target is received RTP");
}
} // namespace

int main() {
    try {
        constexpr uint32_t local = 0x11223344;
        constexpr uint32_t media = 0x55667788;
        const std::string cname = "uu-session-cname";

        rtc::RtcpReceivingSession session;
        std::vector<message_ptr> sent;
        rtc::message_callback send = [&](message_ptr message) {
            sent.push_back(std::move(message));
        };

        // No identity: a bitrate request must fail without sending.
        require(!session.requestBitrate(50000000, send), "no identity means no REMB");
        require(sent.empty(), "no identity must not emit feedback");

        session.setRtcpIdentity(local, cname);
        require(session.hasRtcpIdentity() && session.rtcpSenderSsrc() == local &&
                    session.rtcpCname() == cname,
                "identity is stored");
        require(!session.requestBitrate(50000000, send), "first packet must gate REMB");
        require(sent.empty(), "first-packet gate must not send");
        require(session.receivedRtpSsrc() == 0, "no target before RTP");

        // SR before the first RTP must not resurrect an unvalidated target.
        feed(session, sr(media), send);
        require(sent.size() == 1, "SR before RTP emits only the existing RR");
        require(u8(sent[0], 1) == 201 && u8(sent[0], 0) == 0x81, "SR path is a bare RR");
        sent.clear();

        // First primary RTP establishes the only REMB target.
        feed(session, rtp(media, 100, 90000), send);
        require(session.receivedRtpSsrc() == media, "RTP target is recorded");
        require(sent.empty(), "RTP is forwarded without feedback");

        // Without a requested REMB, the identity-bearing video session still
        // reports as RR+SDES with the same local sender/CNAME and primary-video
        // report block. No REMB subpacket is appended.
        feed(session, sr(media), send);
        require(sent.size() == 1, "SR without REMB emits one RR+SDES");
        verifyRrSdes(sent[0], local, media, cname);
        const auto rrSdesPacket = sent[0];
        sent.clear();

        // A later active REMB carries the same identity. DLSR is measured from
        // the matching SR receive time, so the delayed send is nonzero.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        require(session.requestBitrate(50000000, send), "request after SR");
        require(sent.size() == 1, "one compound message after SR");
        verifyCompound(sent[0], local, media, cname, 50000000);
        require(u32(rrSdesPacket, 4) == u32(sent[0], 4),
                "SR RR and later REMB share local sender");
        require(u32(rrSdesPacket, 8) == u32(sent[0], 8),
                "SR RR and later REMB share report block target");
        require(u32(rrSdesPacket, 32 + 4) == u32(sent[0], 32 + 4),
                "SR RR and later REMB share SDES sender");
        const uint32_t dlsr = u32(sent[0], 28);
        require(dlsr > 0 && dlsr < 65536, "delayed active REMB has matching-SR DLSR");
        sent.clear();

        // A changed target is submitted as another single compound message.
        require(session.requestBitrate(25000000, send), "changed target");
        require(sent.size() == 1, "changed target is one message");
        verifyCompound(sent[0], local, media, cname, 25000000);
        sent.clear();

        // SR repeats the stored 25 Mbps as one compound packet, not RR+REMB.
        feed(session, sr(media), send);
        require(sent.size() == 1, "SR repeat is one message");
        verifyCompound(sent[0], local, media, cname, 25000000);
        sent.clear();

        // Switching away from REMB must clear the stored target too: merely
        // suppressing Client calls leaves the SR-triggered feedback active.
        require(session.requestBitrate(0, send), "clear stored REMB target");
        require(sent.empty(), "clear sends no zero-bandwidth REMB");
        for (unsigned i = 0; i < 3; ++i) {
            feed(session, sr(media), send);
            require(sent.size() == 1, "RR continues after REMB disable");
            require(u8(sent[0], 1) == 201 && u32(sent[0], 4) == local,
                    "disabled REMB retains local RR identity");
            const auto sdes = nextOffset(sent[0], 0);
            require(u8(sent[0], sdes + 1) == 202 && u32(sent[0], sdes + 4) == local,
                    "disabled REMB retains local CNAME chunk");
            require(nextOffset(sent[0], sdes) == sent[0]->size(),
                    "later SR must not resurrect the old REMB");
            sent.clear();
        }
        require(session.requestBitrate(12000000, send), "restore REMB after alternate mode");
        require(sent.size() == 1, "restored mode emits once");
        verifyCompound(sent[0], local, media, cname, 12000000);
        sent.clear();

        // Reconnect: a new session has a new local identity and must wait for
        // its own first primary RTP packet.
        constexpr uint32_t local2 = 0x99aabbcc;
        constexpr uint32_t media2 = 0x01020304;
        const std::string cname2 = "uu-second-session";
        rtc::RtcpReceivingSession session2;
        session2.setRtcpIdentity(local2, cname2);
        require(!session2.requestBitrate(50000000, send), "new session waits for first RTP");
        require(sent.empty(), "new session must not inherit old state");
        feed(session2, rtp(media2, 1, 12345), send);
        require(session2.requestBitrate(50000000, send), "new session sends after its RTP");
        require(sent.size() == 1, "new session one compound");
        verifyCompound(sent[0], local2, media2, cname2, 50000000);
        sent.clear();

        // Invalid identity updates are rejected instead of sending with SSRC 0.
        session2.setRtcpIdentity(0, cname2);
        require(!session2.hasRtcpIdentity(), "zero sender clears identity");
        require(!session2.requestBitrate(50000000, send), "zero sender cannot send");
        require(sent.empty(), "zero sender emits nothing");

        std::cout << "PASS: session REMB first-RTP/SR-repeat/target-change/reconnect\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
