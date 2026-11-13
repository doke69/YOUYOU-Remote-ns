// Host-only mock of the pinned Horizon ABI; no network or IPC is performed.
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <malloc.h>
#include <vector>
#include <ctime>
using u8=uint8_t; using u64=uint64_t; using socklen_t=uint32_t;
struct iovec {void *iov_base; size_t iov_len;};
struct msghdr {void *msg_name; socklen_t msg_namelen; iovec *msg_iov; int msg_iovlen;
    void *msg_control; socklen_t msg_controllen; int msg_flags;};
struct mmsghdr {msghdr msg_hdr; unsigned msg_len;};
struct cmsghdr {socklen_t cmsg_len; int cmsg_level,cmsg_type;};
static_assert(sizeof(mmsghdr)==56 && sizeof(iovec)==16 && sizeof(cmsghdr)==12);
struct uu_batch_scratch {unsigned char *data=nullptr; size_t capacity=0;};
struct _reent {int _errno;};
struct __handle {unsigned device; void *fileStruct;};
struct devoptab_t {const char *name;};
static devoptab_t socDevice{"soc"}, otherDevice{"other"};
static devoptab_t *devoptab_list[]={&socDevice,&otherDevice};
static int bsdFd=73;
static __handle socketHandle{0,&bsdFd}, otherHandle{1,&bsdFd};
__handle *__get_handle(int fd) {return fd==3?&socketHandle:fd==4?&otherHandle:nullptr;}
static int g_bsdErrno=0;
static uint32_t g_bsdResult=0;
// The product links the SDK's actual exported converter. Use a distinguishable
// mock here to prove that both adapters call it rather than passing BSD errno on.
int _convert_errno(int value) {return value==9001?EAGAIN:value==9002?EINTR:EIO;}
static size_t allocations=0, frees=0;
static bool failAllocation=false, oldFirmware=false;
void *__libnx_aligned_alloc(size_t alignment,size_t n) {
    if(failAllocation){failAllocation=false;return nullptr;}
    ++allocations; return _aligned_malloc(n,alignment);
}
void __libnx_free(void *p) {if(p)++frees; _aligned_free(p);}
bool hosversionBefore(int,int,int) {return oldFirmware;}
int bsdRecvMMsg(int,void*,size_t,unsigned,int,timespec*);
