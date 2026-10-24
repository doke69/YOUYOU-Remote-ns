#pragma once
#include "transport_feedback_observer.hpp"
#include <functional>
#include <memory>

namespace uu::twcc {
enum class RtcpProducer { ReceiverReport, NegativeAck, TransportFeedback, Other, KeyframeRequest };
struct RtcpBudgetCounters {
    std::array<uint64_t, 5> attempts{};
    std::array<uint64_t, 5> deferred{};
};
// One instance per authenticated transport, shared by all adapted producers.
// Existing unadapted RR/NACK paths are NOT automatically covered by this gate.
class SharedRtcpBudget {
  public:
    SharedRtcpBudget(uint32_t bytesPerSecond, uint32_t burstBytes,
                     uint32_t initialCreditBytes, uint32_t criticalReserveBytes,
                     int64_t startUs);
    bool trySpend(RtcpProducer producer, size_t packetBytes,
                  size_t overheadBytes, int64_t nowUs);
    RtcpBudgetCounters counters() const;
  private:
    mutable std::mutex mutex_;
    FeedbackBudget budget_;
    uint32_t criticalReserveBytes_;
    RtcpBudgetCounters counters_;
};

struct FeedbackSendPolicy {
    int64_t firstRetryUs;
    int64_t maxRetryUs;
    int64_t maxPendingAgeUs;
    unsigned maxConsecutiveFailures;
    size_t overheadBytes; // Actual SRTCP + UDP/IP (and TURN, if used) allowance.
    std::string rtcpCname; // Same nonsecret CNAME as this RTP session's regular RTCP.
};
enum class FeedbackPollResult {
    NoReport, RetryLater, BudgetDeferred, Submitted, SendFailed, Stopped
};
enum class FeedbackStopReason {
    None, Requested, ClockInvalid, PendingExpired, SendFailureLimit,
    CommitRejected, InternalFailure
};
struct FeedbackSendCounters {
    uint64_t submitted = 0, sendFailures = 0, budgetDeferred = 0;
    FeedbackStopReason stopped = FeedbackStopReason::None;
};
// Scheduler-driven, one poller per observer; creates no thread/socket/timer.
// Every submission is a compound RR + SDES(CNAME) + feedback packet, so this
// sender does not require reduced-size RTCP negotiation. Normal RR scheduling
// and reception report statistics still belong to the existing RTCP session.
// Submit MUST be a short, nonblocking authenticated SRTCP send, returning true
// only on accepted submission. It must not call back into this sender. The
// callback belongs to this connection only; do not retain it across reconnects.
// No sends occur while holding the observer or shared-budget mutex.
class FeedbackSender {
  public:
    using Submit = std::function<bool(const std::vector<uint8_t> &)>;
    FeedbackSender(std::shared_ptr<TransportFeedbackObserver> observer,
                   std::shared_ptr<SharedRtcpBudget> budget, Submit submit,
                   FeedbackSendPolicy policy, uint32_t senderSsrc,
                   uint32_t mediaSsrc);
    FeedbackPollResult poll(int64_t nowUs);
    void stop();
    FeedbackSendCounters counters() const;
  private:
    FeedbackPollResult fail(FeedbackStopReason reason);
    std::shared_ptr<TransportFeedbackObserver> observer_;
    std::shared_ptr<SharedRtcpBudget> budget_;
    Submit submit_;
    FeedbackSendPolicy policy_;
    uint32_t senderSsrc_, mediaSsrc_;
    mutable std::mutex mutex_;
    FeedbackSendCounters counters_;
    std::optional<int64_t> lastPollUs_, pendingSinceUs_;
    uint64_t ticket_ = 0;
    unsigned consecutiveFailures_ = 0;
    int64_t retryDelayUs_ = 0, nextAttemptUs_ = 0;
};
} // namespace uu::twcc
