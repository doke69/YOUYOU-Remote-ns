#pragma once
#include <stdint.h>

typedef struct {
    uint64_t poll_start_us, poll_return_us, lock_start_us, lock_acquired_us;
    uint64_t drain_start_us, first_read_us, last_read_us, drain_end_us;
    uint64_t gap_from_us, gap_to_us, max_read_us, max_dispatch_us, update_start_us, update_us;
    uint64_t recv_start_us, recv_end_us, recv_cpu_us, recv_cpu_window_us;
    uint32_t recv_slow_calls, recv_cpu_status;
    int recv_cpu_valid, recv_result, recv_errno, recv_batch;
    uint64_t poll_cpu_us, poll_cpu_window_us;
    uint32_t poll_cpu_status;
    int poll_only, poll_cpu_valid, poll_timeout_ms, poll_result, poll_errno;
    uint32_t poll_peer_readable;
    int poll_interrupt_readable;
    uint64_t socket_packets;
    uint32_t packets;
    int readable;
    intptr_t socket_id;
} uu_udp_rx_sample;

#ifdef __cplusplus
extern "C" void uu_udp_rx_record(const uu_udp_rx_sample *sample);
namespace uu {
enum class UdpIncidentSource : unsigned { Ingress = 1, Supply = 2 };
void noteUdpReceiveIncident(uint64_t generation, UdpIncidentSource source,
                           uint64_t fromUs, uint64_t toUs);
bool flushUdpReceiveTrace(const char *path);
bool flushUdpReceiveIncidents(const char *path);
void startUdpPlayback(uint64_t nowUs);
void stopUdpPlayback(uint64_t nowUs);
bool flushUdpReceiveStages(const char *path);
}
#else
void uu_udp_rx_record(const uu_udp_rx_sample *sample);
void uu_udp_rx_socket_buffer(intptr_t socket_id, int bytes, int error);
#endif
