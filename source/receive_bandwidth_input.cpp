#include "receive_bandwidth_input.hpp"
#include <algorithm>

namespace uu::rbe {
namespace {
uint16_t read16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t read24(const uint8_t *p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t read32(const uint8_t *p) { return uint32_t(read16(p)) << 16 | read16(p + 2); }
} // namespace

std::optional<InputConfig> inputConfig(const twcc::FeedbackCapabilitiesResult &capabilities,
                                       std::string_view videoMid, int primaryPayloadType) {
    if (capabilities.status != twcc::NegotiationStatus::Ready || primaryPayloadType < 0 || primaryPayloadType > 127)
        return std::nullopt;
    const auto media = std::find_if(capabilities.media.begin(), capabilities.media.end(), [&](const auto &entry) {
        return entry.mid == videoMid && entry.payloadType == primaryPayloadType && entry.clockRate == 90000;
    });
    if (media == capabilities.media.end()) return std::nullopt;
    InputConfig config;
    config.videoPayloads[size_t(primaryPayloadType)] = true;
    for (const int rtx : media->rtxPayloadTypes)
        if (rtx >= 0 && rtx <= 127) config.videoPayloads[size_t(rtx)] = true;
    config.absSendTimeId = media->absSendTimeId;
    config.transmissionOffsetId = media->transmissionOffsetId;
    size_t transportIds = 0;
    for (const auto &extension : media->transportExtensions) {
        for (const uint8_t id : {extension.localId, extension.remoteId}) {
            const auto end = config.transportSequenceIds.begin() + transportIds;
            if (!id || std::find(config.transportSequenceIds.begin(), end, id) != end) continue;
            if (transportIds == config.transportSequenceIds.size()) return std::nullopt;
            config.transportSequenceIds[transportIds++] = id;
        }
    }
    const auto transportEnd = config.transportSequenceIds.begin() + transportIds;
    for (const uint8_t id : {config.absSendTimeId, config.transmissionOffsetId})
        if (id && std::find(config.transportSequenceIds.begin(), transportEnd, id) != transportEnd) return std::nullopt;
    if (config.absSendTimeId && config.absSendTimeId == config.transmissionOffsetId) return std::nullopt;
    return config;
}

ReadResult readPacket(const uint8_t *rtp, size_t size, int64_t arrivalUs, const InputConfig &config) {
    ReadResult result;
    if (!rtp || size < 12 || rtp[0] >> 6 != 2) return result;
    size_t header = 12 + 4 * size_t(rtp[0] & 0x0f);
    if (size < header) return result;
    const bool hasExtension = rtp[0] & 0x10;
    size_t extensionBegin = 0, extensionEnd = header;
    uint16_t profile = 0;
    if (hasExtension) {
        if (size < header + 4) return result;
        profile = read16(rtp + header);
        extensionBegin = header + 4;
        extensionEnd = extensionBegin + 4 * size_t(read16(rtp + header + 2));
        if (extensionEnd > size) return result;
    }
    if (rtp[0] & 0x20) {
        const size_t padding = rtp[size - 1];
        if (!padding || padding > size - extensionEnd) return result;
    }
    auto &packet = result.packet;
    packet.payloadType = rtp[1] & 0x7f;
    packet.rtpTimestamp = read32(rtp + 4);
    packet.ssrc = read32(rtp + 8);
    packet.payloadBytes = size - extensionEnd;
    packet.arrivalUs = arrivalUs;

    bool transportSequence = false;
    const auto transportEnd = std::find(config.transportSequenceIds.begin(), config.transportSequenceIds.end(), 0);
    // Returns false for a negotiated element that cannot be used as specified.
    auto element = [&](uint8_t id, const uint8_t *data, size_t length) {
        if (std::find(config.transportSequenceIds.begin(), transportEnd, id) != transportEnd) {
            if (length != 2 && length != 4) return false;
            transportSequence = true;
        } else if (id == config.absSendTimeId) {
            if (length != 3 || packet.absSendTime) return false;
            packet.absSendTime = read24(data);
        } else if (id == config.transmissionOffsetId) {
            if (length != 3 || packet.transmissionOffset) return false;
            const uint32_t value = read24(data);
            packet.transmissionOffset = int32_t(value & 0x800000 ? value | 0xff000000u : value);
        }
        return true;
    };
    if (hasExtension && profile == 0xBEDE) {
        for (size_t at = extensionBegin; at < extensionEnd;) {
            const uint8_t byte = rtp[at];
            const uint8_t id = byte >> 4;
            if (!id) { ++at; continue; }
            const size_t length = size_t(byte & 0x0f) + 1;
            if (id == 15) break;
            if (at + 1 + length > extensionEnd || !element(id, rtp + at + 1, length)) return {PacketStatus::Malformed, {}};
            at += 1 + length;
        }
    } else if (hasExtension && (profile & 0xfff0) == 0x1000) {
        for (size_t at = extensionBegin; at < extensionEnd;) {
            const uint8_t id = rtp[at];
            if (!id) { ++at; continue; }
            if (at + 2 > extensionEnd) return {PacketStatus::Malformed, {}};
            const size_t length = rtp[at + 1];
            if (at + 2 + length > extensionEnd || !element(id, rtp + at + 2, length)) return {PacketStatus::Malformed, {}};
            at += 2 + length;
        }
    }
    if (!config.videoPayloads[packet.payloadType]) result.status = PacketStatus::NotVideo;
    else if (transportSequence) result.status = PacketStatus::TransportSequence;
    else if (arrivalUs < 0) result.status = PacketStatus::MissingArrival;
    else result.status = PacketStatus::Accepted;
    return result;
}
} // namespace uu::rbe
