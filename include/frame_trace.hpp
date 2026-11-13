#pragma once
#include <cstddef>
#include <cstdint>
#include <ostream>

namespace uu {
// Callers use static reason literals. Store values in the bounded trace and
// format only when exporting, outside the live presentation/receive path.
struct PlayoutTraceRow {
    uint32_t timestamp;
    int64_t decodedMs, dueMs, selectedMs, targetMs, rawDueMs;
    const char *reason;
};
inline std::ostream &operator<<(std::ostream &out, const PlayoutTraceRow &row) {
    return out << row.timestamp << ',' << row.decodedMs << ',' << row.dueMs << ','
               << row.selectedMs << ',' << row.reason << ',' << row.targetMs << ',' << row.rawDueMs;
}
struct ReceiverTraceRow {
    uint32_t timestamp;
    size_t packets, bytes;
    int64_t ageMs;
    const char *reason;
};
inline std::ostream &operator<<(std::ostream &out, const ReceiverTraceRow &row) {
    return out << row.timestamp << ',' << row.packets << ',' << row.bytes << ','
               << row.ageMs << ',' << row.reason;
}
} // namespace uu
