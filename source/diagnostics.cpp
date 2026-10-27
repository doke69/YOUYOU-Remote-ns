#include "diagnostics.hpp"
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
const char *diagnosticPath() noexcept {
#ifdef __SWITCH__
    return "sdmc:/switch/UURemote-ns/connection.log";
#else
    return "connection.log";
#endif
}
namespace {
class AsyncLog {
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    bool stopping_ = false;
    bool writerFailed_ = false;
    std::thread worker_;
    static constexpr size_t kMaxQueued = 512;

    void writeBatch(std::deque<std::string> &batch) noexcept {
        auto *file = std::fopen(diagnosticPath(), "a");
        if (!file) return;
        std::fseek(file, 0, SEEK_END);
        if (std::ftell(file) > 128 * 1024) {
            std::fclose(file);
            file = std::fopen(diagnosticPath(), "w");
            if (!file) return;
        }
        for (const auto &line : batch)
            std::fprintf(file, "%s\n", line.c_str());
        std::fflush(file);
        std::fclose(file);
    }
    void run() noexcept {
        std::deque<std::string> batch;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty() && stopping_) return;
                batch.swap(queue_);
            }
            writeBatch(batch);
            batch.clear();
            cv_.notify_all();
        }
    }
  public:
    AsyncLog() {
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            writerFailed_ = true;
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
            if (writerFailed_) {
                std::deque<std::string> one;
                one.push_back(std::move(line));
                writeBatch(one);
                return;
            }
            std::lock_guard lock(mutex_);
            if (queue_.size() >= kMaxQueued) queue_.pop_front();
            queue_.push_back(std::move(line));
            cv_.notify_one();
        } catch (...) {
        }
    }
    void flush() noexcept {
        try {
            if (writerFailed_) return;
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return queue_.empty(); });
        } catch (...) {
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
void flushDiagnosticsLog() noexcept {
    try {
        asyncLog().flush();
    } catch (...) {
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
