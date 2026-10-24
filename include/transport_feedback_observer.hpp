#pragma once
#ifndef UU_RECEIVE_TIMESTAMPS
#error "Feedback observer/sender require a uniform receive-timestamp Message ABI"
#endif
#include "transport_feedback.hpp"
#include "transport_feedback_negotiation.hpp"
#include <rtc/mediahandler.hpp>
#include <mutex>

namespace uu::twcc {
struct ObservationCounters {
    uint64_t accepted = 0, duplicates = 0, missingExtension = 0;
    uint64_t malformed = 0, unsupportedProfile = 0, missingTimestamp = 0;
    uint64_t tooOld = 0, windowFull = 0, ambiguous = 0, invalidTime = 0;
};
// Construct on the heap before streaming. Attach ONLY as the PeerConnection's
// global authenticated media handler (before per-track RTX unwrap), never at
// raw ICE ingress or once per track. All negotiated audio/video/RTX are observed
// in the same transport sequence space. This observer never sends from RX.
// Requires UU_RECEIVE_TIMESTAMPS in EVERY app/dependency translation unit.
class TransportFeedbackObserver final : public rtc::MediaHandler {
  public:
    TransportFeedbackObserver(NegotiationResult negotiation, FeedbackTiming timing);
    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
    ObservationCounters counters() const;
    // Copies a bounded prepared report under the lock. No send callback is
    // retained here. Caller sends outside the lock then commits only on success.
    std::optional<PreparedFeedback> prepare(int64_t nowUs, uint32_t senderSsrc,
                                            uint32_t mediaSsrc, size_t mtu = 1200);
    bool commit(uint64_t ticket, int64_t sentAtUs);
    // Irreversible for this instance: keep forwarding media, stop observation.
    void stopObserving();
    // Requires detach/quiesce before resetting the shared transport session.
    void reset();
  private:
    const uint8_t extensionId_;
    mutable std::mutex mutex_;
    FeedbackCoordinator coordinator_;
    ObservationCounters counters_;
    bool observing_ = true;
};
} // namespace uu::twcc
