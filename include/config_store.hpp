#pragma once
#include <map>
#include <mutex>
#include <string>
#include <exception>
#include "runtime/latest_snapshot_worker.hpp"

namespace uu {
// Ensures sdmc:/switch/UURemote-ns/config.ini exists and is migrated to the
// bundled default-config.ini schema. Never overwrites an existing player value
// unless it is a known old default covered by a migration. On non-Switch builds
// this is a no-op.
void ensureDefaultConfig() noexcept;

// Persist a quality change through the same first-wins parser used by reload.
// All managed keys are written once; unknown lines/comments and one fixed .bak
// are retained. Frame pacing keeps the first parsed value (including 0 #comment).
void saveQualityConfig(int width, int height, int fps, int quality, bool vsync,
                       int playoutDelay, int maxBitrateMbps, bool hevc);

struct QualityConfigValues {
    int width,height,fps,quality,cap,delay;bool vsync,hevc;
    bool isBitrateOnlyChangeFrom(const QualityConfigValues &previous) const noexcept {
        return quality==6 && previous.quality==6 && cap!=previous.cap &&
            width==previous.width && height==previous.height && fps==previous.fps &&
            delay==previous.delay && vsync==previous.vsync && hevc==previous.hevc;
    }
};

// A single writer owns disk I/O; the latest complete pending snapshot replaces
// older pending targets. shutdown waits for all accepted remaining work.
class QualityConfigWriter {
  public:
    using Save=std::function<void(const QualityConfigValues &)>;
    struct Result {
        uint64_t order=0,elapsedUs=0,maxUs=0,completed=0,failures=0;
        std::exception_ptr error;
    };
    QualityConfigWriter();
    explicit QualityConfigWriter(Save save);
    bool submit(uint64_t order,const QualityConfigValues &values);
    Result snapshot() const;
    runtime::SnapshotWorkerStats stats() const {return worker_.stats();}
    void shutdown() {worker_.shutdown();}
  private:
    struct Job {
        uint64_t order;
        QualityConfigValues values;
        size_t retainedBytes() const noexcept {return sizeof(Job);}
    };
    mutable std::mutex resultMutex_;
    Result result_;
    // Destroy/join the worker before the result it publishes into.
    runtime::LatestSnapshotWorker<Job> worker_;
};

// Thread-safe read-only view of config.ini for all modules. Reload replaces the
// map atomically; readers never observe a partially parsed file.
class ConfigStore {
  public:
    static ConfigStore &instance();

    // Re-read the file from the SD card (Switch) or working directory.
    void reload();

    // First occurrence wins, matching the original per-module parsers.
    std::string value(const std::string &key) const;
    bool has(const std::string &key) const;
    int integer(const std::string &key, int fallback, int minimum, int maximum) const;

  private:
    ConfigStore() = default;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> values_;
};
}
