#include "diagnostics.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#ifdef __SWITCH__
#include <switch.h>
#include <unwind.h>
extern "C" char __start__[];
#endif

namespace uu {
#ifndef UU_VERSION
#define UU_VERSION "unknown"
#endif
const char *diagnosticPath() noexcept {
#ifdef __SWITCH__
    return "sdmc:/switch/UURemote-ns/connection.log";
#else
    return "connection.log";
#endif
}
namespace {
class AsyncLog {
    struct Entry {
        uint64_t sequence = 0;
        std::string line;
    };
    std::mutex mutex_;
    std::condition_variable cv_, flushCv_;
    std::deque<Entry> queue_;
    bool stopping_ = false;
    bool degraded_ = false;
    bool failed_ = false;
    bool dropped_ = false;
    uint64_t accepted_ = 0, completed_ = 0;
    std::mutex degradedWriteMutex_;
    std::thread worker_;
    static constexpr size_t kMaxQueued = 512;
    static constexpr size_t kRotateBytes = 128 * 1024;

    bool writeBatch(const std::deque<Entry> &batch) noexcept {
        try {
            const std::string path = diagnosticPath();
            size_t size = 0;
            if (FILE *probe = std::fopen(path.c_str(), "rb")) {
                if (std::fseek(probe, 0, SEEK_END) == 0) {
                    const long position = std::ftell(probe);
                    if (position > 0) size = size_t(position);
                }
                std::fclose(probe);
            }
            if (size > kRotateBytes) {
                // Fixed rotation: current -> .1 -> .2. Each new file starts
                // with the app-start build label so three rotations still
                // identify the running build.
                errno = 0;
                std::remove((path + ".2").c_str());
                if (errno != 0 && errno != ENOENT) return false;
                errno = 0;
                std::rename((path + ".1").c_str(), (path + ".2").c_str());
                if (errno != 0 && errno != ENOENT) return false;
                errno = 0;
                if (std::rename(path.c_str(), (path + ".1").c_str()) != 0)
                    return false;
                size = 0;
            }
            FILE *file = std::fopen(path.c_str(), "a");
            if (!file) return false;
            bool ok = true;
            auto writeText = [&](const std::string &text) {
                if (std::fwrite(text.data(), 1, text.size(), file) != text.size()) ok = false;
            };
            if (size == 0) writeText(std::string("# build=") + UU_VERSION + "\n");
            for (const auto &entry : batch) writeText(entry.line + "\n");
            if (std::fflush(file) != 0) ok = false;
            if (std::fclose(file) != 0) ok = false;
            return ok;
        } catch (...) {
            return false;
        }
    }
    void run() noexcept {
        std::deque<Entry> batch;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty() && stopping_) return;
                batch.swap(queue_);
            }
            const bool ok = writeBatch(batch);
            uint64_t last = 0;
            for (const auto &entry : batch)
                if (entry.sequence > last) last = entry.sequence;
            batch.clear();
            {
                std::lock_guard lock(mutex_);
                if (last > completed_) completed_ = last;
                if (!ok) failed_ = true;
            }
            flushCv_.notify_all();
        }
    }
  public:
    AsyncLog() {
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            // Honest degraded mode: no waiter can be left forever. Every push
            // writes synchronously and flush observes completion/failure.
            degraded_ = true;
        }
    }
    ~AsyncLog() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void push(std::string line) noexcept {
        try {
            // Set only during construction, before the logger is published.
            const bool direct = degraded_;
            if (direct) {
                // Serialize the sequence assignment with the synchronous write:
                // seq N+1 cannot complete before seq N.
                std::lock_guard writeLock(degradedWriteMutex_);
                std::deque<Entry> one;
                one.push_back({0, std::move(line)}); // allocation can fail before accepted
                uint64_t sequence = 0;
                {
                    std::lock_guard lock(mutex_);
                    sequence = ++accepted_;
                }
                one.front().sequence = sequence;
                const bool ok = writeBatch(one);
                {
                    std::lock_guard lock(mutex_);
                    if (sequence > completed_) completed_ = sequence;
                    if (!ok) failed_ = true;
                }
                flushCv_.notify_all();
                return;
            }
            {
                std::lock_guard lock(mutex_);
                // Insert first. If allocation fails, accepted_ has not moved, so
                // there is no sequence gap for flush to wait on forever.
                queue_.push_back({0, std::move(line)});
                const uint64_t sequence = ++accepted_;
                queue_.back().sequence = sequence;
                if (queue_.size() > kMaxQueued) {
                    queue_.pop_front();
                    dropped_ = true;
                }
                cv_.notify_one();
            }
        } catch (...) {
            // Do not manufacture a completed watermark: other batches may still
            // be in flight. The failed flag makes the eventual flush false once
            // the accepted target is actually settled.
            std::lock_guard lock(mutex_);
            failed_ = true;
            flushCv_.notify_all();
        }
    }
    bool flush() noexcept {
        try {
            uint64_t target = 0;
            {
                std::lock_guard lock(mutex_);
                target = accepted_;
            }
            std::unique_lock lock(mutex_);
            // Failure/drop are sticky result bits, not permission to return while
            // the target's write attempt is still in flight.
            flushCv_.wait(lock, [&] { return completed_ >= target; });
            return !failed_ && !dropped_ && completed_ >= target;
        } catch (...) {
            return false;
        }
    }
};

AsyncLog &asyncLog() {
    static AsyncLog instance;
    return instance;
}
} // namespace
void diagnostic(const char *event) noexcept {
    // Fixed names, numeric error codes and code offsets only; never log protocol data.
    try {
        if (!event) return;
        std::string line = std::to_string(static_cast<long long>(std::time(nullptr)));
        line += ' ';
        line += event;
        asyncLog().push(std::move(line));
    } catch (...) {
    }
}
bool flushDiagnosticsLog() noexcept {
    try {
        return asyncLog().flush();
    } catch (...) {
        return false;
    }
}
std::string exceptionSummary(std::exception_ptr error) {
    if (!error)
        return "exception-unavailable";
    try {
        std::rethrow_exception(error);
    } catch (const std::system_error &e) {
        return "exception-system-error-" + std::to_string(e.code().value());
    } catch (const std::bad_alloc &) {
        return "exception-out-of-memory";
    } catch (const std::logic_error &) {
        return "exception-logic-error";
    } catch (const std::runtime_error &) {
        return "exception-runtime-error";
    } catch (const std::exception &) {
        return "exception-std";
    } catch (...) {
        return "exception-unknown";
    }
}
void diagnosticException(const char *event, std::exception_ptr error) noexcept {
    diagnostic(event);
    try {
        diagnostic(exceptionSummary(error).c_str());
    } catch (...) {
        diagnostic("exception-summary-unavailable");
    }
}
[[noreturn]] void terminateWithDiagnostics() noexcept {
    diagnosticException("runtime-terminate", std::current_exception());
#ifdef __SWITCH__
    int depth = 0;
    _Unwind_Backtrace(
        [](_Unwind_Context *context, void *argument) {
            auto &depth = *static_cast<int *>(argument);
            const uintptr_t ip = _Unwind_GetIP(context), base = reinterpret_cast<uintptr_t>(__start__);
            if (ip >= base && ip - base < 256 * 1024 * 1024) {
                char line[64];
                std::snprintf(line, sizeof(line), "trace-%02d-0x%llx", depth,
                              static_cast<unsigned long long>(ip - base));
                diagnostic(line);
            }
            return ++depth < 16 ? _URC_NO_REASON : _URC_END_OF_STACK;
        },
        &depth);
    flushDiagnosticsLog();
    // libc abort/exit can tear down the shared window while the UI thread is presenting.
    // Exit the process directly after saving evidence, without running global destructors.
    svcExitProcess();
#else
    std::abort();
#endif
}
std::string connectionFailure(const std::string &stage, const std::string &error) {
    std::string name;
    if (stage == "joining")
        name = "加入房间";
    else if (stage == "signaling")
        name = "信令连接";
    else if (stage == "negotiating")
        name = "申请控制权";
    else if (stage == "rtc-init")
        name = "初始化串流";
    else if (stage == "connecting")
        name = "音视频协商";
    else if (stage == "connected")
        name = "远程会话";
    return name.empty() ? error : name + "失败：" + error;
}
} // namespace uu
