#pragma once
#include "receive_bandwidth_input.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace webrtc {
class Clock;
class RemoteBitrateEstimator;
class RemoteBitrateObserver;
} // namespace webrtc

namespace uu::rbe {
struct EstimatorCounters {
    uint64_t packets = 0, absSendTimePackets = 0;
    uint64_t notVideo = 0, transportSequence = 0, missingArrival = 0, malformed = 0, afterClose = 0;
    uint64_t switchesToAbsSendTime = 0, switchesToTransmissionOffset = 0;
    uint32_t packetsSinceAbsSendTime = 0;
    bool usingAbsSendTime = false, closed = false;
};
// Classic path of the pinned ReceiveSideCongestionController: starts with the
// transmission-offset estimator, switches to a NEW abs-send-time instance on the
// first AST packet, and rebuilds the offset estimator after 30 consecutive
// eligible packets without AST. Rebuilds do not carry estimator state.
// One instance per peer generation; the clock must share the arrival
// timestamps' monotonic microsecond domain. The observer is only called from
// incoming()/process() under the instance lock, so once close() returns this
// generation produces no further output.
class ReceiveBandwidthEstimator {
  public:
    static constexpr uint32_t TimeOffsetSwitchThreshold = 30;
    ReceiveBandwidthEstimator(webrtc::Clock &clock, webrtc::RemoteBitrateObserver &observer);
    ~ReceiveBandwidthEstimator();
    // Feeds Accepted packets; every other status is only counted.
    void incoming(const ReadResult &packet);
    // Microseconds until the next call; INT64_MAX when no deadline exists.
    int64_t process();
    void removeStream(uint32_t ssrc);
    void rttUpdate(int64_t averageMs, int64_t maxMs);
    // Removes every seen SSRC, releases the estimator and refuses later input.
    void close();
    // Zero while no estimate is available.
    int64_t latestEstimateBps() const;
    EstimatorCounters counters() const;
  private:
    void pickEstimator();
    webrtc::Clock &clock_;
    webrtc::RemoteBitrateObserver &observer_;
    mutable std::mutex mutex_;
    std::unique_ptr<webrtc::RemoteBitrateEstimator> estimator_;
    std::vector<uint32_t> ssrcs_;
    int64_t nextProcessUs_ = 0;
    EstimatorCounters counters_;
};
} // namespace uu::rbe
