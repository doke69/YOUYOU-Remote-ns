#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace uu {
// Passive observations at the authenticated track and its feedback callback.
// A sequence gap is provisional; observing RTX is not proof of acceptance.
class VideoRepairTrace {
public:
    static constexpr size_t holeCapacity=512,eventCapacity=2048;
    enum Kind {Session=1,Gap=2,Nack=3,SendEnd=4,Rtx=5,LatePrimary=6,AuHandoff=7,Pli=8};
    struct Event {
        uint64_t order=0,generation=0,token=0;
        int64_t atUs=0,sequence=-1,arrivalUs=-1,firstGapUs=-1,firstNackUs=-1,sendStartUs=-1;
        uint32_t timestamp=0,attempts=0,count=0;
        unsigned kind=0;
        bool anchor=false;
    };
    struct Stats {
        uint64_t generation=0,sessions=0,events=0,overwritten=0;
        uint64_t gaps=0,nackPackets=0,nackSequences=0,rtx=0,unmatchedRtx=0,latePrimary=0;
        uint64_t malformed=0,evictedUnresolved=0,untrackedGapSequences=0,maxSendUs=0;
    };
    struct Snapshot {Stats stats;size_t count=0;std::array<Event,eventCapacity> events;};
    VideoRepairTrace();
    void begin(uint64_t generation,int64_t nowUs);
    void packet(uint64_t generation,uint32_t primarySsrc,const uint8_t *data,size_t size,int64_t arrivalUs,int64_t handlerUs);
    uint64_t feedbackBegin(uint64_t generation,const uint8_t *data,size_t size,int64_t nowUs);
    void feedbackEnd(uint64_t generation,uint64_t token,int64_t sendStartUs,int64_t endUs);
    void accessUnit(uint64_t generation,uint32_t timestamp,int64_t readyUs,bool anchor);
    Stats stats() const;
    std::unique_ptr<Snapshot> snapshot() const;
    bool flush(const std::string &path) const;
private:
    struct Hole {
        int64_t sequence=-1,firstGapUs=-1,firstNackUs=-1;
        uint32_t timestamp=0,attempts=0;
        bool valid=false,resolved=false,timestampKnown=false,auSeen=false;
    };
    struct State {
        Stats stats;
        std::array<Hole,holeCapacity> holes{};
        std::array<Event,eventCapacity> events{};
        size_t nextHole=0,nextEvent=0,eventCount=0;
        uint64_t nextToken=0;
        int64_t highestSequence=0;
        bool haveSequence=false;
        uint32_t primarySsrc=0;
    };
    Hole &findOrInsert(int64_t sequence);
    void record(Event event);
    mutable std::mutex mutex_;
    std::unique_ptr<State> state_;
};
}
