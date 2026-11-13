#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace uu {
// Intermediate stick positions may be replaced. Button/trigger transitions,
// player changes and intervening commands remain FIFO barriers.
struct InputMotionKey {
    int player = -1;
    uint64_t buttons = 0;
    int leftTrigger = 0, rightTrigger = 0;
    bool operator==(const InputMotionKey &other) const {
        return player == other.player && buttons == other.buttons &&
               leftTrigger == other.leftTrigger && rightTrigger == other.rightTrigger;
    }
};
// Admission is published under the Inbox mutex. UI threads never read the
// worker-only generation_ directly; they capture this value while enqueuing.
struct InputAdmission {
    bool open = false;
    uint64_t generation = 0;
    uint64_t motionCoalesced = 0;
    size_t queueHighWater = 0;
};

inline bool isPeerInputCommand(const std::string &name) {
    // Short-lived remote actions. Login/refresh/connect/local config stay
    // generation 0 and are intentionally not gated here.
    return name == "input" || name == "release" || name == "text-input" ||
           name == "text-input-cancel" || name == "system-action" || name == "privacy-screen" || name == "cursor-capture";
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
bool tryEnqueuePeerInput(std::mutex &mutex, InputAdmission &admission, Queue &queue, Event event,
                         bool coalesceMotion = false) {
    std::lock_guard lock(mutex);
    if (!admission.open) return false;
    event.generation = admission.generation;
    if (coalesceMotion && event.motion.player >= 0 && !queue.empty() &&
        queue.back().generation == event.generation && queue.back().motion == event.motion) {
        queue.back() = std::move(event);
        ++admission.motionCoalesced;
        return true;
    }
    queue.push_back(std::move(event));
    admission.queueHighWater = std::max(admission.queueHighWater, queue.size());
    return true;
}
} // namespace uu
