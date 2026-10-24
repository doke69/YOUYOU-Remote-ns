#pragma once
#include "stream_stats.hpp"
#include <memory>
namespace uu {
struct DecodeStageSample {double sendUs=0, receiveUs=0, publishUs=0;};
// Diagnostic-only UDP output. No inbound socket commands or protocol payloads.
class LiveTelemetry {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    LiveTelemetry() noexcept;
    ~LiveTelemetry();
    // True only when telemetry.ini configured a valid UDP diagnostic target.
    bool enabled() const noexcept;
    bool observe(uint32_t ticks, bool streaming, bool submitted, double uploadUs,
                 double workUs=0, double presentUs=0, double loopUs=0,
                 bool activeUi=false, bool statsFlag=false) noexcept;
    void publish(const StreamStatsSnapshot &, bool vsync, bool nv12,
                 double refreshMs, unsigned stableSwaps, bool nativeDisplay=false, uint64_t directFrames=0, uint64_t mapHits=0, uint64_t mapImports=0, uint64_t mapCached=0) noexcept;
    // Diagnostic stage timings (schema 11, microseconds, EMA) written by the
    // decode thread and read by publish(). Diagnostic-only; no behaviour change.
    static void setDecodeStages(double sendUs, double recvUs, double publishUs) noexcept;
    static void clearDecodeStages() noexcept;
    // Independent EMAs from existing timers; their sum is approximate, not a
    // new definition of hardware time. Works without configuring UDP telemetry.
    static DecodeStageSample decodeStages() noexcept;
};
}
