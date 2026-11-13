#pragma once
#include <cstdint>

namespace uu {
// Process-wide cumulative transport wall times, not per-core CPU or wire delay.
struct DtlsRxDiagnostics {
    bool timingEnabled = false;
    bool bounded = false;
    uint64_t reads=0, yields=0, readTotalUs=0, readMaxUs=0;
    uint64_t sends=0, sendWaitTotalUs=0, sendWaitMaxUs=0, sendTotalUs=0, sendMaxUs=0;
    uint64_t inputAgeSamples=0,inputAgeMaxUs=0;
};
DtlsRxDiagnostics dtlsRxDiagnostics() noexcept;
}
