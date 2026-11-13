#include "udp_send_diagnostics.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <mutex>
#include <vector>
#include <switch.h>

extern "C" SessionMgr g_bsdSessionMgr; // From the audited libnx object.

namespace {
constexpr uint64_t thresholdUs = 4000;
struct SendTiming {
    uint64_t attachStart, attachEnd, ipcStart, ipcEnd;
    uint64_t threadId;
#ifdef UU_PACKET_STAGE_TRACE
    uint64_t detachEnd;
    unsigned operation;
    Result ipcResult;
#endif
};
// Each slot is exclusively owned between the SDK's attach and detach calls.
std::array<SendTiming, NX_SESSION_MGR_MAX_SESSIONS> active{};
std::array<SendTiming, 512> rows{};
std::mutex rowsMutex;
uint64_t seen = 0;
#ifdef UU_PACKET_STAGE_TRACE
std::array<SendTiming, 32768> receiveRows{};
uint64_t receiveSeen = 0;
#endif
}

#ifdef UU_PACKET_STAGE_TRACE
static int attachTimed(SessionMgr *mgr, unsigned operation) {
#else
extern "C" int uu_bsd_attach_timed(SessionMgr *mgr) {
#endif
    const auto start = uu_udp_now_us();
    const int slot = sessionmgrAttachClient(mgr);
    active[slot] = {start, uu_udp_now_us(), 0, 0, 0
#ifdef UU_PACKET_STAGE_TRACE
                    , 0, operation, 0
#endif
    };
    return slot;
}

#ifdef UU_PACKET_STAGE_TRACE
extern "C" int uu_bsd_attach_timed(SessionMgr *mgr) { return attachTimed(mgr, 0); }
extern "C" int uu_bsd_poll_attach_timed(SessionMgr *mgr) { return attachTimed(mgr, 1); }
extern "C" int uu_bsd_recv_attach_timed(SessionMgr *mgr) { return attachTimed(mgr, 2); }
#endif

extern "C" Result uu_bsd_ipc_timed(Handle session) {
    for (unsigned slot = 0; slot < g_bsdSessionMgr.num_sessions; ++slot) {
        if (sessionmgrGetClientSession(&g_bsdSessionMgr, slot) == session) {
            auto &timing = active[slot];
            timing.ipcStart = uu_udp_now_us();
            const Result result = svcSendSyncRequest(session);
            timing.ipcEnd = uu_udp_now_us();
#ifdef UU_PACKET_STAGE_TRACE
            timing.ipcResult = result;
#endif
            return result;
        }
    }
    return svcSendSyncRequest(session);
}

extern "C" void uu_bsd_detach_timed(SessionMgr *mgr, int slot) {
    auto timing = active[slot];
    sessionmgrDetachClient(mgr, slot);
    const auto limit =
#ifdef UU_PACKET_STAGE_TRACE
        timing.operation ? 8000 :
#endif
        thresholdUs;
    if (timing.ipcEnd && timing.ipcEnd - timing.attachStart >= limit) {
#ifdef UU_PACKET_STAGE_TRACE
        timing.detachEnd = uu_udp_now_us();
#endif
        // Release the BSD slot before querying the ID or taking the trace lock.
        // The response is still in the IPC buffer, so this path must not issue IPC.
        timing.threadId = uu_udp_trace_thread_id();
        std::lock_guard lock(rowsMutex);
#ifdef UU_PACKET_STAGE_TRACE
        if (timing.operation) {
            receiveRows[receiveSeen++ % receiveRows.size()] = timing;
            return;
        }
#endif
        rows[seen % rows.size()] = timing;
        ++seen;
    }
}

bool uu::flushBsdSendTrace(const char *path) {
    std::vector<SendTiming> snapshot;
    snapshot.reserve(rows.size());
    uint64_t total;
    {
        std::lock_guard lock(rowsMutex);
        total = seen;
        const auto count = std::min<uint64_t>(total, rows.size());
        for (uint64_t i = total - count; i < total; ++i)
            snapshot.push_back(rows[i % rows.size()]);
    }
    std::ofstream out(path, std::ios::trunc);
    out << "# bsd_send_slow_schema=1 threshold_us=" << thresholdUs
        << " events=" << total << " overwritten=" << total - snapshot.size() << '\n'
        << "attach_start_us,attach_end_us,ipc_start_us,ipc_end_us,thread_id\n";
    for (const auto &row : snapshot)
        out << row.attachStart << ',' << row.attachEnd << ',' << row.ipcStart << ','
            << row.ipcEnd << ',' << row.threadId << '\n';
    out.close();
    return bool(out);
}

#ifdef UU_PACKET_STAGE_TRACE
bool uu::flushBsdReceiveTrace(const char *path) {
    std::vector<SendTiming> snapshot;
    uint64_t total;
    {
        std::lock_guard lock(rowsMutex);
        total = receiveSeen;
        const auto count = std::min<uint64_t>(total, receiveRows.size());
        snapshot.reserve(count);
        for (auto i = total - count; i < total; ++i)
            snapshot.push_back(receiveRows[i % receiveRows.size()]);
    }
    std::ostringstream out;
    out << "# bsd_receive_schema=1 threshold_us=8000 events=" << total
        << " overwritten=" << total - snapshot.size() << '\n'
        << "operation,attach_start_us,attach_end_us,ipc_start_us,ipc_end_us,detach_end_us,thread_id,ipc_result\n";
    for (const auto &row : snapshot)
        out << row.operation << ',' << row.attachStart << ',' << row.attachEnd << ','
            << row.ipcStart << ',' << row.ipcEnd << ',' << row.detachEnd << ','
            << row.threadId << ',' << row.ipcResult << '\n';
    // Format in memory, then write the capture in one bulk transfer to the SD card.
    const auto bytes = out.str();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return bool(file);
}
#endif
