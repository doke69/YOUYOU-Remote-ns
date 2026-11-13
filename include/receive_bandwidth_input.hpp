#pragma once
#include "transport_feedback_negotiation.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Classic receive-side BWE input boundary. Mirrors the pinned reference
// delivery rule: only video payload types (primary and its RTX) are eligible,
// and a packet carrying a transport-wide sequence never reaches the classic
// estimator. No payload bytes are copied or retained.
namespace uu::rbe {
struct InputConfig {
    std::array<bool, 128> videoPayloads{};
    uint8_t absSendTimeId = 0, transmissionOffsetId = 0;
    std::array<uint8_t, 4> transportSequenceIds{};
};
// From the current common capabilities; nullopt when the video entry is absent
// or the negotiated extension IDs collide.
std::optional<InputConfig> inputConfig(const twcc::FeedbackCapabilitiesResult &capabilities,
                                       std::string_view videoMid, int primaryPayloadType = 96);

enum class PacketStatus { Accepted, NotVideo, TransportSequence, MissingArrival, Malformed };
struct PacketMetadata {
    int64_t arrivalUs = -1;
    uint32_t ssrc = 0, rtpTimestamp = 0;
    uint8_t payloadType = 0;
    // RTP payload plus padding; excludes RTP header, CSRCs and extensions.
    size_t payloadBytes = 0;
    // 24-bit 6.18 fixed-point seconds, wire value.
    std::optional<uint32_t> absSendTime;
    // Signed 24-bit offset in RTP clock units.
    std::optional<int32_t> transmissionOffset;
};
struct ReadResult {
    PacketStatus status = PacketStatus::Malformed;
    PacketMetadata packet;
};
ReadResult readPacket(const uint8_t *rtp, size_t size, int64_t arrivalUs, const InputConfig &config);
} // namespace uu::rbe
