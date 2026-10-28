#include "rtp_jitter.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        require(uu::signedRtpDelta(0xFFFFFFFFu, 0u) == -1, "backward one tick");
        require(uu::signedRtpDelta(0u, 0xFFFFFFFFu) == 1, "forward wrap one tick");
        require(uu::signedRtpDelta(10u, 0xFFFFFFF6u) == 20, "forward across wrap");

        // Steady 16ms packets at 90kHz.
        uu::RtpJitterTracker tracker(90000);
        for (int i = 0; i < 200; ++i)
            tracker.observe(int64_t(i) * 16, uint32_t(i) * 1440u);
        require(tracker.jitterMs() >= 0.0 && tracker.jitterMs() < 1.0,
                "steady stream jitter must stay near zero");

        // Full 32-bit timestamp wrap must be continuous.
        uu::RtpJitterTracker wrapped(90000);
        uint32_t timestamp = 0xFFFFFF00u;
        for (int i = 0; i < 200; ++i) {
            wrapped.observe(int64_t(i) * 1000 / 60, timestamp);
            timestamp += 1500u;
        }
        require(wrapped.jitterMs() >= 0.0 && wrapped.jitterMs() < 1.0,
                "RTP wrap must not spike jitter");

        // A long arrival gap (or wall-clock jump if a caller ever supplied it)
        // creates only a transient, finite sample; the next steady trace decays.
        uu::RtpJitterTracker gap(90000);
        gap.observe(0, 0);
        gap.observe(5000, 1440);
        for (int i = 2; i < 200; ++i)
            gap.observe(5000 + int64_t(i - 1) * 16, uint32_t(i) * 1440u);
        require(gap.jitterMs() < 1.0, "gap must decay; no permanent NaN/overflow");

        tracker.reset();
        require(tracker.jitterMs() == 0.0, "reset clears jitter");
        std::cout << "PASS: signed RTP delta/wrap and steady monotonic jitter trajectory; reset\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
