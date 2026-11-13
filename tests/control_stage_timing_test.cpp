#include "control_stage_timing.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <stdexcept>

int main() {
    using T=uu::ControlStageTiming;
    T t;
    t.record(T::Stats,100,16100,7);
    t.record(T::Stats,20000,20003,8);
    auto s=t.samples[T::Stats];
    assert(s.calls==2 && s.totalUs==16003 && s.maxUs==16000 && s.over16ms==1);
    assert(s.maxCycle==7 && s.maxStartUs==100 && s.maxEndUs==16100);
    t.record(T::Stats,30000,60000,9);
    assert(t.samples[T::Stats].maxCycle==9 && t.samples[T::Stats].over16ms==2);
    t.cycle=10;
    {
        T::Scope outer(t,T::Loop);
        T::Scope inner(t,T::CandidatePair);
    }
    assert(t.samples[T::Loop].calls==1 && t.samples[T::CandidatePair].calls==1);
    assert(t.samples[T::Loop].maxStartUs<=t.samples[T::CandidatePair].maxStartUs);
    assert(t.samples[T::Loop].maxEndUs>=t.samples[T::CandidatePair].maxEndUs);
    // Existing Client exception handling must not lose the interrupted phase.
    try {T::Scope interrupted(t,T::Signal);throw std::runtime_error("test");}
    catch(const std::runtime_error&) {}
    assert(t.samples[T::Signal].calls==1 && t.samples[T::Signal].maxCycle==10);
    t.samples={};
    assert(t.cycle==10 && t.samples[T::Stats].calls==0 && t.samples[T::Signal].maxUs==0);
    t.record(T::Stats,70000,70000,11);
    assert(t.samples[T::Stats].calls==1 && t.samples[T::Stats].maxCycle==11);
    T tail;
    for(uint64_t duration:{0,1,250,251,500,501,999,1000,1001,15999,16000,16001,1000001})
        tail.record(T::InputHandle,100,100+duration,12,"input",++tail.eventSeq,4);
    const std::array<uint64_t,15> expected={1,2,2,3,1,0,0,2,1,0,0,0,0,0,1};
    assert(tail.samples[T::InputHandle].histogram==expected);
    assert(tail.samples[T::InputHandle].maxEventSeq==13);
    assert(tail.samples[T::InputHandle].maxEventGeneration==4);
    assert(std::string(tail.samples[T::InputHandle].maxEventType.data())=="input");
    // Keep the tag from the actual maximum, not the last event in the window.
    tail.record(T::OtherHandle,100,50100,20,"quality-settings",14,4);
    tail.record(T::OtherHandle,60000,60001,21,"rtc-state",15,6);
    assert(tail.samples[T::OtherHandle].maxEventSeq==14);
    assert(tail.samples[T::OtherHandle].maxEventGeneration==4);
    assert(std::string(tail.samples[T::OtherHandle].maxEventType.data())=="quality-settings");
    tail.samples={};
    assert(tail.eventSeq==13 && tail.samples[T::InputHandle].histogram[0]==0);
    assert(tail.samples[T::OtherHandle].maxEventSeq==0 && tail.samples[T::OtherHandle].maxEventType[0]==0);
}
