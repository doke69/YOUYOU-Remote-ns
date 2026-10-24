#pragma once
#include <atomic>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <rtc/rtc.hpp>

namespace uu {
// Read-only bytes borrowed from Packet::owner, or a vector for immediate parsing.
struct RtpPayloadView {
    const uint8_t *bytes=nullptr;size_t length=0;
    RtpPayloadView()=default;
    RtpPayloadView(const uint8_t *p,size_t n):bytes(p),length(n){}
    RtpPayloadView(const std::vector<uint8_t> &v):bytes(v.data()),length(v.size()){}
    const uint8_t *data()const{return bytes;}
    const uint8_t *begin()const{return bytes;}
    const uint8_t *end()const{return bytes?bytes+length:bytes;}
    size_t size()const{return length;}
    bool empty()const{return length==0;}
    uint8_t operator[](size_t i)const{return bytes[i];}
};
// Independent bounded H.264 packet receiver. Only structurally complete access
// units reach the decoder. tick() is driven by Client, not packet arrivals.
class StrictVideoReceiver final : public rtc::MediaHandler {
  public:
    struct Stats {
        uint64_t accepted = 0, dropped = 0, waitingIdr = 0, fastForwards = 0;
        uint64_t nackMessages = 0, rtxRecovered = 0;
        double meanWaitMs = 0;
        int recentWaitMs = 0, maxWaitMs = 0;
        int pendingFrames = 0, blockedMs = 0, nackRetryMs = 45;
        double repairRttMs = 0;
        uint64_t repeatedRequests = 0, pliRequests = 0;
        // Passive wall-time processing diagnostics; cumulative for this receiver.
        uint64_t bufferedBytes = 0, bufferedHighBytes = 0;
        uint64_t inputMessages = 0, inputBatches = 0, maxBatch = 0;
        uint64_t incomingCalls = 0, tickCalls = 0;
        uint64_t incomingTotalUs = 0, incomingMaxUs = 0;
        uint64_t tickTotalUs = 0, tickMaxUs = 0;
        uint64_t memoryBoundRetires = 0, maxFrameBytes = 0;
        bool repairPaced = false;
        uint64_t nackSequences = 0, budgetDeferrals = 0, expiredRepairScans = 0;
    };
    using Output = std::function<void(const uint8_t *, size_t, uint32_t)>;
    using ValidatedOutput = std::function<void(const uint8_t *, size_t, uint32_t, bool)>;
    StrictVideoReceiver(bool rtx, int deadlineMs, Output output, bool hevc = false, bool repairPaced = false,
                        ValidatedOutput validatedOutput = {});
    ~StrictVideoReceiver() override;
    void incoming(rtc::message_vector &, const rtc::message_callback &) override;
    void tick();
    void shutdown();
    void requestRecovery();
    void setRtt(int milliseconds);
    Stats snapshot();
    void setSources(uint32_t primary, uint32_t retransmission);
    uint64_t nackCount() const { return nacks_; }
    uint64_t rtxCount() const { return recovered_; }

  private:
    struct Packet { rtc::message_ptr owner; RtpPayloadView payload; bool marker = false; };
    struct Frame {
        uint32_t timestamp = 0;
        int64_t created = 0, marker = -1;
        std::map<int64_t, Packet> packets;
        size_t bytes = 0, retainedBytes = 0;
        bool possibleIdr = false; // Cached monotonic hint; assemble still validates.
        bool dependentVcl = false; // Recognized non-recovery VCL, never an acceptance proof.
        // Bounded retry cost for skipToReadyIdr: an unchanged candidate is not
        // assembled again until it receives another packet or parameters change.
        uint64_t revision = 1, checkedRevision = 0;
    };
    void process(int64_t now);
    bool skipToReadyIdr(int64_t now);
    void feedback(int64_t now);
    void refillRepairBudget(int64_t now);
    int retryDelayMs() const;
    // Retry spacing that still leaves room for a return trip inside the
    // remaining frame deadline. Never longer than retryDelayMs(); this is what
    // turns the old "at most one attempt at high repair RTT" into a bounded
    // multi-attempt schedule.
    int adaptiveRetryDelayMs(int64_t now, int64_t createdAt) const;
    int frameDeadlineMs() const;
    void pli(int64_t now, const char *reason = "waiting_idr");
    void noteEvent(const char *reason, int64_t now, unsigned delivered = 0);
    void retire(std::map<int64_t, Frame>::iterator, const char *reason);
    bool assemble(const Frame &, int64_t first, std::vector<uint8_t> &, bool &idr,
                  std::vector<uint8_t> &sps, std::vector<uint8_t> &pps, std::vector<uint8_t> &vps) const;
    bool assembleHevc(const Frame &, int64_t, std::vector<uint8_t> &, bool &,
                      std::vector<uint8_t> &, std::vector<uint8_t> &, std::vector<uint8_t> &) const;
    void flush();
    std::mutex mutex_;
    const bool hevc_;
    const bool repairPaced_;
    bool rtx_, stopped_ = false, haveSeq_ = false, haveTs_ = false;
    bool waitingIdr_ = true, haveClosed_ = false, haveBoundary_ = false;
    int deadlineMs_;
    int64_t highestSeq_ = 0, highestTs_ = 0, closedTs_ = 0, nextSeq_ = 0;
    int64_t lastNack_ = 0, lastPli_ = 0;
    // Separate PLI credits: a pre-deadline probe must not consume the credit
    // needed immediately after the frame is retired into waitingIdr_.
    int64_t lastProbePli_ = 0, lastRecoveryPli_ = 0;
    uint32_t primary_ = 0, rtxSource_ = 0;
    std::map<int64_t, Frame> frames_;
    struct Request { int64_t sentAt = 0; unsigned attempts = 0; };
    std::map<int64_t, Request> requested_;
    int fallbackRttMs_ = 45;
    bool haveMeasuredRtt_ = false;
    double repairRttMs_ = 0;
    uint64_t repairSamples_ = 0, repeatedRequests_ = 0, pliRequests_ = 0;
    std::vector<uint8_t> sps_, pps_, vps_;
    size_t buffered_ = 0, retained_ = 0;
    Output output_;
    ValidatedOutput validatedOutput_;
    rtc::message_callback send_;
    std::atomic<uint64_t> nacks_{0}, recovered_{0};
    uint64_t accepted_ = 0, dropped_ = 0, invalid_ = 0, stale_ = 0, duplicates_ = 0;
    uint64_t waiting_ = 0, recoveries_ = 0;
    uint64_t fastForwards_ = 0, waitTotalMs_ = 0;
    int recentWaitMs_ = 0, maxWaitMs_ = 0;
    std::deque<std::string> trace_;
    int64_t startedAtMs_ = 0;
    std::deque<std::string> events_;
    uint64_t eventsDropped_ = 0;
    uint64_t bufferedHighBytes_ = 0;
    uint64_t inputMessages_ = 0, inputBatches_ = 0, maxBatch_ = 0;
    uint64_t incomingCalls_ = 0, tickCalls_ = 0;
    uint64_t incomingTotalUs_ = 0, incomingMaxUs_ = 0;
    uint64_t tickTotalUs_ = 0, tickMaxUs_ = 0;
    uint64_t memoryBoundRetires_ = 0, maxFrameBytes_ = 0;
    uint64_t recoveryDiscarded_ = 0, recoverySkippedRepairScans_ = 0;
    bool sessionMarkerWritten_ = false;
    // Custom-mode experiment: bound requested RTX bursts without changing
    // frame validity, decode order or the existing frame deadline.
    int64_t repairRefillAt_ = 0, repairWindowAt_ = 0, repairScanAt_ = 0;
    double repairTokens_ = 32.0, repairRatePps_ = 128.0;
    uint64_t repairPrimaryPackets_ = 0;
    uint64_t nackSequences_ = 0, budgetDeferrals_ = 0, expiredRepairScans_ = 0;
    // Local diagnostic counters (receiver_v2.txt only; no UDP schema change).
    uint64_t rtxAbsentFrames_ = 0, markerConflicts_ = 0;
    uint64_t idrSkipAttempts_ = 0, idrSkipFailures_ = 0;
    uint64_t probePli_ = 0, recoveryPli_ = 0;
    static constexpr unsigned kMaxRepairAttempts = 4;
};
} // namespace uu
