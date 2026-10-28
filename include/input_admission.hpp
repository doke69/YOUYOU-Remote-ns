#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace uu {
// Admission is published under the Inbox mutex. UI threads never read the
// worker-only generation_ directly; they capture this value while enqueuing.
struct InputAdmission {
    bool open = false;
    uint64_t generation = 0;
};

inline bool isPeerInputCommand(const std::string &name) {
    // Short-lived remote actions. Login/refresh/connect/local config stay
    // generation 0 and are intentionally not gated here.
    return name == "input" || name == "release" || name == "text-input" ||
           name == "text-input-cancel" || name == "system-action";
}

inline void openInputAdmission(InputAdmission &admission, uint64_t generation) {
    admission.open = true;
    admission.generation = generation;
}

inline void closeInputAdmission(InputAdmission &admission) {
    admission.open = false;
    admission.generation = 0;
}

// Locks the same mutex used by the Inbox. The event keeps the peer
// generation that was admissible at enqueue time; drain still drops stale ones.
template <typename Queue, typename Event>
bool tryEnqueuePeerInput(std::mutex &mutex, InputAdmission &admission, Queue &queue, Event event) {
    std::lock_guard lock(mutex);
    if (!admission.open) return false;
    event.generation = admission.generation;
    queue.push_back(std::move(event));
    return true;
}
} // namespace uu
