#include "video_supply_trace.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main() {
    using T=uu::VideoSupplyTrace;
    T trace;trace.begin(7,1000);
    trace.packet(7,1000,10000,11000,false);
    trace.packet(7,1000,11000,12000,false);
    trace.accessUnit(7,1000,13000,14000);
    assert(trace.snapshot().count==1);
    trace.packet(7,2500,150000,151000,false);
    auto snapshot=trace.snapshot();
    assert((snapshot.events[1].flags&(T::IngressGap|T::HandlerGap))==(T::IngressGap|T::HandlerGap));
    trace.accessUnit(7,2500,152000,153000);
    snapshot=trace.snapshot();assert(snapshot.events[2].flags&T::AccessUnitGap);
    assert(snapshot.events[2].frame.firstArrival==150000 && snapshot.events[2].matched);
    trace.packet(7,4000,166000,166500,false);
    trace.packet(7,4000,300000,301000,true);
    trace.accessUnit(7,4000,302000,310500);
    snapshot=trace.snapshot();const auto repaired=snapshot.events[snapshot.count-1];
    assert((repaired.flags&(T::FrameResidence|T::HandoffSlow))==(T::FrameResidence|T::HandoffSlow));
    assert(repaired.frame.primary==1 && repaired.frame.rtx==1 && repaired.frame.lastArrival==300000);
    assert(snapshot.stats.maxFrameResidenceUs==135500 && snapshot.stats.maxHandoffUs==8500);
    trace.packet(7,5500,183000,450000,false);
    snapshot=trace.snapshot();const auto queued=snapshot.events[snapshot.count-1];
    assert((queued.flags&(T::HandlerGap|T::QueueAge))==(T::HandlerGap|T::QueueAge));
    assert(!(queued.flags&T::IngressGap));
    const auto count=snapshot.count;
    trace.packet(7,5500,184000,451000,false);
    assert(trace.snapshot().count==count); // At most one slow queue event per frame.
    trace.accessUnit(7,5500,452000,453000);
    trace.packet(7,4000,460000,461000,false);
    trace.accessUnit(7,4000,460000,462000);
    snapshot=trace.snapshot();assert(snapshot.events[snapshot.count-1].flags&T::LateObservation);
    assert(snapshot.stats.maxFrameResidenceUs<1000000); // No unsigned negative wait.
    trace.begin(8,500000);
    const auto before=trace.stats();
    trace.packet(7,7000,510000,511000,false);trace.accessUnit(7,7000,512000,513000);
    assert(trace.stats().primary==before.primary && trace.stats().accessUnits==before.accessUnits);
    trace.packet(8,0xfffff000u,510000,511000,false);
    trace.accessUnit(8,0xfffff000u,512000,513000);
    trace.packet(8,800,526000,527000,false);trace.accessUnit(8,800,528000,529000);
    assert(trace.stats().unmatched==0);
    trace.packet(8,2300,-1,540000,false);trace.packet(8,3800,560000,550000,false);
    assert(trace.stats().missingArrival==2);

    T bounded;bounded.begin(1,1);
    for(unsigned i=0;i<140;++i)bounded.packet(1,i*1500,100000+i*1000,100100+i*1000,false);
    bounded.accessUnit(1,0,250000,251000);
    assert(bounded.stats().unmatched==1 && bounded.snapshot().events[1].flags&T::UnmatchedFrame);
    for(unsigned i=2;i<=200;++i)bounded.begin(i,300000+i);
    snapshot=bounded.snapshot();assert(snapshot.count==T::eventCapacity);
    assert(snapshot.stats.overwritten==snapshot.stats.events-T::eventCapacity);
    for(size_t i=1;i<snapshot.count;++i)assert(snapshot.events[i].order==snapshot.events[i-1].order+1);
    const auto path=std::filesystem::current_path()/("supply-"+std::to_string(T::nowUs())+".csv");
    assert(bounded.flush(path.string()));
    std::ifstream file(path);std::string data((std::istreambuf_iterator<char>(file)),{});file.close();
    assert(data.find("video_supply_schema=1")!=std::string::npos && data.find("rtp_timestamp,matched")!=std::string::npos);
    assert(!bounded.flush((path/"missing.csv").string()));
    std::filesystem::remove(path);
    std::cout<<"PASS: ingress vs local queue, RTX residency, handoff, stale session, RTP wrap, bounded retention and CSV; bytes="<<sizeof(T)<<'\n';
}
