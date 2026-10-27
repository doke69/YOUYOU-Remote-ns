#include "config_store.hpp"
#include "diagnostics.hpp"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#ifdef __SWITCH__
#include <sys/stat.h>
#include <dirent.h>
#endif

namespace uu {
namespace {
#ifdef __SWITCH__
constexpr const char *kDirectory = "sdmc:/switch/UURemote-ns";
constexpr const char *kPath = "sdmc:/switch/UURemote-ns/config.ini";
constexpr const char *kBackup = "sdmc:/switch/UURemote-ns/config.ini.bak";
constexpr const char *kTemplatePath = "romfs:/default-config.ini";
constexpr unsigned kConfigVersion = 2;

// Keep this in sync with romfs/default-config.ini. It is used only if the
// packaged resource is missing (e.g. a development build with a custom RomFS).
constexpr const char *kFallbackTemplate =
    "config_version=2\n"
    "width=1280\n"
    "height=720\n"
    "fps=60\n"
    "quality=6\n"
    "vsync=1\n"
    "frame_pacing=1\n"
    "software_pacing=1\n"
    "playout_delay_ms=8\n"
    "max_bitrate_mbps=20\n"
    "video_codec=hevc\n"
    "native_display=1\n";

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
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
    const auto equals = line.find('=');
    if (equals == std::string::npos) return {};
    return trim(line.substr(0, equals));
}
std::string valueOf(const std::string &line) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) return {};
    return trim(line.substr(equals + 1));
}
struct Entry {std::string key, value;};
std::vector<Entry> templateEntries() {
    std::string text = readFile(kTemplatePath);
    if (trim(text).empty()) text = kFallbackTemplate;
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
    if (trim(existing).empty()) {
        const std::string backup = readFile(kBackup);
        if (!trim(backup).empty() && writeAtomic(backup)) {
            existing = backup;
            log("config-restored-from-backup");
        }
    }
    if (trim(existing).empty()) {
        const std::string templateText = readFile(kTemplatePath);
        const std::string content = trim(templateText).empty() ? kFallbackTemplate : templateText;
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
        positions[key] = i;
        if (key == "config_version") {
            try {version = unsigned(std::stoul(valueOf(lines[i])));} catch (...) {version = 0;}
        }
    }
    if (version > kConfigVersion) {
        log("config-version-newer-preserved");
        return;
    }

    bool changed = false;
    bool migrated = false;
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
    auto clean=[](std::string value) {
        const auto first=value.find_first_not_of(" \t\r\n");
        if(first==std::string::npos)return std::string{};
        const auto last=value.find_last_not_of(" \t\r\n");
        return value.substr(first,last-first+1);
    };
    std::string line;
    while(std::getline(file,line)) {
        if(const auto comment=line.find('#');comment!=std::string::npos) line.erase(comment);
        const auto equals=line.find('=');
        if(equals==std::string::npos) continue;
        const auto key=clean(line.substr(0,equals));
        if(key.empty()) continue;
        parsed.emplace(key,clean(line.substr(equals+1)));
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
    try {return std::clamp(std::stoi(text),minimum,maximum);}
    catch(...) {return fallback;}
}
} // namespace uu
