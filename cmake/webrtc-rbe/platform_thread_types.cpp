// Narrow platform API bridge. No thread creation, scheduling or affinity.
// Debug thread names are intentionally omitted; estimator math is upstream.
#include "rtc_base/platform_thread_types.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace rtc {
PlatformThreadId CurrentThreadId() {
#if defined(WEBRTC_WIN)
    return GetCurrentThreadId();
#elif defined(__SWITCH__)
    uint64_t id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return static_cast<PlatformThreadId>(id);
#else
    return getpid();
#endif
}
PlatformThreadRef CurrentThreadRef() {
#if defined(WEBRTC_WIN)
    return GetCurrentThreadId();
#else
    return pthread_self();
#endif
}
bool IsThreadRefEqual(const PlatformThreadRef& a, const PlatformThreadRef& b) {
#if defined(WEBRTC_WIN)
    return a == b;
#else
    return pthread_equal(a, b);
#endif
}
void SetCurrentThreadName(const char*) {}
}
