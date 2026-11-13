#pragma once
#include <cstdint>

namespace uu {
// Client-side gate for the single active REMB feedback entry.
//
// It records only whether the local media handler accepted the compound SRTCP
// submission. It never claims that the computer accepted, applied, or acted on
// the target; actual output remains the measured RTP bitrate. The gate keeps
// the first-packet rule, same-target deduplication, target changes, and a
// bounded retry after a rejected local submission.
class RembFeedbackGate {
  public:
    enum class Action {
        Send,           // The caller may submit this target now.
        Duplicate,      // The same target was already accepted locally.
        NotNegotiated,  // REMB was not negotiated or no ceiling is configured.
        FirstRtpMissing,// The primary RTP SSRC is not known yet.
        RetryLater,     // The previous local submission failed recently.
    };

    Action prepare(int64_t nowMs, unsigned wantedBps, uint32_t mediaSsrc, bool negotiated,
                   bool firstRtp) {
        if (!negotiated || wantedBps == 0)
            return Action::NotNegotiated;
        if (accepted_ && targetBps_ == wantedBps && mediaSsrc_ == mediaSsrc)
            return Action::Duplicate;
        if (!firstRtp)
            return Action::FirstRtpMissing;
        if (lastFailedAttemptMs_ && nowMs >= lastFailedAttemptMs_ &&
            nowMs - lastFailedAttemptMs_ < retryDelayMs_)
            return Action::RetryLater;
        return Action::Send;
    }

    void accepted(unsigned targetBps, uint32_t mediaSsrc) {
        targetBps_ = targetBps;
        mediaSsrc_ = mediaSsrc;
        accepted_ = true;
        lastFailedAttemptMs_ = 0;
        ++acceptedCount_;
    }

    void failed(int64_t nowMs) {
        ++deferredCount_;
        lastFailedAttemptMs_ = nowMs;
    }

    // Keep counters but require a new local acceptance. Used when the target or
    // quality mode changes in place.
    void invalidate() {
        targetBps_ = 0;
        mediaSsrc_ = 0;
        accepted_ = false;
        lastFailedAttemptMs_ = 0;
    }

    // Full reconnect/reset: also clears the observable counters.
    void reset() {
        invalidate();
        acceptedCount_ = 0;
        deferredCount_ = 0;
    }

    bool accepted() const { return accepted_; }
    unsigned targetBps() const { return targetBps_; }
    uint32_t mediaSsrc() const { return mediaSsrc_; }
    uint64_t acceptedCount() const { return acceptedCount_; }
    uint64_t deferredCount() const { return deferredCount_; }

  private:
    static constexpr int64_t retryDelayMs_ = 1000;
    unsigned targetBps_ = 0;
    uint32_t mediaSsrc_ = 0;
    bool accepted_ = false;
    uint64_t acceptedCount_ = 0;
    uint64_t deferredCount_ = 0;
    int64_t lastFailedAttemptMs_ = 0;
};
} // namespace uu
