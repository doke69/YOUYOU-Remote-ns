#include "stream_stats.hpp"
#include "config_store.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <rtc/rtp.hpp>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace uu {
namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
std::string configValue(const std::string &wanted) {
    if (const char *env = std::getenv("UU_DIAG"); env && *env && wanted == "diag")
        return trim(env);
    return ConfigStore::instance().value(wanted);
}
bool diagEnabled() {
    const auto value = configValue("diag");
    return !value.empty() && value != "0" && value != "off" && value != "no";
}
const char *diagDirectory() {
#ifdef __SWITCH__
    return "sdmc:/switch/UURemote-ns/diag";
#else
    return "diag";
#endif
}
void appendU32(std::vector<uint8_t> &buffer, uint32_t value) {
    buffer.push_back(uint8_t(value));
    buffer.push_back(uint8_t(value >> 8));
    buffer.push_back(uint8_t(value >> 16));
    buffer.push_back(uint8_t(value >> 24));
}
} // namespace

RtpTrackStats::RtpTrackStats(unsigned clockRate, std::string name, bool rtx)
    : clockRate_(clockRate), name_(std::move(name)), rtx_(rtx), jitterTracker_(clockRate), diag_(diagEnabled()) {
    if (name_ == "video") videoPayloadType_ = 96;
    windowStart_ = monotonicMs();
    if (diag_)
        dump_.reserve(dumpLimit_);
}
RtpTrackStats::~RtpTrackStats() {
    flushDiagnostics();
}
void RtpTrackStats::incoming(rtc::message_vector &messages, const rtc::message_callback &) {
    const auto now = monotonicMs();
    for (auto &message : messages) {
        if (!message)
            continue;
        if (message->type == rtc::Message::Control) {
            info(reinterpret_cast<const uint8_t *>(message->data()), message->size());
            continue;
        }
        if (message->type != rtc::Message::Binary)
            continue;
        auto *data = reinterpret_cast<const uint8_t *>(message->data());
        auto size = message->size();
        if (size < sizeof(rtc::RtpHeader))
            continue;
        auto *header = reinterpret_cast<const rtc::RtpHeader *>(data);
        if (header->version() != 2)
            continue;
        {
            std::lock_guard lock(mutex_);
            if (videoPayloadType_ == 255) {
                videoPayloadType_ = header->payloadType();
            }
            if (header->payloadType() == videoPayloadType_ && !videoSsrc_) videoSsrc_ = header->ssrc();
        }
        if (header->payloadType() != videoPayloadType_)
            continue; // RTX/other payloads are handled by VideoAssembler
        if (header->ssrc() != videoSsrc_)
            continue;
        packet(data, size);
        {
            std::lock_guard lock(mutex_);
            latestTimestamp_ = header->timestamp();
        }
        if (diag_) {
            std::lock_guard lock(mutex_);
            if (dump_.size() + size + 8 <= dumpLimit_) {
                appendU32(dump_, uint32_t(now));
                appendU32(dump_, uint32_t(size));
                dump_.insert(dump_.end(), data, data + size);
            }
        }
    }
}
void RtpTrackStats::info(const uint8_t *data, size_t size) {
    if (size < sizeof(rtc::RtcpHeader))
        return;
    const auto *rtcp = reinterpret_cast<const rtc::RtcpHeader *>(data);
    if (rtcp->version() != 2)
        return;
    if (rtcp->payloadType() != 200 || size < sizeof(rtc::RtcpSr))
        return;
    const auto *sr = reinterpret_cast<const rtc::RtcpSr *>(data);
    std::lock_guard lock(mutex_);
    ++srCount_;
    if (!haveSr_) {
        haveSr_ = true;
        srPacketsBase_ = sr->packetCount();
        receivedAtSrBase_ = received_;
    }
    srPackets_ = sr->packetCount();
    srOctets_ = sr->octetCount();
}
void RtpTrackStats::packet(const uint8_t *data, size_t size) {
    if (size < sizeof(rtc::RtpHeader))
        return;
    const auto *header = reinterpret_cast<const rtc::RtpHeader *>(data);
    if (header->version() != 2)
        return;
    const auto seq = header->seqNumber();
    const auto timestamp = header->timestamp();
    const auto now = monotonicMs();
    std::lock_guard lock(mutex_);
    ++received_;
    ++windowReceived_;
    windowBytes_ += size;
    if (!started_) {
        started_ = true;
        expected_ = uint32_t(seq) + 1;
        windowStart_ = now;
        firstPacketMs_ = now;
    } else {
        const int16_t delta = int16_t(uint16_t(seq) - uint16_t(expected_));
        if (delta >= 0) {
            if (delta > 0) {
                const auto missing = uint16_t(delta);
                lost_ += missing;
                windowLost_ += missing;
                ++gaps_;
                if (rtx_ && name_ == "video") {
                    for (uint16_t i = 0; i < missing && pendingNack_.size() < 64; ++i)
                        pendingNack_.push_back(uint16_t(expected_ + i));
                }
            }
            expected_ = uint32_t(seq) + 1;
        } else {
            ++reorders_;
        }
    }
    if (haveLastSeq_ && seq == lastSeq_)
        ++duplicates_;
    haveLastSeq_ = true;
    lastSeq_ = seq;
    lastPacketMs_ = now;
    ssrcs_.insert(header->ssrc());
    payloadTypes_.insert(int(header->payloadType()));
    // RFC 3550 interarrival jitter with signed 32-bit timestamp extension.
    // Arrival time is monotonic; wall-clock adjustments must not create spikes.
    jitterTracker_.observe(now, timestamp);
}
NetworkStats RtpTrackStats::snapshot() {
    NetworkStats out;
    std::lock_guard lock(mutex_);
    const auto now = monotonicMs();
    if (started_ && now - windowStart_ >= 500) {
        const auto elapsed = now - windowStart_;
        const auto total = windowReceived_ + windowLost_;
        windowLoss_ = total ? double(windowLost_) * 100.0 / double(total) : -1;
        windowJitter_ = jitterTracker_.jitterMs();
        windowBitrate_ = double(windowBytes_) * 8.0 * 1000.0 / double(elapsed) / 1000000.0;
        windowStart_ = now;
        windowReceived_ = windowLost_ = windowBytes_ = 0;
    }
    out.lossPercent = windowLoss_;
    out.jitterMs = windowJitter_;
    out.bitrateMbps = windowBitrate_;
    out.packets = received_;
    out.lost = lost_;
    out.nackSent = nacksSent_;
    out.rtxRecovered = rtxPackets_;
    return out;
}
void RtpTrackStats::flushDiagnostics() {
    if (!diag_)
        return;
    std::lock_guard lock(mutex_);
    if (dump_.empty() && received_ == 0)
        return;
#ifdef _WIN32
    _mkdir(diagDirectory());
#else
    mkdir(diagDirectory(), 0777);
#endif
    const std::string base = diagDirectory() + std::string("/") + name_;
    {
        std::ofstream file(base + "_rtp.bin", std::ios::binary);
        if (file)
            file.write(reinterpret_cast<const char *>(dump_.data()), std::streamsize(dump_.size()));
    }
    {
        std::ofstream file(base + "_info.txt");
        if (file) {
            file << "name=" << name_ << "\n";
            file << "clock_rate=" << clockRate_ << "\n";
            file << "packets=" << received_ << "\n";
            file << "bytes=" << dump_.size() << "\n";
            file << "window_lost_total=" << lost_ << "\n";
            file << "sequence_gaps_before_repair=" << lost_ << "\n";
            file << "loss_semantics=initial_sequence_gaps_before_late_or_rtx_repair\n";
            file << "sequence_gap_percent_before_repair=" <<
                (received_ + lost_ ? double(lost_) * 100 / double(received_ + lost_) : 0) << '\n';
            file << "gap_events=" << gaps_ << "\n";
            file << "reorders=" << reorders_ << "\n";
            file << "duplicates=" << duplicates_ << "\n";
            file << "first_ms=" << firstPacketMs_ << "\n";
            file << "last_ms=" << lastPacketMs_ << "\n";
            file << "ssrcs=";
            for (auto ssrc : ssrcs_)
                file << ssrc << ",";
            file << "\npayload_types=";
            for (auto pt : payloadTypes_)
                file << pt << ",";
            file << "\nsr_count=" << srCount_ << "\n";
            file << "sr_packets=" << srPackets_ << "\n";
            file << "sr_octets=" << srOctets_ << "\n";
            file << "sr_packets_base=" << srPacketsBase_ << "\n";
            file << "received_at_sr_base=" << receivedAtSrBase_ << "\n";
            if (haveSr_ && srPackets_ >= srPacketsBase_) {
                const auto sent = uint64_t(srPackets_ - srPacketsBase_);
                const auto got = received_ - receivedAtSrBase_;
                file << "sr_interval_sent=" << sent << "\n";
                file << "sr_interval_received=" << got << "\n";
                file << "sr_unaligned_difference_percent_not_final_loss="
                     << (sent ? (double(sent > got ? sent - got : 0) * 100.0 / double(sent)) : 0.0)
                     << "\n";
            }
            file << "nacks_sent=" << nacksSent_ << "\n";
            file << "rtx_packets=" << rtxPackets_ << "\n";
            file << "dump_bytes=" << dump_.size() << "\n";
        }
    }
}
} // namespace uu
