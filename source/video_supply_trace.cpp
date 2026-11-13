#include "video_supply_trace.hpp"
#ifdef UU_PACKET_STAGE_TRACE
#include "packet_stage_trace.h"
#endif
#ifdef UU_UDP_RX_TRACE
#include "udp_receive_trace.h"
#endif
#include <algorithm>
#include <chrono>
#include <fstream>

namespace uu {
int64_t VideoSupplyTrace::nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void VideoSupplyTrace::record(Event event) {
    event.order=++stats_.events;event.generation=stats_.generation;
    if(eventCount_==eventCapacity)++stats_.overwritten;else ++eventCount_;
    events_[nextEvent_]=event;nextEvent_=(nextEvent_+1)%eventCapacity;
}
void VideoSupplyTrace::begin(uint64_t generation,int64_t now) {
    std::lock_guard lock(mutex_);
    stats_.generation=generation;++stats_.sessions;
    frames_={};haveFrame_=false;latestFrame_=0;
    lastPrimaryArrival_=lastPrimaryHandler_=lastAu_=-1;
    Event event;event.atUs=now;event.flags=SessionStart;record(event);
}
void VideoSupplyTrace::packet(uint64_t generation,uint32_t timestamp,int64_t arrivalUs,int64_t handlerUs,bool rtx) {
    std::lock_guard lock(mutex_);
    if(generation!=stats_.generation)return;
    // This is the metadata boundary; -1 also represents a missing RTC stamp.
    if(arrivalUs<0 || arrivalUs>handlerUs){arrivalUs=-1;++stats_.missingArrival;}
    Frame *frame=nullptr;
    if(haveFrame_ && frames_[latestFrame_].timestamp==timestamp)frame=&frames_[latestFrame_];
    else if(!haveFrame_ || int32_t(timestamp-frames_[latestFrame_].timestamp)>0) {
        if(haveFrame_)latestFrame_=(latestFrame_+1)%frameCapacity;
        frame=&frames_[latestFrame_];*frame={};frame->timestamp=timestamp;frame->valid=true;haveFrame_=true;
    } else {
        for(auto &candidate:frames_)if(candidate.valid && candidate.timestamp==timestamp){frame=&candidate;break;}
    }
    if(rtx)++stats_.rtx;else ++stats_.primary;
    if(frame) {
        if(frame->firstHandler<0)frame->firstHandler=handlerUs;
        frame->lastHandler=std::max(frame->lastHandler,handlerUs);
        if(arrivalUs>=0) {
            if(frame->firstArrival<0)frame->firstArrival=arrivalUs;
            frame->firstArrival=std::min(frame->firstArrival,arrivalUs);
            frame->lastArrival=std::max(frame->lastArrival,arrivalUs);
        }
        if(rtx)++frame->rtx;else ++frame->primary;
    }
    Event event;event.atUs=handlerUs;event.previousArrival=lastPrimaryArrival_;event.previousHandler=lastPrimaryHandler_;
    if(!rtx) {
        if(lastPrimaryArrival_>=0 && arrivalUs>=lastPrimaryArrival_) {
            const auto gap=uint64_t(arrivalUs-lastPrimaryArrival_);
            stats_.maxIngressGapUs=std::max(stats_.maxIngressGapUs,gap);
            if(gap>=50000)event.flags|=IngressGap;
#ifdef UU_UDP_RX_TRACE
            if(gap>=80000)
                noteUdpReceiveIncident(generation, UdpIncidentSource::Ingress, lastPrimaryArrival_, arrivalUs);
#endif
        }
        if(lastPrimaryHandler_>=0) {
            const auto gap=uint64_t(handlerUs-lastPrimaryHandler_);
            stats_.maxHandlerGapUs=std::max(stats_.maxHandlerGapUs,gap);
            if(gap>=50000)event.flags|=HandlerGap;
        }
        if(arrivalUs>=0)lastPrimaryArrival_=std::max(lastPrimaryArrival_,arrivalUs);
        lastPrimaryHandler_=handlerUs;
    }
    if(arrivalUs>=0) {
        const auto age=uint64_t(handlerUs-arrivalUs);
        stats_.maxQueueAgeUs=std::max(stats_.maxQueueAgeUs,age);
        if(age>=20000 && frame && !frame->queueEvent){event.flags|=QueueAge;frame->queueEvent=true;}
    }
    if(event.flags) {
        event.matched=frame!=nullptr;
        if(frame)event.frame=*frame;else event.frame.timestamp=timestamp;
        record(event);
    }
}
void VideoSupplyTrace::accessUnit(uint64_t generation,uint32_t timestamp,int64_t readyUs,int64_t submittedUs) {
    std::lock_guard lock(mutex_);
    if(generation!=stats_.generation)return;
    ++stats_.accessUnits;
    Event event;event.atUs=submittedUs;event.readyUs=readyUs;event.submittedUs=submittedUs;event.previousAu=lastAu_;
    event.frame.timestamp=timestamp;
    for(const auto &frame:frames_)if(frame.valid && frame.timestamp==timestamp){event.frame=frame;event.matched=true;break;}
    if(lastAu_>=0 && readyUs-lastAu_>=50000)event.flags|=AccessUnitGap;
#ifdef UU_UDP_RX_TRACE
    if(lastAu_>=0 && readyUs-lastAu_>=80000)
        noteUdpReceiveIncident(generation, UdpIncidentSource::Supply, lastAu_, readyUs);
#endif
    lastAu_=readyUs;
#ifdef UU_PACKET_STAGE_TRACE
    uu_packet_header header{};
    header.timestamp = timestamp;
    uu_packet_stage_record(UU_PACKET_AU, header, 0);
#endif
    const auto handoff=uint64_t(submittedUs-readyUs);
    stats_.maxHandoffUs=std::max(stats_.maxHandoffUs,handoff);
    if(handoff>=8000)event.flags|=HandoffSlow;
    if(event.matched) {
        // tick() can emit while another thread has observed a later packet.
        // Keep that observation, but do not report a negative wait as unsigned.
        if(event.frame.lastHandler>readyUs)event.flags|=LateObservation;
        if(readyUs>=event.frame.firstHandler) {
            const auto wait=uint64_t(readyUs-event.frame.firstHandler);
            stats_.maxFrameResidenceUs=std::max(stats_.maxFrameResidenceUs,wait);
            if(wait>=50000)event.flags|=FrameResidence;
        }
    } else {++stats_.unmatched;event.flags|=UnmatchedFrame;}
    if(event.flags)record(event);
}
VideoSupplyTrace::Stats VideoSupplyTrace::stats() const {
    std::lock_guard lock(mutex_);return stats_;
}
VideoSupplyTrace::Snapshot VideoSupplyTrace::snapshot() const {
    std::lock_guard lock(mutex_);
    Snapshot copy;copy.stats=stats_;copy.count=eventCount_;
    for(size_t i=0;i<eventCount_;++i)copy.events[i]=events_[(nextEvent_+eventCapacity-eventCount_+i)%eventCapacity];
    return copy;
}
bool VideoSupplyTrace::flush(const std::string &path) const {
    const auto copy=snapshot(); // No trace lock during file operations.
    std::ofstream out(path,std::ios::trunc);
    out<<"# video_supply_schema=1 events="<<copy.stats.events<<" overwritten="<<copy.stats.overwritten
       <<" missing_arrival="<<copy.stats.missingArrival<<" unmatched="<<copy.stats.unmatched<<'\n';
    out<<"order,generation,flags,at_us,rtp_timestamp,matched,first_arrival_us,last_arrival_us,first_handler_us,last_handler_us,primary,rtx,previous_arrival_us,previous_handler_us,previous_au_us,au_ready_us,submitted_us\n";
    for(size_t i=0;i<copy.count;++i) {
        const auto &e=copy.events[i];const auto &f=e.frame;
        out<<e.order<<','<<e.generation<<','<<e.flags<<','<<e.atUs<<','<<f.timestamp<<','<<e.matched<<','
           <<f.firstArrival<<','<<f.lastArrival<<','<<f.firstHandler<<','<<f.lastHandler<<','<<f.primary<<','<<f.rtx<<','
           <<e.previousArrival<<','<<e.previousHandler<<','<<e.previousAu<<','<<e.readyUs<<','<<e.submittedUs<<'\n';
    }
    out.close();return bool(out);
}
}
