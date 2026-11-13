/**
 * Copyright (c) 2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "conn_poll.h"
#ifdef UU_PACKET_STAGE_TRACE
#include "packet_stage_trace.h"
#endif
#include "agent.h"
#include "log.h"
#include "socket.h"
#include "thread.h"
#include "udp.h"
#ifdef UU_UDP_SEND_TIMING
#include "udp_send_diagnostics.h"
#endif
#ifdef UU_UDP_RX_TRACE
#include "udp_receive_trace.h"
#if defined(__SWITCH__) && defined(UU_UDP_RX_CPU_TIMING)
#include <switch/arm/counter.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
#include <switch/result.h>
#endif
#endif

#include <assert.h>
#include <string.h>
#ifdef UU_SWITCH_BATCH_REUSE
#include "switch_batch_rx.h"
#endif
#ifdef __SWITCH__
#define WSAENOTSOCK ENOTSOCK
#include <sys/uio.h>
// Implemented by the application; marks the single ICE/UDP poll thread so its
// CPU time can be sampled without assuming it shares the RTC worker pool.
void uu_cpu_mark(unsigned role);
#endif

#define BUFFER_SIZE 4096
#if defined(__SWITCH__) && defined(UU_SWITCH_BATCH_RX)
enum { batch_size = 16 };
#endif

typedef struct registry_impl {
	thread_t thread;
#if defined(__SWITCH__) && defined(UU_SWITCH_BATCH_RX)
    // Reused by this registry's poll thread. Keep 64 KiB off the pthread stack.
    char batch_payload[batch_size][BUFFER_SIZE];
#ifdef UU_SWITCH_BATCH_REUSE
    uu_batch_scratch batch_scratch;
#endif
#endif
#if defined(_WIN32) || defined(__SWITCH__)
	socket_t interrupt_sock;
#else
	int interrupt_pipe_out;
	int interrupt_pipe_in;
#endif
} registry_impl_t;

typedef enum conn_state { CONN_STATE_NEW = 0, CONN_STATE_READY, CONN_STATE_FINISHED } conn_state_t;

#ifdef __SWITCH__
static uint64_t uu_recv_calls,uu_recv_messages,uu_recv_batches,uu_recv_max,uu_recv_fallbacks;
static int uu_recv_batch_error;
void uu_juice_batch_diagnostics(uint64_t *calls,uint64_t *messages,uint64_t *batches,
                                uint64_t *max_batch,uint64_t *fallbacks,int *error) {
    *calls=__atomic_load_n(&uu_recv_calls,__ATOMIC_RELAXED);
    *messages=__atomic_load_n(&uu_recv_messages,__ATOMIC_RELAXED);
    *batches=__atomic_load_n(&uu_recv_batches,__ATOMIC_RELAXED);
    *max_batch=__atomic_load_n(&uu_recv_max,__ATOMIC_RELAXED);
    *fallbacks=__atomic_load_n(&uu_recv_fallbacks,__ATOMIC_RELAXED);
    *error=__atomic_load_n(&uu_recv_batch_error,__ATOMIC_RELAXED);
}
#endif

typedef struct conn_impl {
	conn_registry_t *registry;
	conn_state_t state;
	socket_t sock;
#if defined(UU_DSCP_FAMILY_CACHE) || defined(UU_UDP_FAMILY_CACHE)
    int socket_family;
#endif
#ifdef __SWITCH__
    int batch_disabled;
#endif
	mutex_t send_mutex;
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING) && defined(UU_UDP_RX_TRACE)
    uint64_t send_lock_id;
#endif
	int send_ds;
	timestamp_t next_timestamp;
#ifdef UU_UDP_RX_TRACE
	uint64_t rx_last_read_us;
	uint64_t rx_packets;
#endif
} conn_impl_t;

typedef struct pfds_record {
	struct pollfd *pfds;
	nfds_t size;
#ifdef UU_UDP_RX_TRACE
	uint64_t poll_start_us, poll_return_us;
#endif
} pfds_record_t;

int conn_poll_prepare(conn_registry_t *registry, pfds_record_t *pfds, timestamp_t *next_timestamp);
int conn_poll_process(conn_registry_t *registry, pfds_record_t *pfds);
int conn_poll_run(conn_registry_t *registry);

static thread_return_t THREAD_CALL conn_thread_entry(void *arg) {
	thread_set_name_self("juice poll");
	conn_registry_t *registry = (conn_registry_t *)arg;
	conn_poll_run(registry);
	return (thread_return_t)0;
}

int conn_poll_registry_init(conn_registry_t *registry, udp_socket_config_t *config) {
	(void)config;
	registry_impl_t *registry_impl = calloc(1, sizeof(registry_impl_t));
	if (!registry_impl) {
		JLOG_FATAL("Memory allocation failed for connections registry impl");
		return -1;
	}

#if defined(_WIN32) || defined(__SWITCH__)
	udp_socket_config_t interrupt_config;
	memset(&interrupt_config, 0, sizeof(interrupt_config));
	interrupt_config.bind_address = "localhost";
	registry_impl->interrupt_sock = udp_create_socket(&interrupt_config);
	if (registry_impl->interrupt_sock == INVALID_SOCKET) {
		JLOG_FATAL("Dummy socket creation failed");
		free(registry_impl);
		return -1;
	}
#else
	int pipefds[2];
	if (pipe(pipefds)) {
		JLOG_FATAL("Pipe creation failed");
		free(registry_impl);
		return -1;
	}

	fcntl(pipefds[0], F_SETFL, O_NONBLOCK);
	fcntl(pipefds[1], F_SETFL, O_NONBLOCK);
	registry_impl->interrupt_pipe_out = pipefds[1]; // read
	registry_impl->interrupt_pipe_in = pipefds[0];  // write
#endif

	registry->impl = registry_impl;

	JLOG_DEBUG("Starting connections thread");
	int ret = thread_init(&registry_impl->thread, conn_thread_entry, registry);
	if (ret) {
		JLOG_FATAL("Thread creation failed, error=%d", ret);
		goto error;
	}

	return 0;

error:
#if !defined(_WIN32) && !defined(__SWITCH__)
	close(registry_impl->interrupt_pipe_out);
	close(registry_impl->interrupt_pipe_in);
#endif
	free(registry_impl);
	registry->impl = NULL;
	return -1;
}

void conn_poll_registry_cleanup(conn_registry_t *registry) {
	registry_impl_t *registry_impl = registry->impl;

	JLOG_VERBOSE("Waiting for connections thread");
	thread_join(registry_impl->thread, NULL);
#ifdef UU_SWITCH_BATCH_REUSE
    uu_recvmmsg_release(&registry_impl->batch_scratch);
#endif

#if defined(_WIN32) || defined(__SWITCH__)
	closesocket(registry_impl->interrupt_sock);
#else
	close(registry_impl->interrupt_pipe_out);
	close(registry_impl->interrupt_pipe_in);
#endif
	free(registry->impl);
	registry->impl = NULL;
}

int conn_poll_prepare(conn_registry_t *registry, pfds_record_t *pfds, timestamp_t *next_timestamp) {
	timestamp_t now = current_timestamp();
	*next_timestamp = now + 60000;

	mutex_lock(&registry->mutex);
	nfds_t size = (nfds_t)(1 + registry->agents_size);
	if (pfds->size != size) {
		struct pollfd *new_pfds = realloc(pfds->pfds, sizeof(struct pollfd) * size);
		if (!new_pfds) {
			JLOG_FATAL("Memory allocation for poll file descriptors failed");
			goto error;
		}
		pfds->pfds = new_pfds;
		pfds->size = size;
	}

	registry_impl_t *registry_impl = registry->impl;
	struct pollfd *interrupt_pfd = pfds->pfds;
	assert(interrupt_pfd);
#if defined(_WIN32) || defined(__SWITCH__)
	interrupt_pfd->fd = registry_impl->interrupt_sock;
#else
	interrupt_pfd->fd = registry_impl->interrupt_pipe_in;
#endif
	interrupt_pfd->events = POLLIN;

	for (nfds_t i = 1; i < pfds->size; ++i) {
		struct pollfd *pfd = pfds->pfds + i;
		juice_agent_t *agent = registry->agents[i - 1];
		if (!agent) {
			pfd->fd = INVALID_SOCKET;
			pfd->events = 0;
			continue;
		}

		conn_impl_t *conn_impl = agent->conn_impl;
		if (!conn_impl ||
		    (conn_impl->state != CONN_STATE_NEW && conn_impl->state != CONN_STATE_READY)) {
			pfd->fd = INVALID_SOCKET;
			pfd->events = 0;
			continue;
		}

		if (conn_impl->state == CONN_STATE_NEW)
			conn_impl->state = CONN_STATE_READY;

		if (*next_timestamp > conn_impl->next_timestamp)
			*next_timestamp = conn_impl->next_timestamp;

		pfd->fd = conn_impl->sock;
		pfd->events = POLLIN;
	}

	int count = registry->agents_count;
	mutex_unlock(&registry->mutex);
	return count;

error:
	mutex_unlock(&registry->mutex);
	return -1;
}

static int conn_poll_recv(socket_t sock, char *buffer, size_t size, addr_record_t *src
#if defined(UU_UDP_RX_TRACE) && defined(__SWITCH__)
                         , uu_udp_rx_sample *sample
#endif
) {
	JLOG_VERBOSE("Receiving datagram");
	int len;
#if defined(UU_UDP_RX_TRACE) && defined(__SWITCH__)
	while ((len = udp_recvfrom_traced(sock, buffer, size, src, sample)) == 0) {
#else
	while ((len = udp_recvfrom(sock, buffer, size, src)) == 0) {
#endif
		// Empty datagram, ignore
	}

	if (len < 0) {
		if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK) {
			JLOG_VERBOSE("No more datagrams to receive");
			return 0;
		}
		JLOG_ERROR("recvfrom failed, errno=%d", sockerrno);
		return -1;
	}

	addr_unmap_inet6_v4mapped((struct sockaddr *)&src->addr, &src->len);
	return len; // len > 0
}

#if defined(__SWITCH__) && defined(UU_SWITCH_BATCH_RX)
// The result column for UU_PACKET_UDP records the batch admission decision.
enum udp_batch_admission {
    UDP_BATCH_ACCEPTED = 0,
    UDP_BATCH_EMPTY = 1,
    UDP_BATCH_OVERSIZE = 2,
    UDP_BATCH_TRUNCATED = 3,
    UDP_BATCH_ADDRESS_TOO_LONG = 4,
};

static int conn_poll_batch_deliver(juice_agent_t *agent, conn_impl_t *conn_impl,
                                   char *payload, size_t len, const addr_record_t *source
#ifdef UU_UDP_RX_TRACE
                                   , uu_udp_rx_sample *sample, uint64_t read_end
#endif
) {
#ifdef UU_UDP_RX_TRACE
    if (sample->packets++ == 0)
        sample->first_read_us = read_end;
    sample->last_read_us = read_end;
    if (conn_impl->rx_last_read_us &&
        read_end - conn_impl->rx_last_read_us > sample->gap_to_us - sample->gap_from_us) {
        sample->gap_from_us = conn_impl->rx_last_read_us;
        sample->gap_to_us = read_end;
    }
    conn_impl->rx_last_read_us = read_end;
    const uint64_t dispatch_start = uu_udp_now_us();
#else
    (void)conn_impl;
#endif
    const int result = agent_conn_recv(agent, payload, len, source);
#ifdef UU_UDP_RX_TRACE
    const uint64_t dispatch_us = uu_udp_now_us() - dispatch_start;
    if (dispatch_us > sample->max_dispatch_us)
        sample->max_dispatch_us = dispatch_us;
#endif
    return result;
}

// Read only queued datagrams: nonblocking, bounded, no batching delay.
static int conn_poll_drain_batch(juice_agent_t *agent, conn_impl_t *conn_impl
#ifdef UU_UDP_RX_TRACE
                                 , uu_udp_rx_sample *sample
#endif
) {
    registry_impl_t *registry_impl = conn_impl->registry->impl;
    char (*payload)[BUFFER_SIZE] = registry_impl->batch_payload;
    addr_record_t sources[batch_size];
    struct mmsghdr messages[batch_size];
    struct iovec vectors[batch_size];
    int left = 1000;
    while (left > 0) {
        if (conn_impl->batch_disabled) {
            addr_record_t src;
            while (left-- > 0) {
                __atomic_fetch_add(&uu_recv_calls, 1, __ATOMIC_RELAXED);
#ifdef UU_UDP_RX_TRACE
                const uint64_t read_start = uu_udp_now_us();
#endif
                const int ret = conn_poll_recv(conn_impl->sock, payload[0], BUFFER_SIZE, &src
#ifdef UU_UDP_RX_TRACE
                                               , sample
#endif
                );
#ifdef UU_UDP_RX_TRACE
                const uint64_t read_end = uu_udp_now_us();
                const uint64_t elapsed = read_end - read_start;
                if (elapsed > sample->max_read_us)
                    sample->max_read_us = elapsed;
#endif
                if (ret <= 0)
                    return ret;
#ifdef UU_PACKET_STAGE_TRACE
                uu_packet_stage_record(UU_PACKET_UDP, uu_packet_read(payload[0], (size_t)ret), 0);
#endif
                __atomic_fetch_add(&uu_recv_messages, 1, __ATOMIC_RELAXED);
                if (conn_poll_batch_deliver(agent, conn_impl, payload[0], (size_t)ret, &src
#ifdef UU_UDP_RX_TRACE
                                            , sample, read_end
#endif
                ) != 0)
                    return -1;
            }
            return 0;
        }
        const unsigned count = left < batch_size ? (unsigned)left : batch_size;
        memset(messages, 0, sizeof(messages));
        memset(sources, 0, sizeof(sources));
        for (unsigned j = 0; j < count; ++j) {
            vectors[j].iov_base = payload[j];
            vectors[j].iov_len = BUFFER_SIZE;
            messages[j].msg_hdr.msg_name = &sources[j].addr;
            messages[j].msg_hdr.msg_namelen = sizeof(sources[j].addr);
            messages[j].msg_hdr.msg_iov = &vectors[j];
            messages[j].msg_hdr.msg_iovlen = 1;
        }
        struct timespec timeout = {0, 0};
        __atomic_fetch_add(&uu_recv_calls, 1, __ATOMIC_RELAXED);
#ifdef UU_UDP_RX_TRACE
        const uint64_t read_start = uu_udp_now_us();
#endif
#ifdef UU_SWITCH_BATCH_REUSE
        const int received = uu_recvmmsg_reuse(conn_impl->sock, messages, count, MSG_DONTWAIT,
                                              &timeout, &registry_impl->batch_scratch);
#else
        const int received = recvmmsg(conn_impl->sock, messages, count, MSG_DONTWAIT, &timeout);
#endif
        const int error = received < 0 ? sockerrno : 0;
#ifdef UU_UDP_RX_TRACE
        const uint64_t read_end = uu_udp_now_us();
        const uint64_t elapsed = read_end - read_start;
        if (elapsed > sample->max_read_us)
            sample->max_read_us = elapsed;
        if (elapsed >= 8000) {
            ++sample->recv_slow_calls;
            if (elapsed > sample->recv_end_us - sample->recv_start_us) {
                sample->recv_start_us = read_start;
                sample->recv_end_us = read_end;
                sample->recv_result = received;
                sample->recv_errno = error;
                sample->recv_batch = 1;
            }
        }
#endif
        if (received < 0) {
            if (error == SEAGAIN || error == SEWOULDBLOCK || error == SEINTR)
                return 0;
            __atomic_store_n(&uu_recv_batch_error, error, __ATOMIC_RELAXED);
            __atomic_fetch_add(&uu_recv_fallbacks, 1, __ATOMIC_RELAXED);
            conn_impl->batch_disabled = 1;
            JLOG_WARN("Batch receive unavailable, fallback to recvfrom, errno=%d", error);
            continue;
        }
        if (!received)
            return 0;
        __atomic_fetch_add(&uu_recv_messages, (uint64_t)received, __ATOMIC_RELAXED);
        if (received > 1)
            __atomic_fetch_add(&uu_recv_batches, 1, __ATOMIC_RELAXED);
        if ((uint64_t)received > __atomic_load_n(&uu_recv_max, __ATOMIC_RELAXED))
            __atomic_store_n(&uu_recv_max, (uint64_t)received, __ATOMIC_RELAXED);
        left -= received;
        for (int j = 0; j < received; ++j) {
            const size_t len = messages[j].msg_len;
            sources[j].len = messages[j].msg_hdr.msg_namelen;
            enum udp_batch_admission admission = UDP_BATCH_ACCEPTED;
            if (!len)
                admission = UDP_BATCH_EMPTY;
            else if (len > BUFFER_SIZE)
                admission = UDP_BATCH_OVERSIZE;
            else if (messages[j].msg_hdr.msg_flags & MSG_TRUNC)
                admission = UDP_BATCH_TRUNCATED;
            else if (sources[j].len > sizeof(sources[j].addr))
                admission = UDP_BATCH_ADDRESS_TOO_LONG;
#ifdef UU_PACKET_STAGE_TRACE
            const size_t captured_len = len > BUFFER_SIZE ? BUFFER_SIZE : len;
            uu_packet_stage_record(UU_PACKET_UDP, uu_packet_read(payload[j], captured_len), admission);
#endif
            if (admission != UDP_BATCH_ACCEPTED)
                continue;
            addr_unmap_inet6_v4mapped((struct sockaddr *)&sources[j].addr, &sources[j].len);
            if (conn_poll_batch_deliver(agent, conn_impl, payload[j], len, &sources[j]
#ifdef UU_UDP_RX_TRACE
                                        , sample, read_end
#endif
            ) != 0)
                return -1;
        }
    }
    return 0;
}
#endif

int conn_poll_process(conn_registry_t *registry, pfds_record_t *pfds) {
	struct pollfd *interrupt_pfd = pfds->pfds;
	if (interrupt_pfd->revents & POLLIN) {
#if defined(_WIN32) || defined(__SWITCH__)
		char dummy;
		addr_record_t src;
		while (udp_recvfrom(interrupt_pfd->fd, &dummy, 1, &src) >= 0) {
			// Ignore
		}
#else
		char dummy;
		while (read(interrupt_pfd->fd, &dummy, 1) > 0) {
			// Ignore
		}
#endif
	}

#ifdef UU_UDP_RX_TRACE
	const uint64_t lock_start = uu_udp_now_us();
#endif
	mutex_lock(&registry->mutex);
#ifdef UU_UDP_RX_TRACE
	const uint64_t lock_acquired = uu_udp_now_us();
#endif
	for (nfds_t i = 1; i < pfds->size; ++i) {
		struct pollfd *pfd = pfds->pfds + i;
		if (pfd->fd == INVALID_SOCKET)
			continue;

		juice_agent_t *agent = registry->agents[i - 1];
		if (!agent)
			continue;

		conn_impl_t *conn_impl = agent->conn_impl;
		if (!conn_impl || conn_impl->sock != pfd->fd || conn_impl->state != CONN_STATE_READY)
			continue;

		if (pfd->revents & POLLNVAL || pfd->revents & POLLERR) {
			JLOG_WARN("Error when polling socket");
			agent_conn_fail(agent);
			conn_impl->state = CONN_STATE_FINISHED;
			continue;
		}

		const bool readable = (pfd->revents & POLLIN) != 0;
#ifdef UU_UDP_RX_TRACE
		uu_udp_rx_sample sample = {0};
		sample.socket_id = (intptr_t)pfd->fd;
		sample.readable = readable;
		sample.poll_start_us = pfds->poll_start_us;
		sample.poll_return_us = pfds->poll_return_us;
		sample.lock_start_us = lock_start;
		sample.lock_acquired_us = lock_acquired;
#endif
		if (readable) {
#if defined(__SWITCH__) && defined(UU_SWITCH_BATCH_RX)
#ifdef UU_UDP_RX_TRACE
            sample.drain_start_us = uu_udp_now_us();
#endif
            const int batch_result = conn_poll_drain_batch(agent, conn_impl
#ifdef UU_UDP_RX_TRACE
                                                          , &sample
#endif
            );
#ifdef UU_UDP_RX_TRACE
            sample.drain_end_us = uu_udp_now_us();
            conn_impl->rx_packets += sample.packets;
            if (batch_result < 0 && sample.recv_slow_calls) {
                sample.socket_packets = conn_impl->rx_packets;
                sample.update_start_us = sample.drain_end_us;
                uu_udp_rx_record(&sample);
            }
#endif
            if (batch_result < 0) {
                agent_conn_fail(agent);
                conn_impl->state = CONN_STATE_FINISHED;
                continue;
            }
#else
			char buffer[BUFFER_SIZE];
			addr_record_t src;
			int ret = 0;
			int left = 1000; // limit for fairness between sockets
#ifdef UU_UDP_RX_TRACE
			uint64_t read_start = sample.drain_start_us = uu_udp_now_us();
#endif
			while (left--) {
				ret = conn_poll_recv(conn_impl->sock, buffer, BUFFER_SIZE, &src
#if defined(UU_UDP_RX_TRACE) && defined(__SWITCH__)
				                     , &sample
#endif
				);
#ifdef UU_UDP_RX_TRACE
				const uint64_t read_end = uu_udp_now_us();
				if (read_end - read_start > sample.max_read_us)
					sample.max_read_us = read_end - read_start;
#endif
				if (ret <= 0)
					break;
#ifdef UU_UDP_RX_TRACE
				if (sample.packets++ == 0)
					sample.first_read_us = read_end;
				sample.last_read_us = read_end;
				if (conn_impl->rx_last_read_us &&
				    read_end - conn_impl->rx_last_read_us > sample.gap_to_us - sample.gap_from_us) {
					sample.gap_from_us = conn_impl->rx_last_read_us;
					sample.gap_to_us = read_end;
				}
				conn_impl->rx_last_read_us = read_end;
#endif
#ifdef UU_PACKET_STAGE_TRACE
                uu_packet_stage_record(UU_PACKET_UDP, uu_packet_read(buffer, (size_t)ret), 0);
#endif
				const int received = agent_conn_recv(agent, buffer, (size_t)ret, &src);
#ifdef UU_UDP_RX_TRACE
				read_start = uu_udp_now_us();
				if (read_start - read_end > sample.max_dispatch_us)
					sample.max_dispatch_us = read_start - read_end;
#endif
				if (received != 0) {
					JLOG_WARN("Agent receive failed");
					conn_impl->state = CONN_STATE_FINISHED;
					break;
				}
			}
#ifdef UU_UDP_RX_TRACE
			sample.drain_end_us = uu_udp_now_us();
			conn_impl->rx_packets += sample.packets;
			if (sample.recv_slow_calls && (ret < 0 || conn_impl->state == CONN_STATE_FINISHED)) {
				sample.socket_packets = conn_impl->rx_packets;
				sample.update_start_us = sample.drain_end_us;
				uu_udp_rx_record(&sample);
			}
#endif
			if (conn_impl->state == CONN_STATE_FINISHED)
				continue;

			if (ret < 0) {
				agent_conn_fail(agent);
				conn_impl->state = CONN_STATE_FINISHED;
				continue;
			}

#endif
		}
		if (readable || conn_impl->next_timestamp <= current_timestamp()) {
#ifdef UU_UDP_RX_TRACE
			sample.socket_packets = conn_impl->rx_packets;
			sample.update_start_us = uu_udp_now_us();
#endif
			const int updated = agent_conn_update(agent, &conn_impl->next_timestamp);
#ifdef UU_UDP_RX_TRACE
			sample.update_us = uu_udp_now_us() - sample.update_start_us;
			uu_udp_rx_record(&sample);
#endif
			if (updated != 0) {
				JLOG_WARN("Agent update failed");
				conn_impl->state = CONN_STATE_FINISHED;
				continue;
			}
		}
	}
	mutex_unlock(&registry->mutex);
	return 0;
}

int conn_poll_run(conn_registry_t *registry) {
	pfds_record_t pfds;
	pfds.pfds = NULL;
	pfds.size = 0;
	timestamp_t next_timestamp = 0;
	int count;
	while ((count = conn_poll_prepare(registry, &pfds, &next_timestamp)) > 0) {
#ifdef __SWITCH__
		uu_cpu_mark(5);
#endif
		timediff_t timediff = next_timestamp - current_timestamp();
		if (timediff < 0)
			timediff = 0;

		JLOG_VERBOSE("Entering poll on %d sockets for %d ms", count, (int)timediff);
#ifdef UU_UDP_RX_TRACE
#if defined(__SWITCH__) && defined(UU_UDP_RX_CPU_TIMING)
		const uint64_t cpu_window_start = uu_udp_now_us();
		u64 cpu_before = 0;
		Result cpu_status = svcGetInfo(&cpu_before, InfoType_ThreadTickCount,
		                               threadGetCurHandle(), TickCountInfo_Total);
#endif
		pfds.poll_start_us = uu_udp_now_us();
#endif
		int ret = poll(pfds.pfds, pfds.size, (int)timediff);
#ifdef UU_UDP_RX_TRACE
		const int poll_error = ret < 0 ? sockerrno : 0;
		pfds.poll_return_us = uu_udp_now_us();
		if (pfds.poll_return_us - pfds.poll_start_us >= 50000) {
			uu_udp_rx_sample sample = {0};
#if defined(__SWITCH__) && defined(UU_UDP_RX_CPU_TIMING)
			u64 cpu_after = 0;
			if (R_SUCCEEDED(cpu_status))
				cpu_status = svcGetInfo(&cpu_after, InfoType_ThreadTickCount,
				                        threadGetCurHandle(), TickCountInfo_Total);
			sample.poll_cpu_window_us = uu_udp_now_us() - cpu_window_start;
			sample.poll_cpu_status = cpu_status;
			sample.poll_cpu_valid = R_SUCCEEDED(cpu_status) && cpu_after >= cpu_before;
			if (sample.poll_cpu_valid)
				sample.poll_cpu_us = armTicksToNs(cpu_after - cpu_before) / 1000;
#endif
			sample.poll_only = 1;
			sample.socket_id = -1;
			sample.poll_start_us = pfds.poll_start_us;
			sample.poll_return_us = sample.update_start_us = pfds.poll_return_us;
			sample.poll_timeout_ms = (int)timediff;
			sample.poll_result = ret;
			sample.poll_errno = poll_error;
			if (ret > 0) {
				sample.poll_interrupt_readable = (pfds.pfds[0].revents & POLLIN) != 0;
				for (nfds_t i = 1; i < pfds.size; ++i)
					if (pfds.pfds[i].revents & POLLIN)
						++sample.poll_peer_readable;
			}
			uu_udp_rx_record(&sample);
		}
		if (ret < 0) {
#ifdef _WIN32
			WSASetLastError(poll_error);
#else
			errno = poll_error;
#endif
		}
#endif
		JLOG_VERBOSE("Leaving poll");
		if (ret < 0) {
#if defined(_WIN32) || defined(__SWITCH__)
			if (sockerrno == WSAENOTSOCK)
				continue; // prepare again as the fd has been removed
#endif
			if (sockerrno == SEINTR || sockerrno == SEAGAIN) {
				JLOG_VERBOSE("poll interrupted");
				continue;
			} else {
				JLOG_FATAL("poll failed, errno=%d", sockerrno);
				break;
			}
		}

		if (conn_poll_process(registry, &pfds) < 0)
			break;
	}

	JLOG_DEBUG("Leaving connections thread");
	free(pfds.pfds);
	return 0;
}

int conn_poll_init(juice_agent_t *agent, conn_registry_t *registry, udp_socket_config_t *config) {
	conn_impl_t *conn_impl = calloc(1, sizeof(conn_impl_t));
	if (!conn_impl) {
		JLOG_FATAL("Memory allocation failed for connection impl");
		return -1;
	}

	conn_impl->sock = udp_create_socket(config);
	if (conn_impl->sock == INVALID_SOCKET) {
		JLOG_ERROR("UDP socket creation failed");
		free(conn_impl);
		return -1;
	}

#if defined(UU_DSCP_FAMILY_CACHE) || defined(UU_UDP_FAMILY_CACHE)
    conn_impl->socket_family=AF_UNSPEC;
    addr_record_t bound;
    if(udp_get_bound_addr(conn_impl->sock,&bound)==0)
        conn_impl->socket_family=bound.addr.ss_family;
#endif
#if defined(__SWITCH__) && defined(UU_SWITCH_BATCH_RX) && defined(UU_UDP_RX_TRACE)
    int receive_bytes = 0;
    socklen_t length = sizeof(receive_bytes);
    const int status = getsockopt(conn_impl->sock, SOL_SOCKET, SO_RCVBUF, (char *)&receive_bytes, &length);
    uu_udp_rx_socket_buffer((intptr_t)conn_impl->sock, status == 0 ? receive_bytes : -1, status == 0 ? 0 : sockerrno);
#endif
	mutex_init(&conn_impl->send_mutex, 0);
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING) && defined(UU_UDP_RX_TRACE)
    static uint64_t next_send_lock_id;
    // An ID belongs to one lock lifetime, even if a socket descriptor is reused.
    conn_impl->send_lock_id = __atomic_add_fetch(&next_send_lock_id, 1, __ATOMIC_RELAXED);
#endif
	conn_impl->registry = registry;

	agent->conn_impl = conn_impl;
	return 0;
}

void conn_poll_cleanup(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;

	conn_poll_interrupt(agent);

	mutex_destroy(&conn_impl->send_mutex);
	closesocket(conn_impl->sock);
	free(agent->conn_impl);
	agent->conn_impl = NULL;
}

void conn_poll_lock(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	conn_registry_t *registry = conn_impl->registry;
	mutex_lock(&registry->mutex);
}

void conn_poll_unlock(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	conn_registry_t *registry = conn_impl->registry;
	mutex_unlock(&registry->mutex);
}

int conn_poll_interrupt(juice_agent_t *agent) {
	conn_impl_t *conn_impl = agent->conn_impl;
	conn_registry_t *registry = conn_impl->registry;
	registry_impl_t *registry_impl = registry->impl;

	mutex_lock(&registry->mutex);
	conn_impl->next_timestamp = current_timestamp();
	mutex_unlock(&registry->mutex);

	JLOG_VERBOSE("Interrupting connections thread");

	char dummy = 0;
#if defined(_WIN32) || defined(__SWITCH__)
	if (udp_sendto_self(registry_impl->interrupt_sock, &dummy, 0) < 0) {
		if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK) {
			JLOG_WARN("Failed to interrupt poll by triggering socket, errno=%d", sockerrno);
		}
		return -1;
	}
#else
	if (write(registry_impl->interrupt_pipe_out, &dummy, 1) < 0 && errno != EAGAIN &&
	    errno != EWOULDBLOCK) {
		JLOG_WARN("Failed to interrupt poll by writing to pipe, errno=%d", errno);
	}
#endif
	return 0;
}

int conn_poll_send(juice_agent_t *agent, const addr_record_t *dst, const char *data, size_t size,
                   int ds) {
	conn_impl_t *conn_impl = agent->conn_impl;
#ifdef UU_POLL_FIXED_DSCP
    // Experimental socket-wide AF11. Keep the control channel's existing
    // priority, instead of switching socket options for each traffic class.
    ds = 10 << 2;
#endif

#ifdef UU_UDP_SEND_TIMING
    const uint64_t lock_start=uu_udp_now_us();
#endif
	mutex_lock(&conn_impl->send_mutex);
#ifdef UU_UDP_SEND_TIMING
    const uint64_t lock_acquired = uu_udp_now_us();
    uu_udp_record(UU_UDP_LOCK,lock_acquired-lock_start);
#endif

	if (conn_impl->send_ds >= 0 && conn_impl->send_ds != ds) {
		JLOG_VERBOSE("Setting Differentiated Services field to 0x%X", ds);
#ifdef UU_UDP_SEND_TIMING
        const uint64_t dscp_start=uu_udp_now_us();
#endif
#ifdef UU_DSCP_FAMILY_CACHE
        const int dscp_result=conn_impl->socket_family==AF_UNSPEC ?
            udp_set_diffserv(conn_impl->sock,ds) :
            udp_set_diffserv_family(conn_impl->sock,ds,conn_impl->socket_family);
#else
        const int dscp_result=udp_set_diffserv(conn_impl->sock,ds);
#endif
#ifdef UU_UDP_SEND_TIMING
        uu_udp_record(UU_UDP_DSCP,uu_udp_now_us()-dscp_start);
#endif
		if (dscp_result == 0)
			conn_impl->send_ds = ds;
		else
			conn_impl->send_ds = -1; // disable for next time
	}

#ifdef UU_POLL_FIXED_DSCP
    // All sends use the same socket priority. Serialize its setup only.
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING) && defined(UU_UDP_RX_TRACE)
    const uint64_t unlock_start = uu_udp_now_us();
#endif
    mutex_unlock(&conn_impl->send_mutex);
#endif

	JLOG_VERBOSE("Sending datagram, size=%d", size);

#ifdef UU_UDP_SEND_TIMING
    const uint64_t udp_start=uu_udp_now_us();
#endif
#ifdef UU_UDP_FAMILY_CACHE
    int ret=udp_sendto_family(conn_impl->sock,data,size,dst,conn_impl->socket_family);
#else
	int ret = udp_sendto(conn_impl->sock, data, size, dst);
#endif
#ifdef UU_UDP_SEND_TIMING
    const int saved_error=sockerrno;
    const uint64_t udp_end = uu_udp_now_us();
    uu_udp_record(UU_UDP_CALL,udp_end-udp_start);
#ifdef _WIN32
    WSASetLastError(saved_error);
#else
    errno=saved_error;
#endif
#endif
	if (ret < 0) {
		ret = -sockerrno;
		if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK)
			JLOG_INFO("Send failed, buffer is full");
		else if (sockerrno == SEMSGSIZE)
			JLOG_WARN("Send failed, datagram is too large");
		else
			JLOG_WARN("Send failed, errno=%d", sockerrno);
	}

#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING) && defined(UU_UDP_RX_TRACE)
    const bool slow = lock_acquired - lock_start >= UU_UDP_SEND_SLOW_US ||
                      udp_end - udp_start >= UU_UDP_SEND_SLOW_US;
    uu_udp_send_lock_sample lock_sample;
    if (slow) {
#ifndef UU_POLL_FIXED_DSCP
        const uint64_t unlock_start = uu_udp_now_us();
#endif
        lock_sample = (uu_udp_send_lock_sample){conn_impl->send_lock_id, lock_start, lock_acquired,
                                               udp_start, udp_end, unlock_start};
    }
#endif
#ifndef UU_POLL_FIXED_DSCP
	mutex_unlock(&conn_impl->send_mutex);
#endif
#if defined(__SWITCH__) && defined(UU_UDP_SEND_TIMING) && defined(UU_UDP_RX_TRACE)
    if (slow) {
        const int unlock_error = errno;
        uu_udp_send_lock_record(&lock_sample);
        errno = unlock_error;
    }
#endif
	return ret;
}

int conn_poll_get_addrs(juice_agent_t *agent, addr_record_t *records, size_t size) {
	conn_impl_t *conn_impl = agent->conn_impl;

	return udp_get_addrs(conn_impl->sock, records, size);
}
