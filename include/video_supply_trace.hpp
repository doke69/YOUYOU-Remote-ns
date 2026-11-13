#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace uu {
// Passive, per-client trace. Times are steady-clock microseconds, RTP values
// identify frames only; these observations never control delivery or repair.
class VideoSupplyTrace {
  public:
    static constexpr size_t frameCapacity=128,eventCapacity=128;
    enum Flag {IngressGap=1,HandlerGap=2,QueueAge=4,AccessUnitGap=8,
               FrameResidence=16,HandoffSlow=32,UnmatchedFrame=64,SessionStart=128,LateObservation=256};
    struct Frame {
        uint32_t timestamp=0;
        int64_t firstArrival=-1,lastArrival=-1,firstHandler=-1,lastHandler=-1;
        uint64_t primary=0,rtx=0;
        bool valid=false,queueEvent=false;
    };
    struct Event {
        uint64_t order=0,generation=0;unsigned flags=0;
        int64_t atUs=0,previousArrival=-1,previousHandler=-1,previousAu=-1;
        int64_t readyUs=-1,submittedUs=-1;
        bool matched=false;
        Frame frame;
    };
    struct Stats {
        uint64_t generation=0,sessions=0,primary=0,rtx=0,accessUnits=0;
        uint64_t missingArrival=0,unmatched=0,events=0,overwritten=0;
        uint64_t maxIngressGapUs=0,maxHandlerGapUs=0,maxQueueAgeUs=0;
        uint64_t maxFrameResidenceUs=0,maxHandoffUs=0;
    };
    struct Snapshot {Stats stats;size_t count=0;std::array<Event,eventCapacity> events;};
    static int64_t nowUs();
    void begin(uint64_t generation,int64_t now);
    void packet(uint64_t generation,uint32_t timestamp,int64_t arrivalUs,int64_t handlerUs,bool rtx);
    void accessUnit(uint64_t generation,uint32_t timestamp,int64_t readyUs,int64_t submittedUs);
    Stats stats() const;
    Snapshot snapshot() const;
    bool flush(const std::string &path) const;
  private:
    void record(Event event);
    mutable std::mutex mutex_;
    Stats stats_;
    std::array<Frame,frameCapacity> frames_{};
    std::array<Event,eventCapacity> events_{};
    size_t latestFrame_=0,nextEvent_=0,eventCount_=0;
    bool haveFrame_=false;
    int64_t lastPrimaryArrival_=-1,lastPrimaryHandler_=-1,lastAu_=-1;
};
}
