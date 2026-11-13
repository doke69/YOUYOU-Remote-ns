/*
Copyright 2017-2018 libnx Authors

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.*/
// Adapted from libnx v4.12.0 nx/source/runtime/devices/socket.c.
// Source SHA256: 090e8e40f843e2cfd6f9614229a946774ddb713a0b79456ffeb584ab60cbf1ae
// Only the private recvmmsg scratch allocation lifetime differs. Wire format,
// full zeroing, fd validation, errno conversion and receive semantics are kept.
#include "switch_batch_rx.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/uio.h>
#include <switch/services/bsd.h>
#include <switch/runtime/hosversion.h>

// The pinned libnx exports these functions, used by its own socket adapter.
int _convert_errno(int bsdErrno);
void *__libnx_aligned_alloc(size_t alignment, size_t size);
void __libnx_free(void *ptr);

static uint64_t reuse_allocations, reuse_frees, reuse_live_bytes;

void uu_batch_rx_diagnostics(uint64_t *allocations, uint64_t *frees, uint64_t *live_bytes) {
    *allocations = __atomic_load_n(&reuse_allocations, __ATOMIC_RELAXED);
    *frees = __atomic_load_n(&reuse_frees, __ATOMIC_RELAXED);
    *live_bytes = __atomic_load_n(&reuse_live_bytes, __ATOMIC_RELAXED);
}

void uu_recvmmsg_release(uu_batch_scratch *scratch) {
    if (scratch->data) {
        __libnx_free(scratch->data);
        __atomic_fetch_add(&reuse_frees, 1, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&reuse_live_bytes, scratch->capacity, __ATOMIC_RELAXED);
        scratch->data = NULL;
        scratch->capacity = 0;
    }
}

static int _socketGetFd(int fd) {
    __handle *handle = __get_handle(fd);
    if(handle == NULL) {
        errno = EBADF;
        return -1;
    }
    if(strcmp(devoptab_list[handle->device]->name, "soc") != 0) {
        errno = ENOTSOCK;
        return -1;
    }
    return *(int *)handle->fileStruct;
}

static int _socketParseBsdResult(struct _reent *r, int ret) {
    int errno_;
    if(ret != -1)
        return ret; // Nothing to do
    else {
        if(g_bsdErrno == -1) {
            // We're using -1 to signal Switch error codes.
            // Note: all of the bsd:u/s handlers return 0.
            switch(g_bsdResult) {
                case 0xD201:
                    errno_ = ENFILE;
                    break;
                case 0xD401:
                    errno_ = EFAULT;
                    break;
                case 0x10601:
                    errno_ = EBUSY;
                    break;
                default:
                    errno_ = EPIPE;
                    break;
            }
        }
        else
            errno_ = _convert_errno(g_bsdErrno); /* Nintendo actually used the Linux errno definitions for their FreeBSD build :)
                                                    but we still need to convert to newlib errno */
    }

    if(r == NULL)
        errno = errno_;
    else
        r->_errno = errno_;

    return -1;
}

static int _serializeMmsg(u8 *outbuf, size_t outbuf_size, struct mmsghdr *msgvec, unsigned int vlen, bool is_send) {
    void* tmp_ptr = NULL;
    u8 *dataptr = outbuf;
    *dataptr++ = 0x8;

    for (unsigned int i=0; i<vlen; i++) {
        socklen_t msg_controllen = msgvec[i].msg_hdr.msg_controllen;

        if (msg_controllen >= sizeof(struct cmsghdr)) {
            struct cmsghdr *cmsg = msgvec[i].msg_hdr.msg_control;
            if (cmsg) {
                if (cmsg->cmsg_level == 0xffff && cmsg->cmsg_type == 1) {
                    errno = EOPNOTSUPP;
                    return -1;
                }
            }
        }

        socklen_t msg_namelen = msgvec[i].msg_hdr.msg_namelen;
        *((socklen_t*)dataptr) = msg_namelen;
        dataptr+= sizeof(socklen_t);

        if (is_send && (tmp_ptr = msgvec[i].msg_hdr.msg_name)) memcpy(dataptr, tmp_ptr, msg_namelen);
        dataptr+= msg_namelen;

        int msg_iovlen = msgvec[i].msg_hdr.msg_iovlen;
        *((int*)dataptr) = msg_iovlen;
        dataptr+= sizeof(int);

        for (int veci=0; veci<msg_iovlen; veci++) {
            struct iovec *vec = &msgvec[i].msg_hdr.msg_iov[veci];

            u64 iov_len = vec->iov_len;
            *((u64*)dataptr) = iov_len;
            dataptr+= sizeof(u64);

            if (is_send) memcpy(dataptr, vec->iov_base, iov_len);
            dataptr+= iov_len;
        }

        *((socklen_t*)dataptr) = msg_controllen;
        dataptr+= sizeof(socklen_t);

        if (is_send && (tmp_ptr = msgvec[i].msg_hdr.msg_control)) memcpy(dataptr, tmp_ptr, msg_controllen);
        dataptr+= msg_controllen;

        *((int*)dataptr) = msgvec[i].msg_hdr.msg_flags;
        dataptr+= sizeof(int);

        *((int*)dataptr) = msgvec[i].msg_len;
        dataptr+= sizeof(int);
    }

    // sdknso would verify that dataptr isn't larger than outbuf+outbuf_size (Abort otherwise), but that can't happen anyway since the caller allocates enough space.
    return (uintptr_t)dataptr-(uintptr_t)outbuf;
}

static int _deserializeMmsg(struct mmsghdr *msgvec, unsigned int vlen, u8 *inbuf, size_t inbuf_size, bool is_recv) {
    bool bounds_flag=0;
    void* tmp_ptr = NULL;
    u8 *dataptr = &inbuf[0x1];
    uintptr_t inbuf_end = (uintptr_t)&inbuf[inbuf_size];

    // sdknso verifies that dataptr isn't larger than outbuf+outbuf_size at the end prior to returning (Abort otherwise). We'll also verify it during the loop, and also verify that sizes from the buffer are not larger than the original msgvec values.

    for (unsigned int i=0; i<vlen; i++) {
        if ((uintptr_t)dataptr > (uintptr_t)inbuf_end || (uintptr_t)dataptr+sizeof(socklen_t) > inbuf_end) {
            bounds_flag = 1;
            break;
        }

        socklen_t msg_namelen = *((socklen_t*)dataptr);
        dataptr+= sizeof(socklen_t);

        if ((uintptr_t)dataptr+msg_namelen > inbuf_end || msg_namelen > msgvec[i].msg_hdr.msg_namelen) {
            bounds_flag = 1;
            break;
        }
        msgvec[i].msg_hdr.msg_namelen = msg_namelen;

        if (is_recv && (tmp_ptr = msgvec[i].msg_hdr.msg_name)) {
            memcpy(tmp_ptr, dataptr, msg_namelen);
        }
        dataptr+= msg_namelen;

        if ((uintptr_t)dataptr+sizeof(int) > inbuf_end) {
            bounds_flag = 1;
            break;
        }

        int msg_iovlen = *((int*)dataptr);
        dataptr+= sizeof(int);

        if (msg_iovlen > msgvec[i].msg_hdr.msg_iovlen) {
            bounds_flag = 1;
            break;
        }

        msgvec[i].msg_hdr.msg_iovlen = msg_iovlen;

        for (int veci=0; veci<msg_iovlen; veci++) {
            struct iovec *vec = &msgvec[i].msg_hdr.msg_iov[veci];

            if ((uintptr_t)dataptr+sizeof(u64) > inbuf_end) {
                bounds_flag = 1;
                break;
            }

            u64 iov_len = *((u64*)dataptr);
            dataptr+= sizeof(u64);
            if (iov_len > inbuf_size || (uintptr_t)dataptr+iov_len > inbuf_end || iov_len > vec->iov_len) {
                bounds_flag = 1;
                break;
            }

            vec->iov_len = iov_len;
            if (is_recv) memcpy(vec->iov_base, dataptr, iov_len);
            dataptr+= iov_len;
        }

        if (bounds_flag) break;

        if ((uintptr_t)dataptr+sizeof(socklen_t) > inbuf_end) {
            bounds_flag = 1;
            break;
        }

        socklen_t msg_controllen = *((socklen_t*)dataptr);
        dataptr+= sizeof(socklen_t);

        if ((uintptr_t)dataptr+msg_controllen > inbuf_end || msg_controllen > msgvec[i].msg_hdr.msg_controllen) {
            bounds_flag = 1;
            break;
        }

        msgvec[i].msg_hdr.msg_controllen = msg_controllen;

        if (is_recv && (tmp_ptr = msgvec[i].msg_hdr.msg_control)) {
            memcpy(tmp_ptr, dataptr, msg_controllen);
        }
        dataptr+= msg_controllen;

        if ((uintptr_t)dataptr+sizeof(int) > inbuf_end) {
            bounds_flag = 1;
            break;
        }

        msgvec[i].msg_hdr.msg_flags = *((int*)dataptr);
        dataptr+= sizeof(int);

        if ((uintptr_t)dataptr+sizeof(int) > inbuf_end) {
            bounds_flag = 1;
            break;
        }

        msgvec[i].msg_len = *((int*)dataptr);
        dataptr+= sizeof(int);

        if (msg_controllen >= sizeof(struct cmsghdr)) {
            struct cmsghdr *cmsg = msgvec[i].msg_hdr.msg_control;
            if (cmsg) {
                if (cmsg->cmsg_level == 0xffff && cmsg->cmsg_type == 1) {
                    errno = EOPNOTSUPP;
                    return -1;
                }
            }
        }
    }

    if (bounds_flag || (uintptr_t)dataptr > inbuf_end) {
        errno = EFAULT;
        return -1;
    }

    return (uintptr_t)dataptr-(uintptr_t)inbuf;
}

static int _mmsgInitCommon(u8 **buf, size_t *alignsize, struct mmsghdr *msgvec, unsigned int vlen, uu_batch_scratch *scratch) {
    size_t msgdatasize_total = 0;
    size_t bufsize = 1;

    for (unsigned int i=0; i<vlen; i++) {
        bufsize+= sizeof(socklen_t) + msgvec[i].msg_hdr.msg_namelen + sizeof(int);

        int msg_iovlen = msgvec[i].msg_hdr.msg_iovlen;

        for (int veci=0; veci<msg_iovlen; veci++) {
            size_t iov_len = msgvec[i].msg_hdr.msg_iov[veci].iov_len;
            bufsize += sizeof(u64) + iov_len;
            msgdatasize_total += iov_len;
        }

        bufsize+= sizeof(socklen_t) + msgvec[i].msg_hdr.msg_controllen + sizeof(int) + sizeof(int);
    }

    if (msgdatasize_total > 0x80000) {
        errno = EMSGSIZE;
        return -1;
    }

    *alignsize = (bufsize+0xfff) & ~0xfff;
    if (scratch->capacity < *alignsize) {
        u8 *replacement = (u8*)__libnx_aligned_alloc(0x1000, *alignsize);
        if (replacement == NULL) {
            errno = ENOMEM;
            return -1;
        }
        // Allocate first: failure leaves the previous buffer owned and valid.
        uu_recvmmsg_release(scratch);
        scratch->data = replacement;
        scratch->capacity = *alignsize;
        __atomic_fetch_add(&reuse_allocations, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&reuse_live_bytes, *alignsize, __ATOMIC_RELAXED);
    }
    *buf = scratch->data;
    memset(*buf, 0, *alignsize);

    return 0;
}

int uu_recvmmsg_reuse(int sockfd, struct mmsghdr *msgvec, unsigned int vlen, int flags, struct timespec *timeout, uu_batch_scratch *scratch) {
    if (hosversionBefore(7,0,0)) { // This cmd was added with [3.0.0+], but we'll only support the updated [7.0.0+] version of it.
        errno = ENOSYS;
        return -1;
    }

    if(msgvec == NULL || (vlen < 1 || vlen > 0x20)) {
        errno = EINVAL;
        return -1;
    }

    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;

    int ret=0, ret2=0;
    u8 *buf = NULL;
    size_t alignsize=0;

    ret = _mmsgInitCommon(&buf, &alignsize, msgvec, vlen, scratch);
    if (ret==-1) return ret;

    struct timespec tmp_timeout={0};
    if (timeout) tmp_timeout = *timeout;

    ret = _serializeMmsg(buf, alignsize, msgvec, vlen, 0);

    if (ret>=0) ret = _socketParseBsdResult(NULL, bsdRecvMMsg(sockfd, buf, alignsize, vlen, flags, &tmp_timeout));

    if (ret>=0 && ret>vlen) { // sdknso doesn't check this, but we will.
        errno = EFAULT;
        ret = -1;
    }

    if (ret>=0) {
        ret2 = _deserializeMmsg(msgvec, ret, buf, alignsize, 1);
        if (ret2==-1) ret = ret2;
    }

    // Retained by the poll thread owner until registry cleanup.

    return ret;
}
