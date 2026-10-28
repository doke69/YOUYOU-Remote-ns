#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace uu::runtime {
struct SnapshotWorkerStats {
    std::uint64_t accepted=0,replaced=0,rejected=0,failures=0,completed=0;
    std::size_t retainedBytes=0,peakBytes=0;
};

// One running writer + at most one pending latest snapshot. Job must expose
// monotonic order and noexcept retainedBytes(). Writer must not call drain or
// shutdown on its own worker. Budget excludes producer-owned input/current state.
template<class Job>
class LatestSnapshotWorker {
    static_assert(std::is_nothrow_move_constructible_v<Job>);
    static_assert(std::is_nothrow_destructible_v<Job>);
    static_assert(noexcept(std::declval<const Job &>().retainedBytes()));
public:
    using Writer=std::function<void(const Job &)>;
    LatestSnapshotWorker(std::size_t byteLimit,Writer writer)
        :limit_(byteLimit),writer_(std::move(writer)),worker_([this]{run();}) {}
    ~LatestSnapshotWorker(){shutdown();}
    LatestSnapshotWorker(const LatestSnapshotWorker &)=delete;
    LatestSnapshotWorker &operator=(const LatestSnapshotWorker &)=delete;
    bool submit(Job job) {
        const auto bytes=job.retainedBytes();
        std::optional<Job> retired; // release superseded data after unlocking
        // This lock covers bookkeeping only, never writer I/O or freeing a
        // superseded payload. Submission is a reset/control path, not per-frame.
        std::unique_lock lock(mutex_);
        if(stopping_ || job.order<=latestOrder_ || bytes>limit_-inFlightBytes_) {
            ++rejected_;return false;
        }
        if(pending_) {retired.emplace(std::move(*pending_));pending_.reset();++replaced_;}
        latestOrder_=job.order;pending_.emplace(std::move(job));pendingBytes_=bytes;
        ++accepted_;peakBytes_=std::max(peakBytes_,inFlightBytes_+pendingBytes_);
        lock.unlock();ready_.notify_one();return true;
    }
    bool drainFor(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return drained_.wait_for(lock,timeout,[&]{return !pending_ && !writing_;});
    }
    void shutdown() {
        std::lock_guard shutdownLock(shutdownMutex_);
        {std::lock_guard lock(mutex_);stopping_=true;}
        ready_.notify_one();
        if(worker_.joinable()) worker_.join();
    }
    SnapshotWorkerStats stats() const {
        std::lock_guard lock(mutex_);
        return {accepted_,replaced_,rejected_.load(),failures_.load(),completed_.load(),
                inFlightBytes_+pendingBytes_,peakBytes_};
    }
private:
    void run() noexcept {
        for(;;) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock,[&]{return stopping_ || pending_.has_value();});
                if(!pending_) return;
                job.emplace(std::move(*pending_));pending_.reset();
                inFlightBytes_=pendingBytes_;pendingBytes_=0;writing_=true;
            }
            try {writer_(*job);} catch(...) {++failures_;}
            job.reset(); // no large resource destruction under bookkeeping lock
            {
                std::lock_guard lock(mutex_);inFlightBytes_=0;writing_=false;++completed_;
            }
            drained_.notify_all();
        }
    }
    std::size_t limit_;
    Writer writer_;
    mutable std::mutex mutex_;
    std::mutex shutdownMutex_;
    std::condition_variable ready_,drained_;
    std::optional<Job> pending_;
    std::size_t pendingBytes_=0,inFlightBytes_=0,peakBytes_=0;
    std::uint64_t latestOrder_=0,accepted_=0,replaced_=0;
    bool stopping_=false,writing_=false;
    std::atomic<std::uint64_t> rejected_{0},failures_{0},completed_{0};
    std::thread worker_;
};
} // namespace uu::runtime
