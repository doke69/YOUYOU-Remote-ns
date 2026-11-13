#pragma once
#include "transport_feedback.hpp"
#include <cstdint>
#include <string_view>
#include <string>
#include <optional>
#include <vector>
#include <rtc/description.hpp>

namespace uu::twcc {
enum class NegotiationStatus {
    Ready, NotNegotiated, UnsupportedExtension, InconsistentId, AmbiguousMedia,
    InvalidDescription, UnsupportedTransport
};
struct NegotiationResult {
    NegotiationStatus status = NegotiationStatus::NotNegotiated;
    uint8_t extensionId = 0;
    unsigned videoPayloads = 0;
    TransportSequenceVersion version = TransportSequenceVersion::V1;
    uint8_t legacyExtensionId = 0; // Optional negotiated 01 fallback in 02 mode.
};
// Common feedback state. Client must also match the raw snapshots to the
// current stable installed descriptions before using it for REMB admission.
// It does not replace the stricter TWCC extension/transport admission below.
enum class TransportExtensionVersion { V1, V2 };
struct ReceiveTransportExtension {
    TransportExtensionVersion version;
    uint8_t localId, remoteId;
    bool attributesSupported;
};
struct MediaFeedbackCapabilities {
    std::string mid, codec;
    int payloadType = 0, clockRate = 0;
    std::vector<std::string> feedback;
    std::vector<int> rtxPayloadTypes;
    std::vector<ReceiveTransportExtension> transportExtensions;
    // Zero unless both sides map the URI to the same plain one-way-usable ID.
    uint8_t absSendTimeId = 0, transmissionOffsetId = 0;
};
struct FeedbackCapabilitiesResult {
    NegotiationStatus status = NegotiationStatus::NotNegotiated;
    bool localIsOfferer = false;
    std::vector<MediaFeedbackCapabilities> media;
    bool allowsRemb(std::string_view videoMid, int primaryVideoPayloadType) const;
};
// Store only the bounded, validated capability projection between asynchronous
// signaling events. No ICE credentials/candidates/fingerprint/raw SDP retained.
class NegotiationCapabilities {
  public:
    static std::optional<NegotiationCapabilities> fromSdp(std::string_view sdp,
                                                        rtc::Description::Type type);
    bool operator==(const NegotiationCapabilities &other) const {
        return type_ == other.type_ && projection_ == other.projection_ &&
               singleTransport_ == other.singleTransport_;
    }
    rtc::Description::Type type() const { return type_; }
    // Comparison only, never an admission source. The SDK serializer may
    // repeat its synthesized BUNDLE group alongside the original attribute.
    bool matchesInstalledDescription(const rtc::Description &installed) const;
  private:
    NegotiationCapabilities(std::string projection, rtc::Description::Type type, bool singleTransport)
        : projection_(std::move(projection)), type_(type), singleTransport_(singleTransport) {}
    std::string projection_;
    rtc::Description::Type type_;
    bool singleTransport_;
    friend NegotiationResult inspectNegotiationCapabilities(const NegotiationCapabilities &,
                                                            const NegotiationCapabilities &, int);
    friend FeedbackCapabilitiesResult inspectMediaFeedbackCapabilities(
        const NegotiationCapabilities &, const NegotiationCapabilities &, int);
};
FeedbackCapabilitiesResult inspectMediaFeedbackCapabilities(const NegotiationCapabilities &local,
                                                             const NegotiationCapabilities &remote,
                                                             int primaryVideoPayloadType = 96);
NegotiationResult inspectNegotiationCapabilities(const NegotiationCapabilities &local,
                                                 const NegotiationCapabilities &remote,
                                                 int primaryVideoPayloadType = 96);
// Low-level check of parsed capabilities; duplicate raw lines cannot be checked
// here. Do not use this overload alone to enable feedback on unvalidated SDP.
// For the current one-transport bundled peer. Does not modify SDP or enable RX.
NegotiationResult inspectNegotiation(const rtc::Description &local,
                                    const rtc::Description &remote,
                                    int primaryVideoPayloadType = 96);
// Preferred integration entry: checks the ORIGINAL final offer/answer before
// duplicate attributes can be overwritten by the SDP parser. Builds a temporary
// capabilities-only projection, with session extension/direction inheritance.
// Does not retain raw SDP, credentials, candidates, or modify either peer.
NegotiationResult inspectNegotiationSdp(std::string_view localSdp,
                                       rtc::Description::Type localType,
                                       std::string_view remoteSdp,
                                       rtc::Description::Type remoteType,
                                       int primaryVideoPayloadType = 96);
} // namespace uu::twcc
