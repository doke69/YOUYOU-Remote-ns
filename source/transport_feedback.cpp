#include "transport_feedback.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace uu::twcc {
namespace {
uint16_t be16(const uint8_t *p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
void word(std::vector<uint8_t> &out, uint16_t n) {
    out.push_back(uint8_t(n >> 8)); out.push_back(uint8_t(n));
}
void dword(std::vector<uint8_t> &out, uint32_t n) {
    word(out, uint16_t(n >> 16)); word(out, uint16_t(n));
}
} // namespace

TransportSequence readTransportSequence(const uint8_t *rtp, size_t size, uint8_t id) {
    const auto malformed = TransportSequence{ExtensionStatus::Malformed, 0};
    if (!rtp || !id || size < 12 || rtp[0] >> 6 != 2) return malformed;
    const size_t fixed = 12 + 4 * size_t(rtp[0] & 15);
    if (fixed > size) return malformed;
    size_t payloadEnd = size;
    if (rtp[0] & 0x20) {
        const auto padding = rtp[size - 1];
        if (!padding || padding > size - fixed) return malformed;
        payloadEnd -= padding;
    }
    if (!(rtp[0] & 0x10)) return {};
    if (payloadEnd - fixed < 4) return malformed;
    const uint16_t profile = be16(rtp + fixed);
    const size_t length = size_t(be16(rtp + fixed + 2)) * 4;
    if (length > payloadEnd - fixed - 4) return malformed;
    const size_t end = fixed + 4 + length;
    const bool oneByte = profile == 0xbede;
    const bool twoByte = (profile & 0xfff0) == 0x1000;
    if (!oneByte && !twoByte) return {ExtensionStatus::UnsupportedProfile, 0};
    TransportSequence result;
    for (size_t pos = fixed + 4; pos < end;) {
        const uint8_t header = rtp[pos++];
        if (!header) continue; // Both formats allow single-byte zero padding.
        unsigned current;
        size_t bytes;
        if (oneByte) {
            current = header >> 4;
            if (current == 15) break; // RFC 8285: stop parsing this extension.
            if (!current) return malformed;
            bytes = (header & 15) + 1;
        } else {
            current = header;
            if (pos == end) return malformed;
            bytes = rtp[pos++];
        }
        if (bytes > end - pos) return malformed;
        if (current == id) {
            // Do not guess v2 request bytes or accept ambiguous duplicates.
            if (bytes != 2 || result.status == ExtensionStatus::Found) return malformed;
            result = {ExtensionStatus::Found, be16(rtp + pos)};
        }
        pos += bytes;
    }
    return result;
}

EncodedFeedback encodeFeedback(uint32_t senderSsrc, uint32_t mediaSsrc,
    uint16_t baseSequence, uint8_t feedbackCount,
    const std::vector<std::optional<int64_t>> &arrivals, size_t mtu,
    std::optional<int64_t> emptyReferenceUs) {
    EncodedFeedback result;
    if (arrivals.empty() || arrivals.size() > 512 || mtu < 24 || mtu > 1200) return result;
    // Restrict the supported time domain so all subtraction/rounding is safe.
    constexpr int64_t maxTime = int64_t(1) << 55;
    if (emptyReferenceUs && (*emptyReferenceUs < 0 || *emptyReferenceUs > maxTime)) return result;
    std::optional<int64_t> first;
    for (const auto &arrival : arrivals) {
        if (arrival && (*arrival < 0 || *arrival > maxTime)) return result;
        if (!first && arrival) first = *arrival;
    }
    if (!first) first = emptyReferenceUs;
    if (!first) return result;
    const int64_t reference = *first / 64000;
    int64_t reconstructed = reference * 64000;
    std::array<uint8_t, 512> symbols{};
    std::vector<uint8_t> deltas;
    deltas.reserve(2 * arrivals.size());
    for (size_t i = 0; i < arrivals.size(); ++i) {
        if (!arrivals[i]) continue;
        const int64_t delta = *arrivals[i] - reconstructed;
        // Carry reconstructed time forward, so quantization error cannot drift
        // once per packet. Negative deltas preserve sequence-order reordering.
        const int64_t ticks = delta >= 0 ? (delta + 125) / 250 : -((-delta + 125) / 250);
        if (ticks < -32768 || ticks > 32767) {
            result.status = EncodeStatus::SplitRequired;
            result.splitBefore = i;
            return result;
        }
        reconstructed += ticks * 250;
        if (ticks >= 0 && ticks <= 255) {
            symbols[i] = 1;
            deltas.push_back(uint8_t(ticks));
        } else {
            symbols[i] = 2;
            word(deltas, uint16_t(ticks));
        }
    }
    const size_t chunks = (arrivals.size() + 6) / 7;
    const size_t rawSize = 20 + 2 * chunks + deltas.size();
    const size_t alignedSize = (rawSize + 3) & ~size_t(3);
    if (alignedSize > mtu) {
        result.status = EncodeStatus::PacketTooLarge;
        return result;
    }
    auto &out = result.packet;
    out.reserve(alignedSize);
    const auto padding = uint8_t(alignedSize - rawSize);
    out.push_back(uint8_t(0x8f | (padding ? 0x20 : 0))); // V2, FMT15.
    out.push_back(205); // RTPFB, not payload-specific feedback.
    word(out, uint16_t(alignedSize / 4 - 1));
    dword(out, senderSsrc); dword(out, mediaSsrc);
    word(out, baseSequence); word(out, uint16_t(arrivals.size()));
    const auto wrappedReference = uint32_t(reference) & 0xffffff;
    out.push_back(uint8_t(wrappedReference >> 16));
    out.push_back(uint8_t(wrappedReference >> 8));
    out.push_back(uint8_t(wrappedReference)); out.push_back(feedbackCount);
    // Valid two-bit vectors only; compression/RLE is deliberately separate
    // from correctness. Unused final symbols are zero; count bounds parsing.
    for (size_t pos = 0; pos < arrivals.size(); pos += 7) {
        uint16_t chunk = 0xc000;
        for (size_t i = 0; i < 7 && pos + i < arrivals.size(); ++i)
            chunk |= uint16_t(uint16_t(symbols[pos + i]) << (12 - 2 * i));
        word(out, chunk);
    }
    out.insert(out.end(), deltas.begin(), deltas.end());
    if (padding) {
        out.resize(alignedSize, 0);
        out.back() = padding; // Actual RTCP padding, not an extra receive delta.
    }
    result.status = EncodeStatus::Ok;
    return result;
}

RecordedArrival ArrivalWindow::record(uint16_t sequence, int64_t arrivalUs) {
    if (arrivalUs < 0 || arrivalUs > (int64_t(1) << 55)) return {ArrivalStatus::InvalidTime, 0};
    int64_t extended = sequence;
    if (initialized_) {
        const auto delta = uint16_t(sequence - uint16_t(highest_));
        if (delta == 32768) return {ArrivalStatus::AmbiguousSequence, 0};
        const int64_t offset = delta < 32768 ? int64_t(delta) : int64_t(delta) - 65536;
        // Fail without state changes at the counter's arithmetic boundaries.
        if ((offset > 0 && highest_ > std::numeric_limits<int64_t>::max() - offset) ||
            (offset < 0 && highest_ < std::numeric_limits<int64_t>::min() - offset))
            return {ArrivalStatus::AmbiguousSequence, 0};
        extended = highest_ + offset;
        if (extended < first_) {
            if (retired_ || highest_ - extended >= int64_t(Capacity))
                return {ArrivalStatus::TooOld, extended};
            // Startup reordering can extend the first window before anything
            // has been committed; do not guess a reset after later retirement.
        } else if (extended - first_ >= int64_t(Capacity)) {
            return {ArrivalStatus::WindowFull, extended};
        }
        const auto &slot = slots_[index(extended)];
        if (slot.sequence == extended) return {ArrivalStatus::Duplicate, extended};
    }
    if (!initialized_) {
        first_ = highest_ = extended;
        initialized_ = true;
    } else {
        first_ = std::min(first_, extended);
        highest_ = std::max(highest_, extended);
    }
    slots_[index(extended)] = Slot{extended, arrivalUs};
    return {ArrivalStatus::Accepted, extended};
}

std::optional<ArrivalSlice> ArrivalWindow::snapshot(int64_t first, size_t count) const {
    if (!initialized_ || !count || count > 512 || first < first_ || first > highest_ ||
        int64_t(count - 1) > highest_ - first) return {};
    ArrivalSlice out;
    out.firstSequence = first;
    out.timesUs.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const auto seq = first + int64_t(i);
        const auto &slot = slots_[index(seq)];
        if (slot.sequence == seq) out.timesUs[i] = slot.arrivalUs;
    }
    return out;
}

bool ArrivalWindow::retireBefore(int64_t nextSequence) {
    if (!initialized_ || nextSequence < first_ || highest_ == std::numeric_limits<int64_t>::max() ||
        nextSequence > highest_ + 1) return false;
    if (nextSequence != first_) retired_ = true;
    first_ = nextSequence;
    // Sequence tags make old ring slots invisible without clearing 4096 cells
    // on each report. Copies already returned by snapshot() remain independent.
    return true;
}

void ArrivalWindow::reset() {
    slots_.fill(Slot{});
    initialized_ = retired_ = false;
    first_ = highest_ = 0;
}
std::optional<int64_t> ArrivalWindow::firstSequence() const {
    if (!initialized_ || first_ > highest_) return {};
    return first_;
}
std::optional<int64_t> ArrivalWindow::lastSequence() const {
    if (!initialized_ || first_ > highest_) return {};
    return highest_;
}

std::optional<int64_t> ArrivalWindow::arrivalAt(int64_t sequence) const {
    if (!initialized_ || sequence < first_ || sequence > highest_) return {};
    const auto &slot = slots_[index(sequence)];
    if (slot.sequence != sequence) return {};
    return slot.arrivalUs;
}

FeedbackCoordinator::FeedbackCoordinator(FeedbackTiming timing) : timing_(timing) {
    if (timing.intervalUs <= 0 || timing.intervalUs > 1000000 ||
        timing.reorderHoldUs < 0 || timing.reorderHoldUs > 1000000 ||
        timing.lookbackPackets > ArrivalWindow::Capacity / 2)
        throw std::invalid_argument("Invalid transport-feedback timing bounds");
    reported_.fill(Empty);
}
bool FeedbackCoordinator::reported(int64_t sequence) const {
    return reported_[size_t(uint64_t(sequence) & (ArrivalWindow::Capacity - 1))] == sequence;
}
RecordedArrival FeedbackCoordinator::record(uint16_t sequence, int64_t arrivalUs) {
    return history_.record(sequence, arrivalUs);
}

const PreparedFeedback *FeedbackCoordinator::prepare(int64_t nowUs, uint32_t senderSsrc,
    uint32_t mediaSsrc, size_t mtu) {
    if (nowUs < 0 || nowUs > (int64_t(1) << 55)) return nullptr;
    if (pending_) return &pending_->report;
    if (lastSentAtUs_ && (nowUs < *lastSentAtUs_ || nowUs - *lastSentAtUs_ < timing_.intervalUs))
        return nullptr;
    const auto first = history_.firstSequence(), highest = history_.lastSequence();
    if (!first || !highest || !nextTicket_ ||
        *highest > std::numeric_limits<int64_t>::max() - 512 ||
        *first < std::numeric_limits<int64_t>::min() + 512) return nullptr;
    const int64_t cursor = cursor_.value_or(*first);
    int64_t begin = cursor, end = cursor - 1;
    bool overlap = false;
    // Report genuinely late, not-previously-reported arrivals first. Already
    // reported arrivals in an overlapping range get no second delta.
    for (auto seq = *first; seq < cursor && seq <= *highest; ++seq) {
        const auto arrival = history_.arrivalAt(seq);
        if (arrival && !reported(seq) && *arrival <= nowUs) {
            begin = seq;
            end = std::min(cursor - 1, begin + 511);
            overlap = true;
            break;
        }
    }
    if (!overlap) {
        if (begin > *highest) return nullptr;
        // A mature observed packet proves a safe right edge; unknown trailing
        // packets are never manufactured beyond the highest observed sequence.
        // Look past the first 512 positions: a long all-missing prefix must
        // still make progress toward a later mature packet within the window.
        for (auto seq = begin; seq <= *highest; ++seq) {
            const auto arrival = history_.arrivalAt(seq);
            if (arrival && *arrival <= nowUs && nowUs - *arrival >= timing_.reorderHoldUs) end = seq;
        }
        if (end < begin) return nullptr;
        end = std::min(end, begin + 511);
    }
    auto slice = history_.snapshot(begin, size_t(end - begin + 1));
    if (!slice) return nullptr;
    for (size_t i = 0; i < slice->timesUs.size(); ++i) {
        if (reported(begin + int64_t(i))) slice->timesUs[i].reset();
        // Clock anomalies must not be reported as fabricated missing packets.
        if (slice->timesUs[i] && *slice->timesUs[i] > nowUs) return nullptr;
    }
    EncodedFeedback encoded;
    for (;;) {
        encoded = encodeFeedback(senderSsrc, mediaSsrc, uint16_t(begin), feedbackCount_, slice->timesUs, mtu, nowUs);
        if (encoded.status == EncodeStatus::Ok) break;
        size_t count = 0;
        if (encoded.status == EncodeStatus::SplitRequired) count = encoded.splitBefore;
        else if (encoded.status == EncodeStatus::PacketTooLarge) count = slice->timesUs.size() / 2;
        if (!count || count >= slice->timesUs.size()) return nullptr;
        slice->timesUs.resize(count);
    }
    Pending prepared;
    prepared.preparedAtUs = nowUs;
    prepared.report.ticket = nextTicket_++;
    prepared.report.packet = std::move(encoded.packet);
    prepared.report.firstSequence = begin;
    prepared.report.lastSequence = begin + int64_t(slice->timesUs.size()) - 1;
    prepared.report.feedbackCount = feedbackCount_;
    prepared.report.overlap = overlap;
    for (size_t i = 0; i < slice->timesUs.size(); ++i)
        if (slice->timesUs[i]) prepared.received.push_back(begin + int64_t(i));
    pending_ = std::move(prepared);
    return &pending_->report;
}

bool FeedbackCoordinator::commit(uint64_t ticket, int64_t sentAtUs) {
    if (!pending_ || pending_->report.ticket != ticket || sentAtUs < pending_->preparedAtUs ||
        sentAtUs > (int64_t(1) << 55) || (lastSentAtUs_ && sentAtUs < *lastSentAtUs_)) return false;
    for (const auto seq : pending_->received)
        reported_[size_t(uint64_t(seq) & (ArrivalWindow::Capacity - 1))] = seq;
    if (!pending_->report.overlap) cursor_ = pending_->report.lastSequence + 1;
    lastSentAtUs_ = sentAtUs;
    ++feedbackCount_;
    pending_.reset();
    retireHistory();
    return true;
}
void FeedbackCoordinator::retireHistory() {
    const auto first = history_.firstSequence();
    if (!cursor_ || !first) return;
    auto floor = std::max(*first, *cursor_ - int64_t(timing_.lookbackPackets));
    for (auto seq = *first; seq < floor; ++seq) {
        if (history_.arrivalAt(seq) && !reported(seq)) {
            floor = seq; // Never retire a newly arrived but unreported packet.
            break;
        }
    }
    history_.retireBefore(floor);
}
void FeedbackCoordinator::reset() {
    history_.reset();
    reported_.fill(Empty);
    pending_.reset(); cursor_.reset(); lastSentAtUs_.reset();
    feedbackCount_ = 0;
    // Do not reuse a ticket after reset: stale send completions must fail.
}

FeedbackBudget::FeedbackBudget(uint32_t bytesPerSecond, uint32_t burstBytes,
                               uint32_t initialCreditBytes, int64_t startUs)
    : rate_(bytesPerSecond), capacity_(uint64_t(burstBytes) * 1000000),
      credit_(uint64_t(initialCreditBytes) * 1000000), lastRefillUs_(startUs) {
    if (!bytesPerSecond || bytesPerSecond > 1024 * 1024 || !burstBytes ||
        burstBytes > 1024 * 1024 || initialCreditBytes > burstBytes ||
        startUs < 0 || startUs > (int64_t(1) << 55))
        throw std::invalid_argument("Invalid transport feedback budget");
}
bool FeedbackBudget::trySpend(size_t packetBytes, size_t overheadBytes, int64_t nowUs,
                              size_t reservedCreditBytes) {
    if (!packetBytes || packetBytes > 1200 || overheadBytes > 4096 ||
        reservedCreditBytes > capacity_ / 1000000 ||
        nowUs < lastRefillUs_ || nowUs > (int64_t(1) << 55)) return false;
    const uint64_t elapsed = uint64_t(nowUs - lastRefillUs_);
    const uint64_t needed = capacity_ - credit_;
    // Saturate before multiplying: a long idle interval must not overflow.
    const uint64_t fillTime = (needed + rate_ - 1) / rate_;
    credit_ = elapsed >= fillTime ? capacity_ : credit_ + elapsed * rate_;
    lastRefillUs_ = nowUs;
    const uint64_t cost = uint64_t(packetBytes + overheadBytes) * 1000000;
    const uint64_t reserve = uint64_t(reservedCreditBytes) * 1000000;
    if (cost + reserve > credit_) { ++deferred_; return false; }
    credit_ -= cost;
    ++attempts_;
    return true;
}
} // namespace uu::twcc
