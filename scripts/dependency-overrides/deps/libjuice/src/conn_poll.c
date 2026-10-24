/**
 * Copyright (c) 2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "conn_poll.h"
#include "agent.h"
#include "log.h"
#include "socket.h"
#include "thread.h"
#include "udp.h"

#include <assert.h>
#include <string.h>
#ifdef __SWITCH__
#define WSAENOTSOCK ENOTSOCK
#include <sys/uio.h>
// Implemented by the application; marks the single ICE/UDP poll thread so its
// CPU time can be sampled without assuming it shares the RTC worker pool.
void uu_cpu_mark(unsigned role);
#endif

#define BUFFER_SIZE 4096

typedef struct registry_impl {
	thread_t thread;
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
#ifdef __SWITCH__
    int batch_disabled;
#endif
	mutex_t send_mutex;
	int send_ds;
	timestamp_t next_timestamp;
} conn_impl_t;

typedef struct pfds_record {
	struct pollfd *pfds;
	nfds_t size;
} pfds_record_t;

int conn_poll_prepare(conn_registry_t *registry, pfds_record_t *pfds, timestamp_t *next_timestamp);
int conn_poll_process(conn_registry_t *registry, pfds_record_t *pfds);
int conn_poll_recv(socket_t sock, char *buffer, size_t size, addr_record_t *src);
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

int conn_poll_recv(socket_t sock, char *buffer, size_t size, addr_record_t *src) {
	JLOG_VERBOSE("Receiving datagram");
	int len;
	while ((len = udp_recvfrom(sock, buffer, size, src)) == 0) {
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
// Read only queued datagrams: nonblocking, bounded, no batching delay.
static int conn_poll_drain_batch(juice_agent_t *agent,conn_impl_t *conn_impl) {
    enum { batch_size=8 };
    char payload[batch_size][BUFFER_SIZE];
    addr_record_t sources[batch_size];
    struct mmsghdr messages[batch_size];
    struct iovec vectors[batch_size];
    int left=1000;
    while(left>0) {
        if(conn_impl->batch_disabled) {
            addr_record_t src;
            while(left-->0) {
                __atomic_fetch_add(&uu_recv_calls,1,__ATOMIC_RELAXED);
                const int ret=conn_poll_recv(conn_impl->sock,payload[0],BUFFER_SIZE,&src);
                if(ret<=0) return ret;
                __atomic_fetch_add(&uu_recv_messages,1,__ATOMIC_RELAXED);
                if(agent_conn_recv(agent,payload[0],(size_t)ret,&src)!=0) return -1;
            }
            return 0;
        }
        const unsigned count=left<batch_size?(unsigned)left:batch_size;
        memset(messages,0,sizeof(messages));
        memset(sources,0,sizeof(sources));
        for(unsigned j=0;j<count;++j) {
            vectors[j].iov_base=payload[j];vectors[j].iov_len=BUFFER_SIZE;
            messages[j].msg_hdr.msg_name=&sources[j].addr;
            messages[j].msg_hdr.msg_namelen=sizeof(sources[j].addr);
            messages[j].msg_hdr.msg_iov=&vectors[j];messages[j].msg_hdr.msg_iovlen=1;
        }
        struct timespec timeout={0,0};
        __atomic_fetch_add(&uu_recv_calls,1,__ATOMIC_RELAXED);
        int received=recvmmsg(conn_impl->sock,messages,count,MSG_DONTWAIT,&timeout);
        if(received<0) {
            const int error=sockerrno;
            if(error==SEAGAIN || error==SEWOULDBLOCK) return 0;
            if(error==SEINTR) return 0;
            __atomic_store_n(&uu_recv_batch_error,error,__ATOMIC_RELAXED);
            __atomic_fetch_add(&uu_recv_fallbacks,1,__ATOMIC_RELAXED);
            conn_impl->batch_disabled=1;
            JLOG_WARN("Batch receive unavailable, fallback to recvfrom, errno=%d",error);
            continue;
        }
        if(!received) return 0;
        __atomic_fetch_add(&uu_recv_messages,(uint64_t)received,__ATOMIC_RELAXED);
        if(received>1) __atomic_fetch_add(&uu_recv_batches,1,__ATOMIC_RELAXED);
        if((uint64_t)received>__atomic_load_n(&uu_recv_max,__ATOMIC_RELAXED))
            __atomic_store_n(&uu_recv_max,(uint64_t)received,__ATOMIC_RELAXED);
        left-=received;
        for(int j=0;j<received;++j) {
            const ssize_t len=messages[j].msg_len;
            if(len<=0 || len>BUFFER_SIZE || (messages[j].msg_hdr.msg_flags&MSG_TRUNC)) continue;
            sources[j].len=messages[j].msg_hdr.msg_namelen;
            if(sources[j].len>sizeof(sources[j].addr)) continue;
            addr_unmap_inet6_v4mapped((struct sockaddr *)&sources[j].addr,&sources[j].len);
            if(agent_conn_recv(agent,payload[j],(size_t)len,&sources[j])!=0) return -1;
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

	mutex_lock(&registry->mutex);
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

		if (pfd->revents & POLLIN) {
#if defined(__SWITCH__) && defined(UU_SWITCH_BATCH_RX)
            if(conn_poll_drain_batch(agent,conn_impl)<0) {
                agent_conn_fail(agent);conn_impl->state=CONN_STATE_FINISHED;continue;
            }
#else
			char buffer[BUFFER_SIZE];
			addr_record_t src;
			int ret = 0;
			int left = 1000; // limit for fairness between sockets
			while (left-- &&
			       (ret = conn_poll_recv(conn_impl->sock, buffer, BUFFER_SIZE, &src)) > 0) {
				if (agent_conn_recv(agent, buffer, (size_t)ret, &src) != 0) {
					JLOG_WARN("Agent receive failed");
					conn_impl->state = CONN_STATE_FINISHED;
					break;
				}
			}
			if (conn_impl->state == CONN_STATE_FINISHED)
				continue;

			if (ret < 0) {
				agent_conn_fail(agent);
				conn_impl->state = CONN_STATE_FINISHED;
				continue;
			}

#endif

			if (agent_conn_update(agent, &conn_impl->next_timestamp) != 0) {
				JLOG_WARN("Agent update failed");
				conn_impl->state = CONN_STATE_FINISHED;
				continue;
			}

		} else if (conn_impl->next_timestamp <= current_timestamp()) {
			if (agent_conn_update(agent, &conn_impl->next_timestamp) != 0) {
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
		int ret = poll(pfds.pfds, pfds.size, (int)timediff);
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

	mutex_init(&conn_impl->send_mutex, 0);
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

	mutex_lock(&conn_impl->send_mutex);

	if (conn_impl->send_ds >= 0 && conn_impl->send_ds != ds) {
		JLOG_VERBOSE("Setting Differentiated Services field to 0x%X", ds);
		if (udp_set_diffserv(conn_impl->sock, ds) == 0)
			conn_impl->send_ds = ds;
		else
			conn_impl->send_ds = -1; // disable for next time
	}

	JLOG_VERBOSE("Sending datagram, size=%d", size);

	int ret = udp_sendto(conn_impl->sock, data, size, dst);
	if (ret < 0) {
		ret = -sockerrno;
		if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK)
			JLOG_INFO("Send failed, buffer is full");
		else if (sockerrno == SEMSGSIZE)
			JLOG_WARN("Send failed, datagram is too large");
		else
			JLOG_WARN("Send failed, errno=%d", sockerrno);
	}

	mutex_unlock(&conn_impl->send_mutex);
	return ret;
}

int conn_poll_get_addrs(juice_agent_t *agent, addr_record_t *records, size_t size) {
	conn_impl_t *conn_impl = agent->conn_impl;

	return udp_get_addrs(conn_impl->sock, records, size);
}
