#include <rtc/rtcpreceivingsession.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

// Byte-level checks for the single compound RR + SDES(CNAME) + REMB packet.
// These verify packet order, both SSRC identities, the zero REMB media-source
// header field, the FCI target list, and the exact report-block/CNAME layout.
namespace {
using rtc::binary;
using rtc::RembReportBlockValues;

void require(bool value, const char *what) {
    if (!value)
        throw std::runtime_error(what);
}
uint32_t u32(const binary &packet, size_t offset) {
    return uint32_t(std::to_integer<uint8_t>(packet[offset])) << 24 |
           uint32_t(std::to_integer<uint8_t>(packet[offset + 1])) << 16 |
           uint32_t(std::to_integer<uint8_t>(packet[offset + 2])) << 8 |
           uint32_t(std::to_integer<uint8_t>(packet[offset + 3]));
}
uint16_t u16(const binary &packet, size_t offset) {
    return uint16_t(std::to_integer<uint8_t>(packet[offset])) << 8 |
           uint16_t(std::to_integer<uint8_t>(packet[offset + 1]));
}
uint8_t u8(const binary &packet, size_t offset) {
    return std::to_integer<uint8_t>(packet[offset]);
}
} // namespace

int main() {
    try {
        constexpr uint32_t local = 0x11223344;
        constexpr uint32_t media = 0x55667788;
        const std::string cname = "uu-abcd";
        constexpr unsigned bitrate = 8000000;

        RembReportBlockValues report;
        report.mediaSsrc = media;
        report.fractionLost = 0x12;
        report.packetsLost = 0x1234;
        report.highestSeqNo = 0xabcd;
        report.seqNoCycles = 3;
        report.jitter = 0x01020304;
        report.lastSr = 0x05060708;
        report.delaySinceLastSr = 0x090a0b0c;

        const binary packet = rtc::makeCompoundRemb(local, cname, bitrate, report);
        // RR (32) + SDES header and padded chunk (20) + REMB (24).
        require(packet.size() == 76, "compound packet size");

        // RR: V=2, RC=1, PT=201, length 7, local sender, media report block.
        require(u8(packet, 0) == 0x81 && u8(packet, 1) == 201, "RR header");
        require(u16(packet, 2) == 7, "RR length");
        require(u32(packet, 4) == local, "RR sender SSRC is local");
        require(u32(packet, 8) == media, "RR report block SSRC is received RTP SSRC");
        require(u8(packet, 12) == 0x12, "RR fraction lost");
        require((uint32_t(u8(packet, 13)) << 16 | uint32_t(u8(packet, 14)) << 8 |
                 uint32_t(u8(packet, 15))) == 0x001234,
                "RR cumulative lost");
        require(u16(packet, 16) == 0xabcd, "RR highest sequence");
        require(u16(packet, 18) == 3, "RR sequence cycles");
        require(u32(packet, 20) == 0x01020304, "RR jitter");
        require(u32(packet, 24) == 0x05060708, "RR last SR");
        require(u32(packet, 28) == 0x090a0b0c, "RR delay since SR");

        // SDES: V=2, SC=1, PT=202, local chunk, CNAME text, END, zero padding.
        const size_t sdes = 32;
        require(u8(packet, sdes) == 0x81 && u8(packet, sdes + 1) == 202, "SDES header");
        require(u16(packet, sdes + 2) == 4, "SDES length");
        require(u32(packet, sdes + 4) == local, "SDES SSRC is local");
        require(u8(packet, sdes + 8) == 1, "SDES CNAME item type");
        require(u8(packet, sdes + 9) == cname.size(), "SDES CNAME length");
        for (size_t i = 0; i < cname.size(); ++i)
            require(u8(packet, sdes + 10 + i) == uint8_t(cname[i]), "SDES CNAME text");
        require(u8(packet, sdes + 10 + cname.size()) == 0, "SDES END item");
        require(u8(packet, sdes + 11 + cname.size()) == 0, "SDES padding");

        // REMB: FMT=15, PT=206, length 5, local sender, zero media-source,
        // one target SSRC = received primary RTP SSRC.
        const size_t remb = sdes + 20;
        require(remb + 24 == packet.size(), "REMB is last and complete");
        require(u8(packet, remb) == 0x8f && u8(packet, remb + 1) == 206, "REMB header");
        require(u16(packet, remb + 2) == 5, "REMB length");
        require(u32(packet, remb + 4) == local, "REMB sender SSRC is local");
        require(u32(packet, remb + 8) == 0, "REMB media-source header SSRC is zero");
        require(u8(packet, remb + 12) == 'R' && u8(packet, remb + 13) == 'E' &&
                    u8(packet, remb + 14) == 'M' && u8(packet, remb + 15) == 'B',
                "REMB identifier");
        const uint32_t word = u32(packet, remb + 16);
        require((word >> 24) == 1, "REMB target count");
        const uint32_t exponent = (word >> 18) & 0x3f;
        const uint32_t mantissa = word & 0x3ffff;
        require(mantissa << exponent == bitrate, "REMB bitrate round trip");
        require(u32(packet, remb + 20) == media, "REMB FCI target SSRC");

        // The same local identity always builds identical bytes.
        require(rtc::makeCompoundRemb(local, cname, bitrate, report) == packet,
                "stable identity is deterministic");

        // RR + SDES without REMB uses the same local identity and report block;
        // this is the non-REMB primary-video form. No REMB subpacket follows.
        const binary rrSdes = rtc::makeCompoundRrSdes(local, cname, report);
        require(rrSdes.size() == 52, "RR+SDES packet size");
        require(u8(rrSdes, 0) == 0x81 && u8(rrSdes, 1) == 201, "RR+SDES starts with RR");
        require(u16(rrSdes, 2) == 7, "RR+SDES RR length");
        require(u32(rrSdes, 4) == local, "RR+SDES RR sender is local");
        require(u32(rrSdes, 8) == media, "RR+SDES report block targets primary video");
        const size_t rrSdesSdes = 32;
        require(u8(rrSdes, rrSdesSdes) == 0x81 && u8(rrSdes, rrSdesSdes + 1) == 202,
                "RR+SDES second subpacket is SDES");
        require(u16(rrSdes, rrSdesSdes + 2) == 4, "RR+SDES SDES length");
        require(u32(rrSdes, rrSdesSdes + 4) == local, "RR+SDES SDES sender is local");
        require(u8(rrSdes, rrSdesSdes + 8) == 1, "RR+SDES CNAME item");
        require(u8(rrSdes, rrSdesSdes + 9) == cname.size(), "RR+SDES CNAME length");
        for (size_t i = 0; i < cname.size(); ++i)
            require(u8(rrSdes, rrSdesSdes + 10 + i) == uint8_t(cname[i]),
                    "RR+SDES CNAME text");
        require(rrSdesSdes + 20 == rrSdes.size(), "RR+SDES ends after SDES");
        require(rtc::makeCompoundRrSdes(local, cname, report) == rrSdes,
                "RR+SDES is deterministic");

        // Negative cumulative loss stays 24-bit two's complement.
        report.packetsLost = -1;
        auto negative = rtc::makeCompoundRemb(local, cname, bitrate, report);
        require(negative.size() == packet.size(), "negative report size");
        require(u8(negative, 13) == 0xff && u8(negative, 14) == 0xff && u8(negative, 15) == 0xff,
                "negative loss encoding");
        report.packetsLost = -8388608;
        negative = rtc::makeCompoundRemb(local, cname, bitrate, report);
        require(u8(negative, 13) == 0x80 && u8(negative, 14) == 0 && u8(negative, 15) == 0,
                "minimum loss encoding");

        // 50 Mbps is approximated by the 6-bit exponent / 18-bit mantissa.
        report.packetsLost = 0;
        auto fifty = rtc::makeCompoundRemb(local, cname, 50000000, report);
        require(fifty.size() == packet.size(), "50 Mbps packet size");
        const uint32_t fiftyWord = u32(fifty, remb + 16);
        const uint32_t fiftyExp = (fiftyWord >> 18) & 0x3f;
        const uint32_t fiftyMantissa = fiftyWord & 0x3ffff;
        require((fiftyMantissa << fiftyExp) > 49000000 &&
                    (fiftyMantissa << fiftyExp) <= 50000000,
                "50 Mbps approximation");

        // Identity/target validation: no bare or ambiguous REMB is produced.
        const RembReportBlockValues valid = report;
        require(rtc::makeCompoundRemb(0, cname, bitrate, valid).empty(), "zero local SSRC");
        RembReportBlockValues zeroTarget = valid;
        zeroTarget.mediaSsrc = 0;
        require(rtc::makeCompoundRemb(local, cname, bitrate, zeroTarget).empty(), "zero media SSRC");
        RembReportBlockValues sameSsrc = valid;
        sameSsrc.mediaSsrc = local;
        require(rtc::makeCompoundRemb(local, cname, bitrate, sameSsrc).empty(),
                "equal sender/media must be rejected");
        require(rtc::makeCompoundRemb(local, "", bitrate, valid).empty(), "empty CNAME");
        require(rtc::makeCompoundRemb(local, std::string(256, 'a'), bitrate, valid).empty(),
                "oversized CNAME");
        require(rtc::makeCompoundRemb(local, std::string("bad\ncname"), bitrate, valid).empty(),
                "control character in CNAME");
        require(rtc::makeCompoundRemb(local, cname, 0, valid).empty(), "zero bitrate");

        // DLSR is measured from the matching SR receive time in 1/65536 s.
        require(rtc::remb_detail::dlsrUnits(1000, 0) == 0, "no SR means zero DLSR");
        require(rtc::remb_detail::dlsrUnits(1000, 1000) == 0, "same time means zero DLSR");
        require(rtc::remb_detail::dlsrUnits(1000, 2000) == 0, "clock rollback means zero DLSR");
        require(rtc::remb_detail::dlsrUnits(1001, 1000) == 65, "1 ms DLSR");
        require(rtc::remb_detail::dlsrUnits(1500, 1000) == 32768, "500 ms DLSR");
        require(rtc::remb_detail::dlsrUnits(2000, 1000) == 65536, "1 s DLSR");
        require(rtc::remb_detail::dlsrUnits(1000 + 100000000, 1000) == 0xffffffffu,
                "DLSR clamps at 32 bits");

        std::cout << "PASS: compound RR+SDES(CNAME)+REMB, RR+SDES, DLSR bytes/order/SSRC/target\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
