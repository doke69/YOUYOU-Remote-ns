#include "config_store.hpp"
#include "config_migration.hpp"
#include "config_value.hpp"
#include "diagnostics.hpp"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#ifdef __SWITCH__
#include <sys/stat.h>
#include <dirent.h>
#include <switch.h>
#endif

namespace uu {
namespace {
#ifdef __SWITCH__
constexpr const char *kDirectory = "sdmc:/switch/UURemote-ns";
constexpr const char *kPath = "sdmc:/switch/UURemote-ns/config.ini";
constexpr const char *kBackup = "sdmc:/switch/UURemote-ns/config.ini.bak";
constexpr const char *kTemplatePath = "romfs:/default-config.ini";
constexpr unsigned kConfigVersion = 3;

// Keep this in sync with romfs/default-config.ini. It is used only if the
// packaged resource is missing (e.g. a development build with a custom RomFS).
constexpr const char *kFallbackTemplate =
    "config_version=3\n"
    "width=1280\n"
    "height=720\n"
    "fps=60\n"
    "quality=3\n"
    "vsync=1\n"
    "frame_pacing=1\n"
    "software_pacing=1\n"
    "playout_delay_ms=8\n"
    "max_bitrate_mbps=0\n"
    "video_codec=hevc\n"
    "native_display=1\n";

std::string readFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}
std::vector<std::string> linesOf(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}
std::string keyOf(const std::string &line) {
    return config::keyOf(line);
}
std::string valueOf(const std::string &line) {
    return config::valueOf(line);
}
struct Entry {std::string key, value;};
std::vector<Entry> templateEntries() {
    std::string text = readFile(kTemplatePath);
    if (config::trim(text).empty()) text = kFallbackTemplate;
    std::vector<Entry> result;
    for (auto line : linesOf(text)) {
        if (const auto comment = line.find('#'); comment != std::string::npos)
            line.erase(comment);
        const auto key = keyOf(line);
        if (key.empty() || key == "config_version") continue;
        result.push_back({key, valueOf(line)});
    }
    return result;
}
bool oldDefault(unsigned version,const std::string &key, const std::string &value) {
    // Values from the pre-template code defaults. Only these are safe to
    // upgrade automatically; a player's non-default choice is preserved.
    if(version<1) {
        if (key == "quality" && value == "3") return true;
        if (key == "playout_delay_ms" && value == "8") return true;
        if (key == "max_bitrate_mbps" && value == "0") return true;
    }
    // v0.2.7 template default was 16ms; v0.3.6 moves it to 8ms. A v1 value of
    // 16 is the old default, not necessarily a player's choice.
    if(version==1 && key=="playout_delay_ms" && value=="16") return true;
    return false;
}
bool writeAtomic(const std::string &content) {
    if (content.empty()) return false;
    const std::string temporary = std::string(kPath) + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(content.data(), std::streamsize(content.size()));
        output.flush();
        if (!output) {
            std::remove(temporary.c_str());
            return false;
        }
    }
    std::ifstream existing(kPath, std::ios::binary);
    const bool hadConfig = bool(existing);
    existing.close();
    if (hadConfig) {
        std::remove(kBackup);
        if (std::rename(kPath, kBackup) != 0) {
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), kPath) != 0) {
        if (hadConfig) std::rename(kBackup, kPath);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}
void log(const char *message) {diagnostic(message);}
void pruneLegacyBackups() {
    if (DIR *dir = opendir(kDirectory)) {
        const std::string prefix = "config.ini.before-quality-";
        unsigned removed = 0;
        while (auto *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind(prefix, 0) == 0) {
                std::remove((std::string(kDirectory) + "/" + name).c_str());
                ++removed;
            }
        }
        closedir(dir);
        if (removed) {
            const std::string message = "config-legacy-backups-pruned-" + std::to_string(removed);
            log(message.c_str());
        }
    }
}
#endif
} // namespace
void ensureDefaultConfig() noexcept {
#ifdef __SWITCH__
    mkdir(kDirectory, 0777);
    pruneLegacyBackups();
    std::string existing = readFile(kPath);
    if (config::trim(existing).empty()) {
        const std::string backup = readFile(kBackup);
        if (!config::trim(backup).empty() && writeAtomic(backup)) {
            existing = backup;
            log("config-restored-from-backup");
        }
    }
    if (config::trim(existing).empty()) {
        const std::string templateText = readFile(kTemplatePath);
        const std::string content = config::trim(templateText).empty() ? kFallbackTemplate : templateText;
        if (writeAtomic(content)) log("config-first-run");
        else log("config-first-run-write-failed");
        return;
    }

    auto lines = linesOf(existing);
    std::map<std::string, size_t> positions;
    unsigned version = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto key = keyOf(lines[i]);
        if (key.empty()) continue;
        const bool inserted = positions.emplace(key, i).second;
        if (key == "config_version" && inserted) {
            int parsed = 0;
            version = config::parseInt(valueOf(lines[i]), parsed) && parsed >= 0
                          ? unsigned(parsed) : 0;
        }
    }
    if (version > kConfigVersion) {
        log("config-version-newer-preserved");
        return;
    }

    bool changed = false;
    bool migrated = false;
    QualityConfigMigration qualityMigration;
    if(version < kConfigVersion) {
        qualityMigration = migrateQualityConfigLines(lines);
        if(qualityMigration.changed) {
            changed = true;
            migrated = true;
        }
    }
    for (const auto &entry : templateEntries()) {
        const auto found = positions.find(entry.key);
        if (found == positions.end()) {
            lines.push_back(entry.key + "=" + entry.value);
            positions[entry.key] = lines.size() - 1;
            changed = true;
            continue;
        }
        if (version < kConfigVersion) {
            const auto current = valueOf(lines[found->second]);
            if (oldDefault(version,entry.key, current)) {
                lines[found->second] = entry.key + "=" + entry.value;
                changed = true;
                migrated = true;
            }
        }
    }
    const auto versionText=std::to_string(kConfigVersion);
    const auto versionLine = "config_version=" + versionText;
    const auto foundVersion = positions.find("config_version");
    if (foundVersion == positions.end()) {
        lines.push_back(versionLine);
        changed = true;
        migrated = true;
    } else if (valueOf(lines[foundVersion->second]) != versionText) {
        lines[foundVersion->second] = versionLine;
        changed = true;
        migrated = true;
    }

    if (!changed) {
        log(("config-current-v"+versionText).c_str());
        return;
    }
    std::string content;
    for (const auto &line : lines) content += line + '\n';
    if (writeAtomic(content)) {
        if (migrated) log(("config-migrated-v"+versionText).c_str());
        else log(("config-completed-v"+versionText).c_str());
        if (qualityMigration.changed) {
            const auto fact="config-quality-preset-migrated-from-q"+std::to_string(qualityMigration.fromQuality)+
                "-cap"+std::to_string(qualityMigration.fromMbps)+"-to-q"+std::to_string(qualityMigration.toQuality);
            log(fact.c_str());
        } else if (qualityMigration.duplicateKeys) {
            log("config-quality-preset-migration-pending-duplicate");
        } else if (qualityMigration.invalidCustom) {
            log("config-quality-preset-preserved-invalid-q6-cap");
        } else if (qualityMigration.legacyCustom) {
            const auto fact="config-custom-quality-preserved-q6-cap"+std::to_string(qualityMigration.fromMbps);
            log(fact.c_str());
        }
    } else log("config-migration-write-failed");
#endif
}

ConfigStore &ConfigStore::instance() {
    static ConfigStore store;
    return store;
}
void ConfigStore::reload() {
    std::map<std::string,std::string> parsed;
#ifdef __SWITCH__
    std::ifstream file("sdmc:/switch/UURemote-ns/config.ini");
#else
    std::ifstream file("config.ini");
#endif
    std::string line;
    while(std::getline(file,line)) {
        const auto key=config::keyOf(line);
        if(key.empty()) continue;
        parsed.emplace(key,config::valueOf(line));
    }
    {
        std::lock_guard lock(mutex_);
        values_.swap(parsed);
    }
}
std::string ConfigStore::value(const std::string &key) const {
    std::lock_guard lock(mutex_);
    const auto found=values_.find(key);
    return found==values_.end()?std::string{}:found->second;
}
bool ConfigStore::has(const std::string &key) const {
    std::lock_guard lock(mutex_);
    return values_.find(key)!=values_.end();
}
int ConfigStore::integer(const std::string &key,int fallback,int minimum,int maximum) const {
    std::string text;
    {
        std::lock_guard lock(mutex_);
        const auto found=values_.find(key);
        if(found!=values_.end()) text=found->second;
    }
    if(text.empty()) return fallback;
    if(key == "fps")
        return config::choice(text, fallback, {30, 60});
    return config::integer(text, fallback, minimum, maximum);
}
void saveQualityConfig(int width, int height, int fps, int quality, bool vsync,
                       int playoutDelay, int maxBitrateMbps, bool hevc) {
#ifdef __SWITCH__
    const std::string path = "sdmc:/switch/UURemote-ns/config.ini";
#else
    const std::string path = "config.ini";
#endif
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    input.close();
    const std::string framePacing = config::framePacingFromLines(lines);
    std::string retained;
    for (const auto &source : lines) {
        const auto key = config::keyOf(source);
        if (key == "frame_pacing") continue;
        if (key != "width" && key != "height" && key != "fps" && key != "quality" &&
            key != "vsync" && key != "playout_delay_ms" && key != "max_bitrate_mbps" &&
            key != "video_codec")
            retained += source + '\n';
    }
    std::ofstream output(path + ".quality-tmp", std::ios::trunc);
    output << retained << "width=" << width << "\nheight=" << height << "\nfps=" << fps << "\nquality=" << quality << '\n';
    output << "vsync=" << (vsync ? 1 : 0) << '\n';
    output << "frame_pacing=" << framePacing << "\nplayout_delay_ms=" << playoutDelay << '\n';
    output << "max_bitrate_mbps=" << maxBitrateMbps << '\n';
    output << "video_codec=" << (hevc?"hevc":"h264") << '\n';
    output.close();
    if (!output) throw std::runtime_error("画质设置保存失败");
    const auto backup = path + ".bak";
    std::ifstream existing(path);
    const bool hadConfig = bool(existing);
    existing.close();
    if (hadConfig) {
        // Keep exactly one fixed backup; the old timestamped copies are no
        // longer generated. Remove any previous backup before replacing it.
        std::remove(backup.c_str());
        if (std::rename(path.c_str(), backup.c_str()) != 0)
            throw std::runtime_error("画质设置保存失败");
    }
    if (std::rename((path + ".quality-tmp").c_str(), path.c_str()) != 0) {
        if (hadConfig) std::rename(backup.c_str(), path.c_str());
        throw std::runtime_error("画质设置保存失败，已尝试恢复原配置");
    }
#ifdef __SWITCH__
    if (R_FAILED(fsdevCommitDevice("sdmc")))
        throw std::runtime_error("画质设置写入未确认，请重试");
    // Remove legacy timestamped backups left by earlier builds. Only the fixed
    // config.ini.bak remains.
    if (DIR *dir = opendir("sdmc:/switch/UURemote-ns")) {
        const std::string prefix = "config.ini.before-quality-";
        while (auto *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind(prefix, 0) == 0)
                std::remove(("sdmc:/switch/UURemote-ns/" + name).c_str());
        }
        closedir(dir);
    }
#endif
    ConfigStore::instance().reload();
}
} // namespace uu
