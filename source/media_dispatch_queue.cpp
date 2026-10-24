#include "media_dispatch_queue.hpp"
#include "cpu_diagnostics.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace uu {
namespace {
struct Registry { std::mutex mutex;std::weak_ptr<MediaDispatchQueue> latest; };
Registry &registry() {static auto *value=new Registry;return *value;}
}
std::shared_ptr<MediaDispatchQueue> MediaDispatchQueue::create(Deliver deliver, Schedule schedule, Limits limits) {
    if (!deliver || !schedule || !limits.packets || !limits.retainedBytes ||
        !limits.drainBatch || limits.drainBatch>limits.packets)
        throw std::invalid_argument("Invalid media dispatch queue limits or callbacks");
    auto result=std::shared_ptr<MediaDispatchQueue>(new MediaDispatchQueue(std::move(deliver),std::move(schedule),limits));
    auto &r=registry();std::lock_guard lock(r.mutex);r.latest=result;
    return result;
}
MediaDispatchQueue::MediaDispatchQueue(Deliver deliver, Schedule schedule, Limits limits)
    : deliver_(std::move(deliver)),schedule_(std::move(schedule)),limits_(limits) {}

bool MediaDispatchQueue::enqueue(rtc::message_ptr message) {
    if (!message) return false;
    constexpr size_t overhead=sizeof(rtc::Message)+sizeof(Item)+64;
    if (message->capacity()>std::numeric_limits<size_t>::max()-overhead) return false;
    const auto charge=message->capacity()+overhead;
    bool submitNow=false;
    {
        std::lock_guard lock(mutex_);
        if (stats_.stopped) return false;
        if (queue_.size()+stats_.inFlight>=limits_.packets || charge>limits_.retainedBytes ||
            stats_.retainedBytes>limits_.retainedBytes-charge) {
            ++stats_.overflow;
            return false; // Integration must explicitly handle overload, including SRTCP.
        }
        queue_.push_back({std::move(message),charge,std::chrono::steady_clock::now()});
        ++stats_.accepted;
        stats_.queued=queue_.size();stats_.retainedBytes+=charge;
        stats_.highPackets=std::max(stats_.highPackets,stats_.queued+stats_.inFlight);
        stats_.highBytes=std::max(stats_.highBytes,stats_.retainedBytes);
        if (!scheduled_) {scheduled_=true;submitNow=true;}
    }
    if (submitNow) submit();
    return true; // Accepted into the queue, not proof of delivery.
}
void MediaDispatchQueue::submit() {
    try {
        auto self=shared_from_this();
        schedule_([self=std::move(self)] { self->drain(); });
    } catch (...) {
        std::lock_guard lock(mutex_);
        ++stats_.schedulingFailures;
        stopLocked();
    }
}
void MediaDispatchQueue::drain() {
    // Role 4: the authenticated-media consumer shares the RTC pool with
    // doRecv/SCTP/timers. Marking it here makes its thread CPU visible without
    // changing scheduling or adding work to the media callback.
    uu_cpu_mark(4);
    {
        std::lock_guard lock(mutex_);
        ++stats_.drains;
    }
    for (size_t i=0;i<limits_.drainBatch;++i) {
        Item item;
        {
            std::lock_guard lock(mutex_);
            if (stats_.stopped || queue_.empty()) {scheduled_=false;return;}
            item=std::move(queue_.front());queue_.pop_front();
            stats_.queued=queue_.size();stats_.inFlight=1;inFlightBytes_=item.charge;
            const auto waited=std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-item.received).count();
            stats_.maxWaitUs=std::max(stats_.maxWaitUs,uint64_t(std::max<int64_t>(0,waited)));
        }
        try {
            // No queue lock during media callbacks, feedback or endpoint teardown.
            deliver_(std::move(item.message));
            std::lock_guard lock(mutex_);
            stats_.retainedBytes-=inFlightBytes_;inFlightBytes_=0;stats_.inFlight=0;
            ++stats_.delivered;
        } catch (...) {
            std::lock_guard lock(mutex_);
            stats_.retainedBytes-=inFlightBytes_;inFlightBytes_=0;stats_.inFlight=0;
            ++stats_.callbackFailures;
            stopLocked();return;
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (stats_.stopped || queue_.empty()) {scheduled_=false;return;}
        // Keep the reservation while yielding to another pool job. Producers
        // cannot create a second consumer in the gap before resubmission.
    }
    submit();
}
void MediaDispatchQueue::stopLocked() {
    stats_.stopped=true;
    stats_.cancelled+=queue_.size();queue_.clear();
    stats_.queued=0;stats_.retainedBytes=inFlightBytes_;
    scheduled_=false;
}
void MediaDispatchQueue::stop() {std::lock_guard lock(mutex_);stopLocked();}
MediaDispatchQueue::Stats MediaDispatchQueue::snapshot() const {std::lock_guard lock(mutex_);return stats_;}
bool latestMediaDispatchStats(MediaDispatchQueue::Stats &out) {
    std::shared_ptr<MediaDispatchQueue> queue;
    {auto &r=registry();std::lock_guard lock(r.mutex);queue=r.latest.lock();}
    if (!queue) return false;
    out=queue->snapshot();return true;
}
} // namespace uu
