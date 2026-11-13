#include "stream_stats.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace { int64_t clockMs=1000; uint64_t clockCalls=0; }
namespace uu { int64_t monotonicMs() {++clockCalls;return clockMs;} }
#define CHECK(x) do {if(!(x)){std::cerr<<"failed line "<<__LINE__<<": " #x "\n";return 1;}}while(0)

int main(int argc,char **) {
    const bool diagnostic=argc>1;
#ifdef _WIN32
    _putenv_s("UU_DIAG",diagnostic?"1":"0");
#else
    setenv("UU_DIAG",diagnostic?"1":"0",1);
#endif
    // Wire packets and fixed caller-controlled arrival times. Run this same
    // replay against the baseline to compare every observable window and dump.
    auto rtp=[](uint16_t seq,uint32_t timestamp,uint32_t ssrc=96,uint8_t pt=96) {
        auto m=std::make_shared<rtc::Message>(28);
        auto *p=reinterpret_cast<uint8_t*>(m->data());
        p[0]=0x80;p[1]=pt;p[2]=seq>>8;p[3]=seq;
        for(int i=0;i<4;++i) {p[4+i]=timestamp>>(24-i*8);p[8+i]=ssrc>>(24-i*8);}
        return m;
    };
    auto sr=[](uint32_t packets,uint32_t octets) {
        auto m=std::make_shared<rtc::Message>(28,rtc::Message::Control);
        auto *p=reinterpret_cast<uint8_t*>(m->data());p[0]=0x80;p[1]=200;p[3]=6;p[7]=96;
        for(int i=0;i<4;++i) {p[20+i]=packets>>(24-i*8);p[24+i]=octets>>(24-i*8);}
        return m;
    };
    nlohmann::json replay=nlohmann::json::array();
    unsigned sends=0;
    {
        uu::RtpTrackStats video(90000,"video",true),audio(48000,"audio");
        auto feed=[&](uu::RtpTrackStats &stats,rtc::message_vector input) {
            const auto original=input;
            stats.incoming(input,[&](rtc::message_ptr){++sends;});
            if(input!=original)std::abort(); // Observer must not consume/replace packets.
            const auto s=stats.snapshot();
            replay.push_back({s.ssrc,s.packets,s.initialGapPackets,s.initialGapPercent,
                              s.jitterMs,s.bitrateMbps,s.nackSent,s.rtxRecovered});
            return s;
        };
        auto badVersion=rtp(1,0);(*badVersion)[0]=std::byte{0x40};
        CHECK(feed(video,{nullptr,std::make_shared<rtc::Message>(4),badVersion,
                           std::make_shared<rtc::Message>(28,rtc::Message::String),rtp(1,0,96,97)}).packets==0);
        CHECK(feed(video,{rtp(65534,0xffffff00u)}).packets==1);
        clockMs+=16;feed(video,{rtp(65535,0x000004dcu)});
        clockMs+=16;feed(video,{rtp(0,0x00000ab8u)});
        clockMs+=32;CHECK(feed(video,{rtp(2,0x00001670u)}).initialGapPackets==1);
        clockMs+=1;feed(video,{rtp(1,0x00001094u)});
        CHECK(feed(video,{rtp(1,0x00001094u),rtp(3,0,97),rtp(3,0,96,97)}).packets==6);
        feed(video,{sr(100,2800)});
        clockMs=1600;auto s=feed(video,{});
        CHECK(s.packets==6 && s.initialGapPackets==1 && s.ssrc==96);
        CHECK(s.initialGapPercent==20.0 && s.bitrateMbps==0.00224);
        CHECK(feed(audio,{rtp(7,0,12,111)}).ssrc==12);
        CHECK(feed(audio,{rtp(8,960,12,112),rtp(8,960,13,111)}).packets==1);
        clockMs=2000;feed(video,{rtp(3,7500),rtp(4,9000),sr(110,3080)});
        // Multiple windows, sequence wrap, bounded large gaps, duplicate and
        // reordered packets, unrelated SSRC/PT and interleaved SRs.
        uint16_t seq=5;uint32_t timestamp=10500;
        for(unsigned i=0;i<5000;++i) {
            clockMs+=1+i%5;
            if(i%31==0)seq+=3;
            rtc::message_vector input{rtp(seq,timestamp)};
            if(i%23==0)input.push_back(rtp(seq,timestamp));
            if(i%37==0)input.push_back(rtp(uint16_t(seq-1),timestamp-1500));
            if(i%41==0)input.push_back(rtp(seq,timestamp,999));
            if(i%47==0)input.push_back(rtp(seq,timestamp,96,97));
            if(i%101==0)input.push_back(sr(200+i,(200+i)*28));
            feed(video,std::move(input));++seq;timestamp+=1500;
        }
        CHECK(sends==0);
        clockMs+=600;feed(video,{});feed(audio,{});
    }
    std::cout<<replay.dump()<<'\n';
    std::cerr<<"clock_calls="<<clockCalls<<" snapshots="<<replay.size()<<'\n';
    if(diagnostic) {
        std::ifstream info("diag/video_info.txt"),binary("diag/video_rtp.bin",std::ios::binary);
        const std::string report{std::istreambuf_iterator<char>(info),{}};
        const std::string bytes{std::istreambuf_iterator<char>(binary),{}};
        CHECK(report.find("sr_count=52\n")!=std::string::npos);
        CHECK(report.find("sr_packets=5149\n")!=std::string::npos);
        CHECK(report.find("sr_packets_base=100\n")!=std::string::npos);
        CHECK(report.find("received_at_sr_base=6\n")!=std::string::npos);
        CHECK(report.find("payload_types=96,")!=std::string::npos);
        CHECK(!bytes.empty() && bytes.size()%36==0);
    }
    return 0;
}
