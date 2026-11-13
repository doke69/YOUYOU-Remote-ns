#include "remb_feedback.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>

// Target the Client's single active REMB entry. These are policy-only checks:
// they never claim that a PC accepted or applied a REMB target.
namespace {
using uu::RembFeedbackGate;
void require(bool value, const char *what) {
    if (!value)
        throw std::runtime_error(what);
}
} // namespace

int main() {
    try {
        constexpr unsigned fiftyMbps = 50000000u;
        constexpr unsigned twentyFiveMbps = 25000000u;
        constexpr uint32_t media = 0x55667788;
        constexpr uint32_t media2 = 0x01020304;

        RembFeedbackGate gate;
        require(gate.prepare(1000, fiftyMbps, media, false, false) ==
                    RembFeedbackGate::Action::NotNegotiated,
                "not negotiated must not send");
        require(!gate.accepted() && !gate.acceptedCount(), "not negotiated must not mark success");

        require(gate.prepare(1000, fiftyMbps, media, true, false) ==
                    RembFeedbackGate::Action::FirstRtpMissing,
                "first-packet rule");
        require(!gate.accepted() && !gate.acceptedCount(), "first-packet rule must not mark success");

        require(gate.prepare(1000, fiftyMbps, media, true, true) == RembFeedbackGate::Action::Send,
                "send after first primary RTP");
        gate.accepted(fiftyMbps, media);
        require(gate.accepted() && gate.targetBps() == fiftyMbps && gate.mediaSsrc() == media &&
                    gate.acceptedCount() == 1,
                "local acceptance is recorded");

        require(gate.prepare(2000, fiftyMbps, media, true, true) ==
                    RembFeedbackGate::Action::Duplicate,
                "same target and media must deduplicate");
        require(gate.acceptedCount() == 1, "duplicate must not count as a new acceptance");

        require(gate.prepare(2000, twentyFiveMbps, media, true, true) ==
                    RembFeedbackGate::Action::Send,
                "changed target must send once");
        gate.accepted(twentyFiveMbps, media);
        require(gate.targetBps() == twentyFiveMbps && gate.acceptedCount() == 2,
                "changed target replaces the accepted target");

        // The target identity includes the primary RTP SSRC. A new primary
        // source must resend even when the bitrate ceiling is unchanged.
        require(gate.prepare(2000, twentyFiveMbps, media2, true, true) ==
                    RembFeedbackGate::Action::Send,
                "changed media SSRC must send once");
        gate.accepted(twentyFiveMbps, media2);
        require(gate.mediaSsrc() == media2 && gate.acceptedCount() == 3,
                "changed media SSRC replaces the accepted target");

        RembFeedbackGate retry;
        require(retry.prepare(10000, fiftyMbps, media, true, true) == RembFeedbackGate::Action::Send,
                "initial attempt");
        retry.failed(10000);
        require(!retry.accepted() && retry.deferredCount() == 1,
                "failed local submission is not success");
        require(retry.prepare(10500, fiftyMbps, media, true, true) ==
                    RembFeedbackGate::Action::RetryLater,
                "retry is bounded");
        require(retry.prepare(11000, fiftyMbps, media, true, true) == RembFeedbackGate::Action::Send,
                "retry after the bound");
        retry.accepted(fiftyMbps, media);
        require(retry.accepted() && retry.deferredCount() == 1, "accepted after retry");

        // A target/mode change in place keeps counters but requires a new local
        // acceptance; a full reconnect also clears them.
        retry.invalidate();
        require(!retry.accepted() && retry.targetBps() == 0 && retry.mediaSsrc() == 0,
                "invalidate clears acceptance");
        require(retry.acceptedCount() == 1 && retry.deferredCount() == 1,
                "invalidate preserves lifetime counters");
        require(retry.prepare(12000, fiftyMbps, media, true, true) == RembFeedbackGate::Action::Send,
                "send after invalidate");

        retry.accepted(fiftyMbps, media);
        retry.reset();
        require(!retry.accepted() && retry.targetBps() == 0 && retry.mediaSsrc() == 0 &&
                    retry.acceptedCount() == 0 && retry.deferredCount() == 0,
                "reconnect reset clears all state");

        // No target is a configuration/negotiation condition, not a send.
        require(gate.prepare(3000, 0, media, true, true) == RembFeedbackGate::Action::NotNegotiated,
                "zero target must not send");
        // Clock rollback must not trap the gate in RetryLater.
        RembFeedbackGate clockRollback;
        clockRollback.failed(5000);
        require(clockRollback.prepare(4000, fiftyMbps, media, true, true) ==
                    RembFeedbackGate::Action::Send,
                "clock rollback must not block sending");

        std::cout << "PASS: REMB first-packet/dedupe/target-change/retry/reconnect gate\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
