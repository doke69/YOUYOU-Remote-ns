#include "receive_bandwidth_observer.hpp"
#include "route_bitrate_policy.hpp"
#include "modules/remote_bitrate_estimator/include/remote_bitrate_estimator.h"
#include "system_wrappers/include/clock.h"
#include <atomic>
#include <chrono>
#ifdef UU_RBE_CLIENT_SEND
#include "authenticated_feedback_endpoint.hpp"
#include "modules/congestion_controller/remb_throttler.h"
#include <algorithm>
#endif

namespace uu::rbe {
namespace {
// Same steady_clock epoch as IceTransport::RecvCallback receivedAtUs.
// WebRTC's platform clock epoch must not be assumed to match it.
class ArrivalClock final : public webrtc::Clock {
  public:
    webrtc::Timestamp CurrentTime() override {
        return webrtc::Timestamp::Micros(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    webrtc::NtpTime ConvertTimestampToNtpTime(webrtc::Timestamp timestamp) override {
        auto *real = webrtc::Clock::GetRealTimeClock();
        return real->ConvertTimestampToNtpTime(real->CurrentTime() + (timestamp - CurrentTime()));
    }
};
} // namespace

struct ReceiveBandwidthObserver::Impl final : webrtc::RemoteBitrateObserver {
    explicit Impl(InputConfig input, webrtc::Clock *testClock)
        : config(std::move(input)), activeClock(testClock ? *testClock : clock),
#ifdef UU_RBE_CLIENT_SEND
          throttler([this](int64_t bitrate, std::vector<uint32_t> ssrcs) {
              std::lock_guard<std::mutex> lock(outputMutex);
              if (bitrate <= 0 || bitrate > UINT32_MAX || ssrcs.empty() || ssrcs.size() > 255 ||
                  std::find(ssrcs.begin(), ssrcs.end(), 0) != ssrcs.end()) {
                  ++invalid;
                  return;
              }
              if (pending) ++replaced;
              pending = Report{++created, activeClock.TimeInMicroseconds(), uint32_t(bitrate), std::move(ssrcs)};
          }, &activeClock),
#endif
          estimator(activeClock, *this) {}
    void OnReceiveBitrateChanged(const std::vector<uint32_t> &ssrcs, uint32_t bitrate) override {
        // No transport callback and no estimator re-entry under its input lock.
        estimates.fetch_add(1, std::memory_order_relaxed);
#ifdef UU_RBE_CLIENT_SEND
        if (sendingRequested.load(std::memory_order_relaxed)) throttler.OnReceiveBitrateChanged(ssrcs, bitrate);
#else
        (void)ssrcs; (void)bitrate;
#endif
    }
    const InputConfig config;
    ArrivalClock clock;
    webrtc::Clock &activeClock;
    std::atomic<uint64_t> estimates{0};
#ifdef UU_RBE_CLIENT_SEND
    struct Report { uint64_t ticket; int64_t atUs; uint32_t bps; std::vector<uint32_t> ssrcs; };
    std::mutex outputMutex;
    std::optional<Report> pending;
    uint64_t created = 0, replaced = 0, invalid = 0;
    webrtc::RembThrottler throttler;
    std::atomic<bool> sendingRequested{false};
    std::atomic<bool> endpointBound{false};
    mutable std::mutex controlMutex;
    twcc::AuthenticatedFeedbackEndpoint endpoint;
    std::shared_ptr<twcc::SharedRtcpBudget> budget;
    twcc::FeedbackSendPolicy policy{};
    uint32_t senderSsrc = 0;
    bool started = false;
    std::optional<int64_t> lastPollUs;
    int64_t nextAttemptUs = 0, retryDelayUs = 0;
    unsigned consecutiveFailures = 0;
    ReceiveFeedbackCounters counters;
#endif
    ReceiveBandwidthEstimator estimator;
};

ReceiveBandwidthObserver::ReceiveBandwidthObserver(InputConfig config, webrtc::Clock *testClock)
    : impl_(std::make_unique<Impl>(std::move(config), testClock)) {}
ReceiveBandwidthObserver::~ReceiveBandwidthObserver() = default;

void ReceiveBandwidthObserver::incoming(rtc::message_vector &messages, const rtc::message_callback &send) {
#ifndef UU_RBE_CLIENT_SEND
    (void)send;
#endif
    for (const auto &message : messages) {
        if (!message) continue;
#ifdef UU_RBE_CLIENT_SEND
        // Bind once before any submission; subsequent RX reuses that endpoint
        // without taking its binding mutex again.
        if ((message->type == rtc::Message::Binary || message->type == rtc::Message::Control) &&
            impl_->sendingRequested.load(std::memory_order_relaxed) &&
            !impl_->endpointBound.load(std::memory_order_relaxed) && impl_->endpoint.bind(send))
            impl_->endpointBound.store(true, std::memory_order_relaxed);
#endif
        if (message->type != rtc::Message::Binary) continue;
        impl_->estimator.incoming(readPacket(reinterpret_cast<const uint8_t *>(message->data()),
                                             message->size(), message->receivedAtUs, impl_->config));
    }
}
void ReceiveBandwidthObserver::poll() { impl_->estimator.process(); }
void ReceiveBandwidthObserver::close() {
#ifdef UU_RBE_CLIENT_SEND
    impl_->sendingRequested.store(false, std::memory_order_relaxed);
#endif
    impl_->estimator.close(); // Waits for already admitted estimate callbacks.
#ifdef UU_RBE_CLIENT_SEND
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    impl_->endpoint.stop();
    if (impl_->counters.transport.stopped == twcc::FeedbackStopReason::None)
        impl_->counters.transport.stopped = twcc::FeedbackStopReason::Requested;
    std::lock_guard<std::mutex> outputLock(impl_->outputMutex);
    impl_->pending.reset();
#endif
}
ObservationSnapshot ReceiveBandwidthObserver::snapshot() const {
    return {impl_->estimator.counters(), impl_->estimates.load(std::memory_order_relaxed),
            impl_->estimator.latestEstimateBps()};
}
#ifdef UU_RBE_CLIENT_SEND
bool ReceiveBandwidthObserver::startFeedback(std::shared_ptr<twcc::SharedRtcpBudget> budget,
    twcc::FeedbackSendPolicy policy, uint32_t senderSsrc) {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    if (impl_->started || impl_->counters.transport.stopped != twcc::FeedbackStopReason::None ||
        !budget || !senderSsrc || policy.rtcpCname.empty() || policy.rtcpCname.size() > 255 ||
        std::any_of(policy.rtcpCname.begin(), policy.rtcpCname.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        policy.firstRetryUs <= 0 || policy.maxRetryUs < policy.firstRetryUs || policy.maxRetryUs > 10000000 ||
        policy.maxPendingAgeUs < policy.maxRetryUs || policy.maxPendingAgeUs > 60000000 ||
        !policy.maxConsecutiveFailures || policy.maxConsecutiveFailures > 1024 ||
        !policy.overheadBytes || policy.overheadBytes > 4096) return false;
    impl_->budget = std::move(budget);
    impl_->policy = std::move(policy);
    impl_->senderSsrc = senderSsrc;
    impl_->started = true;
    impl_->sendingRequested.store(true, std::memory_order_relaxed);
    return true;
}
twcc::FeedbackPollResult ReceiveBandwidthObserver::pollFeedback(int64_t nowUs, uint32_t ceilingBps) {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    auto &counters = impl_->counters.transport;
    if (counters.stopped != twcc::FeedbackStopReason::None) return twcc::FeedbackPollResult::Stopped;
    if (!impl_->started) return twcc::FeedbackPollResult::NoReport;
    if (nowUs < 0 || nowUs > (int64_t(1) << 55) || (impl_->lastPollUs && nowUs < *impl_->lastPollUs)) {
        counters.stopped = twcc::FeedbackStopReason::ClockInvalid;
    } else {
        impl_->lastPollUs = nowUs;
        if (nowUs < impl_->nextAttemptUs) return twcc::FeedbackPollResult::RetryLater;
        std::optional<Impl::Report> report;
        {
            std::lock_guard<std::mutex> outputLock(impl_->outputMutex);
            report = impl_->pending;
        }
        if (!report) return twcc::FeedbackPollResult::NoReport;
        report->bps = cappedFeedbackBps(report->bps, ceilingBps);
        // RX can replace the mailbox after the caller sampled nowUs. Wait for
        // the next poll instead of treating that normal ordering as expiration.
        if (report->atUs > nowUs) return twcc::FeedbackPollResult::RetryLater;
        if (nowUs - report->atUs > impl_->policy.maxPendingAgeUs) {
            counters.stopped = twcc::FeedbackStopReason::PendingExpired;
        } else if (!impl_->endpoint.ready()) {
            return twcc::FeedbackPollResult::RetryLater;
        } else {
            const auto &cname = impl_->policy.rtcpCname;
            const size_t prefix = 8 + ((11 + cname.size() + 3) & ~size_t(3));
            const size_t total = prefix + 20 + report->ssrcs.size() * 4;
            if (total > 1200) {
                counters.stopped = twcc::FeedbackStopReason::InternalFailure;
            } else {
                // Empty RR avoids fabricating reception statistics. The existing
                // primary RTCP session continues to send its accurate reports.
                std::vector<uint8_t> packet(total, 0);
                auto put32 = [&](size_t at, uint32_t value) {
                    for (int k = 0; k < 4; ++k) packet[at + k] = uint8_t(value >> (24 - k * 8));
                };
                packet[0] = 0x80; packet[1] = 201; packet[3] = 1;
                put32(4, impl_->senderSsrc);
                packet[8] = 0x81; packet[9] = 202;
                const auto sdesWords = (prefix - 8) / 4 - 1;
                packet[10] = uint8_t(sdesWords >> 8); packet[11] = uint8_t(sdesWords);
                put32(12, impl_->senderSsrc);
                packet[16] = 1; packet[17] = uint8_t(cname.size());
                std::copy(cname.begin(), cname.end(), packet.begin() + 18);
                packet[prefix] = 0x8f; packet[prefix + 1] = 206;
                const auto rembWords = (total - prefix) / 4 - 1;
                packet[prefix + 2] = uint8_t(rembWords >> 8); packet[prefix + 3] = uint8_t(rembWords);
                put32(prefix + 4, impl_->senderSsrc); // Media-source header remains zero.
                packet[prefix + 12] = 'R'; packet[prefix + 13] = 'E';
                packet[prefix + 14] = 'M'; packet[prefix + 15] = 'B';
                uint32_t mantissa = report->bps, exponent = 0;
                while (mantissa > 0x3ffff) { mantissa >>= 1; ++exponent; }
                put32(prefix + 16, uint32_t(report->ssrcs.size()) << 24 | exponent << 18 | mantissa);
                for (size_t i = 0; i < report->ssrcs.size(); ++i) put32(prefix + 20 + 4 * i, report->ssrcs[i]);
                if (!impl_->budget->trySpend(twcc::RtcpProducer::Other, packet.size(), impl_->policy.overheadBytes, nowUs)) {
                    ++counters.budgetDeferred;
                    return twcc::FeedbackPollResult::BudgetDeferred;
                }
                counters.attemptedBytes += packet.size();
                bool accepted = false;
                try { accepted = impl_->endpoint.submit(packet); }
                catch (...) { accepted = false; } // Actual external SRTP callback failure boundary.
                if (accepted) {
                    ++counters.submitted;
                    counters.submittedBytes += packet.size();
                    impl_->counters.lastSubmittedBps = report->bps;
                    impl_->counters.lastSubmittedSsrcs = report->ssrcs.size();
                    impl_->consecutiveFailures = 0; impl_->retryDelayUs = 0; impl_->nextAttemptUs = 0;
                    std::lock_guard<std::mutex> outputLock(impl_->outputMutex);
                    if (impl_->pending && impl_->pending->ticket == report->ticket) impl_->pending.reset();
                    return twcc::FeedbackPollResult::Submitted;
                }
                ++counters.sendFailures;
                if (++impl_->consecutiveFailures < impl_->policy.maxConsecutiveFailures) {
                    impl_->retryDelayUs = impl_->retryDelayUs ?
                        std::min(impl_->policy.maxRetryUs, impl_->retryDelayUs * 2) : impl_->policy.firstRetryUs;
                    impl_->nextAttemptUs = nowUs + impl_->retryDelayUs;
                    return twcc::FeedbackPollResult::SendFailed;
                }
                counters.stopped = twcc::FeedbackStopReason::SendFailureLimit;
            }
        }
    }
    impl_->sendingRequested.store(false, std::memory_order_relaxed);
    impl_->endpoint.stop();
    return twcc::FeedbackPollResult::Stopped;
}
ReceiveFeedbackCounters ReceiveBandwidthObserver::feedbackCounters() const {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    auto result = impl_->counters;
    std::lock_guard<std::mutex> outputLock(impl_->outputMutex);
    result.created = impl_->created; result.replaced = impl_->replaced; result.invalid = impl_->invalid;
    return result;
}
#endif
} // namespace uu::rbe
