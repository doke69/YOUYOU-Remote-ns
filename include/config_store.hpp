#pragma once
#include <map>
#include <mutex>
#include <string>

namespace uu {
// Ensures sdmc:/switch/UURemote-ns/config.ini exists and is migrated to the
// bundled default-config.ini schema. Never overwrites an existing player value
// unless it is a known old default covered by a migration. On non-Switch builds
// this is a no-op.
void ensureDefaultConfig() noexcept;

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
