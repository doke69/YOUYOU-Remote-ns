#include "strict_video_receiver.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <rtc/rtp.hpp>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace uu {
namespace {
int64_t clockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t clockUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void nal(std::vector<uint8_t> &out, RtpPayloadView data) {
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), data.begin(), data.end());
}
// first_mb_in_slice is ue(v); a leading 1 bit encodes zero. This is a
// conservative recovery anchor for the negotiated ordinary AVC stream.
bool firstSlice(RtpPayloadView p) {
    const auto type = p.empty() ? 0 : p[0] & 31;
    if (type == 5) return p.size() > 1 && (p[1] & 0x80);
    return type == 28 && p.size() > 2 && (p[1] & 0x80) && (p[1] & 31) == 5 && (p[2] & 0x80);
}
bool bootstrap(RtpPayloadView p, bool hevc) {
    if(hevc) {
        if(p.size()<3 || (p[0]&128) || (p[0]&1) || (p[1]&0xf8) || !(p[1]&7)) return false;
        const int type=(p[0]>>1)&63;
        // Parameter-set start, or proven first slice of IDR/BLA. CRA is not a
        // recovery anchor: following RASL may reference pictures before CRA.
        if(type==32) return true;
        if(type>=16 && type<=20) return p[2]&128;
        if(type==49 && p.size()>3 && (p[2]&128)) {
            const int fragment=p[2]&63;
            return fragment==32 || (fragment>=16 && fragment<=20 && (p[3]&128));
        }
        if(type==48 && p.size()>6) {
            const size_t length=be16(p.data()+2);
            if(length<3 || 4+length>p.size() || ((p[4]>>1)&63)>=48) return false;
            return bootstrap(RtpPayloadView(p.data()+4,length),true);
        }
        return false;
    }
    if (p.empty()) return false;
    const auto type = p[0] & 31;
    if (type == 7) return true;
    if (type == 24 && p.size() > 3 && (p[3] & 31) == 7) return true;
    return firstSlice(p);
}
bool possibleRecoveryPacket(RtpPayloadView p,bool hevc) {
    if(p.empty()) return false;
    if(hevc) return bootstrap(p,true);
    const auto type=p[0]&31;
    return type==5 || type==24 || (type==28 && p.size()>1 && (p[1]&31)==5);
}
#ifdef UU_RECOVERY_FOCUS
// Only ordinary base-layer VCL packets are classified. Aggregation packets,
// parameter sets, SEI and unknown layouts remain on the original recovery path.
bool dependentPicturePacket(RtpPayloadView p, bool hevc) {
    if (hevc) {
        if (p.size()<3 || (p[0]&128) || (p[0]&1) || (p[1]&0xf8) || !(p[1]&7)) return false;
        const unsigned type=(p[0]>>1)&63;
        if (type<=9) return true;
        // A middle/end FU still identifies the original NAL unit type.
        if (type==49 && p.size()>3 && (p[2]&0xc0)!=0xc0) return (p[2]&63)<=9;
        return false;
    }
    if (p.size()<2 || (p[0]&128)) return false;
    const unsigned type=p[0]&31;
    if (type==1) return true;
    return type==28 && p.size()>2 && !(p[1]&32) && (p[1]&0xc0)!=0xc0 && (p[1]&31)==1;
}
#endif
const char *directory() {
#ifdef __SWITCH__
    return "sdmc:/switch/UURemote-ns/diag";
#else
    return "diag";
#endif
}
} // namespace

StrictVideoReceiver::StrictVideoReceiver(bool rtx, int ms, Output out, bool hevc, bool repairPaced, ValidatedOutput validatedOutput)
    : hevc_(hevc), repairPaced_(repairPaced), rtx_(rtx), deadlineMs_(std::clamp(ms, 100, 1200)), output_(std::move(out)), validatedOutput_(std::move(validatedOutput)) {
    startedAtMs_ = clockMs();
    repairRefillAt_ = repairWindowAt_ = startedAtMs_;
    noteEvent("constructed", startedAtMs_);
    // Only a tiny instance marker on setup; bounded traces are saved at shutdown.
#ifdef _WIN32
    _mkdir(directory());
#else
    mkdir(directory(), 0777);
#endif
    std::ofstream marker(std::string(directory()) + "/receiver_session.txt");
    marker << "diagnostic_revision=rx54\nreceiver_started_ms=" << startedAtMs_
           << "\ndeadline_ms=" << deadlineMs_ << '\n';
    marker.flush();
    sessionMarkerWritten_ = bool(marker);
}
StrictVideoReceiver::~StrictVideoReceiver() { shutdown(); }
void StrictVideoReceiver::shutdown() {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    send_ = {};
    noteEvent("shutdown", clockMs());
    flush();
}
void StrictVideoReceiver::setSources(uint32_t primary, uint32_t retransmission) {
    std::lock_guard lock(mutex_);
    // SDP can arrive after a first packet. Never silently replace a bound SSRC.
    if (primary && (!primary_ || primary_ == primary)) primary_ = primary;
    if (retransmission) rtxSource_ = retransmission;
}
void StrictVideoReceiver::requestRecovery() {
    std::lock_guard lock(mutex_);
    if (!waitingIdr_) noteEvent("decoder_recovery", clockMs());
    waitingIdr_ = true;
}
void StrictVideoReceiver::setRtt(int milliseconds) {
    std::lock_guard lock(mutex_);
    if(milliseconds>0) {
        fallbackRttMs_=std::clamp(milliseconds,10,300);
        haveMeasuredRtt_=true;
    }
}
int StrictVideoReceiver::frameDeadlineMs() const {
    // Keep the configured startup allowance until the path is measured. A
    // low-RTT stream should not retain a broken reference frame for 600ms:
    // repeated NACKs have several repair round trips within this budget.
    if (!haveMeasuredRtt_) return deadlineMs_;
    const double repair = repairSamples_ >= 4 ? repairRttMs_ : 0.0;
    const double latency = std::max(double(fallbackRttMs_), repair);
    const int budget = std::clamp(int(latency * 4.0 + 40.0), 160, 600);
    return std::min(deadlineMs_, budget);
}
int StrictVideoReceiver::retryDelayMs() const {
    // Prefer measured NACK-to-RTX response time. Do not estimate from a
    // multiply-requested packet: its response cannot identify which attempt.
    const double estimate=repairSamples_>=4?repairRttMs_:fallbackRttMs_;
    return std::clamp(int(estimate*1.25+5),20,400);
}
int StrictVideoReceiver::adaptiveRetryDelayMs(int64_t now, int64_t createdAt) const {
    const int base = retryDelayMs();
    const int64_t remaining = int64_t(frameDeadlineMs()) - (now - createdAt);
    if (remaining <= 0) return base;
    // Keep at least one request+return inside what is left of the deadline.
    // The old fixed delay could exceed the remaining budget, which made a
    // second attempt unreachable when the measured repair RTT was high.
    const int half = int(std::clamp<int64_t>(remaining / 2 - 5, 10, base));
    return std::min(base, half);
}
StrictVideoReceiver::Stats StrictVideoReceiver::snapshot() {
    std::lock_guard lock(mutex_);
    return Stats{accepted_, dropped_, waiting_, fastForwards_, nacks_.load(), recovered_.load(),
                 accepted_ ? double(waitTotalMs_) / accepted_ : 0, recentWaitMs_, maxWaitMs_,
                 int(frames_.size()),frames_.empty()?0:int(clockMs()-frames_.begin()->second.created),
                 retryDelayMs(),repairRttMs_,repeatedRequests_,pliRequests_,
                 buffered_,bufferedHighBytes_,inputMessages_,inputBatches_,maxBatch_,
                 incomingCalls_,tickCalls_,incomingTotalUs_,incomingMaxUs_,tickTotalUs_,tickMaxUs_,
                 memoryBoundRetires_,maxFrameBytes_,repairPaced_,nackSequences_,budgetDeferrals_,expiredRepairScans_};
}

void StrictVideoReceiver::incoming(rtc::message_vector &messages, const rtc::message_callback &send) {
    const auto processingStarted=clockUs();
    std::lock_guard lock(mutex_);
    ++incomingCalls_;
    ++inputBatches_;
    inputMessages_ += messages.size();
    maxBatch_ = std::max<uint64_t>(maxBatch_, messages.size());

    // Diagnostic: warn about abnormally large RTP batches. Normal batches are <50 packets;
    // >200 may indicate severe network jitter or buffering issues requiring investigation.
    if (messages.size() > 200 && messages.size() == maxBatch_) {
        diagnostic(("receiver-large-batch-" + std::to_string(messages.size())).c_str());
    }

    rtc::message_vector packets;
    packets.swap(messages);
    if (stopped_) {
        const auto wall=uint64_t(std::max<int64_t>(0,clockUs()-processingStarted));
        incomingTotalUs_+=wall;incomingMaxUs_=std::max(incomingMaxUs_,wall);return;
    }
    send_ = send;
    const auto now = clockMs();
    for (const auto &packet : packets) {
        if (!packet || packet->type != rtc::Message::Binary) continue;
        const auto *data = reinterpret_cast<const uint8_t *>(packet->data());
        const size_t size = packet->size();
        if (size < 12 || data[0] >> 6 != 2) { ++invalid_; continue; }
        size_t header = 12 + 4 * (data[0] & 15), end = size;
        if (header > size) { ++invalid_; continue; }
        if (data[0] & 16) {
            if (header + 4 > size) { ++invalid_; continue; }
            header += 4 + size_t(be16(data + header + 2)) * 4;
        }
        if (header > size) { ++invalid_; continue; }
        if (data[0] & 32) {
            const auto pad = data[size - 1];
            if (!pad || pad > end - header) { ++invalid_; continue; }
            end -= pad;
        }
        const auto pt = data[1] & 127;
        const auto ssrc = be32(data + 8), ts = be32(data + 4);
        uint16_t seq = be16(data + 2);
        const bool retransmission = pt == 97;
        if (pt == 96) {
            if (!primary_) primary_ = ssrc;
            if (ssrc != primary_) { ++invalid_; continue; }
        } else if (retransmission && rtx_ && primary_) {
            if (rtxSource_ && ssrc != rtxSource_) { ++invalid_; continue; }
            if (header + 2 >= end) { ++invalid_; continue; }
            seq = be16(data + header);
            header += 2;
        } else { ++invalid_; continue; }
        if (header >= end) { ++invalid_; continue; }
        const int64_t ext = haveSeq_ ? highestSeq_ + int16_t(uint16_t(seq - uint16_t(highestSeq_))) : seq;
        const int64_t time = haveTs_ ? highestTs_ + int32_t(ts - uint32_t(highestTs_)) : ts;
        if (haveClosed_ && time <= closedTs_) { ++stale_; continue; }
        if (haveSeq_ && (ext < highestSeq_ - 8192 || ext > highestSeq_ + 8192)) { ++invalid_; continue; }
        // RTX may create a frame when every primary packet of that AU was lost:
        // RFC 4588 preserves the original timestamp and marker, and the bounded
        // ext window plus frame/memory limits keep this from growing unbounded.
        if (retransmission) {
            if (ext > highestSeq_ || ext < highestSeq_ - 8192) continue;
            auto f = frames_.find(time);
            if (f != frames_.end() && f->second.packets.count(ext)) continue;
        }
        if (!haveSeq_ || ext > highestSeq_) highestSeq_ = ext;
        if (!haveTs_ || time > highestTs_) highestTs_ = time;
        haveSeq_ = haveTs_ = true;
        auto [it, inserted] = frames_.try_emplace(time);
        auto &f = it->second;
        if (inserted) {
            f.timestamp = ts; f.created = now;
            if (retransmission) ++rtxAbsentFrames_;
        }
        // The common in-order arrival belongs at end(). For reordered/duplicate
        // packets, reuse lower_bound for both duplicate detection and insertion.
        auto position = f.packets.end();
        if (!f.packets.empty() && ext <= f.packets.rbegin()->first)
            position = f.packets.lower_bound(ext);
        if (position != f.packets.end() && position->first == ext) { ++duplicates_; continue; }
        // Charge the retained allocation and conservative object/node overhead,
        // not only payload bytes (RTP extensions/padding may otherwise evade bounds).
        const size_t retainedCost=packet->size()+sizeof(rtc::Message)+sizeof(Packet)+64;

        // Adaptive memory limits based on observed frame sizes. Start conservatively
        // with 3MB/frame (accommodates both 720p and most 1080p frames), then upgrade
        // to 4MB if larger frames are observed. Using packet->size() rather than
        // capacity() prevents over-conservative retirement.
        const size_t maxFrameBytes = maxFrameBytes_ < 3 * 1024 * 1024 ? 3 * 1024 * 1024 : 4 * 1024 * 1024;
        const size_t maxBuffered = maxFrameBytes_ < 3 * 1024 * 1024 ? 12 * 1024 * 1024 : 16 * 1024 * 1024;

        if (f.bytes + end - header > maxFrameBytes || f.packets.size() >= 4096 ||
            buffered_ + end - header > maxBuffered ||
            retainedCost > maxBuffered || retained_ > maxBuffered - retainedCost || frames_.size() > 90) {
            ++memoryBoundRetires_;
            maxFrameBytes_ = std::max<uint64_t>(maxFrameBytes_, f.bytes + end - header);
            retire(it, "memory_bound");
            continue;
        }
        if ((data[1] & 128) && f.marker >= 0 && f.marker != ext) {
            ++markerConflicts_;
            retire(it, "conflicting_marker");
            continue;
        }
        const auto added=f.packets.emplace_hint(position, ext, Packet{packet, RtpPayloadView(data+header,end-header), bool(data[1]&128)});
        ++f.revision;
        if(!f.possibleIdr)f.possibleIdr=possibleRecoveryPacket(added->second.payload,hevc_);
#ifdef UU_RECOVERY_FOCUS
        if (!f.dependentVcl) f.dependentVcl=dependentPicturePacket(added->second.payload,hevc_);
#endif
        f.retainedBytes+=retainedCost;retained_+=retainedCost;
        f.bytes += end - header;
        maxFrameBytes_ = std::max<uint64_t>(maxFrameBytes_, f.bytes);
        buffered_ += end - header;
        bufferedHighBytes_ = std::max(bufferedHighBytes_, buffered_);
        if (data[1] & 128) f.marker = ext;
        const auto requested=requested_.find(ext);
        if(retransmission && requested!=requested_.end() && requested->second.attempts==1) {
            const auto elapsed=now-requested->second.sentAt;
            if(elapsed>0 && elapsed<=1000) {
                repairRttMs_=repairSamples_?repairRttMs_*.9+elapsed*.1:double(elapsed);
                ++repairSamples_;
            }
        }
        if (requested != requested_.end()) requested_.erase(requested);
        if (retransmission) ++recovered_;
        else if(repairPaced_) ++repairPrimaryPackets_;
    }
    process(now);
    const auto wall=uint64_t(std::max<int64_t>(0,clockUs()-processingStarted));
    incomingTotalUs_+=wall;incomingMaxUs_=std::max(incomingMaxUs_,wall);
}

bool StrictVideoReceiver::assemble(const Frame &f, int64_t first, std::vector<uint8_t> &out,
                                   bool &idr, std::vector<uint8_t> &sps, std::vector<uint8_t> &pps, std::vector<uint8_t> &vps) const {
    if(hevc_) return assembleHevc(f,first,out,idr,sps,pps,vps);
    // Match receiver's adaptive 4MB frame limit for 1080p high bitrate support
    constexpr size_t maxFrameSize = 4 * 1024 * 1024;
    out.reserve(std::min<size_t>(maxFrameSize,f.bytes+4*f.packets.size()));
    std::vector<uint8_t> fragment;
    bool fragmentOpen = false;
    uint8_t fragmentHeader = 0;
    auto append = [&](RtpPayloadView n) {
        if (n.empty() || (n[0] & 128)) return false;
        const auto type = n[0] & 31;
        if (!type || type >= 24) return false;
        if (out.size() + 4 + n.size() > maxFrameSize) return false;
        if (type == 5) idr = true;
        if (type == 7) sps.assign(n.begin(),n.end());
        if (type == 8) pps.assign(n.begin(),n.end());
        nal(out, n);
        return true;
    };
    if (first > f.marker || f.marker - first >= 4096) return false;
    auto packet = f.packets.lower_bound(first);
    for (int64_t seq = first; seq <= f.marker; ++seq) {
        if (packet == f.packets.end() || packet->first != seq) return false;
        const auto &p = (packet++)->second.payload;
        if (p.empty() || (p[0] & 128)) return false;
        const auto type = p[0] & 31;
        if (type == 28) {
            if (p.size() < 3 || (p[1] & 32)) return false;
            const bool start = p[1] & 128, end = p[1] & 64;
            const uint8_t head = (p[0] & 0xe0) | (p[1] & 31);
            if ((start && end) || (head & 31) == 0 || (head & 31) >= 24) return false;
            if (start) {
                if (fragmentOpen) return false;
                fragment = {head}; fragmentHeader = head; fragmentOpen = true;
            } else if (!fragmentOpen || head != fragmentHeader) return false;
            fragment.insert(fragment.end(), p.begin() + 2, p.end());
            if (end) {
                if (!append(fragment)) return false;
                fragment.clear(); fragmentOpen = false;
            }
        } else if (type == 24) {
            if (fragmentOpen) return false;
            size_t pos = 1;
            while (pos + 2 <= p.size()) {
                size_t len = be16(p.data() + pos); pos += 2;
                if (!len || pos + len > p.size()) return false;
                if (!append(RtpPayloadView(p.data()+pos,len))) return false;
                pos += len;
            }
            if (pos != p.size() || pos == 1) return false;
        } else {
            if (fragmentOpen || !append(p)) return false;
        }
    }
    return !fragmentOpen && !out.empty();
}

bool StrictVideoReceiver::assembleHevc(const Frame &f, int64_t first, std::vector<uint8_t> &out,
                                      bool &idr, std::vector<uint8_t> &sps,
                                      std::vector<uint8_t> &pps, std::vector<uint8_t> &vps) const {
    // Match receiver's adaptive 4MB frame limit for 1080p high bitrate support
    constexpr size_t maxFrameSize = 4 * 1024 * 1024;
    out.reserve(std::min<size_t>(maxFrameSize,f.bytes+4*f.packets.size()));
    // RFC7798 section4.4: SRST, no decoding-order-number field, layer0.
    // Bound output as well as input: aggregation adds Annex-B start codes.
    auto validHeader=[](RtpPayloadView n) {
        return n.size()>=3 && !(n[0]&128) && !(n[0]&1) && !(n[1]&0xf8) && (n[1]&7);
    };
    auto append=[&](RtpPayloadView n) {
        if(!validHeader(n) || out.size()+4+n.size()>maxFrameSize) return false;
        const int type=(n[0]>>1)&63;
        if(type>=48) return false;
        // CRA/RASL open-GOP recovery is intentionally not accepted as IDR.
        // BLA/IDR anchors are sufficient for closed-GOP desktop streaming.
        if(type>=16 && type<=20) idr=true;
        if(type==32) vps.assign(n.begin(),n.end());
        if(type==33) sps.assign(n.begin(),n.end());
        if(type==34) pps.assign(n.begin(),n.end());
        nal(out,n);return true;
    };
    if(first>f.marker || f.marker-first>=4096) return false;
    bool open=false;
    std::vector<uint8_t> fragment;
    uint8_t header0=0,header1=0;
    auto packet=f.packets.lower_bound(first);
    for(int64_t seq=first;seq<=f.marker;++seq) {
        if(packet==f.packets.end() || packet->first!=seq) return false;
        const auto &p=(packet++)->second.payload;
        if(!validHeader(p)) return false;
        const int type=(p[0]>>1)&63;
        if(type==49) {
            if(p.size()<4) return false;
            const bool start=p[2]&128,end=p[2]&64;
            const int unit=p[2]&63;
            const uint8_t h0=(p[0]&0x81)|(unit<<1),h1=p[1];
            if(unit>=48 || (start && end)) return false;
            if(start) {
                if(open) return false;
                fragment={h0,h1};header0=h0;header1=h1;open=true;
            } else if(!open || h0!=header0 || h1!=header1) return false;
            if(fragment.size()+p.size()-3>maxFrameSize) return false;
            fragment.insert(fragment.end(),p.begin()+3,p.end());
            if(end) {
                if(!append(fragment)) return false;
                open=false;fragment.clear();
            }
        } else if(type==48) {
            if(open) return false;
            size_t pos=2;unsigned units=0;
            while(pos+2<=p.size()) {
                const size_t length=be16(p.data()+pos);pos+=2;
                if(length<3 || length>p.size()-pos) return false;
                if(!append(RtpPayloadView(p.data()+pos,length))) return false;
                pos+=length;++units;
            }
            if(pos!=p.size() || units<2) return false;
        } else if(type<48) {
            if(open || !append(p)) return false;
        } else return false; // PACI and reserved packet types not negotiated.
    }
    return !open && !out.empty();
}

void StrictVideoReceiver::retire(std::map<int64_t, Frame>::iterator it, const char *reason) {
    if (std::string(reason) != "complete" && std::string(reason) != "wait_idr")
        noteEvent(reason, clockMs());
    auto &f = it->second;
    if (trace_.size() >= 1500) trace_.pop_front();
    trace_.push_back(std::to_string(f.timestamp) + "," + std::to_string(f.packets.size()) + "," +
                         std::to_string(f.bytes) + "," + std::to_string(clockMs() - f.created) + "," + reason);
    // If a future frame is forcibly evicted, don't invalidate earlier queued frames.
    // Normal retirement is always in timestamp order; pressure clears all queued work.
    if (it != frames_.begin()) {
        dropped_ += frames_.size();
        closedTs_ = frames_.rbegin()->first;
        haveClosed_ = true; haveBoundary_ = false;
        frames_.clear(); buffered_ = retained_ = 0; requested_.clear(); waitingIdr_ = true;
        return;
    }
    haveClosed_ = true; closedTs_ = it->first;
    haveBoundary_ = f.marker >= 0;
    if (haveBoundary_) nextSeq_ = f.marker + 1;
    buffered_ -= f.bytes;retained_-=f.retainedBytes;
    frames_.erase(it);
    if (std::string(reason) != "complete") { ++dropped_; waitingIdr_ = true; }
}

bool StrictVideoReceiver::skipToReadyIdr(int64_t now) {
    if (frames_.size() < 2 || now - frames_.begin()->second.created < 80) return false;
    for (auto it = std::next(frames_.begin()); it != frames_.end(); ++it) {
        auto &f = it->second;
        if (f.packets.empty() || f.marker < 0) continue;
        if(!f.possibleIdr) continue;
        // Do not re-assemble an unchanged candidate on every packet: the old
        // path could copy a 0.1-0.35 MB AU thousands of times per second while
        // the head was blocked. A new packet or a parameter-set update clears
        // the memo below.
        if (f.checkedRevision == f.revision) continue;
        f.checkedRevision = f.revision;
        ++idrSkipAttempts_;
        const auto first = f.packets.begin()->first;
        const auto &previous = std::prev(it)->second;
        // Prove this AU starts at a known boundary or a conservative bootstrap
        // anchor; missing leading slices must never become an accepted IDR.
        const bool anchored = (previous.marker >= 0 && previous.marker + 1 == first) ||
                              bootstrap(f.packets.begin()->second.payload,hevc_);
        if (!anchored || f.marker < first || f.marker - first >= 4096 ||
            f.packets.size() != size_t(f.marker - first + 1)) { ++idrSkipFailures_; continue; }
        std::vector<uint8_t> out, sps, pps, vps;
        bool idr = false;
        if (!assemble(f, first, out, idr, sps, pps, vps) || !idr ||
            (sps.empty() && sps_.empty()) || (pps.empty() && pps_.empty()) ||
            (hevc_ && vps.empty() && vps_.empty())) { ++idrSkipFailures_; continue; }
        const auto target = it->first;
        while (frames_.begin()->first != target) retire(frames_.begin(), "superseded_by_idr");
        haveBoundary_ = true;
        nextSeq_ = first;
        waitingIdr_ = true;
        ++fastForwards_;
        return true;
    }
    return false;
}
void StrictVideoReceiver::process(int64_t now) {
    unsigned delivered = 0;
    const int deadline = frameDeadlineMs();
    // NACK while a frame still exists, including when no new RTP is arriving.
    feedback(now);
    while (!frames_.empty()) {
        auto it = frames_.begin();
        auto &f = it->second;
#ifdef UU_RECOVERY_FOCUS
        // This receiver already forbids delivering non-IDR pictures after a
        // broken reference chain. Do not spend a repair deadline on those AUs.
        if (repairPaced_ && waitingIdr_ && f.dependentVcl && !f.possibleIdr) {
            ++recoveryDiscarded_;
            retire(it, "wait_idr"); // Keep ordinary waiting drops out of the event ring.
            continue;
        }
#endif
        const auto observed = f.packets.empty() ? -1 : f.packets.begin()->first;
        int64_t first = haveBoundary_ ? nextSeq_ : observed;
        bool anchored = haveBoundary_ || (observed >= 0 && bootstrap(f.packets.begin()->second.payload,hevc_));
        if (observed >= 0 && observed < first) { retire(it, "sequence_overlap"); continue; }
        bool contiguous = anchored && first >= 0 && f.marker >= first && f.marker - first < 4096 &&
                          f.packets.size() == size_t(f.marker - first + 1);
        // Unique sorted keys, exact cardinality and matching endpoints prove
        // the whole integer interval is present. assemble() still parses every NAL.
        if(contiguous) contiguous=f.packets.begin()->first==first && f.packets.rbegin()->first==f.marker;
        if (contiguous) {
            std::vector<uint8_t> out, sps, pps, vps;
            bool idr = false;
            if (!assemble(f, first, out, idr, sps, pps, vps)) { retire(it, hevc_?"invalid_hevc":"invalid_h264"); continue; }
            if (!vps.empty()) vps_ = vps;
            if (!sps.empty()) sps_ = sps;
            if (!pps.empty()) pps_ = pps;
            if (!vps.empty() || !sps.empty() || !pps.empty()) {
                // Parameter sets changed: allow the IDR-fast-forward candidates
                // to be validated again even if their own packet set did not.
                for (auto &entry : frames_) entry.second.checkedRevision = 0;
            }
            if (waitingIdr_) {
                if (!idr || sps_.empty() || pps_.empty() || (hevc_ && vps_.empty())) { ++waiting_; retire(it, "wait_idr"); continue; }
                std::vector<uint8_t> recovery;
                if (hevc_ && vps.empty()) nal(recovery,vps_);
                if (sps.empty()) nal(recovery, sps_);
                if (pps.empty()) nal(recovery, pps_);
                recovery.insert(recovery.end(), out.begin(), out.end());
                out.swap(recovery);
                waitingIdr_ = false; ++recoveries_;
            }
            if (validatedOutput_) validatedOutput_(out.data(), out.size(), f.timestamp, idr);
            else output_(out.data(), out.size(), f.timestamp);
            ++delivered;
            recentWaitMs_ = int(now - f.created);
            waitTotalMs_ += recentWaitMs_;
            maxWaitMs_ = std::max(maxWaitMs_, recentWaitMs_);
            ++accepted_;
            retire(it, "complete");
            continue;
        }
        if (skipToReadyIdr(now)) continue;
        // Ask for a recovery point before the old deadline expires, while
        // still allowing RTX to complete this frame in parallel.
        if (now - f.created >= 120 && (f.marker >= 0 || frames_.size() > 1)) pli(now, "pre_deadline_pli");
        if (now - f.created >= deadline) { retire(it, anchored ? "missing_packets" : "unproven_start"); continue; }
        break;
    }
    if (delivered >= 8) noteEvent("delivery_burst", now, delivered);
    for (auto it = requested_.begin(); it != requested_.end();) {
        if (frames_.empty() || it->first < (haveBoundary_ ? nextSeq_ : frames_.begin()->second.packets.begin()->first))
            it = requested_.erase(it);
        else ++it;
    }
    if (waitingIdr_) pli(now);
}

void StrictVideoReceiver::pli(int64_t now, const char *reason) {
    if (!send_ || !primary_) return;
    // Two separate credits. A probe sent while an incomplete frame still has a
    // chance must not consume the recovery request that follows retirement.
    constexpr int64_t probeIntervalMs = 700;
    constexpr int64_t recoveryIntervalMs = 400;
    const bool recovery = waitingIdr_;
    int64_t &last = recovery ? lastRecoveryPli_ : lastProbePli_;
    const int64_t interval = recovery ? recoveryIntervalMs : probeIntervalMs;
    if (now - last < interval) return;
    auto packet = rtc::make_message(rtc::RtcpPli::Size(), rtc::Message::Control);
    reinterpret_cast<rtc::RtcpPli *>(packet->data())->preparePacket(primary_);
    send_(packet); last = now; lastPli_ = now; ++pliRequests_;
    if (recovery) ++recoveryPli_; else ++probePli_;
    noteEvent(reason, now);
}
void StrictVideoReceiver::noteEvent(const char *reason, int64_t now, unsigned delivered) {
    if (events_.size() >= 256) { events_.pop_front(); ++eventsDropped_; }
    events_.push_back(std::to_string(now) + "," + reason + "," + std::to_string(frameDeadlineMs()) + "," +
        std::to_string(frames_.size()) + "," +
        std::to_string(frames_.empty() ? 0 : now - frames_.begin()->second.created) + "," +
        std::to_string(waitingIdr_ ? 1 : 0) + "," + std::to_string(lastPli_) + "," + std::to_string(delivered));
}
void StrictVideoReceiver::refillRepairBudget(int64_t now) {
    if(!repairPaced_)return;
    const auto elapsed=std::max<int64_t>(0,now-repairRefillAt_);
    repairTokens_=std::min(32.0,repairTokens_+double(elapsed)*repairRatePps_/1000.0);
    repairRefillAt_=now;
    const auto window=now-repairWindowAt_;
    if(window>=1000) {
        // Received primary packet rate is a repair-budget input, not a
        // capacity estimate. A quarter-rate budget limits feedback-driven
        // amplification; 64pps floor allows recovery after a brief outage.
        repairRatePps_=std::clamp(double(repairPrimaryPackets_)*250.0/double(window),64.0,512.0);
        repairPrimaryPackets_=0;repairWindowAt_=now;
    }
}
void StrictVideoReceiver::feedback(int64_t now) {
    if (!rtx_ || !send_ || !primary_ || frames_.empty() || now - lastNack_ < 10) return;
    // One scan per 10 ms in every mode. In preset (non-paced) mode the old
    // code had no scan gate and could walk every buffered frame on every
    // incoming packet; that work is never useful more than 100 times/s.
    if (now - repairScanAt_ < 10) return;
    repairScanAt_ = now;
    refillRepairBudget(now);
    const unsigned budget=repairPaced_?unsigned(repairTokens_):544u;
    if(!budget){++budgetDeferrals_;return;}
    unsigned sequences=0;
    std::vector<std::pair<uint16_t, uint16_t>> parts;
    for (auto it = frames_.begin(); it != frames_.end() && parts.size() < 32 && sequences < budget; ++it) {
        auto &f = it->second;
#ifdef UU_RECOVERY_FOCUS
        if (repairPaced_ && waitingIdr_ && f.dependentVcl && !f.possibleIdr) {
            ++recoverySkippedRepairScans_;
            continue;
        }
#endif
        if (f.packets.empty() || now - f.created < 12) continue;
        const int retryDelay = adaptiveRetryDelayMs(now, f.created);
        // Do not request repairs whose estimated return is already beyond
        // this AU's existing deadline. Retirement/IDR policy stays unchanged.
        if (frameDeadlineMs() - (now - f.created) <= retryDelay) {
            ++expiredRepairScans_;continue;
        }
        int64_t start = f.packets.begin()->first;
        if (it == frames_.begin() && haveBoundary_) start = std::min(start, nextSeq_);
        else if (it != frames_.begin()) {
            const auto &prev = std::prev(it)->second;
            if (prev.marker >= 0) start = std::min(start, prev.marker + 1);
        }
        int64_t end = f.marker >= 0 ? f.marker : f.packets.rbegin()->first;
        // A missing marker can be recovered when the next frame is observed.
        const auto next = std::next(it);
        if (f.marker < 0 && next != frames_.end() && !next->second.packets.empty())
            end = std::max(end, next->second.packets.begin()->first - 1);
        if (end - start >= 4096) continue;
        auto present=f.packets.lower_bound(start);
        for (int64_t ext = start; ext <= end && parts.size() < 32 && sequences < budget; ++ext) {
            if(present!=f.packets.end() && present->first==ext){++present;continue;}
            const auto r = requested_.find(ext);
            if(r!=requested_.end()) {
                // Bound retries per sequence so a shrinking adaptive delay
                // cannot turn one loss into a retry storm.
                if(r->second.attempts>=kMaxRepairAttempts) continue;
                if(now-r->second.sentAt<retryDelay) continue;
            }
            auto &request=requested_[ext];
            if(request.attempts) ++repeatedRequests_;
            request.sentAt=now;++request.attempts;++sequences;
            const auto seq = uint16_t(ext);
            const uint16_t diff = parts.empty() ? 0 : uint16_t(seq - parts.back().first);
            if (parts.empty() || diff == 0 || diff > 16) parts.emplace_back(seq, 0);
            else parts.back().second |= uint16_t(1u << (diff - 1));
        }
    }
    if (parts.empty()) return;
    auto packet = rtc::make_message(rtc::RtcpNack::Size(unsigned(parts.size())), rtc::Message::Control);
    auto *nack = reinterpret_cast<rtc::RtcpNack *>(packet->data());
    nack->preparePacket(primary_, unsigned(parts.size()));
    for (size_t i = 0; i < parts.size(); ++i) { nack->parts[i].setPid(parts[i].first); nack->parts[i].setBlp(parts[i].second); }
    if(repairPaced_)repairTokens_=std::max(0.0,repairTokens_-sequences);
    nackSequences_+=sequences;
    send_(packet); ++nacks_; lastNack_ = now;
}
void StrictVideoReceiver::tick() {
    const auto processingStarted=clockUs();
    std::lock_guard lock(mutex_);
    ++tickCalls_;
    if (!stopped_) process(clockMs());
    const auto wall=uint64_t(std::max<int64_t>(0,clockUs()-processingStarted));
    tickTotalUs_+=wall;tickMaxUs_=std::max(tickMaxUs_,wall);
}
void StrictVideoReceiver::flush() {
#ifdef _WIN32
    _mkdir(directory());
#else
    mkdir(directory(), 0777);
#endif
    std::ofstream info(std::string(directory()) + "/receiver_v2.txt");
    info << "diagnostic_revision=rx54\nreceiver_started_ms=" << startedAtMs_
         << "\nsnapshot_ms=" << clockMs() << "\nstopped=" << stopped_ << '\n';
    info << "session_marker_written=" << sessionMarkerWritten_ << "\nevents_dropped=" << eventsDropped_ << '\n';
    info << "codec=" << (hevc_?"hevc":"h264") << "\n";
#ifdef UU_RECOVERY_FOCUS
    info << "recovery_focus=1\n";
#else
    info << "recovery_focus=0\n";
#endif
    info << "recovery_dependent_discarded=" << recoveryDiscarded_
         << "\nrecovery_skipped_repair_scans=" << recoverySkippedRepairScans_ << '\n';
    info << "receiver=strict-v2\naccepted=" << accepted_ << "\ndropped=" << dropped_
         << "\ninvalid=" << invalid_ << "\nstale=" << stale_ << "\nduplicates=" << duplicates_
         << "\nwaiting_idr=" << waiting_ << "\nrecoveries=" << recoveries_ << "\nnacks=" << nacks_.load()
         << "\nrtx_recovered=" << recovered_.load() << "\ndeadline_ms=" << deadlineMs_
         << "\neffective_deadline_ms=" << frameDeadlineMs() << "\nrtt_measured=" << haveMeasuredRtt_ << '\n';
    info << "fast_forwards=" << fastForwards_ << "\nrecent_wait_ms=" << recentWaitMs_
         << "\nmax_wait_ms=" << maxWaitMs_ << "\nmean_wait_ms="
         << (accepted_ ? double(waitTotalMs_) / accepted_ : 0) << '\n';
    info << "repair_rtt_ms=" << repairRttMs_ << "\nnack_retry_ms=" << retryDelayMs()
         << "\nrepeated_requests=" << repeatedRequests_ << "\npli_requests=" << pliRequests_ << '\n';
    info << "repair_paced=" << repairPaced_ << "\nnack_sequences=" << nackSequences_
         << "\nbudget_deferrals=" << budgetDeferrals_ << "\nexpired_repair_scans=" << expiredRepairScans_
         << "\nrepair_rate_pps=" << repairRatePps_ << '\n';
    info << "rtx_absent_frames=" << rtxAbsentFrames_ << "\nmarker_conflicts=" << markerConflicts_
         << "\nidr_skip_attempts=" << idrSkipAttempts_ << "\nidr_skip_failures=" << idrSkipFailures_
         << "\nprobe_pli=" << probePli_ << "\nrecovery_pli=" << recoveryPli_
         << "\nmax_repair_attempts=" << kMaxRepairAttempts << '\n';
    std::ofstream trace(std::string(directory()) + "/receiver_v2_frames.csv");
    trace << "rtp_timestamp,packets,bytes,age_ms,result\n";
    std::ofstream events(std::string(directory()) + "/receiver_events.csv");
    events << "time_ms,event,deadline_ms,pending,blocked_ms,waiting_idr,last_pli_ms,delivered\n";
    for (const auto &event : events_) events << event << '\n';
    for (const auto &line : trace_) trace << line << '\n';
}
} // namespace uu
