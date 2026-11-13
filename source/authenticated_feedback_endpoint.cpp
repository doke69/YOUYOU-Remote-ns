#include "authenticated_feedback_endpoint.hpp"
#include <cstring>

namespace uu::twcc {
bool AuthenticatedFeedbackEndpoint::bind(const rtc::message_callback &callback) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || !callback) return false;
        if (!callback_) callback_ = callback;
        return true;
    } catch (...) { return false; } // Callback-copy failure must not interrupt media.
}
bool AuthenticatedFeedbackEndpoint::ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stopped_ && bool(callback_);
}
bool AuthenticatedFeedbackEndpoint::submit(const std::vector<uint8_t> &packet) {
    if (packet.size() < 8 || packet.size() > 1200 ||
        packet[0] >> 6 != 2 || packet[1] != 201) return false;
    rtc::message_callback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || !callback_) return false;
        callback = callback_;
    }
    // The owner serializes submit/stop. RX bind/ready must remain available
    // while the authenticated transport callback waits on a slow send.
    auto message = rtc::make_message(packet.size(), rtc::Message::Control);
    std::memcpy(message->data(), packet.data(), packet.size());
    callback(std::move(message)); // Patched SRTP callback throws on rejected submission.
    return true;
}
void AuthenticatedFeedbackEndpoint::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    callback_ = nullptr;
}
} // namespace uu::twcc
