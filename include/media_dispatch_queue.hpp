#pragma once
#include <rtc/message.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace uu {
// This queue accepts only already-authenticated owned messages. It does not
// authenticate packets, own a socket, or make a new thread.
class MediaDispatchQueue final : public std::enable_shared_from_this<MediaDispatchQueue> {
  public:
    struct Limits { size_t packets, retainedBytes, drainBatch; };
    struct Stats {
        uint64_t accepted=0, delivered=0, overflow=0, cancelled=0;
        uint64_t callbackFailures=0, schedulingFailures=0, drains=0;
        uint64_t maxWaitUs=0;
        // Retained bytes and high-water marks include the in-flight callback.
        size_t queued=0, inFlight=0, retainedBytes=0, highPackets=0, highBytes=0;
        bool stopped=false;
    };
    using Deliver=std::function<void(rtc::message_ptr)>;
    // Must asynchronously accept exactly one execution or throw before
    // accepting it. Never both accept a job and throw. No inline recursion.
    using Schedule=std::function<void(std::function<void()>)>;
    static std::shared_ptr<MediaDispatchQueue> create(Deliver deliver, Schedule schedule, Limits limits);
    bool enqueue(rtc::message_ptr message);
    // Cancels queued work. An already admitted callback may finish. No waiting
    // or self-join; the callback must use a weak, generation-checked endpoint.
    void stop();
    Stats snapshot() const;
  private:
    struct Item {
        rtc::message_ptr message;
        size_t charge;
        std::chrono::steady_clock::time_point received;
    };
    MediaDispatchQueue(Deliver deliver, Schedule schedule, Limits limits);
    void submit();
    void drain();
    void stopLocked();
    const Deliver deliver_;
    const Schedule schedule_;
    const Limits limits_;
    mutable std::mutex mutex_;
    std::deque<Item> queue_;
    Stats stats_;
    size_t inFlightBytes_=0;
    bool scheduled_=false;
};
// Only the latest constructed transport queue, not a process-wide total.
bool latestMediaDispatchStats(MediaDispatchQueue::Stats &out);
} // namespace uu
