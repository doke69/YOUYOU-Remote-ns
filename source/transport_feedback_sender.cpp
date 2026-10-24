#include "transport_feedback_sender.hpp"
#include <algorithm>
#include <stdexcept>

namespace uu::twcc {
namespace {
void appendSsrc(std::vector<uint8_t> &packet, uint32_t ssrc) {
    packet.push_back(uint8_t(ssrc >> 24)); packet.push_back(uint8_t(ssrc >> 16));
    packet.push_back(uint8_t(ssrc >> 8)); packet.push_back(uint8_t(ssrc));
}
size_t compoundPrefixBytes(const std::string &cname) {
    // RR header+SSRC (8), then SDES header+SSRC+item header+CNAME+END, word aligned.
    return 8 + ((11 + cname.size() + 3) & ~size_t(3));
}
std::vector<uint8_t> compoundFeedback(const std::vector<uint8_t> &feedback,
                                     uint32_t ssrc, const std::string &cname) {
    if (feedback.size() < 20 || feedback[0] >> 6 != 2 ||
        (feedback[0] & 31) != 15 || feedback[1] != 205 ||
        (size_t(feedback[2]) * 256 + feedback[3] + 1) * 4 != feedback.size())
        throw std::invalid_argument("Invalid prepared transport feedback");
    const auto prefixBytes = compoundPrefixBytes(cname);
    if (prefixBytes + feedback.size() > 1200)
        throw std::invalid_argument("Compound transport feedback exceeds MTU");
    std::vector<uint8_t> result;
    result.reserve(prefixBytes + feedback.size());
    result.insert(result.end(), {0x80, 201, 0, 1}); // RR with no reception report blocks.
    appendSsrc(result, ssrc);
    const auto sdesWords = (prefixBytes - 8) / 4 - 1;
    result.insert(result.end(), {0x81, 202, uint8_t(sdesWords >> 8), uint8_t(sdesWords)});
    appendSsrc(result, ssrc);
    result.push_back(1); result.push_back(uint8_t(cname.size()));
    result.insert(result.end(), cname.begin(), cname.end());
    result.push_back(0); // SDES END is distinct from RTCP padding.
    while (result.size() < prefixBytes) result.push_back(0);
    result.insert(result.end(), feedback.begin(), feedback.end()); // Padded packet stays last.
    return result;
}
} // namespace
SharedRtcpBudget::SharedRtcpBudget(uint32_t bytesPerSecond, uint32_t burstBytes,
    uint32_t initialCreditBytes, uint32_t criticalReserveBytes, int64_t startUs)
    : budget_(bytesPerSecond, burstBytes, initialCreditBytes, startUs),
      criticalReserveBytes_(criticalReserveBytes) {
    if (criticalReserveBytes >= burstBytes)
        throw std::invalid_argument("Invalid critical RTCP reserve");
}
bool SharedRtcpBudget::trySpend(RtcpProducer producer, size_t packetBytes,
    size_t overheadBytes, int64_t nowUs) {
    const auto index = size_t(producer);
    if (index >= counters_.attempts.size()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool critical = producer == RtcpProducer::ReceiverReport ||
        producer == RtcpProducer::NegativeAck || producer == RtcpProducer::KeyframeRequest;
    const bool accepted = budget_.trySpend(packetBytes, overheadBytes, nowUs,
                                          critical ? 0 : criticalReserveBytes_);
    if (accepted) ++counters_.attempts[index]; else ++counters_.deferred[index];
    return accepted;
}
RtcpBudgetCounters SharedRtcpBudget::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

FeedbackSender::FeedbackSender(std::shared_ptr<TransportFeedbackObserver> observer,
    std::shared_ptr<SharedRtcpBudget> budget, Submit submit, FeedbackSendPolicy policy,
    uint32_t senderSsrc, uint32_t mediaSsrc)
    : observer_(std::move(observer)), budget_(std::move(budget)), submit_(std::move(submit)),
      policy_(policy), senderSsrc_(senderSsrc), mediaSsrc_(mediaSsrc) {
    if (!observer_ || !budget_ || !submit_ || policy.firstRetryUs <= 0 ||
        policy.maxRetryUs < policy.firstRetryUs || policy.maxRetryUs > 10000000 ||
        policy.maxPendingAgeUs < policy.maxRetryUs || policy.maxPendingAgeUs > 60000000 ||
        !policy.maxConsecutiveFailures || policy.maxConsecutiveFailures > 1024 ||
        !policy.overheadBytes || policy.overheadBytes > 4096 || !senderSsrc ||
        policy.rtcpCname.empty() || policy.rtcpCname.size() > 255 ||
        std::any_of(policy.rtcpCname.begin(), policy.rtcpCname.end(),
                    [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Invalid transport feedback send policy");
}
FeedbackPollResult FeedbackSender::fail(FeedbackStopReason reason) {
    counters_.stopped = reason;
    observer_->stopObserving();
    return FeedbackPollResult::Stopped;
}
FeedbackPollResult FeedbackSender::poll(int64_t nowUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counters_.stopped != FeedbackStopReason::None) return FeedbackPollResult::Stopped;
    if (nowUs < 0 || nowUs > (int64_t(1) << 55) || (lastPollUs_ && nowUs < *lastPollUs_))
        return fail(FeedbackStopReason::ClockInvalid);
    lastPollUs_ = nowUs;
    try {
        // Even during retry backoff, a stale pending report must expire.
        if (pendingSinceUs_ && nowUs - *pendingSinceUs_ >= policy_.maxPendingAgeUs)
            return fail(FeedbackStopReason::PendingExpired);
        if (nowUs < nextAttemptUs_) return FeedbackPollResult::RetryLater;
        auto pending = observer_->prepare(nowUs, senderSsrc_, mediaSsrc_,
                                           1200 - compoundPrefixBytes(policy_.rtcpCname));
        if (!pending) return FeedbackPollResult::NoReport;
        if (ticket_ && ticket_ != pending->ticket)
            return fail(FeedbackStopReason::CommitRejected); // External reset/second sender.
        if (!ticket_) { ticket_ = pending->ticket; pendingSinceUs_ = nowUs; }
        auto compound = compoundFeedback(pending->packet, senderSsrc_, policy_.rtcpCname);
        if (!budget_->trySpend(RtcpProducer::TransportFeedback, compound.size(),
                              policy_.overheadBytes, nowUs)) {
            ++counters_.budgetDeferred;
            return FeedbackPollResult::BudgetDeferred;
        }
        bool accepted = false;
        try { accepted = submit_(compound); }
        catch (...) { accepted = false; } // Never log callback exception text/input.
        if (!accepted) {
            ++counters_.sendFailures;
            if (++consecutiveFailures_ >= policy_.maxConsecutiveFailures)
                return fail(FeedbackStopReason::SendFailureLimit);
            retryDelayUs_ = retryDelayUs_ == 0 ? policy_.firstRetryUs :
                std::min(policy_.maxRetryUs, retryDelayUs_ * 2);
            nextAttemptUs_ = nowUs + retryDelayUs_;
            return FeedbackPollResult::SendFailed;
        }
        if (!observer_->commit(pending->ticket, nowUs))
            return fail(FeedbackStopReason::CommitRejected);
        ++counters_.submitted;
        ticket_ = 0; pendingSinceUs_.reset(); consecutiveFailures_ = 0;
        retryDelayUs_ = 0; nextAttemptUs_ = 0;
        return FeedbackPollResult::Submitted;
    } catch (...) {
        return fail(FeedbackStopReason::InternalFailure);
    }
}
void FeedbackSender::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counters_.stopped == FeedbackStopReason::None) fail(FeedbackStopReason::Requested);
}
FeedbackSendCounters FeedbackSender::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}
} // namespace uu::twcc
