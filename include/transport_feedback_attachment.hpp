#pragma once
#include "transport_feedback_sender.hpp"
#include <atomic>

namespace uu::twcc {
class AuthenticatedFeedbackEndpoint;
enum class FeedbackAttachmentState { Observing, WaitingForEndpoint, Sending, Stopped, Faulted };
// One instance for one final validated offer/answer + transport generation.
// Caller attaches this globally ONLY after inspectNegotiationSdp returns Ready.
// If another global handler exists, append this as the LAST node: this library's
// incomingChain visits next() before incoming(). Do not put mutators after it.
// Stop/detach before any renegotiation, ICE restart or connection teardown;
// never reuse this instance or its callback for a different generation.
class FeedbackAttachment final : public rtc::MediaHandler {
  public:
    FeedbackAttachment(NegotiationResult negotiation, FeedbackTiming timing);
    ~FeedbackAttachment() override;
    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
    // Default mode is observation only. Returns false unless this translation
    // unit was explicitly built with UU_TWCC_SEND_EXPERIMENT. No implicit defaults
    // for sender identity, rate budget or retry policy.
    bool startSending(std::shared_ptr<SharedRtcpBudget> budget, FeedbackSendPolicy policy,
                      uint32_t senderSsrc, uint32_t mediaSsrc);
    FeedbackPollResult poll(int64_t nowUs);
    void stop(); // Waits for an in-flight short submission; never re-enable.
    FeedbackAttachmentState state() const;
    ObservationCounters observationCounters() const;
    FeedbackSendCounters sendCounters() const;
  private:
    std::shared_ptr<TransportFeedbackObserver> observer_;
    std::shared_ptr<AuthenticatedFeedbackEndpoint> endpoint_;
    mutable std::mutex controlMutex_;
    std::unique_ptr<FeedbackSender> sender_;
    FeedbackAttachmentState state_ = FeedbackAttachmentState::Observing;
    std::atomic<bool> sendingRequested_{false};
};
} // namespace uu::twcc
