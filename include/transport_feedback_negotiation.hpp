#pragma once
#include <cstdint>
#include <string_view>
#include <string>
#include <optional>
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
  private:
    NegotiationCapabilities(std::string projection, rtc::Description::Type type, bool singleTransport)
        : projection_(std::move(projection)), type_(type), singleTransport_(singleTransport) {}
    std::string projection_;
    rtc::Description::Type type_;
    bool singleTransport_;
    friend NegotiationResult inspectNegotiationCapabilities(const NegotiationCapabilities &,
                                                            const NegotiationCapabilities &, int);
};
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
