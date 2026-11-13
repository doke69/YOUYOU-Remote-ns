#include "packet_stage_trace.h"
#include "udp_send_diagnostics.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <mutex>

namespace {
constexpr uint64_t contextUs = 150000;
struct Row {
    uint64_t atUs;
    uu_packet_header header;
    uu_packet_stage stage;
    int result;
};
struct Window {
    uint64_t generation = 0, fromUs = 0, toUs = 0;
};
struct Capture {
    Window window;
    std::array<Row, 32768> rows;
    size_t count = 0;
    uint64_t copyUs = 0;
    bool historyLimited = false, truncated = false;
};

std::mutex packetMutex;
std::array<Row, 65536> history;
std::array<Capture, 4> captures;
Window pending;
uint64_t written = 0, skipped = 0;

// Called with packetMutex held. Keep the four longest observed gaps.
void capturePending() {
    const auto start = uu_udp_now_us();
    const auto severity = pending.toUs - pending.fromUs;
    auto capture = std::min_element(captures.begin(), captures.end(), [](const auto &left, const auto &right) {
        return left.window.toUs - left.window.fromUs < right.window.toUs - right.window.fromUs;
    });
    if (capture->window.toUs - capture->window.fromUs >= severity) {
        ++skipped;
        pending = {};
        return;
    }
    if (capture->window.generation) ++skipped;
    capture->window = pending;
    capture->count = 0;
    const auto first = written - std::min<uint64_t>(written, history.size());
    const auto lower = pending.fromUs - std::min(pending.fromUs, contextUs);
    const auto upper = pending.toUs + contextUs;
    capture->historyLimited = first && history[first % history.size()].atUs > lower;
    // Walk back from the end and keep the newest data if a window exceeds capacity.
    auto i = written;
    while (i > first && history[(i - 1) % history.size()].atUs > upper) --i;
    while (i > first && history[(i - 1) % history.size()].atUs >= lower &&
           capture->count < capture->rows.size()) {
        capture->rows[capture->count++] = history[(--i) % history.size()];
    }
    capture->truncated = i > first && history[(i - 1) % history.size()].atUs >= lower;
    capture->copyUs = uu_udp_now_us() - start;
    pending = {};
}
}

extern "C" void uu_packet_stage_record(uu_packet_stage stage, uu_packet_header header, int result) {
    std::lock_guard lock(packetMutex);
    const auto at = uu_udp_now_us();
    if (pending.generation && at > pending.toUs + contextUs) capturePending();
    history[written++ % history.size()] = {at, header, stage, result};
}

void uu::notePacketStageGap(uint64_t generation, uint64_t fromUs, uint64_t toUs) {
    std::lock_guard lock(packetMutex);
    if (pending.generation == generation && fromUs <= pending.toUs && pending.fromUs <= toUs) {
        pending.fromUs = std::min(pending.fromUs, fromUs);
        pending.toUs = std::max(pending.toUs, toUs);
        return;
    }
    if (pending.generation) capturePending();
    pending = {generation, fromUs, toUs};
}

bool uu::flushPacketStages(const char *path) {
    // Called after disconnect, so the producer threads are stopped.
    std::lock_guard lock(packetMutex);
    if (pending.generation) capturePending();
    std::ostringstream out;
    out << "# packet_stage_schema=1 context_us=" << contextUs << " skipped=" << skipped << '\n'
        << "generation,from_us,to_us,history_limited,truncated,copy_us,at_us,stage,kind,pt,ssrc,seq,rtp_ts,bytes,result\n";
    for (const auto &capture : captures) {
        const auto &w = capture.window;
        if (!w.generation) continue;
        for (auto i = capture.count; i > 0; --i) {
            const auto &r = capture.rows[i - 1];
            out << w.generation << ',' << w.fromUs << ',' << w.toUs << ',' << capture.historyLimited << ','
                << capture.truncated << ',' << capture.copyUs << ',' << r.atUs << ',' << r.stage << ','
                << unsigned(r.header.kind) << ',' << unsigned(r.header.type) << ',' << r.header.ssrc << ','
                << r.header.sequence << ',' << r.header.timestamp << ',' << r.header.bytes << ',' << r.result << '\n';
        }
    }
    // Format in memory, then write the capture in one bulk transfer to the SD card.
    const auto bytes = out.str();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return bool(file);
}
