#include "transport_feedback_attachment.hpp"
#include "authenticated_feedback_endpoint.hpp"
#include <cstring>

namespace uu::twcc {
// The callback is supplied by PeerConnection::forwardMedia AFTER SRTP/SRTCP
// authentication. That patched callback weakly binds its original transport,
// rejects replacement/closed connections and throws on failed submission.
// Only one connection-generation owner may bind this endpoint.
FeedbackAttachment::FeedbackAttachment(NegotiationResult negotiation, FeedbackTiming timing)
    : observer_(std::make_shared<TransportFeedbackObserver>(negotiation, timing)),
      endpoint_(std::make_shared<AuthenticatedFeedbackEndpoint>()), version_(negotiation.version) {}
FeedbackAttachment::~FeedbackAttachment() {
    try { stop(); } catch (...) {} // Destruction must not leak callback diagnostics.
}
void FeedbackAttachment::incoming(rtc::message_vector &messages, const rtc::message_callback &send) {
    if (sendingRequested_.load(std::memory_order_relaxed)) {
        for (const auto &message : messages) {
            if (message && (message->type == rtc::Message::Binary || message->type == rtc::Message::Control)) {
                endpoint_->bind(send);
                break;
            }
        }
    }
    observer_->incoming(messages, send); // No send is performed by this RX path.
}
bool FeedbackAttachment::startSending(std::shared_ptr<SharedRtcpBudget> budget,
    FeedbackSendPolicy policy, uint32_t senderSsrc, uint32_t mediaSsrc) {
    std::lock_guard<std::mutex> lock(controlMutex_);
#if defined(UU_TWCC_SEND_EXPERIMENT) && UU_TWCC_SEND_EXPERIMENT
    if ((state_ != FeedbackAttachmentState::Observing &&
         state_ != FeedbackAttachmentState::WaitingForExtension) || sender_) return false;
    std::weak_ptr<AuthenticatedFeedbackEndpoint> endpoint = endpoint_;
    try {
        auto sender = std::make_unique<FeedbackSender>(observer_, std::move(budget),
            [endpoint](const std::vector<uint8_t> &packet) {
                const auto current = endpoint.lock();
                return current && current->submit(packet);
            }, std::move(policy), senderSsrc, mediaSsrc);
        sender_ = std::move(sender);
    } catch (...) {
        return false; // Invalid optional policy/allocation must not stop media.
    }
    state_ = FeedbackAttachmentState::WaitingForEndpoint;
    startupSinceUs_.reset();
    lastPollUs_.reset();
    sendingRequested_.store(true, std::memory_order_relaxed);
    return true;
#else
    (void)budget; (void)policy; (void)senderSsrc; (void)mediaSsrc;
    return false;
#endif
}
FeedbackPollResult FeedbackAttachment::poll(int64_t nowUs) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (state_ == FeedbackAttachmentState::Stopped || state_ == FeedbackAttachmentState::Faulted)
        return FeedbackPollResult::Stopped;
    const bool clockInvalid = nowUs < 0 || nowUs > (int64_t(1) << 55) ||
        (lastPollUs_ && nowUs < *lastPollUs_);
    if (!sender_) {
        const auto observed = observer_->counters();
        if (observed.accepted) {
            startupSinceUs_.reset();
            lastPollUs_.reset();
            state_ = FeedbackAttachmentState::Observing;
            return FeedbackPollResult::NoReport;
        }
        if (!observed.missingExtension) return FeedbackPollResult::NoReport;
        if (!startupSinceUs_) startupSinceUs_ = nowUs;
        if (clockInvalid || nowUs - *startupSinceUs_ >= 2000000) {
            faultReason_ = clockInvalid ? FeedbackStopReason::ClockInvalid
                                      : FeedbackStopReason::MissingExtensionTimeout;
            observer_->stopObserving();
            endpoint_->stop();
            state_ = FeedbackAttachmentState::Faulted;
            return FeedbackPollResult::Stopped;
        }
        lastPollUs_ = nowUs;
        state_ = FeedbackAttachmentState::WaitingForExtension;
        return FeedbackPollResult::RetryLater;
    }
    // An authenticated endpoint still needs a fresh incoming callback after
    // startSending. Missing that callback is not the same as a V2 peer which
    // is receiving media normally but has not requested feedback yet.
    const bool endpointReady = endpoint_->ready();
    const auto counters = sender_->counters();
    bool waitingForRequest = false;
    if (version_ == TransportSequenceVersion::V2 && endpointReady && !counters.submitted) {
        const auto observed = observer_->counters();
        waitingForRequest = !observed.requestsQueued && !observed.requestHistoryUnavailable;
    }
    if (!startupSinceUs_) startupSinceUs_ = nowUs;
    const bool startupExpired = !waitingForRequest && !counters.submitted &&
        !clockInvalid && nowUs - *startupSinceUs_ >= 2000000;
    if (clockInvalid || startupExpired) {
        faultReason_ = clockInvalid ? FeedbackStopReason::ClockInvalid : FeedbackStopReason::StartupTimeout;
        sendingRequested_.store(false, std::memory_order_relaxed);
        sender_->stop();
        endpoint_->stop();
        state_ = FeedbackAttachmentState::Faulted;
        return FeedbackPollResult::Stopped;
    }
    lastPollUs_ = nowUs;
    if (waitingForRequest) {
        startupSinceUs_.reset();
        state_ = FeedbackAttachmentState::WaitingForRequest;
        return FeedbackPollResult::NoReport;
    }
    if (!endpointReady) return FeedbackPollResult::RetryLater;
    state_ = FeedbackAttachmentState::Sending;
    const auto result = sender_->poll(nowUs);
    if (result == FeedbackPollResult::Stopped) {
        sendingRequested_.store(false, std::memory_order_relaxed);
        endpoint_->stop();
        state_ = FeedbackAttachmentState::Faulted;
    }
    return result;
}
void FeedbackAttachment::stop() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (state_ == FeedbackAttachmentState::Stopped) return;
    sendingRequested_.store(false, std::memory_order_relaxed);
    if (sender_) sender_->stop(); else observer_->stopObserving();
    endpoint_->stop();
    state_ = FeedbackAttachmentState::Stopped;
}
FeedbackAttachmentState FeedbackAttachment::state() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return state_;
}
ObservationCounters FeedbackAttachment::observationCounters() const {
    return observer_->counters();
}
FeedbackSendCounters FeedbackAttachment::sendCounters() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    auto counters = sender_ ? sender_->counters() : FeedbackSendCounters{};
    if (faultReason_ != FeedbackStopReason::None) counters.stopped = faultReason_;
    return counters;
}
} // namespace uu::twcc
