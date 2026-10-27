#pragma once
#include <cstddef>
#include <cstdint>

namespace uu {
struct PlayoutDecision {
    bool available = false;
    std::size_t discard = 0;
};

// One eligible FIFO frame per display poll. Only discard enough overdue frames
// to get back within the lateness budget; never select a future frame or flush
// all eligible frames just because more than one arrived together.
template<class Queue>
PlayoutDecision choosePlayout(const Queue &frames, int64_t now, int64_t maxLatenessMs=50) {
    if (frames.empty() || frames.front().dueMs > now) return {};
    PlayoutDecision result{true, 0};
    if(maxLatenessMs<0)maxLatenessMs=0;
    while (result.discard + 1 < frames.size() &&
           now-frames[result.discard].dueMs > maxLatenessMs &&
           frames[result.discard + 1].dueMs <= now)
        ++result.discard;
    return result;
}
} // namespace uu
