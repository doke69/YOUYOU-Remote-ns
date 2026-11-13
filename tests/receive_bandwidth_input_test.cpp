#include "receive_bandwidth_input.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using uu::rbe::PacketStatus;

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

struct Element {
    uint8_t id;
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> rtp(uint8_t pt, uint32_t ssrc, uint32_t timestamp, const std::vector<Element> &elements,
                                size_t payload, uint8_t padding = 0, bool twoByte = false, uint8_t csrcs = 0) {
    std::vector<uint8_t> packet{uint8_t(0x80 | csrcs), pt, 0x12, 0x34,
        uint8_t(timestamp >> 24), uint8_t(timestamp >> 16), uint8_t(timestamp >> 8), uint8_t(timestamp),
        uint8_t(ssrc >> 24), uint8_t(ssrc >> 16), uint8_t(ssrc >> 8), uint8_t(ssrc)};
    packet.insert(packet.end(), size_t(csrcs) * 4, 0xcc);
    if (!elements.empty()) {
        packet[0] |= 0x10;
        std::vector<uint8_t> body;
        for (const auto &element : elements) {
            if (twoByte) body.insert(body.end(), {element.id, uint8_t(element.data.size())});
            else body.push_back(uint8_t(element.id << 4 | (element.data.size() - 1)));
            body.insert(body.end(), element.data.begin(), element.data.end());
        }
        while (body.size() % 4) body.push_back(0);
        const uint16_t profile = twoByte ? 0x1000 : 0xBEDE;
        packet.insert(packet.end(), {uint8_t(profile >> 8), uint8_t(profile), 0, uint8_t(body.size() / 4)});
        packet.insert(packet.end(), body.begin(), body.end());
    }
    packet.insert(packet.end(), payload, 0xaa);
    if (padding) {
        packet[0] |= 0x20;
        packet.insert(packet.end(), padding - 1, 0);
        packet.push_back(padding);
    }
    return packet;
}

static uu::rbe::ReadResult read(const std::vector<uint8_t> &packet, const uu::rbe::InputConfig &config,
                                int64_t arrivalUs = 1000) {
    return uu::rbe::readPacket(packet.data(), packet.size(), arrivalUs, config);
}

static uu::twcc::FeedbackCapabilitiesResult capabilities() {
    uu::twcc::FeedbackCapabilitiesResult result;
    result.status = uu::twcc::NegotiationStatus::Ready;
    uu::twcc::MediaFeedbackCapabilities audio;
    audio.mid = "0"; audio.codec = "opus"; audio.payloadType = 111; audio.clockRate = 48000;
    uu::twcc::MediaFeedbackCapabilities video;
    video.mid = "1"; video.codec = "H264"; video.payloadType = 96; video.clockRate = 90000;
    video.rtxPayloadTypes = {97};
    video.absSendTimeId = 3;
    video.transmissionOffsetId = 2;
    result.media = {audio, video};
    return result;
}

int main() {
    try {
        auto caps = capabilities();
        const auto config = uu::rbe::inputConfig(caps, "1");
        require(config && config->videoPayloads[96] && config->videoPayloads[97] && !config->videoPayloads[111],
                "video primary and RTX payload types");
        require(!uu::rbe::inputConfig(caps, "0") && !uu::rbe::inputConfig(caps, "9"), "non-video mid rejected");

        auto conflict = caps;
        conflict.media[1].transmissionOffsetId = 3;
        require(!uu::rbe::inputConfig(conflict, "1"), "AST/TOF ID conflict rejected");
        conflict = caps;
        conflict.media[1].transportExtensions = {{uu::twcc::TransportExtensionVersion::V1, 3, 3, true}};
        require(!uu::rbe::inputConfig(conflict, "1"), "AST/transport ID conflict rejected");
        auto notReady = caps;
        notReady.status = uu::twcc::NegotiationStatus::NotNegotiated;
        require(!uu::rbe::inputConfig(notReady, "1"), "invalid capabilities rejected");

        // AST 24-bit and signed TOF, payload+padding excludes header/CSRC/extension.
        auto result = read(rtp(96, 0x11223344, 0xfffffff0, {{3, {0xff, 0xff, 0xfe}}, {2, {0xff, 0xff, 0xfb}}}, 1000, 20,
                               false, 2), *config, 123456);
        require(result.status == PacketStatus::Accepted, "accepted video packet");
        require(result.packet.ssrc == 0x11223344 && result.packet.rtpTimestamp == 0xfffffff0, "ssrc/timestamp");
        require(result.packet.payloadBytes == 1020, "payload plus padding");
        require(result.packet.absSendTime == 0xfffffeu, "AST 24-bit wire value");
        require(result.packet.transmissionOffset == -5, "TOF sign extension");
        require(result.packet.arrivalUs == 123456, "arrival preserved in microseconds");
        result = read(rtp(96, 1, 0, {{2, {0x7f, 0xff, 0xff}}}, 10), *config);
        require(result.packet.transmissionOffset == 0x7fffff && !result.packet.absSendTime, "TOF max positive");

        result = read(rtp(96, 1, 0, {{3, {0, 0, 1}}}, 10, 0, true), *config);
        require(result.status == PacketStatus::Accepted && result.packet.absSendTime == 1u, "two-byte extensions");
        result = read(rtp(97, 0x55, 0, {}, 500), *config);
        require(result.status == PacketStatus::Accepted && result.packet.ssrc == 0x55 && result.packet.payloadType == 97,
                "RTX keeps its own SSRC and is not rewritten");
        require(read(rtp(111, 1, 0, {}, 100), *config).status == PacketStatus::NotVideo, "audio not fed");
        require(read(rtp(96, 1, 0, {}, 100), *config, -1).status == PacketStatus::MissingArrival,
                "missing arrival counted, never fabricated");
        result = read(rtp(96, 1, 0, {{9, {1, 2, 3}}}, 100), *config);
        require(result.status == PacketStatus::Accepted && !result.packet.absSendTime &&
                    !result.packet.transmissionOffset, "unnegotiated ID ignored");

        auto noAst = caps;
        noAst.media[1].absSendTimeId = 0;
        const auto noAstConfig = uu::rbe::inputConfig(noAst, "1");
        require(noAstConfig && !read(rtp(96, 1, 0, {{3, {0, 0, 1}}}, 10), *noAstConfig).packet.absSendTime,
                "AST not negotiated is not read");

        auto transport = caps;
        transport.media[1].transportExtensions = {{uu::twcc::TransportExtensionVersion::V1, 5, 5, true}};
        const auto transportConfig = uu::rbe::inputConfig(transport, "1");
        require(transportConfig.has_value(), "transport config");
        require(read(rtp(96, 1, 0, {{5, {0, 7}}, {3, {0, 0, 1}}}, 100), *transportConfig).status ==
                    PacketStatus::TransportSequence, "transport-sequence packet is not fed to classic RBE");
        require(read(rtp(96, 1, 0, {{3, {0, 0, 1}}}, 100), *transportConfig).status == PacketStatus::Accepted,
                "packet without transport sequence still eligible");
        require(read(rtp(96, 1, 0, {{5, {0, 7, 0}}}, 100), *transportConfig).status == PacketStatus::Malformed,
                "transport sequence length mismatch");

        require(read(rtp(96, 1, 0, {{3, {0, 1}}}, 100), *config).status == PacketStatus::Malformed, "AST length");
        require(read(rtp(96, 1, 0, {{2, {0, 1, 2, 3}}}, 100), *config).status == PacketStatus::Malformed, "TOF length");
        require(read(rtp(96, 1, 0, {{3, {0, 0, 1}}, {3, {0, 0, 2}}}, 100), *config).status == PacketStatus::Malformed,
                "duplicate AST element");
        auto broken = rtp(96, 1, 0, {{3, {0, 0, 1}}}, 0);
        broken[15] = 9;
        require(read(broken, *config).status == PacketStatus::Malformed, "extension block beyond packet");
        broken = rtp(96, 1, 0, {}, 10, 4);
        broken.back() = 0;
        require(read(broken, *config).status == PacketStatus::Malformed, "zero padding count");
        broken.back() = 15;
        require(read(broken, *config).status == PacketStatus::Malformed, "padding beyond payload");
        broken = rtp(96, 1, 0, {}, 0);
        broken[0] |= 0x0f;
        require(read(broken, *config).status == PacketStatus::Malformed, "CSRC list beyond packet");
        broken = rtp(96, 1, 0, {}, 10);
        broken[0] = 0x40;
        require(read(broken, *config).status == PacketStatus::Malformed, "RTP version");
        require(uu::rbe::readPacket(nullptr, 0, 0, *config).status == PacketStatus::Malformed, "null input");
        result = read(rtp(96, 1, 0, {}, 0, 200), *config);
        require(result.status == PacketStatus::Accepted && result.packet.payloadBytes == 200, "padding-only probe");

        std::cout << "PASS: RBE input routing, AST/TOF, payload+padding, transport diversion, malformed rejection\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
