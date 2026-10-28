#include "cpu_diagnostics.hpp"
#ifdef UU_MEDIA_DISPATCH
#include "media_dispatch_queue.hpp"
#endif
#include "live_telemetry.hpp"
#include "protocol.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifdef __SWITCH__
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" void uu_juice_batch_diagnostics(uint64_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *,int *);
extern "C" int uu_juice_udp_socket_diagnostics(uint16_t, int *, int *, int *, int *);
#endif
namespace uu {
namespace {
// Process-local diagnostic stages; written by Media's decode thread, read by
// publish(). Relaxed atomics: values are independent samples, no ordering.
struct DecodeStageState {
    std::atomic<double> sendUs{0}, recvUs{0}, publishUs{0};
};
DecodeStageState decodeStageState;
constexpr size_t kGroupLimit = 1200;
constexpr size_t kGapBins = 513; // 0..511 exact, 512 = >=512ms overflow
}
void LiveTelemetry::setDecodeStages(double sendUs, double recvUs, double publishUs) noexcept {
    decodeStageState.sendUs.store(sendUs,std::memory_order_relaxed);
    decodeStageState.recvUs.store(recvUs,std::memory_order_relaxed);
    decodeStageState.publishUs.store(publishUs,std::memory_order_relaxed);
}
void LiveTelemetry::clearDecodeStages() noexcept { setDecodeStages(0,0,0); }
DecodeStageSample LiveTelemetry::decodeStages() noexcept {
    return {decodeStageState.sendUs.load(std::memory_order_relaxed),
            decodeStageState.recvUs.load(std::memory_order_relaxed),
            decodeStageState.publishUs.load(std::memory_order_relaxed)};
}
struct LiveTelemetry::Impl {
    int fd=-1;
#ifdef __SWITCH__
    sockaddr_in target{};
#endif
    std::string pair,session;
    uint64_t sequence=0,sent=0,failed=0,oversizeCount=0,exceptionCount=0,sendFailures=0,lastSenderLog=0;
    size_t lastPayloadBytes=0;
    bool firstPublishLogged=false;
    uint32_t window=0,now=0,lastFrame=0,sessionStart=0;
    bool started=false,streaming=false,activeUi=false,statsFlag=false;
    uint64_t loops=0,uploads=0,gapSamples=0,totalUploads=0;uint32_t maxGap=0;
    uint64_t loopOver25=0,loopOver50=0,gapOverflow=0;
    double uploadUs=0,uploadMaxUs=0,workUs=0,workMaxUs=0,presentUs=0,presentMaxUs=0;
    double loopUs=0,loopMaxUs=0;
    std::array<uint32_t,kGapBins> histogram{};
    Impl() {
#ifdef __SWITCH__
        std::ifstream file("sdmc:/switch/UURemote-ns/telemetry.ini");
        std::map<std::string,std::string> settings;
        std::string line;
        while(std::getline(file,line)) {
            if(!line.empty() && line.back()=='\r') line.pop_back();
            const auto eq=line.find('=');
            if(eq!=line.npos && line.size()<256) settings[line.substr(0,eq)]=line.substr(eq+1);
        }
        if(settings["enabled"]!="1") return;
        const auto &host=settings["host"];const auto &port=settings["port"];pair=settings["pair"];
        if(pair.size()!=48 || !std::all_of(pair.begin(),pair.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return;
        if(port.empty() || port.size()>5 || !std::all_of(port.begin(),port.end(),[](unsigned char c){return c>='0'&&c<='9';})) return;
        const auto number=std::stoul(port);
        if(number<1024 || number>65535 || inet_pton(AF_INET,host.c_str(),&target.sin_addr)!=1) return;
        const auto ip=ntohl(target.sin_addr.s_addr);
        if((ip>>24)!=10 && (ip>>20)!=0xac1 && (ip>>16)!=0xc0a8) return;
        target.sin_len=sizeof target;target.sin_family=AF_INET;target.sin_port=htons(uint16_t(number));
        session=uuid().substr(0,12);
        fd=socket(AF_INET,SOCK_DGRAM,0);
        if(fd<0) return;
        const int flags=fcntl(fd,F_GETFL,0);
        if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) {close(fd);fd=-1;return;}
        diagnostic("live-telemetry-enabled");
#endif
    }
    ~Impl() {
#ifdef __SWITCH__
        if(fd>=0) close(fd);
#endif
    }
    void clearWindow() {
        window=now;loops=uploads=gapSamples=0;uploadUs=0;uploadMaxUs=0;
        workUs=0;workMaxUs=0;presentUs=0;presentMaxUs=0;loopUs=0;loopMaxUs=0;
        loopOver25=0;loopOver50=0;gapOverflow=0;maxGap=0;histogram.fill(0);
    }
};
LiveTelemetry::LiveTelemetry() noexcept {try{impl_=std::make_unique<Impl>();}catch(...){diagnostic("live-telemetry-init-unavailable");}}
LiveTelemetry::~LiveTelemetry()=default;
bool LiveTelemetry::enabled() const noexcept {return impl_ && impl_->fd>=0;}
bool LiveTelemetry::observe(uint32_t ticks,bool streaming,bool submitted,double uploadUs,
                            double workUs,double presentUs,double loopUs,
                            bool activeUi,bool statsFlag) noexcept {
    if(!impl_ || impl_->fd<0) return false;
    auto &p=*impl_;p.now=ticks;
    if(!p.started || p.streaming!=streaming) {
        p.started=true;p.streaming=streaming;p.lastFrame=0;p.sessionStart=ticks;p.clearWindow();
    }
    ++p.loops;
    p.workUs+=workUs;p.workMaxUs=std::max(p.workMaxUs,workUs);
    p.presentUs+=presentUs;p.presentMaxUs=std::max(p.presentMaxUs,presentUs);
    p.loopUs+=loopUs;p.loopMaxUs=std::max(p.loopMaxUs,loopUs);
    if(loopUs>25000) ++p.loopOver25;
    if(loopUs>50000) ++p.loopOver50;
    p.activeUi=activeUi;p.statsFlag=statsFlag;
    if(streaming && submitted) {
        if(p.lastFrame) {
            const auto gap=uint32_t(ticks-p.lastFrame);
            const auto bin=std::min<uint32_t>(gap,512);
            ++p.histogram[bin];++p.gapSamples;p.maxGap=std::max(p.maxGap,gap);
            if(gap>=512) ++p.gapOverflow;
        }
        p.lastFrame=ticks;++p.uploads;++p.totalUploads;p.uploadUs+=uploadUs;
        p.uploadMaxUs=std::max(p.uploadMaxUs,uploadUs);
    }
    return uint32_t(ticks-p.window)>=1000;
}
void LiveTelemetry::publish(const StreamStatsSnapshot &s,bool vsync,bool nv12,double refreshMs,unsigned stableSwaps,
                            bool nativeDisplay,uint64_t directFrames,uint64_t mapHits,uint64_t mapImports,uint64_t mapCached) noexcept {
    (void)nv12;
    if(!impl_ || impl_->fd<0) return;
    auto &p=*impl_;
    try {
        unsigned p95=0;uint64_t count=0;
        const auto threshold=(p.gapSamples*95+99)/100;
        if(threshold) for(size_t bin=0;bin<p.histogram.size();++bin) {
            count+=p.histogram[bin];if(count>=threshold) {p95=unsigned(bin);break;}
        }
        const auto elapsed=std::max<uint32_t>(p.now-p.window,1u);
        auto r=[](double value){return std::isfinite(value)?std::round(value*1000)/1000:0.0;};
        auto stageUs=[](double value){return std::isfinite(value)?std::llround(value):0LL;};
        auto avg=[](double sum,uint64_t count){return count?sum/double(count):0.0;};
        auto small=[](std::string value){
            if(value.size()>96) {
                size_t n=96;
                while(n>0 && (static_cast<unsigned char>(value[n])&0xc0)==0x80)--n;
                value.resize(n);
            }
            return value;
        };
        const auto nowMs=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto stageSendUs=decodeStageState.sendUs.load(std::memory_order_relaxed);
        const auto stageRecvUs=decodeStageState.recvUs.load(std::memory_order_relaxed);
        const auto stagePublishUs=decodeStageState.publishUs.load(std::memory_order_relaxed);
        const auto seq=++p.sequence;
        uint64_t batchCalls=0,batchMessages=0,batchMulti=0,batchMax=0,batchFallback=0;int batchError=0;
        int udpReceiveBytes=-1, udpSendBytes=-1, udpReceiveError=-1, udpSendError=-1, udpMatch=0;
#ifdef __SWITCH__
        uu_juice_batch_diagnostics(&batchCalls,&batchMessages,&batchMulti,&batchMax,&batchFallback,&batchError);
        // The selected endpoint can be a translated address. Only report a
        // socket match when its bound port actually matches; unknown stays -1.
        const auto endpointEnd=s.localEndpoint.find(' ');
        const auto colon=s.localEndpoint.rfind(':',endpointEnd);
        if(s.transport.find("UDP")!=std::string::npos &&
           s.localEndpoint.find("[host]")!=std::string::npos && colon!=std::string::npos) {
            const auto portText=s.localEndpoint.substr(colon+1,
                endpointEnd==std::string::npos?std::string::npos:endpointEnd-colon-1);
            if(!portText.empty() && portText.size()<=5 &&
               std::all_of(portText.begin(),portText.end(),[](unsigned char c){return c>='0'&&c<='9';})) {
                const auto port=std::strtoul(portText.c_str(),nullptr,10);
                if(port>0 && port<=65535) udpMatch=uu_juice_udp_socket_diagnostics(uint16_t(port),
                    &udpReceiveBytes,&udpSendBytes,&udpReceiveError,&udpSendError);
            }
        }
#endif
        auto base=[&](const char *kind){
            return Json{{"schema",
#ifdef UU_MEDIA_DISPATCH
                17
#else
                16
#endif
                },{"parts",4},{"kind",kind},{"pair",p.pair},{"session",p.session},
                {"seq",seq},{"epoch",s.mediaEpoch},{"ticks_ms",p.now},{"streaming",p.streaming?1:0}};
        };
        std::array<Json,4> groups;
        uu_cpu_mark(2);
        const auto cpu=cpuDiagnosticsSnapshot();
        groups[0]=base("core");
        groups[0].update(Json{
            {"version",UU_VERSION},{"width",s.width},{"height",s.height},{"requested_fps",s.requestedFps},
            {"codecflag",s.codec.rfind("H.265",0)==0?1:0},{"actualmbps",r(s.video.bitrateMbps)},
            {"targetcap",s.maxBitrateMbps},{"hostknown",s.hostBitrateKnown?1:0},
            {"hostmbps",s.hostBitrateKnown?s.hostBitrateMbps:-1},
            {"feedback",s.bitrateFeedbackSent?1:0},{"vsync",vsync?1:0},
            {"buffer",s.playoutDelayMs},{"pacing",s.pacing?1:0},
            {"snapshot_age_ms",s.sampledMs?std::max<int64_t>(0,nowMs-s.sampledMs):-1},
            {"session_age_ms",p.now-p.sessionStart},{"sent",p.sent},{"dropdiag",p.failed},
            {"path",small(s.transport)},{"local",small(s.localEndpoint)},{"remote",small(s.remoteEndpoint)},
            {"send_us",stageUs(stageSendUs)},{"recv_us",stageUs(stageRecvUs)},{"publish_us",stageUs(stagePublishUs)},
            {"cpu_net",r(cpu[0].percent)},{"cpu_dec",r(cpu[1].percent)},
            {"cpu_ui",r(cpu[2].percent)},{"cpu_ctl",r(cpu[3].percent)},
            {"mask_net",cpu[0].mask},{"mask_dec",cpu[1].mask},
            {"mask_ui",cpu[2].mask},{"mask_ctl",cpu[3].mask},
            {"cpu_net_rc",cpu[0].error},{"cpu_dec_rc",cpu[1].error},
            {"cpu_ui_rc",cpu[2].error},{"cpu_ctl_rc",cpu[3].error},
            {"udp_match",udpMatch},{"udp_rx_bytes",udpReceiveBytes},{"udp_tx_bytes",udpSendBytes},
            {"udp_rx_errno",udpReceiveError},{"udp_tx_errno",udpSendError}});
        groups[1]=base("network");
        groups[1].update(Json{
            {"rtt_ms",s.rttMs},{"jitter_ms",r(s.video.jitterMs)},{"initial_gap_pct",r(s.video.lossPercent)},
            {"packets",s.video.packets},{"lost",s.video.lost},
            {"accepted",s.receiverAccepted},{"rx_drop",s.receiverDropped},{"wait_idr",s.receiverWaitingIdr},
            {"idr_ff",s.receiverFastForwards},{"wait_recent",s.receiverRecentWaitMs},{"wait_max",s.receiverMaxWaitMs},
            {"pending",s.receiverPending},{"block_ms",s.receiverBlockedMs},{"repair_ms",r(s.repairRttMs)},
            {"nack",s.nackSent},{"rtx",s.rtxRecovered},{"pli",s.receiverPli},
            {"nack_retry",s.nackRetryMs},{"nack_repeat",s.nackRepeated},{"kf_requests",s.keyframeRequests},
            {"rx_buf",s.rxBufferedBytes},{"rx_buf_high",s.rxBufferedHigh},
            {"rx_msgs",s.rxInputMessages},{"rx_batches",s.rxInputBatches},{"rx_max_batch",s.rxMaxBatch},
            {"rx_in_calls",s.rxIncomingCalls},{"rx_tick_calls",s.rxTickCalls},
            {"rx_in_us",s.rxIncomingTotalUs},{"rx_in_max_us",s.rxIncomingMaxUs},
            {"rx_tick_us",s.rxTickTotalUs},{"rx_tick_max_us",s.rxTickMaxUs},
            {"rx_mem_drop",s.rxMemoryBoundRetires},{"rx_frame_max",s.rxMaxFrameBytes},
            {"repair_paced",s.repairPaced?1:0},{"nack_seq",s.nackSequences},
            {"nack_defer",s.nackBudgetDeferrals},{"nack_old",s.nackExpiredScans},
            {"udp_calls",batchCalls},{"udp_msgs",batchMessages},{"udp_multi",batchMulti},
            {"udp_batch_max",batchMax},{"udp_fallback",batchFallback},{"udp_batch_rc",batchError}});
        groups[2]=base("media");
        groups[2].update(Json{
            {"decoded",s.decodedTotal},{"published",s.publishedTotal},{"cpu_xfer",s.cpuTransfers},
            {"gpu_pub",s.nativePublished},{"dec_fps",r(s.decodeFps)},{"dec_ema",r(s.decodeMs)},
            {"dec_err",s.decodeErrors},{"drops",s.droppedFrames},{"q_in",s.inputQueued},
            {"ready",s.readyFrames},{"skip_due",s.skipDue},{"skip_bound",s.skipBound},
            {"late",s.selectedLateMs},{"hw",s.hardwareFrame?1:0},
            {"au_max",s.auMaxBytes},{"au_bytes",s.auBytesTotal},{"au_count",s.auCount},
            {"au_over",s.auOversize},{"q_high_frames",s.queueHighFrames},{"q_high_bytes",s.queueHighBytes},
            {"q_wait_last",s.queueWaitLastMs},{"q_wait_max",s.queueWaitMaxMs},{"q_wait_oldest",s.oldestQueueWaitMs},
            {"dec_max",r(s.decodeMaxMs)},{"dec_gt8",s.decodeGt8},{"dec_gt16",s.decodeGt16},
            {"dec_gt33",s.decodeGt33},{"last_dec_age",s.lastDecodedAgeMs}});
        groups[3]=base("render");
        groups[3].update(Json{
            {"display_fps",r(p.uploads*1000.0/elapsed)},{"render_hz",r(p.loops*1000.0/elapsed)},
            {"upload_avg_ms",r(avg(p.uploadUs,p.uploads)/1000.0)},{"upload_max_ms",r(p.uploadMaxUs/1000.0)},
            {"work_avg_ms",r(avg(p.workUs,p.loops)/1000.0)},{"work_max_ms",r(p.workMaxUs/1000.0)},
            {"present_avg_ms",r(avg(p.presentUs,p.loops)/1000.0)},{"present_max_ms",r(p.presentMaxUs/1000.0)},
            {"loop_avg_ms",r(avg(p.loopUs,p.loops)/1000.0)},{"loop_max_ms",r(p.loopMaxUs/1000.0)},
            {"loop_over25",p.loopOver25},{"loop_over50",p.loopOver50},
            {"gap_p95_ms",p95},{"gap_max_ms",p.maxGap},
            {"gap_overflow",p.gapOverflow},
            {"frame_age_ms",p.streaming&&p.lastFrame?uint32_t(p.now-p.lastFrame):0},
            {"present_total",p.totalUploads},{"native_display",nativeDisplay?1:0},
            {"gpu_present",directFrames},{"map_hit",mapHits},{"map_new",mapImports},{"map_live",mapCached},
            {"active_ui",p.activeUi?1:0},{"stats_flag",p.statsFlag?1:0},
            {"refresh_ms",r(refreshMs)},{"stable_swaps",stableSwaps}});
#ifdef UU_MEDIA_DISPATCH
        MediaDispatchQueue::Stats dispatch;
        const bool dispatchPresent=latestMediaDispatchStats(dispatch);
        groups[3].update(Json{{"md_enabled",1},{"md_active",dispatchPresent&&!dispatch.stopped?1:0},
            {"md_accepted",dispatch.accepted},{"md_done",dispatch.delivered},
            {"md_q",dispatch.queued},{"md_bytes",dispatch.retainedBytes},
            {"md_high_bytes",dispatch.highBytes},{"md_overflow",dispatch.overflow},
            {"md_fail",dispatch.callbackFailures+dispatch.schedulingFailures},
            {"md_wait_max_us",dispatch.maxWaitUs}});
#endif
        std::array<std::string,4> payloads;
        std::array<bool,4> valid{};
        size_t maxBytes=0,oversizedGroups=0;
        for(size_t i=0;i<groups.size();++i) {
            payloads[i]=groups[i].dump();
            maxBytes=std::max(maxBytes,payloads[i].size());
            if(payloads[i].size()<=kGroupLimit) valid[i]=true;
            else {++oversizedGroups;++p.oversizeCount;++p.failed;}
        }
        p.lastPayloadBytes=maxBytes;
        if(oversizedGroups && (p.lastSenderLog==0 || p.sequence-p.lastSenderLog>=10)) {
            diagnostic(("telemetry-sender schema12 oversized_groups="+std::to_string(oversizedGroups)+
                        " max_bytes="+std::to_string(maxBytes)+
                        " count="+std::to_string(p.oversizeCount)).c_str());
            p.lastSenderLog=p.sequence;
        }
        const auto sentBefore=p.sent;
#ifdef __SWITCH__
        for(size_t i=0;i<groups.size();++i) {
            if(!valid[i]) continue;
            const auto &payload=payloads[i];
            const auto ret=sendto(p.fd,payload.data(),payload.size(),0,
                reinterpret_cast<const sockaddr *>(&p.target),sizeof p.target);
            if(ret==ssize_t(payload.size())) ++p.sent;
            else {++p.failed;++p.sendFailures;}
        }
#else
        for(size_t i=0;i<groups.size();++i) if(valid[i]) ++p.failed;
#endif
        if(!p.firstPublishLogged && p.sent>sentBefore) {
            diagnostic(("telemetry-sender schema12 ok max_bytes="+std::to_string(maxBytes)).c_str());
            p.firstPublishLogged=true;
        }
        if(p.sendFailures && (p.lastSenderLog==0 || p.sequence-p.lastSenderLog>=10)) {
            diagnostic(("telemetry-sender schema12 send_failures="+std::to_string(p.sendFailures)+
                        " failed="+std::to_string(p.failed)).c_str());
            p.lastSenderLog=p.sequence;
        }
    } catch(...) {
        ++p.exceptionCount;++p.failed;
        if(p.lastSenderLog==0 || p.sequence-p.lastSenderLog>=10) {
            diagnostic(("telemetry-sender schema12 exception_count="+std::to_string(p.exceptionCount)).c_str());
            p.lastSenderLog=p.sequence;
        }
    }
    p.clearWindow();
}
}
