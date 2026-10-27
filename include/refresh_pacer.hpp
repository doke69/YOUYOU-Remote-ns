#pragma once
#include <algorithm>

namespace uu {
// Scheduling only: the caller chooses how to sleep and whether pacing is on.
// A late refresh is never repaid with a burst of renders.
class RefreshPacer {
    static constexpr double periodUs = 1000000.0 / 60.0;
    double deadlineUs_ = 0;
  public:
    void reset() { deadlineUs_ = 0; }
    double remaining(double startedUs, double finishedUs) {
        if(deadlineUs_<=0 || finishedUs>deadlineUs_+periodUs)
            deadlineUs_=startedUs+periodUs;
        return std::max(0.0,deadlineUs_-finishedUs);
    }
    void advance(double wakeUs) {
        deadlineUs_+=periodUs;
        if(deadlineUs_<=wakeUs) deadlineUs_=wakeUs+periodUs;
    }
};
} // namespace uu
