#include "transport_feedback_observer.hpp"
#include <stdexcept>

namespace uu::twcc {
TransportFeedbackObserver::TransportFeedbackObserver(NegotiationResult negotiation,
                                                       FeedbackTiming timing)
    : extensionId_(negotiation.extensionId), coordinator_(timing) {
    if (negotiation.status != NegotiationStatus::Ready || !extensionId_ ||
        !negotiation.videoPayloads)
        throw std::invalid_argument("Transport feedback has not been negotiated");
}

void TransportFeedbackObserver::incoming(rtc::message_vector &messages,
                                         const rtc::message_callback &) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!observing_) return;
    for (const auto &message : messages) {
        if (!message || message->type != rtc::Message::Binary) continue;
        const auto extension = readTransportSequence(
            reinterpret_cast<const uint8_t *>(message->data()), message->size(), extensionId_);
        switch (extension.status) {
        case ExtensionStatus::Missing: ++counters_.missingExtension; continue;
        case ExtensionStatus::Malformed: ++counters_.malformed; continue;
        case ExtensionStatus::UnsupportedProfile: ++counters_.unsupportedProfile; continue;
        case ExtensionStatus::Found: break;
        }
        if (message->receivedAtUs < 0) { ++counters_.missingTimestamp; continue; }
        switch (coordinator_.record(extension.sequence, message->receivedAtUs).status) {
        case ArrivalStatus::Accepted: ++counters_.accepted; break;
        case ArrivalStatus::Duplicate: ++counters_.duplicates; break;
        case ArrivalStatus::TooOld: ++counters_.tooOld; break;
        case ArrivalStatus::WindowFull: ++counters_.windowFull; break;
        case ArrivalStatus::AmbiguousSequence: ++counters_.ambiguous; break;
        case ArrivalStatus::InvalidTime: ++counters_.invalidTime; break;
        }
    }
    // Leave ownership, order, payloads and send callback untouched.
}
ObservationCounters TransportFeedbackObserver::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}
std::optional<PreparedFeedback> TransportFeedbackObserver::prepare(int64_t nowUs,
    uint32_t senderSsrc, uint32_t mediaSsrc, size_t mtu) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!observing_) return std::nullopt;
    const auto *pending = coordinator_.prepare(nowUs, senderSsrc, mediaSsrc, mtu);
    return pending ? std::optional<PreparedFeedback>(*pending) : std::nullopt;
}
bool TransportFeedbackObserver::commit(uint64_t ticket, int64_t sentAtUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    return observing_ && coordinator_.commit(ticket, sentAtUs);
}
void TransportFeedbackObserver::stopObserving() {
    std::lock_guard<std::mutex> lock(mutex_);
    observing_ = false;
}
void TransportFeedbackObserver::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    coordinator_.reset();
    counters_ = {};
    observing_ = false;
}
} // namespace uu::twcc
