#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum uu_udp_stage { UU_UDP_LOCK, UU_UDP_DSCP, UU_UDP_CALL, UU_UDP_ADDRESS, UU_UDP_SYSCALL, UU_UDP_STAGE_COUNT };
typedef struct {uint64_t calls,total_us,max_us,ge16ms;} uu_udp_stage_sample;
/* Process-wide, cumulative and independently sampled atomics; not a transaction
 * or per-message trace. UDP_CALL is POLL only, ADDRESS/SYSCALL cover udp_sendto. */
uint64_t uu_udp_now_us(void);
void uu_udp_record(enum uu_udp_stage stage,uint64_t elapsed);
void uu_udp_snapshot(uu_udp_stage_sample result[UU_UDP_STAGE_COUNT]);
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING)
enum { UU_UDP_SEND_SLOW_US = 8000 };
uint64_t uu_udp_trace_thread_id(void);
void uu_udp_send_slow_record(uint64_t start_us, uint64_t end_us);
typedef struct {
    uint64_t lock_id, wait_start_us, acquired_us;
    uint64_t udp_start_us, udp_end_us, unlock_start_us;
} uu_udp_send_lock_sample;
void uu_udp_send_lock_record(const uu_udp_send_lock_sample *sample);
#endif
#ifdef __cplusplus
}
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING)
namespace uu {
bool flushUdpSendTrace(const char *path);
bool flushUdpSendLocks(const char *path);
#ifdef UU_BSD_SEND_TIMING
bool flushBsdSendTrace(const char *path);
#ifdef UU_PACKET_STAGE_TRACE
bool flushBsdReceiveTrace(const char *path);
#endif
#endif
}
#endif
#endif
