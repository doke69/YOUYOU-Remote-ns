#include "transport_feedback_attachment.hpp"
#include <cstring>

namespace uu::twcc {
// The callback is supplied by PeerConnection::forwardMedia AFTER SRTP/SRTCP
// authentication. That patched callback uses a weak peer reference, loads the
// current authenticated transport and throws on failed sendMedia submission.
// Only one connection-generation owner may bind this endpoint.
class AuthenticatedFeedbackEndpoint {
  public:
    bool bind(const rtc::message_callback &callback) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || !callback) return false;
            if (!callback_) callback_ = callback; // Retain once, never replace.
            return true;
        } catch (...) { return false; } // Do not drop media on callback-copy failure.
    }
    bool ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !stopped_ && bool(callback_);
    }
    bool submit(const std::vector<uint8_t> &packet) {
        // Serialize invalidation with the short submission. No observer/budget
        // lock is held. Once stop returns no captured callback can be invoked.
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || !callback_ || packet.size() < 8 || packet.size() > 1200 ||
            packet[0] >> 6 != 2 || packet[1] != 201) return false;
        auto message = rtc::make_message(packet.size(), rtc::Message::Control);
        std::memcpy(message->data(), packet.data(), packet.size());
        callback_(std::move(message)); // Throws on rejection; sender catches it.
        return true;
    }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        callback_ = nullptr;
    }
  private:
    mutable std::mutex mutex_;
    rtc::message_callback callback_;
    bool stopped_ = false;
};

FeedbackAttachment::FeedbackAttachment(NegotiationResult negotiation, FeedbackTiming timing)
    : observer_(std::make_shared<TransportFeedbackObserver>(negotiation, timing)),
      endpoint_(std::make_shared<AuthenticatedFeedbackEndpoint>()) {}
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
    if (state_ != FeedbackAttachmentState::Observing || sender_) return false;
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
    if (!sender_) return FeedbackPollResult::NoReport;
    if (!endpoint_->ready()) return FeedbackPollResult::RetryLater;
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
    return sender_ ? sender_->counters() : FeedbackSendCounters{};
}
} // namespace uu::twcc
