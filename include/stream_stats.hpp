#pragma once
#include "rtp_jitter.hpp"
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

namespace uu {
struct NetworkStats {
    double lossPercent = -1;   // Observed initial sequence gaps; before RTX recovery
    double jitterMs = -1;      // RFC 3550 interarrival jitter
    double bitrateMbps = -1;   // RTP bitrate over the last window
    uint64_t packets = 0;      // total RTP packets received
    uint64_t lost = 0;         // total RTP packets missing (sequence gaps)
    uint64_t nackSent = 0;     // RTCP NACK packets sent
    uint64_t rtxRecovered = 0; // RTX retransmission packets received
};

struct StreamStatsSnapshot {
    uint64_t decodedTotal = 0, publishedTotal = 0, mediaEpoch = 0;
    int inputQueued = 0;
    int64_t sampledMs = 0;
    bool hardwareFrame = false;
    uint64_t cpuTransfers=0,nativePublished=0;
    uint64_t skipDue = 0, skipBound = 0;
    int selectedLateMs = 0;
    int rttMs = -1;            // SCTP/DTLS round trip over the same UDP path
    std::string transport = "—";
    std::string localEndpoint = "—", remoteEndpoint = "—";
    double presentFps = -1;
    int requestedFps = 0, requestedQuality = 0, maxBitrateMbps = 0;
    int hostBitrateMbps = 0;
    bool hostBitrateKnown = false;
    bool bitrateFeedbackSent = false;
    uint64_t qualitySent=0,qualityAcked=0,qualityFailed=0;
    uint64_t touchPackets=0,touchFailures=0;unsigned touchPoints=0;
    bool qualityPending=false;
    int activeScreenId=-1,qualityCode=0;unsigned qualityErrors=0;
    bool pacing = false;
    int playoutDelayMs = 0, readyFrames = 0;
    uint64_t presentationSkipped = 0, clockResets = 0, missingTimestamps = 0;
    NetworkStats video, audio;
    std::string codec = "—";    // H.264 profile/level parsed from SPS
    std::string encoder = "—";  // encoder signature parsed from SEI (soft/hard)
    std::string decoder = "—";  // FFmpeg software / NVDEC hardware
    int width = 0, height = 0;
    double decodeFps = 0;
    double decodeMs = 0;
    uint64_t decodeErrors = 0;
    uint64_t droppedFrames = 0;
    uint64_t keyframeRequests = 0;
    int keyframeRecoveryMs = -1;
    uint64_t nackSent = 0;
    uint64_t rtxRecovered = 0;
    bool strictReceiver = false;
    uint64_t receiverAccepted = 0, receiverDropped = 0, receiverWaitingIdr = 0, receiverFastForwards = 0;
    double receiverMeanWaitMs = 0;
    int receiverRecentWaitMs = 0, receiverMaxWaitMs = 0;
    int receiverPending = 0, receiverBlockedMs = 0, nackRetryMs = 45;
    double repairRttMs = 0;
    uint64_t nackRepeated = 0, receiverPli = 0;
    // Schema-12 diagnostic-only snapshots. These are cached under statsMutex;
    // UI code never reads decoder-worker fields directly.
    uint64_t auMaxBytes = 0, auBytesTotal = 0, auCount = 0, auOversize = 0;
    uint64_t queueHighFrames = 0, queueHighBytes = 0;
    int queueWaitLastMs = 0, queueWaitMaxMs = 0, oldestQueueWaitMs = 0;
    double decodeMaxMs = 0;
    uint64_t decodeGt8 = 0, decodeGt16 = 0, decodeGt33 = 0;
    int64_t lastDecodedAgeMs = 0;
    // Passive strict-receiver processing counters (cumulative; wall time).
    uint64_t rxBufferedBytes=0, rxBufferedHigh=0;
    uint64_t rxInputMessages=0, rxInputBatches=0, rxMaxBatch=0;
    uint64_t rxIncomingCalls=0, rxTickCalls=0;
    uint64_t rxIncomingTotalUs=0, rxIncomingMaxUs=0, rxTickTotalUs=0, rxTickMaxUs=0;
    uint64_t rxMemoryBoundRetires=0, rxMaxFrameBytes=0;
    bool repairPaced=false;
    uint64_t nackSequences=0,nackBudgetDeferrals=0,nackExpiredScans=0;
};

// RTP level accounting for a single media track. Inserted at the end of the
// libdatachannel media handler chain so it observes raw RTP/RTCP messages.
// Sequence gaps here describe initial reception. The independent receiver
// handles NACK/RTX and exposes recovered packets and final AU retirement.
class RtpTrackStats : public rtc::MediaHandler {
  public:
    RtpTrackStats(unsigned clockRate, std::string name, bool rtx = false);
    ~RtpTrackStats() override;
    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
    NetworkStats snapshot();

  private:
    void packet(const uint8_t *data, size_t size);
    void info(const uint8_t *data, size_t size);
    void flushDiagnostics();

    mutable std::mutex mutex_;
    const unsigned clockRate_;
    const std::string name_;
    const bool rtx_;
    bool started_ = false;
    uint32_t expected_ = 0; // extended sequence number of the next packet
    uint64_t received_ = 0, lost_ = 0;
    RtpJitterTracker jitterTracker_;
    int64_t windowStart_ = 0;
    uint64_t windowReceived_ = 0, windowLost_ = 0, windowBytes_ = 0;
    double windowLoss_ = -1, windowJitter_ = -1, windowBitrate_ = -1;
    // RTX / NACK recovery
    uint8_t videoPayloadType_ = 255;
    uint32_t videoSsrc_ = 0;
    uint32_t latestTimestamp_ = 0;
    std::vector<uint16_t> pendingNack_;
    int64_t lastNackMs_ = 0;
    uint64_t nacksSent_ = 0, rtxPackets_ = 0;
    // Diagnostic counters
    bool diag_ = false;
    std::vector<uint8_t> dump_;
    size_t dumpLimit_ = 24u * 1024u * 1024u;
    uint64_t gaps_ = 0, reorders_ = 0, duplicates_ = 0;
    uint32_t lastSeq_ = 0;
    bool haveLastSeq_ = false;
    std::set<uint32_t> ssrcs_;
    std::set<int> payloadTypes_;
    bool haveSr_ = false;
    uint32_t srPackets_ = 0, srOctets_ = 0, srCount_ = 0;
    uint32_t srPacketsBase_ = 0;
    uint64_t receivedAtSrBase_ = 0;
    int64_t firstPacketMs_ = 0;
    int64_t lastPacketMs_ = 0;
};
} // namespace uu
