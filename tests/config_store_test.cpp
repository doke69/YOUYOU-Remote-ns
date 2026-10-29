#include "config_store.hpp"
#include "config_migration.hpp"
#include "config_value.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static void writeFile(const fs::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

static std::string readFile(const fs::path &path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int occurrences(const std::string &text, const std::string &needle) {
    int count = 0;
    for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size())
        ++count;
    return count;
}

int main() {
    try {
        const fs::path original = fs::current_path();
        const fs::path temp = original /
                              ("uu-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp);
        fs::current_path(temp);
        struct Restore { fs::path original; ~Restore() { fs::current_path(original); } } restore{original};

        const fs::path config = temp / "config.ini";
        writeFile(config,
                  "config_version=3\n"
                  "width=1280\nwidth=1920\n"
                  "fps=45\n"
                  "quality=3\n"
                  "frame_pacing=0 # keep off\nframe_pacing=1\n"
                  "video_codec=hevc # keep\n"
                  "# comment line\n"
                  "unknown_key=value # keep\n");
        uu::ConfigStore::instance().reload();
        require(uu::ConfigStore::instance().value("width") == "1280", "reload must be first-wins");
        require(uu::ConfigStore::instance().integer("width", 1280, 320, 1920) == 1280, "first width value");
        require(uu::ConfigStore::instance().value("video_codec") == "hevc", "inline comment stripped");
        require(uu::ConfigStore::instance().integer("fps", 60, 30, 60) == 60, "45 fps must fall back to 60");
        require(uu::ConfigStore::instance().integer("fps", 60, 30, 60) != 45, "fps enum must not be 45");

        auto reloadValue = [&](const std::string &key, const std::string &value) {
            writeFile(config, "config_version=3\n" + key + "=" + value + "\n");
            uu::ConfigStore::instance().reload();
        };
        for (const auto &[text, expected] : std::vector<std::pair<std::string,int>>{
                 {"30", 30}, {"60", 60}, {"45", 60}, {"60abc", 60}, {"3junk", 60},
                 {"", 60}, {"999999999999999999999", 60}}) {
            reloadValue("fps", text);
            require(uu::ConfigStore::instance().integer("fps", 60, 30, 60) == expected,
                    "fps parse/fallback mismatch");
        }
        reloadValue("width", "320abc");
        require(uu::ConfigStore::instance().integer("width", 1280, 320, 1920) == 1280,
                "integer must consume the whole value");
        reloadValue("width", "");
        require(uu::ConfigStore::instance().integer("width", 1280, 320, 1920) == 1280,
                "empty integer must fall back");

        std::vector<std::string> duplicate{"quality=6", "quality=3", "max_bitrate_mbps=20"};
        const auto duplicateMigration = uu::migrateQualityConfigLines(duplicate);
        require(duplicateMigration.duplicateKeys && duplicateMigration.unresolved &&
                    !duplicateMigration.changed, "duplicate F02 keys stay unresolved");
        require(duplicate[0] == "quality=6" && duplicate[2] == "max_bitrate_mbps=20",
                "duplicate legacy values must not be rewritten");

        writeFile(config,
                  "config_version=3\n"
                  "width=1\nwidth=2\n"
                  "quality=6\nquality=3\n"
                  "fps=45\n"
                  "frame_pacing=0 # keep off\nframe_pacing=1\n"
                  "# top comment\n"
                  "unknown_key=value # keep\n");
        uu::saveQualityConfig(1280, 720, 60, 3, true, 8, 0, true);
        const std::string saved = readFile(config);
        require(occurrences(saved, "width=") == 1, "save must write one width");
        require(occurrences(saved, "quality=") == 1, "save must write one quality");
        require(occurrences(saved, "frame_pacing=") == 1, "save must write one frame_pacing");
        require(saved.find("frame_pacing=0") != std::string::npos, "first frame_pacing 0 with comment");
        require(saved.find("frame_pacing=1") == std::string::npos, "last frame_pacing must not win");
        require(saved.find("fps=60") != std::string::npos, "saved fps canonical");
        require(saved.find("# top comment") != std::string::npos, "unknown comments retained");
        require(saved.find("unknown_key=value # keep") != std::string::npos, "unknown config retained");

        // Fixed backup and actual reload after save.
        require(fs::exists(temp / "config.ini.bak"), "fixed backup must exist");
        require(readFile(temp / "config.ini.bak").find("width=1") != std::string::npos,
                "backup keeps previous file");
        uu::ConfigStore::instance().reload();
        require(uu::ConfigStore::instance().integer("fps", 60, 30, 60) == 60, "reload after save");
        require(uu::ConfigStore::instance().value("frame_pacing") == "0", "saved pacing first value");

        uu::saveQualityConfig(1280, 720, 60, 6, true, 16, 50, true);
        uu::ConfigStore::instance().reload();
        require(uu::ConfigStore::instance().integer("quality", 3, 1, 6) == 6, "custom quality survives reload");
        require(uu::ConfigStore::instance().integer("max_bitrate_mbps", 0, 0, 100) == 50, "custom cap survives reload");
        require(uu::ConfigStore::instance().integer("playout_delay_ms", 8, 0, 100) == 16, "playout delay survives reload");

        const std::vector<std::string> pacingLines{"frame_pacing=1 # on", "frame_pacing=0"};
        require(uu::config::framePacingFromLines(pacingLines) == "1", "save parser first wins");
        const std::vector<std::string> pacingComment{"frame_pacing=0 # off", "frame_pacing=1"};
        require(uu::config::framePacingFromLines(pacingComment) == "0", "save parser strips comment");

        // Save/reload boundary parity: 0/1, negative, >1, invalid and comments.
        auto savedPacing = [&] {
            std::ifstream in(config);
            std::string line;
            while (std::getline(in, line)) {
                if (uu::config::keyOf(line) == "frame_pacing")
                    return uu::config::valueOf(line);
            }
            return std::string{};
        };
        for (const auto &[raw, expected] : std::vector<std::pair<std::string,std::string>>{
                 {"0", "0"}, {"1", "1"}, {"-1", "0"}, {"2", "1"},
                 {"abc", "1"}, {"0 # comment", "0"}, {"1 # comment", "1"}}) {
            writeFile(config, "config_version=3\nframe_pacing=" + raw + "\n");
            uu::ConfigStore::instance().reload();
            const int runtime = uu::ConfigStore::instance().integer("frame_pacing", 1, 0, 1);
            uu::saveQualityConfig(1280, 720, 60, 3, true, 8, 0, true);
            require(savedPacing() == expected, "frame_pacing save normalization mismatch");
            require(savedPacing() == std::to_string(runtime), "save/reload frame_pacing parity");
        }

        fs::current_path(original);
        fs::remove_all(temp);
        std::cout << "PASS: production ConfigStore first-wins/full integer/fps enum; migration duplicate contract; "
                     "saveQuality one-key/comment/0-comment/fixed-bak\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
