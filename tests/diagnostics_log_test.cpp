#include "diagnostics.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

static std::atomic<bool> slowFlush{false}, failFlush{false}, failOpen{false},
    failWrite{false}, failClose{false}, fflushEntered{false};

extern "C" FILE *__real_fopen(const char *, const char *);
extern "C" FILE *__wrap_fopen(const char *path, const char *mode) {
    if (failOpen.load()) return nullptr;
    return __real_fopen(path, mode);
}
extern "C" int __real_fflush(FILE *);
extern "C" int __wrap_fflush(FILE *file) {
    fflushEntered = true;
    if (slowFlush.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (failFlush.load()) return EOF;
    return __real_fflush(file);
}
extern "C" int __real_fclose(FILE *);
extern "C" int __wrap_fclose(FILE *file) {
    if (failClose.load()) { __real_fclose(file); return EOF; }
    return __real_fclose(file);
}
extern "C" size_t __real_fwrite(const void *, size_t, size_t, FILE *);
extern "C" size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    if (failWrite.load()) return 0;
    return __real_fwrite(data, size, count, file);
}

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static void waitFflushEntered() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!fflushEntered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(fflushEntered.load(), "writer did not enter fflush");
}
static std::string readAll(const std::filesystem::path &path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "normal";
        const auto original = std::filesystem::current_path();
        const auto dir = original / ("diagnostics-log-" + mode + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);

        if (mode == "fflush") {
            failFlush = true;
            uu::diagnostic("fflush-failure");
            require(!uu::flushDiagnosticsLog(), "fflush failure must be visible");
        } else if (mode == "open") {
            failOpen = true;
            uu::diagnostic("open-failure");
            require(!uu::flushDiagnosticsLog(), "open failure must be visible");
        } else if (mode == "write") {
            failWrite = true;
            uu::diagnostic("write-failure");
            require(!uu::flushDiagnosticsLog(), "write failure must be visible");
        } else if (mode == "close") {
            failClose = true;
            uu::diagnostic("close-failure");
            require(!uu::flushDiagnosticsLog(), "close failure must be visible");
        } else if (mode == "drop") {
            slowFlush = true;
            fflushEntered = false;
            uu::diagnostic("queue-root");
            waitFflushEntered();
            for (int i = 0; i < 700; ++i) uu::diagnostic("queue-drop");
            const auto started = std::chrono::steady_clock::now();
            require(!uu::flushDiagnosticsLog(), "dropped queue entry must be visible");
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            require(elapsed >= 150, "drop/failed result returned before target write attempt");
        } else if (mode == "failslow") {
            slowFlush = true;
            failFlush = true;
            fflushEntered = false;
            uu::diagnostic("fail-inflight");
            waitFflushEntered();
            const auto started = std::chrono::steady_clock::now();
            require(!uu::flushDiagnosticsLog(), "slow fflush failure must be visible");
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            require(elapsed >= 150, "failed result returned before target write attempt");
        } else {
            require(uu::flushDiagnosticsLog(), "empty flush");
            slowFlush = true;
            fflushEntered = false;
            uu::diagnostic("slow-inflight");
            waitFflushEntered();
            const auto started = std::chrono::steady_clock::now();
            require(uu::flushDiagnosticsLog(), "slow flush should complete");
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            require(elapsed >= 150, "flush returned before in-flight fflush completed");
            slowFlush = false;

            // Fixed current/.1/.2 rotation, and every new file names this build.
            for (int i = 0; i < 3; ++i) {
                {
                    std::ofstream grow("connection.log", std::ios::trunc);
                    grow << std::string(140 * 1024, 'x');
                }
                uu::diagnostic("rotation");
                require(uu::flushDiagnosticsLog(), "rotation flush");
                require(std::filesystem::exists("connection.log.1"), "rotation .1");
            }
            require(std::filesystem::exists("connection.log.2"), "rotation .2");
            const auto current = readAll("connection.log");
            require(current.rfind("# build=test-build\n", 0) == 0, "new file build label");
        }

        slowFlush = false;
        std::filesystem::current_path(original);
        std::error_code cleanupError;
        std::filesystem::remove_all(dir, cleanupError);
        std::cout << "PASS: diagnostics target-sequence flush, slow in-flight, fixed rotation/build label, "
                     "drop/open/write/fflush/close failure modes\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
