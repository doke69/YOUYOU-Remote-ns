#pragma once
#include <cstdint>

namespace uu {
// RTP timestamps are 32-bit modular. Extend them with a signed modular delta so
// normal wrap is continuous; transport stats must feed this from a monotonic
// clock, not wall time.
inline int32_t signedRtpDelta(uint32_t current, uint32_t previous) {
    const uint32_t raw = current - previous;
    if (raw <= 0x7fffffffu) return int32_t(raw);
    return int32_t(int64_t(raw) - 0x100000000LL);
}

class RtpJitterTracker {
  public:
    explicit RtpJitterTracker(unsigned clockRate) : clockRate_(clockRate) {}

    void observe(int64_t arrivalMs, uint32_t timestamp) {
        if (!haveTimestamp_) {
            extendedTimestamp_ = int64_t(timestamp);
            haveTimestamp_ = true;
        } else {
            extendedTimestamp_ += signedRtpDelta(timestamp, lastTimestamp_);
        }
        lastTimestamp_ = timestamp;
        const int64_t arrival = arrivalMs * int64_t(clockRate_) / 1000;
        const int64_t transit = arrival - extendedTimestamp_;
        if (haveTransit_) {
            int64_t delta = transit - lastTransit_;
            if (delta < 0) delta = -delta;
            jitter_ += (double(delta) - jitter_) / 16.0;
        }
        haveTransit_ = true;
        lastTransit_ = transit;
    }

    double jitterUnits() const { return jitter_; }
    double jitterMs() const { return clockRate_ ? jitter_ / double(clockRate_) * 1000.0 : 0.0; }
    void reset() {
        haveTimestamp_ = haveTransit_ = false;
        extendedTimestamp_ = lastTransit_ = 0;
        lastTimestamp_ = 0;
        jitter_ = 0;
    }

  private:
    unsigned clockRate_;
    bool haveTimestamp_ = false;
    bool haveTransit_ = false;
    uint32_t lastTimestamp_ = 0;
    int64_t extendedTimestamp_ = 0;
    int64_t lastTransit_ = 0;
    double jitter_ = 0;
};
} // namespace uu
