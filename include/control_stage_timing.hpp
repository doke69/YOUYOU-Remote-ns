#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace uu {
// Owned and read only by Client::run's control thread. Wall time includes descheduling.
struct ControlStageTiming {
    enum Stage { Loop, Idle, Drain, InputHandle, OtherHandle, InputQueue,
                 Quality, VideoScale, Stats, Rtt, CandidatePair, Signal, SignalRead,
                 SignalPacket, Receiver, Recovery, Feedback, QualityCommit,
                 QualityConfigSave, QualityAckKeyframe, BitrateApply, QualityConfigEnqueue, Count };
    inline static constexpr const char *names[Count]={
        "loop","idle","drain","input_handle","other_handle","input_queue",
        "quality","video_scale","stats","rtt","candidate_pair","signal",
        "signal_read","signal_packet","receiver","recovery","feedback",
        "quality_commit","quality_config_save","quality_ack_keyframe","bitrate_apply","quality_config_enqueue"};
    inline static constexpr std::array<uint64_t,14> histogramBounds={
        0,250,500,1000,2000,4000,8000,16000,25000,33000,50000,100000,250000,1000000};
    struct Sample {
        uint64_t calls=0,totalUs=0,maxUs=0,over16ms=0,maxCycle=0,maxStartUs=0,maxEndUs=0;
        std::array<uint64_t,histogramBounds.size()+1> histogram{};
        uint64_t maxEventSeq=0,maxEventGeneration=0;
        std::array<char,48> maxEventType{};
    };
    std::array<Sample,Count> samples{};
    uint64_t cycle=0,reportedAtUs=0,eventSeq=0,qualityChanges=0,lastQualityChangeUs=0;
    static uint64_t nowUs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void record(Stage stage,uint64_t start,uint64_t end,uint64_t loop,
                const char *eventType=nullptr,uint64_t sequence=0,uint64_t generation=0) {
        auto &s=samples[stage];
        const auto elapsed=end-start;
        ++s.calls;s.totalUs+=elapsed;s.over16ms+=elapsed>=16000;
        if(stage==Loop || stage==InputQueue || stage==InputHandle || stage==OtherHandle) {
            size_t bucket=0;
            while(bucket<histogramBounds.size() && elapsed>histogramBounds[bucket])++bucket;
            ++s.histogram[bucket];
        }
        if(s.calls==1 || elapsed>s.maxUs) {
            s.maxUs=elapsed;s.maxCycle=loop;s.maxStartUs=start;s.maxEndUs=end;
            s.maxEventSeq=sequence;s.maxEventGeneration=generation;
            s.maxEventType[0]='\0';
            if(eventType)std::snprintf(s.maxEventType.data(),s.maxEventType.size(),"%s",eventType);
        }
    }
    struct Scope {
        ControlStageTiming &owner;
        Stage stage;
        uint64_t start,cycle;
        const char *eventType;
        uint64_t sequence,generation;
        Scope(ControlStageTiming &o,Stage s,const char *type=nullptr,uint64_t seq=0,uint64_t gen=0)
            :owner(o),stage(s),start(nowUs()),cycle(o.cycle),eventType(type),sequence(seq),generation(gen) {}
        ~Scope() {owner.record(stage,start,nowUs(),cycle,eventType,sequence,generation);}
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
    };
};
}
