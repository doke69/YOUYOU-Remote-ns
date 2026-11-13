#pragma once
#ifndef UU_RECEIVE_TIMESTAMPS
#error "RBE observation requires the uniform receive-timestamp Message ABI"
#endif
#include "receive_bandwidth_estimator.hpp"
#include <rtc/mediahandler.hpp>
#ifdef UU_RBE_CLIENT_SEND
#include "transport_feedback_sender.hpp"
#endif

namespace uu::rbe {
struct ObservationSnapshot {
    EstimatorCounters input;
    uint64_t estimates = 0;
    int64_t estimateBps = 0;
};
#ifdef UU_RBE_CLIENT_SEND
struct ReceiveFeedbackCounters {
    twcc::FeedbackSendCounters transport;
    uint64_t created = 0, replaced = 0, invalid = 0;
    uint32_t lastSubmittedBps = 0;
    size_t lastSubmittedSsrcs = 0;
};
#endif
// Authenticated global handler only. Does not retain/modify messages, send,
// advertise capabilities, or own frames. One instance per validated peer.
class ReceiveBandwidthObserver final : public rtc::MediaHandler {
  public:
    explicit ReceiveBandwidthObserver(InputConfig config, webrtc::Clock *testClock = nullptr);
    ~ReceiveBandwidthObserver() override;
    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
    void poll(); // Existing Client loop only; no additional worker.
    void close();
    ObservationSnapshot snapshot() const;
#ifdef UU_RBE_CLIENT_SEND
    // Starts only after Client has cleared the SR-repeated compatibility target.
    // Estimates go through the original throttler and a single latest mailbox.
    bool startFeedback(std::shared_ptr<twcc::SharedRtcpBudget> budget,
                       twcc::FeedbackSendPolicy policy, uint32_t senderSsrc);
    twcc::FeedbackPollResult pollFeedback(int64_t nowUs, uint32_t ceilingBps=0);
    ReceiveFeedbackCounters feedbackCounters() const;
#endif
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace uu::rbe
