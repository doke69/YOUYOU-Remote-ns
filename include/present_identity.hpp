#pragma once
#include <cstdint>

namespace uu {

// Tracks which video identity has actually been submitted/presented on each
// backend. A successful upload is not enough; the caller passes draw/submit
// evidence. Repeated presentation of the same identity is not a new frame.
struct PresentIdentity {
    uint64_t lastGpuSerial=0;
    uint64_t lastSdlSerial=0;

    void reset() noexcept { lastGpuSerial=lastSdlSerial=0; }

    bool gpuSubmitted(uint64_t serial) noexcept {
        if(!serial || serial==lastGpuSerial)return false;
        lastGpuSerial=serial;
        return true;
    }
    bool sdlPresented(bool drawn,uint64_t serial) noexcept {
        if(!drawn || !serial || serial==lastSdlSerial)return false;
        lastSdlSerial=serial;
        return true;
    }
};

} // namespace uu
