#include "receive_bandwidth_estimator.hpp"
#include "api/rtp_headers.h"
#include "api/units/timestamp.h"
#include "modules/remote_bitrate_estimator/remote_bitrate_estimator_abs_send_time.h"
#include "modules/remote_bitrate_estimator/remote_bitrate_estimator_single_stream.h"
#include <algorithm>
#include <limits>

namespace uu::rbe {
ReceiveBandwidthEstimator::ReceiveBandwidthEstimator(webrtc::Clock &clock, webrtc::RemoteBitrateObserver &observer)
    : clock_(clock), observer_(observer),
      estimator_(std::make_unique<webrtc::RemoteBitrateEstimatorSingleStream>(&observer_, &clock_)) {}

ReceiveBandwidthEstimator::~ReceiveBandwidthEstimator() = default;

void ReceiveBandwidthEstimator::pickEstimator() {
    // AST has no periodic deadline; switching back to TOF must wake Process.
    nextProcessUs_ = 0;
    if (counters_.usingAbsSendTime)
        estimator_ = std::make_unique<webrtc::RemoteBitrateEstimatorAbsSendTime>(&observer_, &clock_);
    else
        estimator_ = std::make_unique<webrtc::RemoteBitrateEstimatorSingleStream>(&observer_, &clock_);
}

void ReceiveBandwidthEstimator::incoming(const ReadResult &result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counters_.closed) {
        ++counters_.afterClose;
        return;
    }
    switch (result.status) {
    case PacketStatus::Accepted: break;
    case PacketStatus::NotVideo: ++counters_.notVideo; return;
    case PacketStatus::TransportSequence: ++counters_.transportSequence; return;
    case PacketStatus::MissingArrival: ++counters_.missingArrival; return;
    case PacketStatus::Malformed: ++counters_.malformed; return;
    }
    const auto &packet = result.packet;
    webrtc::RTPHeader header;
    header.payloadType = packet.payloadType;
    header.timestamp = packet.rtpTimestamp;
    header.ssrc = packet.ssrc;
    header.extension.hasAbsoluteSendTime = packet.absSendTime.has_value();
    header.extension.absoluteSendTime = packet.absSendTime.value_or(0);
    header.extension.hasTransmissionTimeOffset = packet.transmissionOffset.has_value();
    header.extension.transmissionTimeOffset = packet.transmissionOffset.value_or(0);
    const int64_t arrivalMs = webrtc::Timestamp::Micros(packet.arrivalUs).ms();

    ++counters_.packets;
    if (std::find(ssrcs_.begin(), ssrcs_.end(), packet.ssrc) == ssrcs_.end()) ssrcs_.push_back(packet.ssrc);
    if (header.extension.hasAbsoluteSendTime) {
        ++counters_.absSendTimePackets;
        if (!counters_.usingAbsSendTime) {
            counters_.usingAbsSendTime = true;
            ++counters_.switchesToAbsSendTime;
            pickEstimator();
        }
        counters_.packetsSinceAbsSendTime = 0;
    } else if (counters_.usingAbsSendTime &&
               ++counters_.packetsSinceAbsSendTime >= TimeOffsetSwitchThreshold) {
        counters_.usingAbsSendTime = false;
        ++counters_.switchesToTransmissionOffset;
        pickEstimator();
    }
    estimator_->IncomingPacket(arrivalMs, packet.payloadBytes, header);
}

int64_t ReceiveBandwidthEstimator::process() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counters_.closed) return std::numeric_limits<int64_t>::max();
    const auto nowUs = clock_.TimeInMicroseconds();
    if (nextProcessUs_ == std::numeric_limits<int64_t>::max()) return nextProcessUs_;
    if (nowUs < nextProcessUs_) return nextProcessUs_ - nowUs;
    const auto next = estimator_->Process();
    if (next.IsPlusInfinity()) {
        nextProcessUs_ = std::numeric_limits<int64_t>::max();
        return nextProcessUs_;
    }
    const auto delayUs = std::max<int64_t>(next.us(), 0);
    nextProcessUs_ = nowUs + delayUs;
    return delayUs;
}

void ReceiveBandwidthEstimator::removeStream(uint32_t ssrc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counters_.closed) return;
    estimator_->RemoveStream(ssrc);
    ssrcs_.erase(std::remove(ssrcs_.begin(), ssrcs_.end(), ssrc), ssrcs_.end());
}

void ReceiveBandwidthEstimator::rttUpdate(int64_t averageMs, int64_t maxMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!counters_.closed) estimator_->OnRttUpdate(averageMs, maxMs);
}

void ReceiveBandwidthEstimator::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counters_.closed) return;
    for (const uint32_t ssrc : ssrcs_) estimator_->RemoveStream(ssrc);
    ssrcs_.clear();
    estimator_.reset();
    counters_.closed = true;
}

int64_t ReceiveBandwidthEstimator::latestEstimateBps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counters_.closed) return 0;
    const auto estimate = estimator_->LatestEstimate();
    return estimate.IsFinite() ? estimate.bps() : 0;
}

EstimatorCounters ReceiveBandwidthEstimator::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}
} // namespace uu::rbe
