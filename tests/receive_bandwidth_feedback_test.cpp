#include "receive_bandwidth_observer.hpp"
#include "authenticated_feedback_endpoint.hpp"
#include "system_wrappers/include/clock.h"
#include <iostream>
#include <stdexcept>
#include <condition_variable>
#include <thread>

int main() {
    using Result = uu::twcc::FeedbackPollResult;
    constexpr uint32_t local = 0x10203040, primary = 0x12345678, repair = 0x22334455;
    const uu::twcc::FeedbackSendPolicy policy{20000, 200000, 1000000, 3, 256, "uu-rbe-test"};
    uu::rbe::InputConfig config;
    config.videoPayloads[96] = config.videoPayloads[97] = true;
    // Same admission as the r3tv failure: TWCC ID6 was negotiated but is
    // absent on the actual media. After TWCC releases the global handler,
    // those RTP packets must still produce a real classic estimate/REMB.
    config.transportSequenceIds[0] = 6;
    // Reused wire boundary for the real estimator, not synthetic estimate injection.
    auto feed = [&](uu::rbe::ReceiveBandwidthObserver &observer, webrtc::SimulatedClock &clock,
                    const rtc::message_callback &send, uint32_t ssrc, int sequence) {
        clock.AdvanceTimeMicroseconds(1200);
        auto message = rtc::make_message(1212);
        auto *data = reinterpret_cast<uint8_t *>(message->data());
        data[0] = 0x80; data[1] = ssrc == repair ? 97 : 96;
        const uint32_t timestamp = uint32_t(uint64_t(sequence) * 108);
        for (int k = 0; k < 4; ++k) {
            data[4 + k] = uint8_t(timestamp >> (24 - 8 * k));
            data[8 + k] = uint8_t(ssrc >> (24 - 8 * k));
        }
        message->receivedAtUs = clock.TimeInMicroseconds();
        rtc::message_vector messages{message};
        observer.incoming(messages, send);
        observer.poll();
        return messages.size() == 1 && messages[0] == message && message.use_count() == 2;
    };
    auto u32 = [](const rtc::message_ptr &message, size_t at) {
        uint32_t value = 0;
        for (int k = 0; k < 4; ++k) value = value << 8 | std::to_integer<uint8_t>((*message)[at + k]);
        return value;
    };
    webrtc::SimulatedClock clock(0);
    uu::rbe::ReceiveBandwidthObserver observer(config, &clock);
    std::vector<rtc::message_ptr> sent;
    const rtc::message_callback send = [&](rtc::message_ptr message) { sent.push_back(std::move(message)); };
    for (int i = 0; i < 5000; ++i) {
        if (!feed(observer, clock, send, i % 2 ? repair : primary, i)) return 1;
        if (observer.pollFeedback(clock.TimeInMicroseconds()) != Result::NoReport) return 2;
    }
    if (!sent.empty() || !observer.snapshot().estimates || observer.feedbackCounters().created) return 3;
    auto budget = std::make_shared<uu::twcc::SharedRtcpBudget>(16000, 4800, 4800, 1200, clock.TimeInMicroseconds());
    if (observer.startFeedback(nullptr, policy, local)) return 4;
    if (!observer.startFeedback(budget, policy, local) || observer.startFeedback(budget, policy, local)) return 5;
    for (int i = 5000; i < 10000; ++i) {
        const auto count = sent.size();
        if (!feed(observer, clock, send, i % 2 ? repair : primary, i) || sent.size() != count) return 6;
        const auto result = observer.pollFeedback(clock.TimeInMicroseconds());
        if (result == Result::Stopped || result == Result::SendFailed) return 7;
    }
    const auto counters = observer.feedbackCounters();
    if (!counters.transport.submitted || sent.size() != counters.transport.submitted || counters.invalid ||
        counters.lastSubmittedSsrcs != 2 || counters.created >= observer.snapshot().estimates) return 8;
    for (const auto &message : sent) {
        const auto *data = reinterpret_cast<const uint8_t *>(message->data());
        if (message->type != rtc::Message::Control || message->size() > 1200 ||
            data[0] != 0x80 || data[1] != 201 || data[3] != 1 || u32(message, 4) != local) return 9;
        const size_t sdes = 8;
        if (data[sdes] != 0x81 || data[sdes + 1] != 202 || u32(message, sdes + 4) != local ||
            data[sdes + 8] != 1 || data[sdes + 9] != policy.rtcpCname.size()) return 10;
        for (size_t i = 0; i < policy.rtcpCname.size(); ++i)
            if (data[sdes + 10 + i] != uint8_t(policy.rtcpCname[i])) return 11;
        const size_t remb = sdes + (size_t(data[sdes + 2]) * 256 + data[sdes + 3] + 1) * 4;
        if (data[remb] != 0x8f || data[remb + 1] != 206 || u32(message, remb + 4) != local ||
            u32(message, remb + 8) || u32(message, remb + 12) != 0x52454d42) return 12;
        const uint32_t word = u32(message, remb + 16);
        const auto count = word >> 24;
        if (count != 2 || message->size() != remb + 20 + count * 4 ||
            size_t(data[remb + 2]) * 256 + data[remb + 3] + 1 != (message->size() - remb) / 4) return 13;
        const auto first = u32(message, remb + 20), second = u32(message, remb + 24);
        if (!((first == primary && second == repair) || (first == repair && second == primary))) return 14;
        const uint64_t rate = uint64_t(word & 0x3ffff) << ((word >> 18) & 63);
        if (!rate) return 15;
    }
    observer.close();
    const auto beforeClose = sent.size();
    feed(observer, clock, send, primary, 10000);
    if (observer.pollFeedback(clock.TimeInMicroseconds()) != Result::Stopped || sent.size() != beforeClose) return 16;

    webrtc::SimulatedClock failureClock(0);
    uu::rbe::ReceiveBandwidthObserver failing(config, &failureClock);
    unsigned attempts = 0;
    const rtc::message_callback reject = [&](rtc::message_ptr) { ++attempts; throw std::runtime_error("test rejection"); };
    auto failureBudget = std::make_shared<uu::twcc::SharedRtcpBudget>(16000, 4800, 4800, 1200, 0);
    if (!failing.startFeedback(failureBudget, policy, local)) return 17;
    for (int i = 0; i < 12000 && !failing.feedbackCounters().created; ++i) feed(failing, failureClock, reject, primary, i);
    if (!failing.feedbackCounters().created || attempts) return 18;
    const auto createdAt = failureClock.TimeInMicroseconds();
    if (failing.pollFeedback(createdAt - 1) != Result::RetryLater || attempts) return 19;
    if (failing.pollFeedback(createdAt) != Result::SendFailed || attempts != 1) return 20;
    if (failing.pollFeedback(createdAt) != Result::RetryLater || attempts != 1) return 21;
    failureClock.AdvanceTimeMicroseconds(20000);
    if (failing.pollFeedback(failureClock.TimeInMicroseconds()) != Result::SendFailed || attempts != 2) return 22;
    failureClock.AdvanceTimeMicroseconds(40000);
    if (failing.pollFeedback(failureClock.TimeInMicroseconds()) != Result::Stopped || attempts != 3 ||
        failing.feedbackCounters().transport.submitted ||
        failing.feedbackCounters().transport.stopped != uu::twcc::FeedbackStopReason::SendFailureLimit) return 23;
    if (failing.pollFeedback(failureClock.TimeInMicroseconds()) != Result::Stopped || attempts != 3) return 24;

    webrtc::SimulatedClock emptyClock(0);
    uu::rbe::ReceiveBandwidthObserver deferred(config, &emptyClock);
    auto emptyBudget = std::make_shared<uu::twcc::SharedRtcpBudget>(1, 4800, 0, 1200, 0);
    if (!deferred.startFeedback(emptyBudget, policy, local)) return 25;
    for (int i = 0; i < 12000 && !deferred.feedbackCounters().created; ++i) feed(deferred, emptyClock, send, primary, i);
    const auto beforeDeferred = sent.size();
    if (deferred.pollFeedback(emptyClock.TimeInMicroseconds()) != Result::BudgetDeferred) return 26;
    emptyClock.AdvanceTimeMicroseconds(1000001);
    if (deferred.pollFeedback(emptyClock.TimeInMicroseconds()) != Result::Stopped || sent.size() != beforeDeferred ||
        deferred.feedbackCounters().transport.stopped != uu::twcc::FeedbackStopReason::PendingExpired) return 27;

    // A new endpoint never rebinds the original generation's callback.
    uu::twcc::AuthenticatedFeedbackEndpoint endpoint;
    unsigned original = 0, replacement = 0;
    endpoint.bind([&](rtc::message_ptr) { ++original; });
    endpoint.bind([&](rtc::message_ptr) { ++replacement; });
    if (!endpoint.submit(std::vector<uint8_t>{0x80, 201, 0, 1, 0, 0, 0, 1}) || original != 1 || replacement) return 28;
    endpoint.stop();
    if (endpoint.submit(std::vector<uint8_t>{0x80, 201, 0, 1, 0, 0, 0, 1}) || original != 1) return 29;

    // Keep receiving and replacing the mailbox while a synthetic transport
    // submission is blocked. Committing the old ticket must retain the newer report.
    webrtc::SimulatedClock queuedClock(0);
    uu::rbe::ReceiveBandwidthObserver queued(config, &queuedClock);
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    unsigned queuedSends = 0;
    const rtc::message_callback blocked = [&](rtc::message_ptr) {
        std::unique_lock<std::mutex> lock(mutex);
        ++queuedSends;
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    };
    auto queuedBudget = std::make_shared<uu::twcc::SharedRtcpBudget>(16000, 4800, 4800, 1200, 0);
    if (!queued.startFeedback(queuedBudget, policy, local)) return 30;
    int sequence = 0;
    for (; sequence < 12000 && !queued.feedbackCounters().created; ++sequence)
        feed(queued, queuedClock, blocked, primary, sequence);
    if (!queued.feedbackCounters().created) return 31;
    Result firstResult = Result::NoReport;
    std::thread submit([&] { firstResult = queued.pollFeedback(queuedClock.TimeInMicroseconds()); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    for (int i = 0; i < 500; ++i, ++sequence) feed(queued, queuedClock, blocked, primary, sequence);
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    cv.notify_all();
    submit.join();
    if (firstResult != Result::Submitted || queuedSends != 1 || !queued.feedbackCounters().replaced) return 32;
    if (queued.pollFeedback(queuedClock.TimeInMicroseconds()) != Result::Submitted || queuedSends != 2) return 33;
    queued.close();
    webrtc::SimulatedClock rollbackClock(0);
    uu::rbe::ReceiveBandwidthObserver rollback(config, &rollbackClock);
    auto badPolicy = policy;
    badPolicy.rtcpCname = std::string(256, 'x');
    if (rollback.startFeedback(queuedBudget, badPolicy, local)) return 34;
    if (!rollback.startFeedback(queuedBudget, policy, local) ||
        rollback.pollFeedback(100) != Result::NoReport || rollback.pollFeedback(99) != Result::Stopped ||
        rollback.feedbackCounters().transport.stopped != uu::twcc::FeedbackStopReason::ClockInvalid) return 35;
    // After a real RTP estimate, an authenticated SR/RR can provide the send
    // endpoint even if the desktop has stopped producing new video packets.
    webrtc::SimulatedClock controlClock(0);
    uu::rbe::ReceiveBandwidthObserver controlOnly(config, &controlClock);
    for (int i = 0; i < 5000; ++i) feed(controlOnly, controlClock, send, primary, i);
    auto controlBudget = std::make_shared<uu::twcc::SharedRtcpBudget>(16000, 4800, 4800, 1200,
                                                                   controlClock.TimeInMicroseconds());
    if (!controlOnly.startFeedback(controlBudget, policy, local)) return 36;
    controlClock.AdvanceTimeMicroseconds(200000);
    controlOnly.poll();
    if (!controlOnly.feedbackCounters().created) return 37;
    auto control = rtc::make_message(8, rtc::Message::Control);
    rtc::message_vector controls{control};
    const auto beforeControl = sent.size();
    controlOnly.incoming(controls, send);
    if (sent.size() != beforeControl || controls.front() != control ||
        controlOnly.pollFeedback(controlClock.TimeInMicroseconds()) != Result::Submitted) return 38;
    const auto beforeCeiling=controlOnly.feedbackCounters().transport.submitted;
    for(int i=5000;i<5500;++i){
        if(!feed(controlOnly,controlClock,send,primary,i)) return 39;
        const auto result=controlOnly.pollFeedback(controlClock.TimeInMicroseconds(),2000000);
        if(result==Result::Submitted && controlOnly.feedbackCounters().lastSubmittedBps>2000000) return 40;
    }
    if(controlOnly.feedbackCounters().transport.submitted<=beforeCeiling) return 41;
    controlOnly.close();
    std::cout << "PASS: real RBE -> official throttler -> bounded mailbox -> compound multi-SSRC REMB, poll-only send, retry/budget/expiry and endpoint stop\n";
}
