#pragma once
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace uu::config {
// One parsing boundary for config.ini, migration rewriting and quality saves.
// Inline comments are not values; first occurrence is authoritative.
inline std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::string keyOf(const std::string &line) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) return {};
    return trim(line.substr(0, equals));
}

inline std::string valueOf(const std::string &line) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) return {};
    auto value = line.substr(equals + 1);
    if (const auto comment = value.find('#'); comment != std::string::npos)
        value.erase(comment);
    return trim(value);
}

inline bool parseInt(const std::string &text, int &out) {
    const auto value = trim(text);
    if (value.empty()) return false;
    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || !end || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX)
        return false;
    out = int(parsed);
    return true;
}

inline int integer(const std::string &text, int fallback, int minimum, int maximum) {
    int value = 0;
    if (!parseInt(text, value)) return fallback;
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

// A bounded/discrete field: values outside the documented set fall back.
inline int choice(const std::string &text, int fallback, std::initializer_list<int> allowed) {
    int value = 0;
    if (!parseInt(text, value)) return fallback;
    for (const int candidate : allowed)
        if (value == candidate) return value;
    return fallback;
}

inline bool firstValue(const std::vector<std::string> &lines, const std::string &wanted,
                       std::string &out) {
    for (const auto &line : lines) {
        if (keyOf(line) == wanted) {
            out = valueOf(line);
            return true;
        }
    }
    return false;
}

// saveQuality must retain the first parsed frame_pacing, including "0 # off".
inline std::string framePacingFromLines(const std::vector<std::string> &lines) {
    std::string raw;
    if (!firstValue(lines, "frame_pacing", raw)) return "1";
    // Reuse the runtime ConfigStore integer boundary: 0/1 are accepted and
    // out-of-range/invalid values clamp/fallback exactly as reload does.
    return std::to_string(integer(raw, 1, 0, 1));
}

inline void setValue(std::string &line, const std::string &value) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) return;
    const auto comment = line.find('#');
    const auto valueEnd = comment == std::string::npos ? line.size() : comment;
    const auto original = line.substr(equals + 1, valueEnd - (equals + 1));
    const auto first = original.find_first_not_of(" \t\r\n");
    const auto last = original.find_last_not_of(" \t\r\n");
    const std::string leading = first == std::string::npos ? "" : original.substr(0, first);
    const std::string trailing = last == std::string::npos ? "" : original.substr(last + 1);
    std::string suffix;
    if (comment != std::string::npos) suffix = line.substr(comment);
    line = line.substr(0, equals + 1) + leading + value + trailing + suffix;
}
} // namespace uu::config
