#include "video_repair_trace.hpp"
#include <algorithm>
#include <fstream>

namespace uu {
VideoRepairTrace::VideoRepairTrace():state_(std::make_unique<State>()) {}
void VideoRepairTrace::record(Event event) {
    auto &s=*state_;
    event.order=++s.stats.events;event.generation=s.stats.generation;
    if(s.eventCount==eventCapacity)++s.stats.overwritten;else ++s.eventCount;
    s.events[s.nextEvent]=event;s.nextEvent=(s.nextEvent+1)%eventCapacity;
}
VideoRepairTrace::Hole &VideoRepairTrace::findOrInsert(int64_t sequence) {
    auto &s=*state_;
    for(auto &h:s.holes)if(h.valid && h.sequence==sequence)return h;
    auto &h=s.holes[s.nextHole];s.nextHole=(s.nextHole+1)%holeCapacity;
    if(h.valid && !h.resolved)++s.stats.evictedUnresolved;
    h={};h.valid=true;h.sequence=sequence;return h;
}
void VideoRepairTrace::begin(uint64_t generation,int64_t nowUs) {
    std::lock_guard lock(mutex_);auto &s=*state_;
    s.stats.generation=generation;++s.stats.sessions;
    s.holes={};s.nextHole=0;s.haveSequence=false;s.primarySsrc=0;
    Event e;e.kind=Session;e.atUs=nowUs;record(e);
}
void VideoRepairTrace::packet(uint64_t generation,uint32_t primarySsrc,const uint8_t *data,size_t size,int64_t arrivalUs,int64_t handlerUs) {
    // Own the variable RTP header/RTX OSN boundary once, then use numeric data.
    std::lock_guard lock(mutex_);auto &s=*state_;
    if(generation!=s.stats.generation)return;
    if(size<12 || data[0]>>6!=2){++s.stats.malformed;return;}
    const unsigned pt=data[1]&127;
    if(pt!=96 && pt!=97)return;
    size_t header=12+4*(data[0]&15),end=size;
    if(header>size){++s.stats.malformed;return;}
    if(data[0]&16) {
        if(size-header<4){++s.stats.malformed;return;}
        const size_t extension=4+4*size_t(uint16_t(uint16_t(data[header+2])<<8|data[header+3]));
        if(extension>size-header){++s.stats.malformed;return;}
        header+=extension;
    }
    if(data[0]&32) {
        const size_t padding=data[size-1];
        if(!padding || padding>size-header){++s.stats.malformed;return;}
        end-=padding;
    }
    const uint32_t source=uint32_t(data[8])<<24|uint32_t(data[9])<<16|uint32_t(data[10])<<8|data[11];
    const uint32_t timestamp=uint32_t(data[4])<<24|uint32_t(data[5])<<16|uint32_t(data[6])<<8|data[7];
    if(!primarySsrc || (pt==96 && source!=primarySsrc))return;
    if(header>=end || (pt==97 && end-header<3)){++s.stats.malformed;return;}
    s.primarySsrc=primarySsrc;
    const uint16_t seq=pt==97?uint16_t(uint16_t(data[header])<<8|data[header+1]):uint16_t(uint16_t(data[2])<<8|data[3]);
    const int64_t ext=s.haveSequence?s.highestSequence+int16_t(uint16_t(seq-uint16_t(s.highestSequence))):seq;
    if(pt==96 && (!s.haveSequence || ext>s.highestSequence)) {
        if(s.haveSequence && ext>s.highestSequence+1) {
            const auto missing=uint64_t(ext-s.highestSequence-1);
            const auto retained=std::min<uint64_t>(missing,holeCapacity);
            s.stats.untrackedGapSequences+=missing-retained;
            for(uint64_t i=1;i<=retained;++i) {
                auto &h=findOrInsert(s.highestSequence+int64_t(i));
                if(h.firstGapUs<0)h.firstGapUs=handlerUs;
                ++s.stats.gaps;
                Event e;e.kind=Gap;e.atUs=handlerUs;e.sequence=h.sequence;e.firstGapUs=h.firstGapUs;record(e);
            }
        }
        s.highestSequence=ext;s.haveSequence=true;return;
    }
    Hole *hole=nullptr;
    for(auto &h:s.holes)if(h.valid && h.sequence==ext){hole=&h;break;}
    if(pt==97) {
        ++s.stats.rtx;if(!hole)++s.stats.unmatchedRtx;
    } else if(!hole || hole->resolved)return;
    if(hole) {
        hole->resolved=true;hole->timestampKnown=true;hole->timestamp=timestamp;
        if(pt==96)++s.stats.latePrimary;
    }
    Event e;e.kind=pt==97?Rtx:LatePrimary;e.atUs=handlerUs;e.arrivalUs=arrivalUs;
    e.sequence=ext;e.timestamp=timestamp;e.count=hole?1:0;
    if(hole){e.firstGapUs=hole->firstGapUs;e.firstNackUs=hole->firstNackUs;e.attempts=hole->attempts;}
    record(e);
}
uint64_t VideoRepairTrace::feedbackBegin(uint64_t generation,const uint8_t *data,size_t size,int64_t nowUs) {
    std::lock_guard lock(mutex_);auto &s=*state_;
    if(generation!=s.stats.generation)return 0;
    if(size<4)return 0;
    const bool nack=data[1]==205 && (data[0]&31)==1;
    const bool pli=data[1]==206 && (data[0]&31)==1;
    if(!nack && !pli)return 0; // Only the receiver's standalone NACK/PLI.
    if(data[0]>>6!=2 || (data[0]&32) || size<12 ||
       (size_t(uint16_t(uint16_t(data[2])<<8|data[3]))+1)*4!=size ||
       (nack && (size<16 || size>12+32*4 || (size-12)%4)) || (pli && size!=12)) {++s.stats.malformed;return 0;}
    const uint32_t media=uint32_t(data[8])<<24|uint32_t(data[9])<<16|uint32_t(data[10])<<8|data[11];
    if(s.primarySsrc && media!=s.primarySsrc)return 0;
    const auto token=++s.nextToken;
    if(pli){Event e;e.kind=Pli;e.atUs=nowUs;e.token=token;record(e);return token;}
    ++s.stats.nackPackets;
    for(size_t at=12;at<size;at+=4) {
        const uint16_t pid=uint16_t(uint16_t(data[at])<<8|data[at+1]);
        const uint16_t blp=uint16_t(uint16_t(data[at+2])<<8|data[at+3]);
        for(unsigned bit=0;bit<=16;++bit) {
            if(bit && !(blp&(1u<<(bit-1))))continue;
            const uint16_t seq=uint16_t(pid+bit);
            const int64_t ext=s.haveSequence?s.highestSequence+int16_t(uint16_t(seq-uint16_t(s.highestSequence))):seq;
            auto &h=findOrInsert(ext);
            if(h.firstNackUs<0)h.firstNackUs=nowUs;
            ++h.attempts;++s.stats.nackSequences;
            Event e;e.kind=Nack;e.atUs=nowUs;e.token=token;e.sequence=ext;e.attempts=h.attempts;
            e.firstGapUs=h.firstGapUs;e.firstNackUs=h.firstNackUs;record(e);
        }
    }
    return token;
}
void VideoRepairTrace::feedbackEnd(uint64_t generation,uint64_t token,int64_t sendStartUs,int64_t endUs) {
    if(!token)return;
    std::lock_guard lock(mutex_);auto &s=*state_;
    if(generation!=s.stats.generation)return;
    if(endUs>=sendStartUs)s.stats.maxSendUs=std::max(s.stats.maxSendUs,uint64_t(endUs-sendStartUs));
    Event e;e.kind=SendEnd;e.atUs=endUs;e.sendStartUs=sendStartUs;e.token=token;record(e);
}
void VideoRepairTrace::accessUnit(uint64_t generation,uint32_t timestamp,int64_t readyUs,bool anchor) {
    std::lock_guard lock(mutex_);auto &s=*state_;
    if(generation!=s.stats.generation)return;
    unsigned matched=0;
    for(auto &h:s.holes)if(h.valid && h.timestampKnown && h.timestamp==timestamp && !h.auSeen){h.auSeen=true;++matched;}
    if(matched || anchor) {
        Event e;e.kind=AuHandoff;e.atUs=readyUs;e.timestamp=timestamp;e.count=matched;e.anchor=anchor;record(e);
    }
}
VideoRepairTrace::Stats VideoRepairTrace::stats() const {std::lock_guard lock(mutex_);return state_->stats;}
std::unique_ptr<VideoRepairTrace::Snapshot> VideoRepairTrace::snapshot() const {
    // Fixed storage on the heap; Switch worker stacks do not hold this array.
    auto copy=std::make_unique<Snapshot>();
    std::lock_guard lock(mutex_);const auto &s=*state_;
    copy->stats=s.stats;copy->count=s.eventCount;
    for(size_t i=0;i<s.eventCount;++i)copy->events[i]=s.events[(s.nextEvent+eventCapacity-s.eventCount+i)%eventCapacity];
    return copy;
}
bool VideoRepairTrace::flush(const std::string &path) const {
    const auto copy=snapshot();std::ofstream out(path,std::ios::trunc);
    out<<"# video_repair_schema=1 events="<<copy->stats.events<<" overwritten="<<copy->stats.overwritten
       <<" evicted_unresolved="<<copy->stats.evictedUnresolved<<" untracked_gap_sequences="<<copy->stats.untrackedGapSequences<<'\n';
    out<<"order,generation,kind,at_us,sequence,rtp_timestamp,token,arrival_us,first_gap_us,first_nack_us,send_start_us,attempts,count,anchor\n";
    for(size_t i=0;i<copy->count;++i) {
        const auto &e=copy->events[i];
        out<<e.order<<','<<e.generation<<','<<e.kind<<','<<e.atUs<<','<<e.sequence<<','<<e.timestamp<<','<<e.token<<','
           <<e.arrivalUs<<','<<e.firstGapUs<<','<<e.firstNackUs<<','<<e.sendStartUs<<','<<e.attempts<<','<<e.count<<','<<e.anchor<<'\n';
    }
    out.close();return bool(out);
}
}
