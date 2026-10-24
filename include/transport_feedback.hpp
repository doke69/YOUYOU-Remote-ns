#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <optional>
#include <vector>

namespace uu::twcc {
// Pure helpers only: not wired to SDP, packet reception or RTCP sending.
enum class ExtensionStatus { Found, Missing, UnsupportedProfile, Malformed };
struct TransportSequence {
    ExtensionStatus status = ExtensionStatus::Missing;
    uint16_t sequence = 0;
};
TransportSequence readTransportSequence(const uint8_t *rtp, size_t size,
                                        uint8_t negotiatedExtensionId);

enum class EncodeStatus { Ok, InvalidInput, SplitRequired, PacketTooLarge };
struct EncodedFeedback {
    EncodeStatus status = EncodeStatus::InvalidInput;
    std::vector<uint8_t> packet;
    // On SplitRequired, split before this index and use a new reference time.
    size_t splitBefore = 0;
};
// Each position is one consecutive TRANSPORT-wide sequence, including gaps.
// Times use one stable nonnegative monotonic microsecond clock across reports.
// Missing means not yet observed, not a declaration of permanent packet loss.
// At most 512 statuses; no implicit sequence tracking. An all-missing range
// requires an explicit reference on the same monotonic clock.
EncodedFeedback encodeFeedback(uint32_t senderSsrc, uint32_t mediaSsrc,
    uint16_t baseSequence, uint8_t feedbackCount,
    const std::vector<std::optional<int64_t>> &arrivalTimesUs,
    size_t maxPacketBytes = 1200,
    std::optional<int64_t> emptyReferenceUs = std::nullopt);

enum class ArrivalStatus { Accepted, Duplicate, TooOld, WindowFull, AmbiguousSequence, InvalidTime };
struct RecordedArrival { ArrivalStatus status; int64_t extendedSequence = 0; };
struct ArrivalSlice {
    int64_t firstSequence = 0;
    std::vector<std::optional<int64_t>> timesUs;
};
// Single-owner helper, or protect ALL calls with one external mutex. Allocate
// once off the receive-thread stack. record() performs no heap allocations.
// It has no send policy: failed sends must not call retireBefore(). Late
// overlap reporting and the safe report horizon belong to the coordinator.
class ArrivalWindow {
  public:
    static constexpr size_t Capacity = 4096;
    RecordedArrival record(uint16_t transportSequence, int64_t arrivalUs);
    std::optional<ArrivalSlice> snapshot(int64_t first, size_t count) const;
    bool retireBefore(int64_t nextSequence);
    void reset();
    std::optional<int64_t> firstSequence() const;
    std::optional<int64_t> lastSequence() const;
    std::optional<int64_t> arrivalAt(int64_t extendedSequence) const;
  private:
    static constexpr int64_t Empty = std::numeric_limits<int64_t>::min();
    struct Slot { int64_t sequence = Empty; int64_t arrivalUs = 0; };
    static size_t index(int64_t sequence) { return size_t(uint64_t(sequence) & (Capacity - 1)); }
    std::array<Slot, Capacity> slots_{};
    bool initialized_ = false, retired_ = false;
    int64_t first_ = 0, highest_ = 0;
};

struct FeedbackTiming {
    int64_t intervalUs;
    int64_t reorderHoldUs;
    size_t lookbackPackets;
};
struct PreparedFeedback {
    uint64_t ticket = 0;
    std::vector<uint8_t> packet;
    int64_t firstSequence = 0, lastSequence = 0;
    uint8_t feedbackCount = 0;
    bool overlap = false;
};
// Single owner/external mutex, like ArrivalWindow. Timing must be chosen by
// the caller: no implicit bandwidth estimator, RTCP budget or network sends.
class FeedbackCoordinator {
  public:
    explicit FeedbackCoordinator(FeedbackTiming timing);
    RecordedArrival record(uint16_t sequence, int64_t arrivalUs);
    // A prepared packet remains byte-identical until committed or reset.
    // The pointer is valid only until a modifying call; copy under the same
    // external lock if it must be sent outside that lock.
    const PreparedFeedback *prepare(int64_t nowUs, uint32_t senderSsrc,
                                     uint32_t mediaSsrc, size_t mtu = 1200);
    // Commit only after successful authenticated RTCP submission. Failure:
    // leave pending untouched and retry subject to the caller's send budget.
    bool commit(uint64_t ticket, int64_t sentAtUs);
    void reset();
  private:
    static constexpr int64_t Empty = std::numeric_limits<int64_t>::min();
    struct Pending {
        PreparedFeedback report;
        std::vector<int64_t> received;
        int64_t preparedAtUs = 0;
    };
    bool reported(int64_t sequence) const;
    void retireHistory();
    FeedbackTiming timing_;
    ArrivalWindow history_;
    std::array<int64_t, ArrivalWindow::Capacity> reported_{};
    std::optional<Pending> pending_;
    std::optional<int64_t> cursor_, lastSentAtUs_;
    uint64_t nextTicket_ = 1;
    uint8_t feedbackCount_ = 0;
};

// Caller-provided RTCP byte budget, NOT a bandwidth estimator. Use a shared
// policy for all feedback producers; this helper alone cannot enforce total
// RR/NACK/TWCC traffic. Amounts include caller-specified SRTCP/network overhead.
// Single owner/external lock. Attempts consume credit even when submission
// fails, preventing an unbounded retry loop; coordinator commit stays separate.
class FeedbackBudget {
  public:
    FeedbackBudget(uint32_t bytesPerSecond, uint32_t burstBytes,
                   uint32_t initialCreditBytes, int64_t startUs);
    bool trySpend(size_t packetBytes, size_t overheadBytes, int64_t nowUs,
                  size_t reservedCreditBytes = 0);
    uint64_t attempts() const { return attempts_; }
    uint64_t deferred() const { return deferred_; }
  private:
    uint32_t rate_;
    uint64_t capacity_, credit_;
    int64_t lastRefillUs_;
    uint64_t attempts_ = 0, deferred_ = 0;
};
} // namespace uu::twcc
