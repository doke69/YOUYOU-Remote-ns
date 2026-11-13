#include "udp_receive_trace.h"
#include "diagnostics.hpp"
#ifdef UU_PACKET_STAGE_TRACE
#include "packet_stage_trace.h"
#endif
#include "udp_send_diagnostics.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING)
#include <switch.h>
#endif

namespace {
constexpr size_t capacity = 512;
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING)
struct SlowSend {
    uint64_t startUs, endUs, threadId;
};
std::mutex slowSendMutex;
std::array<SlowSend, 512> slowSends;
uint64_t slowSendCount = 0;
struct SlowSendLock {
    uu_udp_send_lock_sample times;
    uint64_t threadId;
};
std::array<SlowSendLock, 512> slowSendLocks;
uint64_t slowSendLockCount = 0;
#endif
constexpr size_t sessionCapacity = 4;
constexpr size_t incidentsPerSession = 4;
constexpr size_t rowsPerIncident = 96;
constexpr uint64_t contextUs = 150000;

struct Row {
    uint64_t order;
    uu_udp_rx_sample sample;
};

struct StagePeak {
    uint64_t durationUs;
    Row row;
};

struct StagePeaks {
    uint64_t seen = 0;
    std::array<StagePeak, 8> rows{};

    void keep(uint64_t duration, uint64_t order, const uu_udp_rx_sample &sample) {
        const auto count = std::min<uint64_t>(seen, rows.size());
        ++seen;
        auto entry = rows.begin() + count;
        if (count == rows.size()) {
            entry = std::min_element(rows.begin(), rows.end(), [](const StagePeak &a, const StagePeak &b) {
                return a.durationUs < b.durationUs;
            });
            if (entry->durationUs >= duration)
                return;
        }
        *entry = {duration, {order, sample}};
    }
};

struct Incident {
    uint64_t id = 0, generation = 0, fromUs = 0, toUs = 0, severityUs = 0;
    unsigned sources = 0;
    size_t count = 0;
    bool truncated = false, historyLimited = false;
    std::array<Row, rowsPerIncident> rows;

    void keep(uint64_t order, const uu_udp_rx_sample &s) {
        if (!id || s.poll_start_us > toUs + contextUs ||
            s.update_start_us + s.update_us + contextUs < fromUs)
            return;

        // A trigger may extend a window after later UDP rows have been kept.
        // Insert by recorder order so rescanning history never duplicates rows.
        const auto end = rows.begin() + count;
        const auto pos = std::lower_bound(rows.begin(), end, order,
                                          [](const Row &row, uint64_t value) { return row.order < value; });
        if (pos != end && pos->order == order)
            return;
        if (count == rows.size()) {
            truncated = true;
            return;
        }
        std::move_backward(pos, end, end + 1);
        *pos = {order, s};
        ++count;
    }
};

struct Session {
    bool used = false;
    uint64_t generation = 0;
    std::array<Incident, incidentsPerSession> incidents;
};

std::mutex traceMutex;
std::array<uu_udp_rx_sample, capacity> trace;
std::array<Session, sessionCapacity> sessions;
// Readable poll, other poll, and recvfrom have separate quotas.
using StageSet = std::array<StagePeaks, 3>;
std::array<StageSet, 2> stages; // Playback and non-playback retain separate peaks.
uint64_t playbackStartUs = 0, playbackStopUs = 0;

// Called with traceMutex held. Calls crossing either boundary belong outside playback.
size_t phaseFor(uint64_t fromUs, uint64_t toUs) {
    return playbackStartUs && fromUs >= playbackStartUs &&
           (!playbackStopUs || toUs < playbackStopUs) ? 0 : 1;
}
std::atomic<uint64_t> longDrains{0};
uint64_t events = 0, incidentId = 0, incidentsSkipped = 0, sessionsEvicted = 0;

constexpr const char *rowHeader =
    "order,socket_id,readable,poll_start_us,poll_return_us,lock_start_us,lock_acquired_us,"
    "drain_start_us,first_read_us,last_read_us,drain_end_us,gap_from_us,gap_to_us,"
    "max_read_us,max_dispatch_us,update_start_us,update_us,packets,socket_packets,"
    "recv_start_us,recv_end_us,recv_cpu_us,recv_cpu_window_us,recv_slow_calls,"
    "recv_cpu_valid,recv_cpu_status,recv_result,recv_errno,"
    "poll_only,poll_cpu_us,poll_cpu_window_us,poll_cpu_valid,poll_cpu_status,"
    "poll_timeout_ms,poll_result,poll_errno,poll_peer_readable,poll_interrupt_readable,recv_batch\n";

void writeRow(std::ostream &out, const Row &row) {
    const auto &s = row.sample;
    out << row.order << ',' << s.socket_id << ',' << s.readable << ','
        << s.poll_start_us << ',' << s.poll_return_us << ',' << s.lock_start_us << ',' << s.lock_acquired_us << ','
        << s.drain_start_us << ',' << s.first_read_us << ',' << s.last_read_us << ',' << s.drain_end_us << ','
        << s.gap_from_us << ',' << s.gap_to_us << ',' << s.max_read_us << ',' << s.max_dispatch_us << ','
        << s.update_start_us << ',' << s.update_us << ',' << s.packets << ',' << s.socket_packets << ','
        << s.recv_start_us << ',' << s.recv_end_us << ',' << s.recv_cpu_us << ',' << s.recv_cpu_window_us << ','
        << s.recv_slow_calls << ',' << s.recv_cpu_valid << ',' << s.recv_cpu_status << ','
        << s.recv_result << ',' << s.recv_errno << ','
        << s.poll_only << ',' << s.poll_cpu_us << ',' << s.poll_cpu_window_us << ','
        << s.poll_cpu_valid << ',' << s.poll_cpu_status << ',' << s.poll_timeout_ms << ','
        << s.poll_result << ',' << s.poll_errno << ',' << s.poll_peer_readable << ','
        << s.poll_interrupt_readable << ',' << s.recv_batch << '\n';
}
}

#if defined(__SWITCH__) && defined(UU_SWITCH_BATCH_RX)
extern "C" void uu_udp_rx_socket_buffer(intptr_t socket_id, int bytes, int error) {
    uu::diagnostic(("ice-udp-rcvbuf-fd-" + std::to_string(socket_id) + "-bytes-" +
                    std::to_string(bytes) + "-errno-" + std::to_string(error)).c_str());
}
#endif

extern "C" void uu_udp_rx_record(const uu_udp_rx_sample *sample) {
    const auto &s = *sample;
    if (s.poll_only) {
        std::lock_guard lock(traceMutex);
        auto &peaks = stages[phaseFor(s.poll_start_us, s.poll_return_us)][s.poll_peer_readable ? 0 : 1];
        peaks.keep(s.poll_return_us - s.poll_start_us, peaks.seen + 1, s);
        return;
    }
    if (s.drain_end_us - s.drain_start_us >= 20000)
        longDrains.fetch_add(1, std::memory_order_relaxed);
    const bool firstDrain = s.packets && s.socket_packets == s.packets;
    if (!firstDrain && !s.recv_slow_calls && s.gap_to_us - s.gap_from_us < 20000 &&
        s.lock_acquired_us - s.lock_start_us < 4000 &&
        (!s.readable || s.drain_start_us - s.poll_return_us < 4000) &&
        s.max_read_us < 4000 && s.max_dispatch_us < 4000 && s.update_us < 4000)
        return;

    std::lock_guard lock(traceMutex);
    trace[events % capacity] = s;
    ++events;
    if (s.recv_end_us - s.recv_start_us >= 50000)
        stages[phaseFor(s.recv_start_us, s.recv_end_us)][2].keep(s.recv_end_us - s.recv_start_us, events, s);
    for (auto &session : sessions)
        for (auto &incident : session.incidents)
            incident.keep(events, s);
}

void uu::noteUdpReceiveIncident(uint64_t generation, UdpIncidentSource source,
                               uint64_t fromUs, uint64_t toUs) {
    const auto severity = toUs - fromUs;
#ifdef UU_PACKET_STAGE_TRACE
    notePacketStageGap(generation, fromUs, toUs);
#endif
    std::lock_guard lock(traceMutex);
    auto session = std::find_if(sessions.begin(), sessions.end(), [generation](const Session &s) {
        return s.used && s.generation == generation;
    });
    if (session == sessions.end()) {
        session = std::min_element(sessions.begin(), sessions.end(), [](const Session &a, const Session &b) {
            if (a.used != b.used)
                return !a.used;
            return a.generation < b.generation;
        });
        if (session->used)
            ++sessionsEvicted;
        session->used = true;
        session->generation = generation;
        for (auto &incident : session->incidents)
            incident.id = 0;
    }

    auto &incidents = session->incidents;
    auto entry = std::find_if(incidents.begin(), incidents.end(), [fromUs, toUs](const Incident &i) {
        return i.id && i.fromUs < toUs && fromUs < i.toUs;
    });
    if (entry == incidents.end()) {
        entry = std::min_element(incidents.begin(), incidents.end(), [](const Incident &a, const Incident &b) {
            if (bool(a.id) != bool(b.id))
                return !a.id;
            return a.severityUs < b.severityUs;
        });
        if (entry->id) {
            ++incidentsSkipped;
            if (entry->severityUs >= severity)
                return;
        }
        entry->id = ++incidentId;
        entry->generation = generation;
        entry->fromUs = fromUs;
        entry->toUs = toUs;
        entry->severityUs = severity;
        entry->sources = 0;
        entry->count = 0;
        entry->truncated = false;
        entry->historyLimited = false;
    } else {
        entry->fromUs = std::min(entry->fromUs, fromUs);
        entry->toUs = std::max(entry->toUs, toUs);
        entry->severityUs = std::max(entry->severityUs, severity);
    }
    entry->sources |= static_cast<unsigned>(source);

    const auto first = events - std::min<uint64_t>(events, capacity);
    if (first && trace[first % capacity].poll_start_us + contextUs > entry->fromUs)
        entry->historyLimited = true;
    for (auto i = first; i < events; ++i)
        entry->keep(i + 1, trace[i % capacity]);
}

bool uu::flushUdpReceiveTrace(const char *path) {
    std::vector<Row> snapshot;
    snapshot.reserve(capacity);
    uint64_t count, first;
    {
        std::lock_guard lock(traceMutex);
        count = events;
        first = count - std::min<uint64_t>(count, capacity);
        for (auto i = first; i < count; ++i)
            snapshot.push_back({i + 1, trace[i % capacity]});
    }
    std::ofstream out(path, std::ios::trunc);
    out << "# udp_receive_schema=3 events=" << count << " overwritten=" << first
        << " long_drains=" << longDrains.load(std::memory_order_relaxed) << '\n'
        << rowHeader;
    for (const auto &row : snapshot)
        writeRow(out, row);
    out.close();
    return bool(out);
}

bool uu::flushUdpReceiveIncidents(const char *path) {
    std::vector<Incident> snapshot;
    snapshot.reserve(sessionCapacity * incidentsPerSession);
    uint64_t skipped, evicted;
    {
        std::lock_guard lock(traceMutex);
        skipped = incidentsSkipped;
        evicted = sessionsEvicted;
        for (const auto &session : sessions)
            for (const auto &incident : session.incidents)
                if (incident.id)
                    snapshot.push_back(incident);
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const Incident &a, const Incident &b) {
        return a.id < b.id;
    });
    std::ofstream out(path, std::ios::trunc);
    out << "# udp_incident_schema=3 incidents=" << snapshot.size() << " discarded_incidents=" << skipped
        << " evicted_sessions=" << evicted << " context_us=" << contextUs << '\n'
        << "incident_id,generation,sources,from_us,to_us,severity_us,rows,truncated,history_limited,"
        << rowHeader;
    for (const auto &incident : snapshot) {
        for (size_t i = 0; i < std::max<size_t>(1, incident.count); ++i) {
            out << incident.id << ',' << incident.generation << ',' << incident.sources << ','
                << incident.fromUs << ',' << incident.toUs << ',' << incident.severityUs << ','
                << incident.count << ',' << incident.truncated << ',' << incident.historyLimited << ',';
            writeRow(out, incident.count ? incident.rows[i] : Row{});
        }
    }
    out.close();
    return bool(out);
}


void uu::startUdpPlayback(uint64_t nowUs) {
    std::lock_guard lock(traceMutex);
    playbackStartUs = nowUs;
    playbackStopUs = 0;
}

void uu::stopUdpPlayback(uint64_t nowUs) {
    std::lock_guard lock(traceMutex);
    if (playbackStartUs && !playbackStopUs)
        playbackStopUs = nowUs;
}

#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING)
uint64_t uu_udp_trace_thread_id(void) {
    u64 id = 0;
    return R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) ? id : 0;
}

void uu_udp_send_slow_record(uint64_t start_us, uint64_t end_us) {
    const SlowSend sample{start_us, end_us, uu_udp_trace_thread_id()};
    std::lock_guard lock(slowSendMutex);
    slowSends[slowSendCount % slowSends.size()] = sample;
    ++slowSendCount;
}

void uu_udp_send_lock_record(const uu_udp_send_lock_sample *times) {
    const SlowSendLock sample{*times, uu_udp_trace_thread_id()};
    std::lock_guard lock(slowSendMutex);
    slowSendLocks[slowSendLockCount % slowSendLocks.size()] = sample;
    ++slowSendLockCount;
}

bool uu::flushUdpSendLocks(const char *path) {
    std::vector<SlowSendLock> snapshot;
    snapshot.reserve(slowSendLocks.size());
    uint64_t seen;
    {
        std::lock_guard lock(slowSendMutex);
        seen = slowSendLockCount;
        const auto count = std::min<uint64_t>(seen, slowSendLocks.size());
        for (uint64_t i = seen - count; i < seen; ++i)
            snapshot.push_back(slowSendLocks[i % slowSendLocks.size()]);
    }
    std::ofstream out(path, std::ios::trunc);
    out << "# udp_send_lock_schema=1 threshold_us=" << UU_UDP_SEND_SLOW_US
        << " events=" << seen << " overwritten=" << seen - snapshot.size() << '\n'
        << "lock_id,thread_id,wait_start_us,acquired_us,udp_start_us,udp_end_us,unlock_start_us\n";
    for (const auto &row : snapshot) {
        const auto &t = row.times;
        out << t.lock_id << ',' << row.threadId << ',' << t.wait_start_us << ',' << t.acquired_us << ','
            << t.udp_start_us << ',' << t.udp_end_us << ',' << t.unlock_start_us << '\n';
    }
    out.close();
    return bool(out);
}

bool uu::flushUdpSendTrace(const char *path) {
    std::vector<SlowSend> snapshot;
    snapshot.reserve(slowSends.size());
    uint64_t seen;
    {
        std::lock_guard lock(slowSendMutex);
        seen = slowSendCount;
        const uint64_t count = std::min<uint64_t>(seen, slowSends.size());
        for (uint64_t i = seen - count; i < seen; ++i)
            snapshot.push_back(slowSends[i % slowSends.size()]);
    }
    std::ofstream out(path, std::ios::trunc);
    out << "# udp_send_slow_schema=1 threshold_us=" << UU_UDP_SEND_SLOW_US
        << " events=" << seen << " overwritten=" << seen - snapshot.size() << '\n'
        << "start_us,end_us,thread_id\n";
    for (const auto &sample : snapshot)
        out << sample.startUs << ',' << sample.endUs << ',' << sample.threadId << '\n';
    out.close();
    return bool(out);
}
#endif

bool uu::flushUdpReceiveStages(const char *path) {
    std::array<StageSet, 2> snapshot;
    {
        std::lock_guard lock(traceMutex);
        snapshot = stages;
    }
    constexpr const char *phases[] = {"playback", "outside_playback"};
    constexpr const char *names[] = {"poll_readable", "poll_other", "recvfrom"};
    std::ofstream out(path, std::ios::trunc);
    out << "# udp_stage_schema=2 threshold_us=50000";
    for (size_t phase = 0; phase < snapshot.size(); ++phase)
        for (size_t stage = 0; stage < snapshot[phase].size(); ++stage)
            out << ' ' << phases[phase] << '_' << names[stage] << "_seen=" << snapshot[phase][stage].seen;
    out << '\n' << "phase,stage,duration_us," << rowHeader;
    for (size_t phase = 0; phase < snapshot.size(); ++phase) {
        for (size_t stage = 0; stage < snapshot[phase].size(); ++stage) {
            auto &peaks = snapshot[phase][stage];
            const auto end = peaks.rows.begin() + std::min<uint64_t>(peaks.seen, peaks.rows.size());
            std::sort(peaks.rows.begin(), end, [](const StagePeak &a, const StagePeak &b) {
                return a.row.order < b.row.order;
            });
            for (auto it = peaks.rows.begin(); it != end; ++it) {
                const char *name = it->row.sample.recv_batch ? "recvmmsg" : names[stage];
                out << phases[phase] << ',' << name << ',' << it->durationUs << ',';
                writeRow(out, it->row);
            }
        }
    }
    out.close();
    return bool(out);
}
