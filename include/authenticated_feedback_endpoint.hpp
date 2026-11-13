#pragma once
#include <rtc/mediahandler.hpp>
#include <mutex>
#include <vector>

namespace uu::twcc {
// Shared by TWCC and classic REMB. The callback comes only from the patched
// authenticated global media handler and binds the original SRTP transport.
class AuthenticatedFeedbackEndpoint {
  public:
    bool bind(const rtc::message_callback &callback) noexcept;
    bool ready() const;
    // The owning feedback controller serializes submit() with stop().
    // Transport callbacks run outside mutex_ so RX can keep binding/observing.
    bool submit(const std::vector<uint8_t> &packet);
    void stop();
  private:
    mutable std::mutex mutex_;
    rtc::message_callback callback_;
    bool stopped_ = false;
};
} // namespace uu::twcc
