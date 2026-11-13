#include "video_repair_trace.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

std::vector<uint8_t> rtp(uint16_t sequence,uint32_t timestamp,bool rtx=false) {
    std::vector<uint8_t> p{0x80,uint8_t(rtx?97:96),uint8_t(sequence>>8),uint8_t(sequence),
        uint8_t(timestamp>>24),uint8_t(timestamp>>16),uint8_t(timestamp>>8),uint8_t(timestamp),0,0,0,9};
    if(rtx){p.push_back(uint8_t(sequence>>8));p.push_back(uint8_t(sequence));}
    p.push_back(0x26);return p;
}
std::vector<uint8_t> nack(uint16_t pid,uint16_t blp=0) {
    return {0x81,205,0,3,0,0,0,1,0,0,0,9,uint8_t(pid>>8),uint8_t(pid),uint8_t(blp>>8),uint8_t(blp)};
}
int main() {
    using T=uu::VideoRepairTrace;
    T t;t.begin(7,0);
    auto a=rtp(65534,1000),b=rtp(1,2500);
    t.packet(7,9,a.data(),a.size(),1000,1100);t.packet(7,9,b.data(),b.size(),2000,2100);
    assert(t.stats().gaps==2);
    auto n=nack(65535,1);const auto original=n;
    const auto token=t.feedbackBegin(7,n.data(),n.size(),15000);
    assert(token && n==original && t.stats().nackSequences==2);
    t.feedbackEnd(7,token,15100,25100);
    assert(t.stats().maxSendUs==10000);
    auto retry=t.feedbackBegin(7,n.data(),n.size(),60000);assert(retry!=token);
    t.feedbackEnd(7,retry,60100,60200);
    auto r1=rtp(65535,1500,true),r2=rtp(0,1500,true);
    t.packet(7,9,r1.data(),r1.size(),75000,76000);t.packet(7,9,r2.data(),r2.size(),77000,78000);
    t.accessUnit(7,1500,79000,false);
    auto s=t.snapshot();unsigned returns=0,outputs=0;
    for(size_t i=0;i<s->count;++i) {
        const auto &e=s->events[i];
        if(e.kind==T::Rtx){assert(e.firstGapUs==2100 && e.firstNackUs==15000 && e.attempts==2 && e.count==1);++returns;}
        if(e.kind==T::AuHandoff){assert(e.timestamp==1500 && e.count==2);++outputs;}
    }
    assert(returns==2 && outputs==1 && t.stats().rtx==2 && t.stats().unmatchedRtx==0);

    // A primary packet can close a provisional gap without any retransmission.
    T reordered;reordered.begin(1,0);
    for(auto seq:{10,12,11}){auto p=rtp(uint16_t(seq),3000);reordered.packet(1,9,p.data(),p.size(),1000+seq,2000+seq);}
    assert(reordered.stats().gaps==1 && reordered.stats().latePrimary==1);
    auto missing=rtp(3,100,true);reordered.packet(1,9,missing.data(),missing.size(),3000,3100);
    assert(reordered.stats().unmatchedRtx==1);
    const auto before=reordered.stats();
    reordered.packet(9,9,a.data(),a.size(),4000,4100);reordered.feedbackBegin(9,n.data(),n.size(),5000);
    assert(reordered.stats().events==before.events);

    // Truncated CSRC/extension/padding/RTX and malformed feedback only affect diagnostics.
    auto invalid=rtp(20,4000);invalid[0]=0x8f;
    reordered.packet(1,9,invalid.data(),invalid.size(),6000,6100);
    invalid=rtp(20,4000);invalid[0]=0x90;
    reordered.packet(1,9,invalid.data(),invalid.size(),6000,6100);
    invalid=rtp(20,4000);invalid[0]=0xa0;invalid.back()=255;
    reordered.packet(1,9,invalid.data(),invalid.size(),6000,6100);
    invalid=rtp(20,4000,true);invalid.pop_back();
    reordered.packet(1,9,invalid.data(),invalid.size(),6000,6100);
    invalid=n;invalid[3]=4;assert(!reordered.feedbackBegin(1,invalid.data(),invalid.size(),6500));
    assert(reordered.stats().malformed==5);
    const std::vector<uint8_t> pli{0x81,206,0,2,0,0,0,1,0,0,0,9};
    assert(reordered.feedbackBegin(1,pli.data(),pli.size(),7000));
    reordered.accessUnit(1,7000,8000,true);

    // Bounded metadata stays explicit when a gap exceeds the observation budget.
    T bounded;bounded.begin(1,0);a=rtp(0,1000);b=rtp(2000,2000);
    bounded.packet(1,9,a.data(),a.size(),1,2);bounded.packet(1,9,b.data(),b.size(),3,4);
    assert(bounded.stats().gaps==T::holeCapacity && bounded.stats().untrackedGapSequences==1999-T::holeCapacity);
    b=rtp(3000,3000);bounded.packet(1,9,b.data(),b.size(),5,6);
    assert(bounded.stats().evictedUnresolved==T::holeCapacity);
    for(unsigned i=2;i<=2200;++i)bounded.begin(i,i);
    s=bounded.snapshot();assert(s->count==T::eventCapacity && s->stats.overwritten>0);
    for(size_t i=1;i<s->count;++i)assert(s->events[i].order==s->events[i-1].order+1);
    const auto path=std::filesystem::current_path()/"repair-trace-test.csv";
    assert(bounded.flush(path.string()));std::ifstream file(path);
    const std::string csv((std::istreambuf_iterator<char>(file)),{});file.close();
    assert(csv.find("video_repair_schema=1")!=std::string::npos && csv.find("first_gap_us,first_nack_us")!=std::string::npos);
    std::filesystem::remove(path);
}
