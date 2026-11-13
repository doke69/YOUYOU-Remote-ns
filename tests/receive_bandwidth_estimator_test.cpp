#include "receive_bandwidth_estimator.hpp"
#include "modules/remote_bitrate_estimator/include/remote_bitrate_estimator.h"
#include "system_wrappers/include/clock.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <thread>

// Uses the real pinned WebRTC estimator, not a replacement control law.
int main() {
    struct Observer : webrtc::RemoteBitrateObserver {
        uint64_t calls = 0;
        std::vector<uint32_t> streams;
        std::mutex mutex;
        std::condition_variable cv;
        bool block = false, entered = false, release = false;
        void OnReceiveBitrateChanged(const std::vector<uint32_t> &ssrcs, uint32_t) override {
            ++calls;
            streams = ssrcs;
            std::unique_lock<std::mutex> lock(mutex);
            if (block) {
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
            }
        }
    } observer;
    webrtc::SimulatedClock clock(0);
    uu::rbe::ReceiveBandwidthEstimator estimator(clock, observer);
    uu::rbe::ReadResult packet;
    packet.status = uu::rbe::PacketStatus::Accepted;
    packet.packet.ssrc = 0x12345678;
    packet.packet.payloadType = 96;
    packet.packet.payloadBytes = 1200;
    packet.packet.arrivalUs = 0;
    packet.packet.absSendTime = 0;
    packet.packet.transmissionOffset = -37;
    estimator.incoming(packet);
    if (!estimator.counters().usingAbsSendTime) return 1;
    if (estimator.process() != std::numeric_limits<int64_t>::max()) return 13;
    packet.packet.absSendTime.reset();
    for (int i = 0; i < 29; ++i) estimator.incoming(packet);
    if (!estimator.counters().usingAbsSendTime || estimator.counters().packetsSinceAbsSendTime != 29) return 2;
    for (const auto status : {uu::rbe::PacketStatus::NotVideo, uu::rbe::PacketStatus::TransportSequence,
                              uu::rbe::PacketStatus::MissingArrival, uu::rbe::PacketStatus::Malformed}) {
        auto excluded = packet;
        excluded.status = status;
        estimator.incoming(excluded);
    }
    if (estimator.counters().packetsSinceAbsSendTime != 29) return 3;
    packet.packet.absSendTime = 0; // A present zero is not a missing extension.
    estimator.incoming(packet);
    if (estimator.counters().packetsSinceAbsSendTime != 0) return 4;
    packet.packet.absSendTime.reset();
    for (int i = 0; i < 30; ++i) estimator.incoming(packet);
    const auto switched = estimator.counters();
    if (switched.usingAbsSendTime || switched.switchesToAbsSendTime != 1 ||
        switched.switchesToTransmissionOffset != 1 || switched.notVideo != 1 ||
        switched.transportSequence != 1 || switched.missingArrival != 1 || switched.malformed != 1) return 5;
    if (estimator.process() == std::numeric_limits<int64_t>::max()) return 14;
    estimator.close();

    for (const bool ast : {false, true}) {
        Observer output;
        webrtc::SimulatedClock replayClock(0);
        uu::rbe::ReceiveBandwidthEstimator replay(replayClock, output);
        int64_t arrival = 0, nextProcess = 0, healthy = 0;
        for (int i = 0; i < 22000; ++i) {
            arrival += i < 12000 ? 1200 : 4000;
            replayClock.AdvanceTimeMicroseconds(arrival - replayClock.TimeInMicroseconds());
            packet.packet.arrivalUs = arrival;
            packet.packet.rtpTimestamp = uint32_t(0xffff0000u + uint64_t(i) * 108);
            packet.packet.transmissionOffset = 37;
            if (ast) packet.packet.absSendTime = uint32_t((63000000 + int64_t(i) * 1200) * (1 << 18) / 1000000) & 0xffffff;
            else packet.packet.absSendTime.reset();
            replay.incoming(packet);
            if (arrival >= nextProcess) {
                const int64_t delay = replay.process();
                if (delay < 0) return 6;
                nextProcess = delay == std::numeric_limits<int64_t>::max() ? delay : arrival + std::max<int64_t>(delay, 1000);
            }
            if (i == 11999) healthy = replay.latestEstimateBps();
        }
        const auto estimate = replay.latestEstimateBps();
        if (!healthy || estimate <= 0 || estimate >= healthy * 8 / 10 || !output.calls ||
            output.streams != std::vector<uint32_t>{packet.packet.ssrc}) return 7;
        replay.rttUpdate(40, 60);
        replay.removeStream(packet.packet.ssrc);
        if (replay.latestEstimateBps() != 0) return 8;
        replay.close();
        const auto calls = output.calls;
        replay.incoming(packet);
        if (replay.process() != std::numeric_limits<int64_t>::max() || replay.latestEstimateBps() ||
            output.calls != calls || replay.counters().afterClose != 1) return 9;
        replay.close();
        uu::rbe::ReceiveBandwidthEstimator fresh(replayClock, output);
        if (fresh.latestEstimateBps() || fresh.counters().packets || fresh.counters().closed) return 10;
        std::cout << (ast ? "AST" : "TOF") << " healthy=" << healthy << " congested=" << estimate << '\n';
    }

    // A real already-admitted observer callback must finish before close returns.
    Observer blocked;
    blocked.block = true;
    webrtc::SimulatedClock blockedClock(0);
    uu::rbe::ReceiveBandwidthEstimator closing(blockedClock, blocked);
    std::atomic<bool> finished{false}, closed{false};
    std::thread incoming([&] {
        auto input = packet;
        input.packet.absSendTime.reset();
        for (int i = 0; i < 12000; ++i) {
            blockedClock.AdvanceTimeMicroseconds(1200);
            input.packet.arrivalUs = blockedClock.TimeInMicroseconds();
            input.packet.rtpTimestamp = uint32_t(i * 108);
            closing.incoming(input);
            closing.process();
        }
        finished = true;
        blocked.cv.notify_all();
    });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(blocked.mutex);
        blocked.cv.wait(lock, [&] { return blocked.entered || finished.load(); });
        entered = blocked.entered;
    }
    std::thread stop([&] { closing.close(); closed = true; });
    // The callback is still held, so neither the close barrier nor its effects may complete.
    const bool premature = closed.load();
    {
        std::lock_guard<std::mutex> lock(blocked.mutex);
        blocked.release = true;
    }
    blocked.cv.notify_all();
    incoming.join();
    stop.join();
    if (!entered || premature || !closed || !closing.counters().closed) return 11;
    const auto calls = blocked.calls;
    closing.incoming(packet);
    closing.process();
    if (blocked.calls != calls) return 12;
    std::cout << "PASS: AST/TOF switch threshold, exclusions, real congestion response, process/removal, close barrier and fresh generation\n";
}
