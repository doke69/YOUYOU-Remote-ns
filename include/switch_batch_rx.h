#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Zero-initialize once. Owned exclusively by one libjuice registry's poll
// thread; release only after that thread has joined. Never share across threads.
typedef struct uu_batch_scratch {
    unsigned char *data;
    size_t capacity;
} uu_batch_scratch;

int uu_recvmmsg_reuse(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
                     int flags, struct timespec *timeout, uu_batch_scratch *scratch);
void uu_recvmmsg_release(uu_batch_scratch *scratch);
// Process totals; updated only when a buffer is allocated or released.
void uu_batch_rx_diagnostics(uint64_t *allocations, uint64_t *frees, uint64_t *live_bytes);

#ifdef __cplusplus
}
#endif
