#include "cpu_diagnostics.hpp"
#include "client.hpp"
#include "config_store.hpp"
#include "quality_rpc.hpp"
#include "diagnostics.hpp"
#include "event_queue.hpp"
#include "turn_stream.hpp"
#include "rtc_diagnostics.hpp"
#include "rtc_signaling.hpp"
#include "signal_pump.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <chrono>
#include <cerrno>
#include <rtc/rtp.hpp>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#ifdef __SWITCH__
#include <switch.h>
#include <unistd.h>
#include <dirent.h>
#endif

namespace uu {
namespace {
bool verboseDiagnostics() {
    static const bool enabled=ConfigStore::instance().integer("diag_verbose",0,0,1)!=0;
    return enabled;
}
// Diagnostic-only, bounded JSON shape summary. String values are never
// printed; only keys, numeric/boolean values, string lengths and array sizes.
// Diagnostic-only bounded JSON shape representation. Recursively traverses
// JSON structure, showing object/array shapes and safe string values.
// Related: jsonFlatten() below provides an alternative flat representation.
// TODO: Consider extracting shared traversal logic into a visitor pattern.
std::string jsonShape(const Json &value,unsigned depth=0) {
    if(depth>5)return "...";
    if(value.is_object()) {
        std::string out="{";unsigned count=0;
        for(auto it=value.begin();it!=value.end() && count<32 && out.size()<1600;++it,++count) {
            if(out.size()>1)out+=",";
            out+=it.key()+":";
            const auto key=it.key();
            const bool allowString=key=="type" || key=="capture_impl" || key=="encoder_impl" ||
                key=="video_codec" || key=="codec" || key=="codec_name" || key=="chroma_sampling" ||
                key=="resolution_type" || key=="profile" || key=="level";
            if(allowString && it.value().is_string()) {
                auto text=it.value().get<std::string>();
                if(text.size()>48)text.resize(48);
                out+="\""+text+"\"";
            } else out+=jsonShape(it.value(),depth+1);
        }
        if(count<value.size())out+=",...";
        out+="}";return out;
    }
    if(value.is_array()) {
        std::string out="[";unsigned count=0;
        for(const auto &item:value) {
            if(count>=8){out+=",...";break;}
            if(count)out+=",";
            out+=jsonShape(item,depth+1);++count;
        }
        out+="]";return out;
    }
    if(value.is_number_integer())return std::to_string(value.get<long long>());
    if(value.is_number_unsigned())return std::to_string(value.get<unsigned long long>());
    if(value.is_number_float()){
        char out[32];std::snprintf(out,sizeof out,"%.3f",value.get<double>());return out;
    }
    if(value.is_boolean())return value.get<bool>()?"true":"false";
    if(value.is_string()) {
        const auto text=value.get<std::string>();
        if(!text.empty() && (text.front()=='{' || text.front()=='[')) {
            try { const auto nested=Json::parse(text); return "json-string("+jsonShape(nested,depth+1)+")"; }
            catch(...) {}
        }
        return "string("+std::to_string(text.size())+")";
    }
    if(value.is_null())return "null";
    return "?";
}
std::string streamerDataShape(const uint8_t *data,size_t size) {
    if(!data || !size)return "empty";
    if(size>65536)return "oversize-"+std::to_string(size);
    try {
        const auto value=Json::parse(std::string(reinterpret_cast<const char *>(data),size));
        return jsonShape(value);
    }catch(...) {
        return "non-json-size-"+std::to_string(size);
    }
}
// Diagnostic-only flattened path/value dump for forward_setting. Known safe
// strings may be shown; all other strings are reduced to their length.
// Related: jsonShape() above provides hierarchical shape representation.
// Both functions traverse JSON but produce different output formats.
void jsonFlatten(const Json &value,const std::string &path,std::string &out,unsigned depth=0) {
    if(out.size()>2800 || depth>8)return;
    if(value.is_object()) {
        unsigned count=0;
        for(auto it=value.begin();it!=value.end() && count<32 && out.size()<2800;++it,++count) {
            const auto child=path.empty()?it.key():path+"."+it.key();
            jsonFlatten(it.value(),child,out,depth+1);
        }
        return;
    }
    if(value.is_array()) {
        unsigned count=0;
        for(const auto &item:value) {
            if(count>=32 || out.size()>2800)break;
            jsonFlatten(item,path+"["+std::to_string(count)+"]",out,depth+1);++count;
        }
        return;
    }
    if(out.size()>1)out+=";";
    out+=path+"=";
    if(value.is_number_integer())out+=std::to_string(value.get<long long>());
    else if(value.is_number_unsigned())out+=std::to_string(value.get<unsigned long long>());
    else if(value.is_number_float()){char b[32];std::snprintf(b,sizeof b,"%.3f",value.get<double>());out+=b;}
    else if(value.is_boolean())out+=value.get<bool>()?"true":"false";
    else if(value.is_string()) {
        const auto text=value.get<std::string>();
        const bool safe=path.find("type")!=std::string::npos || path.find("codec")!=std::string::npos ||
            path.find("impl")!=std::string::npos || path.find("profile")!=std::string::npos ||
            path.find("level")!=std::string::npos || path.find("resolution_type")!=std::string::npos;
        if(safe && text.size()<=48)out+=text;else out+="string("+std::to_string(text.size())+")";
    }else if(value.is_null())out+="null";else out+="?";
}
// Bounded, read-only summary of PC capability values. Never includes client_id,
// tokens, ICE credentials or arbitrary strings.
struct ForwardSettingInfo {
    std::string display,codecs,capture,encoder;
    int track=-1;
    std::string summary() const {
        std::string out;
        auto append=[&](const std::string &value){
            if(value.empty())return;
            if(!out.empty())out+=";";
            out+=value;
        };
        append(display);
        append(codecs);
        std::string sender;
        if(!capture.empty())sender+="capture-"+capture;
        if(!encoder.empty()){if(!sender.empty())sender+="-";sender+="encoder-"+encoder;}
        if(track>=0){if(!sender.empty())sender+="-";sender+="track"+std::to_string(track);}
        append(sender);
        return out;
    }
};
std::optional<ForwardSettingInfo> parseForwardSetting(const Json &event) {
    if(!event.is_array() || event.size()<2 || !event[1].is_object())return std::nullopt;
    const auto &root=event[1];
    if(!root.contains("data") || !root["data"].is_object())return std::nullopt;
    const auto &data=root["data"];
    ForwardSettingInfo out;
    if(data.contains("device_capability") && data["device_capability"].is_object()){
        const auto &cap=data["device_capability"];
        if(cap.contains("display_info") && cap["display_info"].is_array()){
            unsigned n=0;
            for(const auto &d:cap["display_info"]){
                if(n++>=8)break;
                if(!d.is_object())continue;
                if(!out.display.empty())out.display+=";";
                out.display+="id"+std::to_string(d.value("id",0))+
                    "-fps"+std::to_string(d.value("fps",0))+
                    "-hdr"+std::to_string(d.value("hdr",0))+
                    "-type"+std::to_string(d.value("type",0));
            }
        }
        if(cap.contains("video_codec_capability") && cap["video_codec_capability"].is_array()){
            unsigned n=0;
            for(const auto &c:cap["video_codec_capability"]){
                if(n++>=12)break;
                if(!c.is_object())continue;
                if(!out.codecs.empty())out.codecs+=";";
                out.codecs+="codec"+std::to_string(c.value("video_codec",0))+
                    "-impl"+std::to_string(c.value("codec_impl",0))+
                    "-bit"+std::to_string(c.value("bit_depth",0))+
                    "-chroma"+std::to_string(c.value("chroma_sampling",0))+
                    "-"+std::to_string(c.value("width",0))+"x"+std::to_string(c.value("height",0));
            }
        }
    }
    if(data.contains("sender_para_info") && data["sender_para_info"].is_object()){
        const auto &sender=data["sender_para_info"];
        out.capture=sender.value("capture_impl",std::string{});
        out.encoder=sender.value("encoder_impl",std::string{});
        out.track=sender.value("video_track_index",-1);
    }
    if(out.display.empty() && out.codecs.empty() && out.capture.empty() &&
       out.encoder.empty() && out.track<0)return std::nullopt;
    return out;
}
// Client capability sent to the PC through the control event's streamer_data.
// Resolution is a separate concept from the quality/bitrate preset; these
// entries bound the encoder output to what the Switch can actually present.
std::string clientCapabilityJson(int width,int height,int fps,int quality) {
    auto display=[&](int id){
        return Json{{"id",id},{"width",width},{"height",height},{"fps",fps},
                    {"hdr",-2},{"type",0},{"dpi_scale",100}};
    };
    auto capability=[&](int codec,int w,int h){
        return Json{{"video_codec",codec},{"codec_impl",1},{"bit_depth",8},
                    {"chroma_sampling",1},{"max_width",w},{"max_height",h},
                    {"width",w},{"height",h},{"max_frame_quality",quality},
                    {"result",0}};
    };
    Json cap;
    cap["remote_display_info"]=Json::array({display(0)});
    cap["local_display_info"]=Json::array({display(0)});
    cap["frame_quality_capability"]=Json::array({
        capability(1,1280,720),capability(2,1280,720),
        capability(1,1920,1080),capability(2,1920,1080)
    });
    return cap.dump();
}
// RTP and RTX have independent SSRC/sequence spaces (RFC 4588). The stock
// receiving session keeps one sequence counter; only feed the primary stream
// into that counter, while preserving raw RTX for the assembler downstream.
class PrimaryVideoRtcpSession final : public rtc::RtcpReceivingSession {
    uint32_t primary_ = 0;
    bool haveTransit_ = false, loggedReportFix_ = false;
    uint32_t previousTransit_ = 0;
    double jitter_ = 0;
    uint32_t reportExpected_ = 0, reportReceived_ = 0, reportBase_ = 0;
    bool haveReport_ = false;
    std::mutex sessionMutex_;
    static uint32_t read32(const uint8_t *p) {
        return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
    }
  public:
    bool requestKeyframe(const rtc::message_callback &send) override {
        std::lock_guard lock(sessionMutex_);
        return rtc::RtcpReceivingSession::requestKeyframe(send);
    }
    bool requestBitrate(unsigned int bitrate,const rtc::message_callback &send) override {
        std::lock_guard lock(sessionMutex_);
        return rtc::RtcpReceivingSession::requestBitrate(bitrate,send);
    }
    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override {
        std::lock_guard lock(sessionMutex_);
        // A single known-valid primary RTP packet needs no filtered copy.
        // RTX/control/invalid cases retain the original separate accounting path.
        const bool direct=messages.size()==1 && messages[0] &&
            messages[0]->type==rtc::Message::Binary && messages[0]->size()>=sizeof(rtc::RtpHeader) &&
            (std::to_integer<uint8_t>((*messages[0])[0])>>6)==2 &&
            (std::to_integer<uint8_t>((*messages[0])[1])&127)==96 &&
            (!primary_ || read32(reinterpret_cast<const uint8_t *>(messages[0]->data())+8)==primary_);
        rtc::message_vector primaryMessages;
        for (const auto &message : messages) {
            if (!message) continue;
            const auto *data = reinterpret_cast<const uint8_t *>(message->data());
            const auto size = message->size();
            if (size < 8 || data[0]>>6 != 2) continue;
            if (message->type == rtc::Message::Binary && size >= 12 && (data[1]&127) == 96) {
                const auto ssrc = read32(data+8);
                if (!primary_) {
                    primary_=ssrc;
                    initSeq(uint16_t(uint16_t(data[2])<<8 | data[3]));
                    mProbation=0;
                }
                if (ssrc == primary_) {
                    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    const uint32_t transit=uint32_t(ms*90)-read32(data+4);
                    if(haveTransit_) {
                        const int64_t delta=int32_t(transit-previousTransit_);
                        jitter_+=(double(delta<0?-delta:delta)-jitter_)/16.0;
                    }
                    previousTransit_=transit;haveTransit_=true;
                    if (!direct) primaryMessages.push_back(message);
                }
            } else if (message->type == rtc::Message::Control && primary_ &&
                       ((data[1] == 200 && size >= 28) || data[1] == 201) && read32(data+4) == primary_) {
                primaryMessages.push_back(message);
            }
        }
        auto &accounting=direct?messages:primaryMessages;
        rtc::RtcpReceivingSession::incoming(accounting,[this,&send](rtc::message_ptr message) {
            if(message && message->type==rtc::Message::Control &&
               message->size()>=rtc::RtcpRr::SizeWithReportBlocks(1)) {
                auto *rr=reinterpret_cast<rtc::RtcpRr *>(message->data());
                if(rr->header.payloadType()==201) {
                    // Report the actual extended maximum: cycles in the upper
                    // 16 bits, highest primary sequence in the lower 16 bits.
                    rr->getReportBlock(0)->setSeqNo(mMaxSeq,uint16_t(mCycles>>16));
                    rr->getReportBlock(0)->setJitter(uint32_t(jitter_));
                    // RFC 3550 A.3: late/duplicate primary packets can make
                    // interval loss negative. Unsigned subtraction in the
                    // base session would wrap and advertise false loss.
                    const uint32_t expected=mCycles+uint32_t(mMaxSeq)-mBaseSeq+1;
                    const bool sameSource=haveReport_ && reportBase_==mBaseSeq &&
                        expected>=reportExpected_ && mReceived>=reportReceived_;
                    const uint32_t expectedInterval=sameSource?expected-reportExpected_:expected;
                    const uint32_t receivedInterval=sameSource?mReceived-reportReceived_:mReceived;
                    reportExpected_=expected;reportReceived_=mReceived;reportBase_=mBaseSeq;
                    haveReport_=true;
                    const int64_t intervalLost=int64_t(expectedInterval)-int64_t(receivedInterval);
                    const auto fraction=uint8_t(expectedInterval && intervalLost>0?
                        std::min<int64_t>(255,(intervalLost*256)/expectedInterval):0);
                    const auto totalLost=std::clamp<int64_t>(int64_t(expected)-int64_t(mReceived),
                                                            -8388608,8388607);
                    rr->getReportBlock(0)->setPacketsLost(fraction,uint32_t(totalLost));
                    if(!loggedReportFix_) {diagnostic("rtcp-primary-feedback-corrected");loggedReportFix_=true;}
                }
            }
            send(std::move(message));
        });
        // Keep the original packet order and both payload types for the next
        // receiver. Only accounting is filtered; retransmissions are not lost.
    }
};
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
std::string configValue(const std::string &wanted) {
    return ConfigStore::instance().value(wanted);
}
int configInt(const std::string &key, int fallback, int minimum, int maximum) {
    return ConfigStore::instance().integer(key,fallback,minimum,maximum);
}
bool diagEnabled() {
    const auto value = configValue("diag");
    return !value.empty() && value != "0" && value != "off" && value != "no";
}
const char *protocolLabel(rtc::Candidate::TransportType type) {
    using Type = rtc::Candidate::TransportType;
    switch (type) {
    case Type::Udp:
        return "UDP";
    case Type::TcpActive:
    case Type::TcpPassive:
    case Type::TcpSo:
    case Type::TcpUnknown:
        return "TCP";
    default:
        return "—";
    }
}
bool privateCandidate(rtc::Candidate &candidate) {
    candidate.resolve(rtc::Candidate::ResolveMode::Simple);
    const auto address = candidate.address();
    if (!address) return false;
    if (candidate.family() == rtc::Candidate::Family::Ipv6) {
        std::string a = *address;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return a.rfind("fc", 0) == 0 || a.rfind("fd", 0) == 0 || a.rfind("fe80:", 0) == 0;
    }
    unsigned a=0,b=0,c=0,d=0;
    return std::sscanf(address->c_str(), "%u.%u.%u.%u", &a,&b,&c,&d) == 4 &&
           (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254));
}
std::string candidateIdentity(rtc::Candidate candidate) {
    if(!candidate.resolve(rtc::Candidate::ResolveMode::Simple)) return {};
    if(!candidate.address() || !candidate.port()) return {};
    return *candidate.address()+"|"+std::to_string(*candidate.port())+"|"+protocolLabel(candidate.transportType());
}
std::string selectedPath(rtc::Candidate local, rtc::Candidate remote, bool knownRemoteRelay) {
    using Type = rtc::Candidate::Type;
    const bool privateLocal = privateCandidate(local), privateRemote = privateCandidate(remote);
    const bool relay = local.type() == Type::Relayed || remote.type() == Type::Relayed;
    std::string path = relay ? "TURN 中继" : knownRemoteRelay ? "远端中继（端点匹配）" : privateLocal && privateRemote ? "私网地址直连" : "P2P 直连";
    const auto family = remote.family();
    path += family == rtc::Candidate::Family::Ipv6 ? " · IPv6" :
            family == rtc::Candidate::Family::Ipv4 ? " · IPv4" : " · 地址族未知";
    return path + " · " + protocolLabel(local.transportType());
}
std::string endpoint(rtc::Candidate candidate) {
    candidate.resolve(rtc::Candidate::ResolveMode::Simple);
    const auto address=candidate.address();
    const auto port=candidate.port();
    std::string result=address.value_or("未知地址");
    if (candidate.family()==rtc::Candidate::Family::Ipv6) result="["+result+"]";
    if (port) result+=":"+std::to_string(*port);
    using Type=rtc::Candidate::Type;
    const char *type=candidate.type()==Type::Host ? "host" : candidate.type()==Type::ServerReflexive ? "srflx" :
                     candidate.type()==Type::PeerReflexive ? "prflx" : candidate.type()==Type::Relayed ? "relay" : "unknown";
    return result+" ["+type+"]";
}
void recordSelectedPath(const std::string &summary) {
    if (!diagEnabled()) return;
#ifdef __SWITCH__
    const std::string dir="sdmc:/switch/UURemote-ns/diag";
    mkdir(dir.c_str(),0777);
#else
    const std::string dir="diag";
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(),0777);
#endif
#endif
    std::ofstream file(dir+"/selected_path.txt");
    file << summary;
}
void saveQuality(int width, int height, int fps, int quality, bool vsync, int playoutDelay, int maxBitrateMbps, bool hevc) {
#ifdef __SWITCH__
    const std::string path = "sdmc:/switch/UURemote-ns/config.ini";
#else
    const std::string path = "config.ini";
#endif
    std::ifstream input(path);
    std::string line, retained, framePacing = "1";
    while (std::getline(input, line)) {
        const auto key = trim(line.substr(0, line.find('=')));
        if (key == "frame_pacing") {
            const auto value = trim(line.substr(line.find('=') + 1));
            framePacing = value == "0" ? "0" : "1";
            continue;
        }
        if (key != "width" && key != "height" && key != "fps" && key != "quality" && key != "vsync" &&
            key != "playout_delay_ms" && key != "max_bitrate_mbps" && key != "video_codec") retained += line + '\n';
    }
    input.close();
    std::ofstream output(path + ".quality-tmp", std::ios::trunc);
    output << retained << "width=" << width << "\nheight=" << height << "\nfps=" << fps << "\nquality=" << quality << '\n';
    output << "vsync=" << (vsync ? 1 : 0) << '\n';
    output << "frame_pacing=" << framePacing << "\nplayout_delay_ms=" << playoutDelay << '\n';
    output << "max_bitrate_mbps=" << maxBitrateMbps << '\n';
    output << "video_codec=" << (hevc?"hevc":"h264") << '\n';
    output.close();
    if (!output) throw std::runtime_error("画质设置保存失败");
    const auto backup = path + ".bak";
    std::ifstream existing(path);
    const bool hadConfig = bool(existing);
    existing.close();
    if (hadConfig) {
        // Keep exactly one fixed backup; the old timestamped copies are no
        // longer generated. Remove any previous backup before replacing it.
        std::remove(backup.c_str());
        if (std::rename(path.c_str(), backup.c_str()) != 0)
            throw std::runtime_error("画质设置保存失败");
    }
    if (std::rename((path + ".quality-tmp").c_str(), path.c_str()) != 0) {
        if (hadConfig) std::rename(backup.c_str(), path.c_str());
        throw std::runtime_error("画质设置保存失败，已尝试恢复原配置");
    }
#ifdef __SWITCH__
    // Remove legacy timestamped backups left by earlier builds. Only the fixed
    // config.ini.bak remains.
    if (DIR *dir = opendir("sdmc:/switch/UURemote-ns")) {
        const std::string prefix = "config.ini.before-quality-";
        while (auto *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind(prefix, 0) == 0)
                std::remove(("sdmc:/switch/UURemote-ns/" + name).c_str());
        }
        closedir(dir);
    }
#endif
    ConfigStore::instance().reload();
}
} // namespace
void Client::saveIceDiagnostics() {
    if(!diagEnabled() || !iceDiagnostics_.is_object()) return;
    try {
#ifdef __SWITCH__
        const std::string dir="sdmc:/switch/UURemote-ns/diag";
        mkdir(dir.c_str(),0777);
#else
        const std::string dir="diag";
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(),0777);
#endif
#endif
        std::ofstream file(dir+"/ice_candidates.json");
        if(file) file << iceDiagnostics_.dump(2);
    } catch(...) { diagnostic("ice-diagnostics-unavailable"); }
}
void Client::recordIceCandidate(const char *direction, const char *stage, rtc::Candidate candidate) {
    // Keep relay correlation even when detailed diagnostics are disabled.
    if(std::string(direction)=="remote" && candidate.type()==rtc::Candidate::Type::Relayed) {
        const auto identity=candidateIdentity(candidate);
        if(!identity.empty()) remoteRelayEndpoints_.insert(identity);
    }
    if(!diagEnabled() || !iceDiagnostics_.is_object()) return;
    try {
        // Record only parsed numeric endpoints and public ICE attributes.
        // Never dump the signaling object, candidate tail, ufrag, or password.
        candidate.resolve(rtc::Candidate::ResolveMode::Simple);
        auto &entries=iceDiagnostics_["candidates"];
        const auto address=endpoint(candidate);
        for(auto &entry:entries) {
            if(entry["direction"]==direction && entry["endpoint"]==address &&
               entry["priority"]==candidate.priority()) {
                auto &stages=entry["stages"];
                if(std::find(stages.begin(),stages.end(),Json(stage))==stages.end()) {
                    stages.push_back(stage); saveIceDiagnostics();
                }
                return;
            }
        }
        if(entries.size()>=256) {iceDiagnostics_["truncated"]=true;return;}
        entries.push_back({{"direction",direction},{"endpoint",address},{"priority",candidate.priority()},
                           {"address",candidate.address().value_or("")},{"port",candidate.port().value_or(0)},
                           {"candidate_type",int(candidate.type())},
                           {"resolved",candidate.isResolved()},{"private_address",privateCandidate(candidate)},
                           {"protocol",protocolLabel(candidate.transportType())},
                           {"stages",Json::array({stage})}});
        saveIceDiagnostics();
    } catch(...) {diagnostic("ice-candidate-diagnostic-unavailable");}
}
Client::Client(std::string key, std::string client, std::shared_ptr<Media> media) : media_(std::move(media)) {
    streamWidth_ = configInt("width", 1280, 320, 1920);
    streamHeight_ = configInt("height", 720, 180, 1080);
    const bool supportedResolution=
        (streamWidth_==960 && streamHeight_==540) ||
        (streamWidth_==1280 && streamHeight_==720) ||
        (streamWidth_==1920 && streamHeight_==1080);
    if(!supportedResolution){
        diagnostic(("config-resolution-normalized-"+std::to_string(streamWidth_)+"x"+
                    std::to_string(streamHeight_)).c_str());
        streamWidth_=1280;streamHeight_=720;
    }
    streamFps_ = configInt("fps", 60, 30, 60);
    streamQuality_ = configInt("quality", 3, 1, 6);
    maxBitrateMbps_=configInt("max_bitrate_mbps",0,0,100);
    if(streamQuality_==6 && maxBitrateMbps_==0)streamQuality_=4;
    state_.maxBitrateMbps=maxBitrateMbps_;
    hevc_=configValue("video_codec")=="hevc";state_.hevc=hevc_;
    keyframeIntervalMs_ = configInt("keyframe_interval_ms", 4000, 0, 60000);
    rtxEnabled_ = configInt("rtx", 1, 0, 1) != 0;
    state_.width = streamWidth_; state_.height = streamHeight_; state_.fps = streamFps_; state_.quality = streamQuality_;
    state_.vsync = configInt("vsync",1,0,1) != 0;
    state_.playoutDelayMs = configInt("playout_delay_ms",8,0,100);
    // Media reads config.ini independently. Apply the 8 ms fallback only when
    // the key is absent and pacing is enabled, preserving saved values and an
    // explicit frame_pacing=0 (setPlayoutDelay would otherwise force pacing on).
    if(configValue("playout_delay_ms").empty() && configInt("frame_pacing",1,0,1)!=0)
        media_->setPlayoutDelay(state_.playoutDelayMs);
    autoReconnect_=configInt("auto_reconnect",1,0,1)!=0;
    reconnectLimit_=configInt("reconnect_attempts",5,1,10);
    api_ = std::make_unique<Api>(std::move(key), std::move(client), cancel_);
    worker_ = std::thread([this] { run(); });
}
Client::~Client() {
    stop_ = true;
    cancel_ = true;
    inbox_->cv.notify_all();
    worker_.join();
}
Snapshot Client::snapshot() {
    std::lock_guard lock(stateMutex_);
    return state_;
}
void Client::setState(const std::string &stage, const std::string &detail) {
    diagnostic(stage.c_str());
    std::lock_guard lock(stateMutex_);
    state_.stage = stage;
    state_.detail = detail;
    state_.streaming = stage == "connected";
    state_.authenticated = api_->authenticated();
}
void Client::post(std::string type, Json data, uint64_t generation) {
    std::lock_guard lock(inbox_->mutex);
    inbox_->events.push_back({std::move(type), std::move(data), generation});
    inbox_->cv.notify_one();
}
void Client::command(const std::string &name, Json data) {
    if (name == "disconnect" || name == "logout" || name == "cancel" || name == "sms-open" || name == "login")
        cancel_ = true;
    post(name, std::move(data));
}
void Client::drainEvents() {
    drainEventBatch(inbox_->events,inbox_->mutex,[this]{return stop_.load();},[this](const Event &e){
        if(e.generation && e.generation!=generation_) return;
        handle(e);
    });
}
void Client::finishTextInput(int result,const char *notice) {
    pendingText_.reset();
    std::lock_guard lock(stateMutex_);
    state_.textPending=false;state_.textResult=result;state_.textNotice=notice;++state_.textRevision;
}
void Client::run() {
    while (!stop_) {
        uu_cpu_mark(3);
        try {
            {
                std::unique_lock lock(inbox_->mutex);
                if (inbox_->events.empty())
                    inbox_->cv.wait_for(lock, std::chrono::milliseconds(4));
            }
            drainEvents();
            if (stop_)
                break;
            if(reconnectAt_ && monotonicMs()>=reconnectAt_ && !cancel_) {
                reconnectAt_=0;
                if(reconnectArmed_ && api_->authenticated())
                    post("retry-connect",{{"deviceId",connectedDeviceId_}},generation_);
            }
            if(connectedSince_ && monotonicMs()-connectedSince_>=30000) {
                reconnectAttempts_=0;connectedSince_=0;
            }
            tickQuality();
            tickVideoScale();
            if(pendingText_ && monotonicMs()>=pendingText_->deadline)
                finishTextInput(3,"电脑未确认文字同步，请重试；尚未粘贴");
            if (strictVideo_)
                strictVideo_->tick();
            // Ask the sender for a new IDR frame when the decoder lost
            // references, when the picture stalled, or periodically as an
            // anti-corruption safety net on lossy Wi-Fi links.
            if (video_ && snapshot().streaming) {
                const auto now = monotonicMs();
                if (now - lastKeyframeRequest_ >= 700) {
                    const bool requested = media_->takeKeyframeRequest();
                    if (requested && strictVideo_)
                        strictVideo_->requestRecovery();
                    const bool stalled = media_->frameAgeMs() > 1500;
                    const bool periodic =
                        keyframeIntervalMs_ > 0 && now - lastKeyframeRequest_ >= keyframeIntervalMs_;
                    if (requested || stalled || periodic) {
                        video_->requestKeyframe();
                        lastKeyframeRequest_ = now;
                        ++keyframeRequests_;
                        pendingKeyframeAt_ = now;
                        keyframeCountAtRequest_ = media_->keyframeCount();
                        diagnostic(requested ? "rtc-keyframe-media"
                                             : stalled ? "rtc-keyframe-stall" : "rtc-keyframe-periodic");
                    }
                }
            }
            if (pendingKeyframeAt_ && media_->keyframeCount() != keyframeCountAtRequest_) {
                lastKeyframeRecoveryMs_ = int(monotonicMs() - pendingKeyframeAt_);
                pendingKeyframeAt_ = 0;
            }
            if (monotonicMs() - lastStatsUpdate_ >= 500)
                refreshStats(monotonicMs());
            if (socket_) {
                pumpSignaling(
                    [this](WsMessage &m) {
                        const auto start = monotonicMs();
                        const bool received = socket_->receive(m);
                        const auto elapsed = monotonicMs() - start;
                        if (elapsed >= 100)
                            diagnostic(("signal-read-slow-ms-" + std::to_string(elapsed)).c_str());
                        return received;
                    },
                    [this](const WsMessage &m) { packet(m); },
                    [this] {
                        if (stop_ || cancel_)
                            return true;
                        std::lock_guard lock(inbox_->mutex);
                        return !inbox_->events.empty();
                    },
                    [] { return monotonicMs(); });
                // Connected/failed callbacks may arrive while processing SDP/ICE.
                // Apply them before the deadline, including callbacks queued at its boundary.
                drainEvents();
                if (socket_ && monotonicMs() - lastPacket_ > heartbeatTimeout_)
                    throw std::runtime_error("信令心跳超时");
            }
            if (deadline_ && monotonicMs() > deadline_) {
                if (peer_) {
                    diagnostic(("timeout-peer-state-" + std::to_string(int(peer_->state()))).c_str());
                    diagnostic(("timeout-ice-state-" + std::to_string(int(peer_->iceState()))).c_str());
                }
                throw std::runtime_error("连接超时，请检查电脑和网络后重试");
            }
            if (!qr_.is_null() && monotonicMs() >= qrNext_)
                pollQr();
        } catch (const std::exception &error) {
            if (stop_)
                break;
            const auto failure = connectionFailure(snapshot().stage, error.what());
            diagnosticException("client-exception", std::current_exception());
            const auto reason = rtcLogEvent(error.what());
            if (!reason.empty())
                diagnostic(reason.c_str());
            disconnect();
            if (cancel_) {
                cancel_ = false;
                reconnectArmed_ = false; // User cancelled, disable auto-reconnect
                setState("idle", "已取消");
            } else {
                const bool transient=reason=="failure-connect-timeout" || reason=="failure-signal-heartbeat" ||
                    reason=="failure-signal-read" || reason=="failure-signal-remote-close" ||
                    reason=="failure-signal-write-timeout" || reason=="failure-signal-write" ||
                    reason=="failure-peer-disconnected" || reason=="failure-ice" || reason=="failure-dtls-or-sctp";
                if(autoReconnect_ && reconnectArmed_ && api_->authenticated() && transient &&
                   reconnectAttempts_<reconnectLimit_ && !connectedDeviceId_.empty()) {
                    const int delay=std::min(16000,1000<<std::min(reconnectAttempts_,4));
                    ++reconnectAttempts_;reconnectAt_=monotonicMs()+delay;
                    diagnostic(("auto-reconnect-scheduled-"+std::to_string(reconnectAttempts_)).c_str());
                    setState("reconnecting",std::to_string(delay/1000)+" 秒后重新连接（"+
                        std::to_string(reconnectAttempts_)+"/"+std::to_string(reconnectLimit_)+"），B 取消");
                } else {reconnectAt_=0;reconnectArmed_=false;setState("error",failure);} // Fatal or non-transient error, disable auto-reconnect
            }
        } catch (...) {
            diagnostic("client-unknown-exception");
            reconnectAt_=0;reconnectArmed_=false;
            disconnect();
            cancel_ = false;
            setState("error", "发生异常，连接阶段已写入 connection.log");
        }
    }
    cancel_ = false;
    disconnect();
}
void Client::handle(const Event &event) {
    const auto &e = event.type;
    const auto &data = event.data;
    if(e=="disconnect" || e=="cancel" || e=="logout" || e=="login" || e=="sms-open") {
        reconnectArmed_=false;reconnectAt_=0;reconnectAttempts_=0;
    }
    if(e=="quality-screens") {
        const int screen=data.at("screen").get<int>();
        if(screen!=activeScreenId_){
            activeScreenId_=screen;
            pendingVideoScale_.reset();videoScaleAttempted_=false;videoScaleVerified_=false;nextVideoScaleCheck_=0;
            setVideoScaleNotice("屏幕信息已更新，等待核对视频尺寸");
            diagnostic(("quality-current-screen-"+std::to_string(screen)+"-count-"+std::to_string(data.at("count").get<unsigned>())).c_str());
            if(pendingQuality_ && pendingQuality_->screenId!=screen)
                fallbackQuality("电脑屏幕已变化，正在重新连接…");
        }
    } else if(e=="capture-setting-sync") {
        const unsigned syncFields=data.value("fields",0u);
        const bool hasFps=bool(syncFields&1u),hasQuality=bool(syncFields&2u);
        const bool hasWidth=bool(syncFields&4u),hasHeight=bool(syncFields&8u);
        const bool hasBitrate=bool(syncFields&16u);
        const int syncFps=data.value("fps",0)==1?30:(data.value("fps",0)==2?60:0);
        const int syncQuality=data.value("quality",0);
        const int syncWidth=data.value("width",0),syncHeight=data.value("height",0);
        const int64_t syncBitrate=data.value("bitrate",int64_t(0));
        const int syncMbps=int(syncBitrate>0?(syncBitrate+500000)/1000000:0);
        const int targetFps=pendingQuality_?pendingQuality_->target.fps:streamFps_;
        const int targetQuality=pendingQuality_?pendingQuality_->target.quality:streamQuality_;
        const int targetWidth=pendingQuality_?pendingQuality_->target.width:streamWidth_;
        const int targetHeight=pendingQuality_?pendingQuality_->target.height:streamHeight_;
        const int targetCap=pendingQuality_?pendingQuality_->target.cap:maxBitrateMbps_;
        const bool complete=hasFps && hasQuality && hasWidth && hasHeight;
        const bool matches=complete &&
            syncFps==targetFps && syncQuality==targetQuality &&
            syncWidth==targetWidth && syncHeight==targetHeight &&
            (!hasBitrate || !targetCap || syncMbps==targetCap);
        {
            std::lock_guard lock(stateMutex_);
            if(hasFps)state_.appliedFps=syncFps;
            if(hasQuality)state_.appliedQuality=syncQuality;
            if(hasWidth)state_.appliedWidth=syncWidth;
            if(hasHeight)state_.appliedHeight=syncHeight;
            if(hasBitrate){state_.appliedBitrateMbps=syncMbps;state_.appliedBitrateKnown=true;}
            state_.appliedValid=syncFields!=0;
            if(state_.appliedValid){
                state_.settingAccepted=true;
                // Partial sync is evidence for the fields it carries, not proof
                // that the whole request was applied.
                state_.settingApplied=matches;
                state_.settingRejected=false;
                state_.settingErrorCode=0;
                state_.settingSource="sync";
            }
        }
        diagnostic(("capture-setting-sync-fps-"+std::to_string(syncFps)+
                    "-quality-"+std::to_string(syncQuality)+
                    "-chroma-"+std::to_string(data.value("chroma",0))+
                    "-bitrate-"+std::to_string(syncBitrate)+
                    "-resolution-"+std::to_string(syncWidth)+"x-"+std::to_string(syncHeight)+
                    "-type-"+std::to_string(data.value("resolution_type",0))+
                    "-pixel-"+std::to_string(data.value("pixel_width",0))+"x-"+std::to_string(data.value("pixel_height",0))+
                    "-orientation-"+std::to_string(data.value("orientation",0))+
                    "-fields-"+std::to_string(syncFields)+
                    "-target-fps-"+std::to_string(targetFps)+
                    "-quality-"+std::to_string(targetQuality)+
                    "-resolution-"+std::to_string(targetWidth)+"x-"+std::to_string(targetHeight)+
                    "-matches-"+std::to_string(matches?1:0)).c_str());
    } else if(e=="screen-wire") {
        if(verboseDiagnostics())diagnostic(("screen-wire-"+data.get<std::string>()).c_str());
    } else if(e=="streamer-data") {
        if(verboseDiagnostics())diagnostic(("streamer-data-"+data.get<std::string>()).c_str());
    } else if(e=="channel-data") {
        diagnostic(("channel-data-"+data.value("label",std::string("?"))+"-"+data.value("shape",std::string("?"))).c_str());
    } else if (e == "quality-settings") {
        const int width = data.at("width").get<int>(), height = data.at("height").get<int>();
        const int fps = data.at("fps").get<int>(), quality = data.at("quality").get<int>();
        if (!((width == 960 && height == 540) || (width == 1280 && height == 720) ||
              (width == 1920 && height == 1080)) || (fps != 30 && fps != 60) || quality < 1 || quality > 6)
            throw std::runtime_error("画质设置无效");
        const bool vsync = data.value("vsync",true);
        const int playoutDelay=data.value("playout_delay_ms",state_.playoutDelayMs);
        if(playoutDelay<0 || playoutDelay>100) throw std::runtime_error("播放缓冲设置无效");
        const int maxBitrate=data.value("max_bitrate_mbps",maxBitrateMbps_);
        if(maxBitrate<0 || maxBitrate>100 || (quality==6 && maxBitrate==0)) throw std::runtime_error("自定义码率设置无效");
        const bool hevc=data.value("hevc",hevc_);
        if(pendingQuality_){std::lock_guard lock(stateMutex_);state_.qualityNotice="正在更新画质，请稍候";return;}
        const QualityValues target{width,height,fps,quality,maxBitrate,playoutDelay,vsync,hevc};
        const bool resolutionChanged=width!=streamWidth_ || height!=streamHeight_;
        const bool captureChanged=width!=streamWidth_ || height!=streamHeight_ || fps!=streamFps_ || quality!=streamQuality_ ||
            ((quality==6 || streamQuality_==6) && maxBitrate!=maxBitrateMbps_);
        const bool capChanged=maxBitrate!=maxBitrateMbps_;
        const bool streamChanged=captureChanged || capChanged || hevc!=hevc_;
        if(!peer_ || (!snapshot().streaming && !streamChanged)){commitQuality(target,false,"设置已保存，连接时应用");return;}
        if(!snapshot().streaming){reconnectQuality(target,"正在重新连接以应用设置…");return;}
        // Custom encoder limits are not a measurement of available bandwidth.
        // Rebuild the track when crossing modes so a prior stored REMB
        // cannot keep being repeated by the RTCP receiving session.
        if(hevc!=hevc_ || ((quality==6)!=(streamQuality_==6)) ||
           (quality!=6 && ((capChanged && maxBitrate==0) || (capChanged && !rembAvailable_)))){
            reconnectQuality(target,"正在重新连接以应用编码或码率设置…");return;
        }
        if(!captureChanged){
            try{commitQuality(target,capChanged,streamChanged?"码率反馈已在线更新":"显示设置已更新");}
            catch(...){reconnectQuality(target,"在线更新未完成，正在重连应用…");}
            return;
        }
        if(resolutionChanged){
            // PC 4.42 acknowledges ResolutionSettingRequest but does not apply
            // the new capture geometry until the capture session is rebuilt.
            // Reconnect explicitly instead of stalling for a timeout.
            diagnostic("resolution-change-explicit-reconnect");
            reconnectQuality(target,"分辨率变化需要重建编码会话…");
            return;
        }
        const auto it=channels_.find("TEXT_DATA_CHANNEL");
        if(it==channels_.end() || !it->second->isOpen() || it->second->bufferedAmount()>65536){
            reconnectQuality(target,"设置通道尚未就绪，正在重连应用…");return;
        }
        if(activeScreenId_<0){
            diagnostic("quality-current-screen-unavailable");
            reconnectQuality(target,"尚未收到电脑屏幕信息，正在重连应用…");return;
        }
        const auto id=nextQualityRpc_++;
        const bool qualityOnly=!resolutionChanged;
        const bool fpsChanged=fps!=streamFps_;
        const bool qualityChanged=quality!=streamQuality_;
        const bool customBitrateChanged=((quality==6 || streamQuality_==6) && maxBitrate!=maxBitrateMbps_);
        Bytes bytes;
        std::string rpcMode;
        QualityRpcKind rpcKind=QualityRpcKind::QualityOnly;
        if(qualityOnly && fpsChanged && !qualityChanged && !customBitrateChanged && fpsSettingSupported_){
            // Protocol-native FpsSettingRequest: frame rate only. This is safe
            // for custom-bitrate (q6) sessions because it carries no bitrate or
            // resolution field.
            bytes=fpsSettingRpc(id,fps);
            rpcMode="fps-setting";rpcKind=QualityRpcKind::Fps;
        }else if(qualityOnly && qualityChanged && !fpsChanged && !customBitrateChanged && quality!=6 && streamQuality_!=6 && qualitySettingSupported_){
            // Protocol-native VideoQualitySettingReqeust: frame quality only.
            bytes=videoQualitySettingRpc(id,quality);
            rpcMode="video-quality-setting";rpcKind=QualityRpcKind::VideoQuality;
        }else if(qualityOnly){
            bytes=qualityUpdateRpc(id,fps,quality,activeScreenId_,quality==6?maxBitrate:0);
            rpcMode="quality-only";rpcKind=QualityRpcKind::QualityOnly;
        }else{
            bytes=captureSettingsRpc(id,width,height,fps,quality,activeScreenId_,quality==6?maxBitrate:0);
            rpcMode="capture";rpcKind=QualityRpcKind::Capture;
        }
        diagnostic(("encoder-bitrate-request-online-id-"+std::to_string(id)+
                    "-bps-"+std::to_string(quality==6?uint64_t(maxBitrate)*1000000u:0)+
                    "-quality-"+std::to_string(quality)).c_str());
        pendingQuality_=PendingQuality{target,id,monotonicMs()+5000,false,false,activeScreenId_,rpcKind,false};
        {std::lock_guard lock(stateMutex_);state_.qualityPending=true;state_.qualityNotice="正在等待电脑确认画质…";
         state_.settingAccepted=false;state_.settingApplied=false;state_.appliedValid=false;state_.settingRejected=false;
         state_.settingErrorCode=0;state_.settingSource="pending";state_.appliedBitrateKnown=false;}
        try{
            if(!it->second->send(reinterpret_cast<const std::byte *>(bytes.data()),bytes.size())){
                fallbackQuality("设置发送受阻，正在重连应用…");return;
            }
            ++qualitySent_;
            diagnostic((rpcMode+"-rpc-sent-"+std::to_string(id)).c_str());
        }catch(...){fallbackQuality("设置发送失败，正在重连应用…");}
    } else if(e=="quality-rpc-ack") {
        if(pendingVideoScale_ && data.at("id").get<uint64_t>()==pendingVideoScale_->id){
            if(pendingVideoScale_->acknowledged)return;
            if(data.value("errors",0u) || data.value("other",0u)){
                pendingVideoScale_.reset();
                setVideoScaleNotice("电脑未接受视频缩放，桌面设置保持不变");
                diagnostic("video-scale-not-accepted");return;
            }
            const auto media=media_->stats();
            pendingVideoScale_->acknowledged=true;
            pendingVideoScale_->deadline=monotonicMs()+5000;
            pendingVideoScale_->mediaEpoch=media.epoch;
            pendingVideoScale_->publishedBefore=media.publishedTotal;
            nextVideoScaleCheck_=0;
            setVideoScaleNotice("电脑已应答，正在核对实际视频尺寸");
            diagnostic("video-scale-acknowledged");return;
        }
        if(!pendingQuality_ || pendingQuality_->acknowledged || data.at("id").get<uint64_t>()!=pendingQuality_->id)return;
        const bool capture=data.value("capture",false);
        const auto errors=data.value("errors",0u),other=data.value("other",0u);
        lastQualityCode_=data.value("code",0);lastQualityErrors_=errors;
        // Log only bounded numeric metadata, never remote strings or payloads.
        diagnostic(("quality-rpc-response-capture-"+std::to_string(capture)+
                    "-entries-"+std::to_string(data.value("entries",0u))+"-errors-"+std::to_string(errors)+"-code-"+std::to_string(data.value("code",0))+
                    "-other-"+std::to_string(other)).c_str());
        if(errors || other){
            diagnostic("quality-rpc-response-not-accepted");
            {
                std::lock_guard lock(stateMutex_);
                state_.settingAccepted=false;state_.settingApplied=false;
                state_.settingRejected=true;state_.settingErrorCode=data.value("code",0);
                state_.settingSource="ack";
            }
            const auto failedKind=pendingQuality_?pendingQuality_->kind:QualityRpcKind::QualityOnly;
            // A rejected fps/quality/bitrate update must not tear down a working
            // stream. Keep the old encoder settings, surface rejected state, and
            // let the user retry or reconnect explicitly.
            if(pendingQuality_ && failedKind==QualityRpcKind::QualityOnly){
                pendingQuality_.reset();++qualityFailed_;
                {std::lock_guard lock(stateMutex_);
                 state_.qualityPending=false;
                 state_.qualityNotice="电脑拒绝在线设置，保持当前串流";}
                qualityNoticeUntil_=monotonicMs()+8000;
                diagnostic("quality-update-rejected-stream-kept");
                return;
            }
            if(pendingQuality_ && !pendingQuality_->retried){
                const auto kind=pendingQuality_->kind;
                if(kind==QualityRpcKind::Fps || kind==QualityRpcKind::VideoQuality){
                    if(kind==QualityRpcKind::Fps)fpsSettingSupported_=false;
                    if(kind==QualityRpcKind::VideoQuality)qualitySettingSupported_=false;
                    const auto target=pendingQuality_->target;
                    pendingQuality_.reset();
                    const auto retryId=nextQualityRpc_++;
                    Bytes retry;
                    try{
                        retry=qualityUpdateRpc(retryId,target.fps,target.quality,activeScreenId_,
                                               target.quality==6?target.cap:0);
                    }catch(...){reconnectQuality(target,"兼容设置构造失败，正在重连应用…");return;}
                    pendingQuality_=PendingQuality{target,retryId,monotonicMs()+5000,false,false,activeScreenId_,
                        QualityRpcKind::QualityOnly,true};
                    {std::lock_guard lock(stateMutex_);state_.qualityPending=true;state_.qualityNotice="正在重试兼容设置…";state_.appliedValid=false;}
                    auto channel=channels_.find("TEXT_DATA_CHANNEL");
                    if(channel==channels_.end() || !channel->second->isOpen() ||
                       !channel->second->send(reinterpret_cast<const std::byte *>(retry.data()),retry.size())){
                        fallbackQuality("兼容设置通道不可用，正在重连应用…");return;
                    }
                    ++qualitySent_;
                    diagnostic(("setting-unsupported-fallback-quality-update-"+
                                std::to_string(retryId)).c_str());
                    return;
                }
            }
            fallbackQuality("电脑未确认在线设置，正在重连应用…");return;
        }
        // Original CaptureSettingResp leaves errors empty when the optional
        // capture body is absent. A matching header-only reply is an RPC
        // acknowledgment, not evidence of actual encoder FPS or quality.
        pendingQuality_->headerOnly=!capture;
        ++qualityAcked_;pendingQuality_->acknowledged=true;pendingQuality_->deadline=monotonicMs()+5000;
        {
            std::lock_guard lock(stateMutex_);
            state_.settingAccepted=true;
            if(!state_.appliedValid)state_.settingApplied=false;
            state_.settingRejected=false;state_.settingErrorCode=0;
            state_.settingSource="ack";
        }
        diagnostic(("quality-rpc-acked-"+std::to_string(pendingQuality_->id)).c_str());
        if(video_)video_->requestKeyframe();
        finishQuality();
    } else if(e=="startup"){
        cancel_=false;
        if(!api_->restoreSession()){post("login");return;}
        {std::lock_guard lock(stateMutex_);state_.nickname=api_->nickname();state_.qrUrl.clear();}
        setState("loading","正在恢复登录并同步设备…");
        try{refresh();}
        catch(...){
            if(!api_->authenticated()){post("login");return;}
            setState("idle","登录已保留，网络暂不可用；请刷新重试");
        }
    } else if (e == "login") {
        disconnect();
        cancel_ = false;
        {
            std::lock_guard lock(stateMutex_);
            state_.loginMode = "qr";
        }
        setState("login", "正在生成登录二维码…");
        api_->initialize();
        qr_ = api_->request("POST", "/api/v1/qrcode/gen/login");
        if (!qr_.is_object() || !qr_.contains("token") || !qr_.contains("qrcode_jump_url"))
            throw std::runtime_error("登录二维码响应不完整");
        qrExpires_ = monotonicMs() + 180000;
        qrNext_ = monotonicMs() + 1000;
        qrStatus_ = 1;
        {
            std::lock_guard lock(stateMutex_);
            state_.qrUrl = qr_["qrcode_jump_url"].get<std::string>();
        }
        setState("login", "请使用手机 UU远程 App 扫码并确认");
    } else if (e == "sms-open") {
        disconnect();
        cancel_ = false;
        {
            std::lock_guard lock(stateMutex_);
            state_.loginMode = "sms";
        }
        setState("sms", "输入手机号并发送验证码");
    } else if (e == "sms-code" || e == "sms-login") {
        cancel_ = false;
        auto request = [this](const std::string &path, const Json &body) {
            api_->initialize();
            return api_->request("POST", path, body);
        };
        if (e == "sms-code") {
            setState("sms-sending", "正在发送短信验证码…");
            sms_.sendCode(request, data, monotonicMs());
            {
                std::lock_guard lock(stateMutex_);
                state_.smsRetryAt = sms_.retryAt();
            }
            setState("sms", "验证码已发送，输入 6 位验证码后登录");
        } else {
            setState("sms-verifying", "正在验证短信验证码…");
            completeLogin(sms_.login(request, data));
        }
    } else if (e == "cancel" || e == "disconnect") {
        if(e=="disconnect"){
            const int graceMs=std::clamp(data.value("graceMs",0),0,1000);
            if(graceMs>0)std::this_thread::sleep_for(std::chrono::milliseconds(graceMs));
        }
        reconnectArmed_ = false; // User-initiated disconnect, disable auto-reconnect
        disconnect();
        cancel_ = false;
        {
            std::lock_guard lock(stateMutex_);
            state_.loginMode = "qr";
        }
        setState("idle", "");
    } else if (e == "logout") {
        disconnect();
        cancel_ = false;
        api_->logout();
        {
            std::lock_guard lock(stateMutex_);
            state_.devices = Json::array();
            state_.nickname.clear();
        }
        setState("idle", "");
    } else if (e == "refresh") {
        if (api_->authenticated()) {
            cancel_ = false;
            setState("loading", "正在同步设备…");
            refresh();
        } else
            setState("idle", "请先扫码登录");
    } else if (e == "connect" || e=="retry-connect") {
        if(e=="connect") {reconnectArmed_=true;reconnectAttempts_=0;reconnectAt_=0;}
        else if(!reconnectArmed_) return;
        connectedDeviceId_ = data.at("deviceId").get<std::string>();
        disconnect();
        {std::lock_guard lock(stateMutex_);state_.activeDeviceId=connectedDeviceId_;}
        cancel_ = false;
        setState("joining", "正在加入远控房间…");
        auto room = api_->request(
            "POST", "/api/v1/room/join/by_device/" + percentEncode(data.at("deviceId").get<std::string>()),
            {{"force_join", false}});
        auto url = roomSignalUrl(room);
        if (!room.contains("token") || !room["token"].is_string() ||
            room["token"].get_ref<const std::string &>().empty())
            throw std::runtime_error("加入房间未返回信令凭据");
        socket_ = std::make_unique<SignalSocket>(cancel_);
        setState("signaling", "正在连接 UU 信令服务器…");
        socket_->open(url, room.at("token").get<std::string>());
        lastPacket_ = monotonicMs();
        deadline_ = monotonicMs() + 45000;
    } else if(e=="text-channel-state") {
        std::lock_guard lock(stateMutex_);state_.textChannelReady=data.get<bool>();
    } else if(e=="control-channel-state") {
        std::lock_guard lock(stateMutex_);state_.controlChannelReady=data.get<bool>();
    } else if(e=="text-input-cancel") {
        if(pendingText_)finishTextInput(3,"已取消文字发送");
    } else if(e=="text-input") {
        if(pendingText_)return;
        const auto ch=channels_.find("TEXT_DATA_CHANNEL");
        if(!snapshot().streaming || ch==channels_.end() || !ch->second->isOpen() || ch->second->bufferedAmount()>65536){
            finishTextInput(3,"电脑输入通道尚未就绪，请稍后重试");return;
        }
        try{
            const auto id=nextQualityRpc_++;
            const auto bytes=clipboardTextRpc(id,data.at("text").get<std::string>());
            pendingText_=PendingText{id,monotonicMs()+5000};
            {std::lock_guard lock(stateMutex_);state_.textPending=true;state_.textResult=1;state_.textNotice="正在发送文字…";++state_.textRevision;}
            // false means accepted for queueing, not rejection. Only a matching
            // CB_SUCCEEDED response may trigger Ctrl+V on the control channel.
            ch->second->send(reinterpret_cast<const std::byte *>(bytes.data()),bytes.size());
        }catch(...){finishTextInput(3,"文字发送失败，草稿已保留");}
    } else if(e=="text-input-ack") {
        if(!pendingText_ || data.at("id").get<uint64_t>()!=pendingText_->id)return;
        if(data.at("status").get<unsigned>()!=1){finishTextInput(3,"电脑未接受文字，草稿已保留");return;}
        if(heldKeys_.count(17) || heldKeys_.count(86)){
            finishTextInput(3,"文字已同步，请松开 Ctrl 和 V 后手动粘贴");return;
        }
        const auto ch=channels_.find("CONTROL_DATA_CHANNEL");
        if(ch==channels_.end() || !ch->second->isOpen() || ch->second->bufferedAmount()>65536){
            finishTextInput(3,"文字已同步，但粘贴通道未就绪；可在电脑手动粘贴");return;
        }
        try{
            // Send one complete chord in order, with no retries that could
            // paste twice. Do not persist or log text/clipboard contents.
            for(const auto &[action,key]:std::array<std::pair<const char *,int>,4>{{
                {"kbd_press",17},{"kbd_press",86},{"kbd_release",86},{"kbd_release",17}}}){
                const auto wire=Json{{"action",action},{"key",key}}.dump();
                ch->second->send(reinterpret_cast<const std::byte *>(wire.data()),wire.size());
                if(std::string(action)=="kbd_press")heldKeys_.insert(key);else heldKeys_.erase(key);
            }
            finishTextInput(2,"已发送粘贴操作");
        }catch(...){release();finishTextInput(3,"粘贴发送失败，请检查电脑输入框");}
    } else if(e=="system-action") {
        auto inform=[&](const char *message){std::lock_guard lock(stateMutex_);state_.qualityNotice=message;qualityNoticeUntil_=monotonicMs()+6000;};
        if(data.contains("deviceId") && data.at("deviceId").get<std::string>()!=connectedDeviceId_){
            inform("连接电脑已变化，未应用上一台电脑的设置");return;
        }
        const auto channel=channels_.find(systemActionChannel);
        if(!snapshot().streaming || channel==channels_.end() || !channel->second->isOpen() || channel->second->bufferedAmount()>65536){
            inform("电脑控制通道尚未就绪，未发送操作");return;
        }
        const auto action=data.at("action").get<unsigned>();
        const auto enabled=data.contains("enabled")?std::optional<bool>(data.at("enabled").get<bool>()):std::nullopt;
        const auto bytes=simpleActionMessage(nextQualityRpc_++,action,enabled);
        try {
            const bool sent=channel->second->send(reinterpret_cast<const std::byte *>(bytes.data()),bytes.size());
            diagnostic(("system-action-control-"+std::to_string(action)+(sent?"-sent":"-queued")).c_str());
            if(!data.value("quiet",false))inform(sent?"操作请求已发送，请确认电脑端结果":"操作请求已排队，等待电脑端处理");
        }catch(...){diagnostic("system-action-control-send-failed");inform("电脑控制操作发送失败");}
    } else if (e == "input") {
        input(data);
    } else if (e == "release") {
        release();
    } else if (e == "local") {
        emit("soac",
             soac(remote_, data.at("type").get<std::string>(), {{"sdp", data.at("sdp")}}, ice_, control_));
        diagnostic(data.value("type", "") == "answer" ? "soac-answer-sent" : "soac-offer-sent");
#ifdef UU_TWCC_OBSERVE_CLIENT
        // The local event contains exactly the SDP generated for transmission;
        // retain only its validated capability projection, never a new raw copy.
        try {
            stopFeedbackObservation();
            feedbackLocalCaps_ = twcc::NegotiationCapabilities::fromSdp(
                data.at("sdp").get_ref<const std::string &>(),
                rtc::Description::stringToType(data.at("type").get<std::string>()));
            refreshFeedbackObservation();
        } catch (...) {
            feedbackLocalCaps_.reset();
            diagnostic("twcc-observation-local-unavailable");
        }
#endif
    } else if (e == "candidate") {
        emit("soac", soac(remote_, "candidate", data, ice_, control_));
        recordIceCandidate("local","sent",rtc::Candidate(data.at("candidate").get<std::string>(),"0"));
        diagnostic("soac-candidate-sent");
    } else if (e == "ice-state" || e == "gathering-state" || e == "signaling-state") {
        const auto label = "rtc-" + e + "-" + std::to_string(data.get<int>());
        diagnostic(label.c_str());
#ifdef UU_TWCC_OBSERVE_CLIENT
        if (e == "signaling-state" && data == int(rtc::PeerConnection::SignalingState::Stable))
            refreshFeedbackObservation();
#endif
        if (e == "ice-state" && data != int(rtc::PeerConnection::IceState::Closed))
            iceState_ = static_cast<rtc::PeerConnection::IceState>(data.get<int>());
        if (e == "ice-state" &&
            (data == int(rtc::PeerConnection::IceState::Connected) ||
             data == int(rtc::PeerConnection::IceState::Completed)) &&
            !snapshot().streaming)
            setState("connecting", "网络通道已就绪，正在协商加密和数据通道…");
    } else if (e == "peer-state") {
        auto s = data.get<int>();
        diagnostic(("rtc-peer-state-" + std::to_string(s)).c_str());
        if (s == int(rtc::PeerConnection::State::Connected)) {
            deadline_ = 0;
            connectedSince_=monotonicMs();reconnectAt_=0;
            applyBitrateCap();
            if (video_ && video_->requestKeyframe()) {
                lastKeyframeRequest_ = monotonicMs();
                ++keyframeRequests_;
                pendingKeyframeAt_ = lastKeyframeRequest_;
                keyframeCountAtRequest_ = media_->keyframeCount();
                diagnostic("rtc-keyframe-initial");
            }
            setState("connected", "");
        } else if (s == int(rtc::PeerConnection::State::Failed) ||
                   s == int(rtc::PeerConnection::State::Disconnected) ||
                   s == int(rtc::PeerConnection::State::Closed)) {
            if (snapshot().streaming)
                throw std::runtime_error("音视频连接已中断，请重新连接");
            if (iceState_ == rtc::PeerConnection::IceState::Connected ||
                iceState_ == rtc::PeerConnection::IceState::Completed)
                throw std::runtime_error("加密或数据通道建立失败，详细阶段见 connection.log");
            throw std::runtime_error("ICE 网络连接失败，详细阶段见 connection.log");
        }
    } else if (e == "async-error")
        throw std::runtime_error(data.get<std::string>());
}
void Client::pollQr() {
    if (monotonicMs() > qrExpires_) {
        qr_ = nullptr;
        {
            std::lock_guard lock(stateMutex_);
            state_.qrUrl.clear();
        }
        setState("idle", "二维码已过期，请重新生成");
        return;
    }
    Json data = qr_;
    data["login_status"] = qrStatus_;
    auto result = api_->request("POST", "/api/v1/qrcode/login/status", data, 65000);
    qrStatus_ = result.value("login_status", 1);
    qrNext_ = monotonicMs() + 1000;
    if (qrStatus_ == 4) {
        auto login = api_->request("POST", "/api/v1/login/by_qrcode", qr_);
        completeLogin(login);
    } else if (qrStatus_ == 3) {
        qr_ = nullptr;
        setState("idle", "手机已取消登录，请重试");
    } else if (qrStatus_ == 2)
        setState("login", "扫码成功，请在手机上确认登录");
}
void Client::completeLogin(const Json &response) {
    if (cancel_)
        throw std::runtime_error("Canceled");
    auto account = loginAccount(response);
    api_->login(response);
    qr_ = nullptr;
    {
        std::lock_guard lock(stateMutex_);
        state_.qrUrl.clear();
        state_.nickname = account.nickname;
        state_.loginMode = "qr";
    }
    setState("loading", "登录成功，正在同步设备…");
    refresh();
    if(!api_->sessionSaved())setState("idle","登录成功，但保存登录失败；下次打开需要重新登录");
}
void Client::refresh() {
    auto response = api_->request("GET", "/api/v1/device/list");
    Json devices = Json::array();
    std::set<std::string> seen;
    for (auto name : {"my_binded_devices", "others_shared_devices"})
        if (response.contains(name) && response[name].is_array())
            for (auto d : response[name]) {
                auto id = d.value("device_id", std::string{});
                if (id.empty() || id == api_->device() || !seen.insert(id).second)
                    continue;
                devices.push_back(std::move(d));
            }
    bool empty = devices.empty();
    {
        std::lock_guard lock(stateMutex_);
        state_.devices = std::move(devices);
    }
    setState("idle", empty ? "暂无电脑，请在原版 UU 电脑端登录同一账号" : "");
}
void Client::emit(const std::string &name, const Json &data, const std::string &ack,
                  const Bytes &attachment) {
    if (!socket_)
        return;
    Json args = Json::array({name});
    if (!data.is_null())
        args.push_back(data);
    std::string id;
    if (!ack.empty()) {
        int n = nextAck_++;
        acks_[n] = ack;
        id = std::to_string(n);
    }
    socket_->send((attachment.empty() ? "42" : "451-") + id + args.dump());
    if (!attachment.empty())
        socket_->sendBinary(attachment);
}
void Client::packet(const WsMessage &message) {
    lastPacket_ = monotonicMs();
    if (message.opcode == 2) {
        if (remainingAttachments_ <= 0)
            throw std::runtime_error("Unexpected signaling attachment");
        if (--remainingAttachments_ == 0)
            dispatch(pending_);
        return;
    }
    std::string text(message.data.begin(), message.data.end());
    if (text.empty())
        return;
    if (text[0] == '0') {
        auto hello = Json::parse(text.substr(1));
        heartbeatTimeout_ = hello.value("pingInterval", 25000) + hello.value("pingTimeout", 20000) + 5000;
        socket_->send("40");
        return;
    }
    if (text[0] == '2') {
        socket_->send("3" + text.substr(1));
        return;
    }
    if (text[0] == '1')
        throw std::runtime_error("UU 已关闭连接");
    if (text[0] != '4')
        return;
    if (remainingAttachments_)
        throw std::runtime_error("Interleaved signaling attachment");
    auto p = parseSocketIo(text);
    if (p.attachments) {
        pending_ = p;
        remainingAttachments_ = p.attachments;
    } else
        dispatch(p);
}
void Client::dispatch(const SioPacket &p) {
    if (p.type == 0) {
        emit("room_info", nullptr, "room");
        return;
    }
    if (p.type == 1 || p.type == 4)
        throw std::runtime_error("UU 信令拒绝连接");
    if (p.type == 3 || p.type == 6) {
        auto it = acks_.find(p.id);
        if (it == acks_.end())
            return;
        auto event = it->second;
        acks_.erase(it);
        auto value = ackValue(p.data, event);
        if(verboseDiagnostics() && event!="soac")
            diagnostic(("socket-ack-"+event+"-id-"+std::to_string(p.id)+"-shape-"+jsonShape(p.data)).c_str());
        if (event == "room") {
            auto publisher = value.at("publisher");
            remote_ = publisher.is_object() ? publisher.value("client_id", publisher.value("id", Json{}))
                                            : publisher;
            if ((!remote_.is_string() && !remote_.is_number()) || remote_ == "")
                throw std::runtime_error("房间内没有在线电脑");
            control_ = uuid();
            auto options = connectOptions(streamWidth_, streamHeight_, streamFps_, streamQuality_,streamQuality_==6?maxBitrateMbps_:0,hevc_);
            diagnostic(("encoder-bitrate-request-connect-bps-"+
                        std::to_string(streamQuality_==6?uint64_t(maxBitrateMbps_)*1000000u:0)+
                        "-quality-"+std::to_string(streamQuality_)).c_str());
            options.insert(options.begin(), 4);
            diagnostic(("stream-options-" + std::to_string(streamWidth_) + "x" +
                        std::to_string(streamHeight_) + "-" + std::to_string(streamFps_) + "fps-q" +
                        std::to_string(streamQuality_) + "-cap" +
                        (streamQuality_==6?std::to_string(maxBitrateMbps_):std::string("auto")) +
                        "-kf" + std::to_string(keyframeIntervalMs_) +
                        (hevc_?"-decoder-cap-h265":"-decoder-cap-h264"))
                           .c_str());
            setState("negotiating", "正在申请远控权限…");
            // Preserve room ICE configuration if control does not repeat it.
            ice_ = value.value("iceServers", Json::array());
            emit("control",
                 {{"app_data", {{"_placeholder", true}, {"num", 0}}},
                  {"streamer_data", clientCapabilityJson(streamWidth_,streamHeight_,streamFps_,streamQuality_)},
                  {"app_control_id", control_}},
                 "control", options);
            diagnostic(("client-capability-json-bytes-"+
                        std::to_string(clientCapabilityJson(streamWidth_,streamHeight_,streamFps_,streamQuality_).size())).c_str());
        } else if (event == "control")
            beginPeer(value);
    } else if (p.type == 2 || p.type == 5) {
        if (!p.data.is_array() || p.data.empty() || !p.data[0].is_string())
            throw std::runtime_error("Invalid signaling event");
        auto event = p.data[0].get<std::string>();
        if(verboseDiagnostics() && event!="soac")
            diagnostic(("socket-event-"+event+"-id-"+std::to_string(p.id)+"-shape-"+jsonShape(p.data)).c_str());
        if(event=="forward_setting") {
            if(const auto info=parseForwardSetting(p.data)){
                const auto summary=info->summary();
                if(!summary.empty())diagnostic(("forward-setting-summary-"+summary).c_str());
                std::lock_guard lock(stateMutex_);
                state_.serverDisplayInfo=info->display;
                state_.serverCodecInfo=info->codecs;
                state_.serverCaptureImpl=info->capture;
                state_.serverEncoderImpl=info->encoder;
                state_.serverVideoTrack=info->track;
                state_.serverCapabilityValid=true;
            }
            if(verboseDiagnostics()) {
                std::string flat;
                jsonFlatten(p.data,"",flat);
                if(!flat.empty())diagnostic(("forward-setting-"+flat).c_str());
            }
        }
        if (event == "soac" && p.data.size() > 1) {
            if (peer_)
                handleSoac(p.data[1]);
            else if (earlySoac_.size() < 128)
                earlySoac_.push_back(p.data[1]);
        } else if (event == "released" || event == "left")
            throw std::runtime_error("远控权限已释放或电脑已退出房间");
        if (p.id >= 0)
            socket_->send("43" + std::to_string(p.id) + "[]");
    }
}
void Client::beginPeer(const Json &result) {
#ifdef UU_TWCC_OBSERVE_CLIENT
    stopFeedbackObservation();
    feedbackLocalCaps_.reset(); feedbackRemoteCaps_.reset();
#endif
    lastSelectedPath_.clear();
    remoteRelayEndpoints_.clear();
    recordSelectedPath("state=awaiting_selected_pair\n");
    auto servers = result.value("iceServers", ice_);
    remote_ = result.value("client_id", remote_);
    ice_ = result.at("ice_id");
    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    iceDiagnostics_={{"version",UU_VERSION},{"force_relay",result.value("force_relay",false)},
                     {"candidates",Json::array()},{"path_history",Json::array()},{"truncated",false},
                     {"route_policy",configInt("route_policy",1,0,1)?"ipv6-lan-public-relay":"rfc-default"}};
#ifdef __SWITCH__
    errno=0;
    const int ipv6Socket=socket(AF_INET6,SOCK_DGRAM,0);
    iceDiagnostics_["ipv6_udp_socket_available"]=ipv6Socket>=0;
    if(ipv6Socket<0) iceDiagnostics_["ipv6_udp_socket_errno"]=errno;
    else close(ipv6Socket);
    u32 interfaceAddress=0,mask=0,gateway=0,dns1=0,dns2=0;
    if(R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&interfaceAddress,&mask,&gateway,&dns1,&dns2))) {
        auto ipv4=[](u32 address) {
            return std::to_string(address&255)+"."+std::to_string((address>>8)&255)+"."+
                   std::to_string((address>>16)&255)+"."+std::to_string((address>>24)&255);
        };
        iceDiagnostics_["interface_ipv4"]=ipv4(interfaceAddress);
        iceDiagnostics_["interface_mask"]=ipv4(mask);
        const auto hostid=u32(gethostid());
        iceDiagnostics_["gethostid_ipv4"]=ipv4(hostid);
        iceDiagnostics_["gethostid_matches_interface"]=hostid==interfaceAddress;
    }
#endif
    if (result.value("force_relay", false))
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    for (const auto &s : servers) {
        auto urls = s.value("urls", s.value("url", Json{}));
        if (urls.is_string())
            urls = Json::array({urls});
        if (!urls.is_array())
            continue;
        for (const auto &u : urls) {
            rtc::IceServer server(u.get<std::string>());
            server.username = s.value("username", std::string{});
            server.password = s.value("credential", s.value("password", std::string{}));
            config.iceServers.push_back(std::move(server));
        }
    }
    // libjuice's native TURN transport is UDP. Adapt only server types actually
    // advertised by this authenticated room; never guess a TLS port or disable
    // certificate validation. Direct ICE candidates remain unaffected.
    using Relay=rtc::IceServer::RelayType;
    std::array<int,3> available{};
    auto relayIndex=[](Relay type){return type==Relay::TurnTls?2:type==Relay::TurnTcp?1:0;};
    for(const auto &server:config.iceServers)
        if(server.type==rtc::IceServer::Type::Turn) ++available[relayIndex(server.relayType)];
    const auto requested=configValue("turn_transport");
    int mode=requested=="tls"?2:requested=="tcp"?1:requested=="udp"?0:reconnectAttempts_%3;
#ifndef __SWITCH__
    mode=0;
#endif
    if(!available[mode]) {
        mode=available[0]?0:available[1]?1:available[2]?2:0;
        diagnostic("turn-transport-requested-mode-unavailable");
    }
    turnUplinkMode_=mode==2?"TLS":mode==1?"TCP":"UDP";
    std::vector<rtc::IceServer> filtered;
    for(auto server:config.iceServers) {
        if(server.type==rtc::IceServer::Type::Stun) {filtered.push_back(std::move(server));continue;}
        if(relayIndex(server.relayType)!=mode) continue;
        if(mode!=0) {
#ifdef __SWITCH__
            if(turnBridges_.size()>=2) continue;
            auto bridge=std::make_shared<TurnStreamBridge>(server.hostname,server.port?server.port:(mode==2?5349:3478),mode==2);
            server.hostname="127.0.0.1";server.port=bridge->localPort();server.relayType=Relay::TurnUdp;
            turnBridges_.push_back(std::move(bridge));
#else
            continue;
#endif
        }
        filtered.push_back(std::move(server));
    }
    config.iceServers=std::move(filtered);
    iceDiagnostics_["turn_server_transports_available"]={{"udp",available[0]},{"tcp",available[1]},{"tls",available[2]}};
    iceDiagnostics_["turn_server_uplink"]=turnUplinkMode_;
    iceDiagnostics_["ice_server_count"]=config.iceServers.size();
    saveIceDiagnostics();
    setState("rtc-init", "正在初始化原生串流…");
    diagnostic("rtc-runtime");
    rtc::Preload();
    diagnostic("rtc-peer");
    peer_ = std::make_shared<rtc::PeerConnection>(config);
    auto weak = std::weak_ptr<Inbox>(inbox_);
    auto gen = generation_;
    auto post = [weak, gen](std::string type, Json data) {
        if (auto box = weak.lock()) {
            std::lock_guard lock(box->mutex);
            if (box->events.size() < 2048)
                box->events.push_back({std::move(type), std::move(data), gen});
            box->cv.notify_one();
        }
    };
    peer_->onLocalDescription([post](rtc::Description desc) {
        post("local", {{"type", desc.typeString()}, {"sdp", std::string(desc)}});
    });
    peer_->onLocalCandidate([post](rtc::Candidate c) {
        post("candidate", {{"candidate", std::string(c)}, {"sdpMid", c.mid()}, {"sdpMLineIndex", 0}});
    });
    peer_->onStateChange([post](rtc::PeerConnection::State s) { post("peer-state", int(s)); });
    peer_->onIceStateChange([post](rtc::PeerConnection::IceState s) { post("ice-state", int(s)); });
    peer_->onGatheringStateChange(
        [post](rtc::PeerConnection::GatheringState s) { post("gathering-state", int(s)); });
    peer_->onSignalingStateChange(
        [post](rtc::PeerConnection::SignalingState s) { post("signaling-state", int(s)); });
    rtc::Description::Video v("0", rtc::Description::Direction::RecvOnly);
    diagnostic("rtc-video");
    // One codec per connection: RTX/PT and decoder must describe the same bitstream.
    // HEVC is explicitly opt-in; H264 remains the default/manual fallback.
    if(hevc_ && configValue("receiver")=="legacy")
        throw std::runtime_error("H.265 需要严格接收器，请使用 H.264 或 receiver=strict");
    if(hevc_) v.addH265Codec(96,"profile-id=1;tier-flag=0;level-id=123;tx-mode=SRST;sprop-max-don-diff=0");
    else v.addH264Codec(96, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
    diagnostic(hevc_?"video-codec-offered-hevc-main":"video-codec-offered-h264");
    if (rtxEnabled_) {
        v.addRtxCodec(97, 96, 90000);
        diagnostic("rtc-video-rtx");
    }
    // Let the confirmed UU quality tier drive sender rate control. The former
    // fixed SDP 5000 kbps declaration limited every tier with the same hint.
    video_ = peer_->addTrack(v);
    auto videoStats = std::make_shared<RtpTrackStats>(90000, "video", false);
    auto videoSession = std::make_shared<PrimaryVideoRtcpSession>();
    videoSession->addToChain(videoStats);
    videoStats_ = videoStats;
    std::weak_ptr<Media> media = media_;
    auto mediaGeneration = media_->generation();
    const bool hevc=hevc_;
    const bool strict = configValue("receiver") != "legacy";
    if (strict) {
        strictVideo_ = std::make_shared<StrictVideoReceiver>(rtxEnabled_,
            configInt("receiver_deadline_ms", 600, 100, 1200),
            [media, mediaGeneration, hevc](const uint8_t *data, size_t size, uint32_t timestamp) {
                if (auto m = media.lock()) m->video(data, size, mediaGeneration, timestamp, hevc);
            }, hevc, streamQuality_==6,
            [media, mediaGeneration, hevc](const uint8_t *data, size_t size, uint32_t timestamp, bool idr) {
                if (auto m = media.lock()) m->video(data, size, mediaGeneration, timestamp, hevc, idr);
            });
        strictVideo_->addToChain(videoSession);
        video_->setMediaHandler(strictVideo_);
        diagnostic("video-receiver-strict-v2");
    } else {
        const int jitterMs = configInt("jitter_ms", 120, 0, 400);
        auto assembler = std::make_shared<VideoAssembler>(rtxEnabled_, jitterMs);
        assembler->addToChain(videoSession);
        video_->setMediaHandler(assembler);
        videoAssembler_ = assembler;
        diagnostic("video-receiver-legacy");
    }
    video_->onFrame([media, mediaGeneration, hevc](rtc::binary data, rtc::FrameInfo info) {
        if (auto m = media.lock())
            m->video(reinterpret_cast<const uint8_t *>(data.data()), data.size(), mediaGeneration, info.timestamp, hevc);
    });
    rtc::Description::Audio a("1", rtc::Description::Direction::RecvOnly);
    diagnostic("rtc-audio");
    a.addOpusCodec(111);
    audio_ = peer_->addTrack(a);
    auto opus = std::make_shared<rtc::OpusRtpDepacketizer>();
    auto audioStats = std::make_shared<RtpTrackStats>(48000, "audio", false);
    auto audioSession = std::make_shared<rtc::RtcpReceivingSession>();
    opus->addToChain(audioSession);
    audioSession->addToChain(audioStats);
    audio_->setMediaHandler(opus);
    audioStats_ = audioStats;
    audio_->onFrame([media, mediaGeneration](rtc::binary data, rtc::FrameInfo) {
        if (auto m = media.lock())
            m->audio(reinterpret_cast<const uint8_t *>(data.data()), data.size(), mediaGeneration);
    });
    diagnostic("rtc-channels");
    for (const auto *label : {"CONTROL_DATA_CHANNEL", "TEXT_DATA_CHANNEL", "STREAMER_DATA_CHANNEL",
                              "FILE_DATA_CHANNEL", "BINARY_DATA_CHANNEL"}) {
        auto channel=peer_->createDataChannel(label);channels_[label]=channel;
        if(std::string(label)=="TEXT_DATA_CHANNEL" || std::string(label)==systemActionChannel){
            std::weak_ptr<Inbox> inbox=inbox_;const auto generation=generation_;
            const std::string eventName=std::string(label)==systemActionChannel?"control-channel-state":"text-channel-state";
            auto stateChanged=[inbox,generation,eventName](bool ready){
                if(auto queue=inbox.lock()){
                    std::lock_guard lock(queue->mutex);
                    queue->events.push_back({eventName,ready,generation});queue->cv.notify_one();
                }
            };
            channel->onOpen([stateChanged]{stateChanged(true);});
            channel->onClosed([stateChanged]{stateChanged(false);});
        }
        {
            const bool acceptRpc=std::string(label)=="TEXT_DATA_CHANNEL" || std::string(label)=="FILE_DATA_CHANNEL";
            const bool streamerData=std::string(label)=="STREAMER_DATA_CHANNEL";
            const std::string labelText=label;
            const bool traceChannel=labelText!="TEXT_DATA_CHANNEL" && labelText!="FILE_DATA_CHANNEL";
            std::weak_ptr<Inbox> inbox=inbox_;const auto generation=generation_;
            channel->onMessage([inbox,generation,acceptRpc,streamerData,labelText,traceChannel](rtc::message_variant message) {
                const uint8_t *bytes=nullptr;size_t size=0;
                if(auto *binary=std::get_if<rtc::binary>(&message)){bytes=reinterpret_cast<const uint8_t *>(binary->data());size=binary->size();}
                else if(auto *text=std::get_if<std::string>(&message)){bytes=reinterpret_cast<const uint8_t *>(text->data());size=text->size();}
                const auto screens=parseScreenSources(bytes,size);
                const auto settingSync=parseCaptureSettingSync(bytes,size);
                const auto ack=acceptRpc?parseCaptureRpcAck(bytes,size):std::optional<CaptureRpcAck>{};
                const auto textAck=acceptRpc?parseTextRpcAck(bytes,size):std::optional<TextRpcAck>{};
                std::string streamerShape,channelShape;
                if(verboseDiagnostics() && streamerData)
                    streamerShape=streamerDataShape(bytes,size);
                if(verboseDiagnostics() && traceChannel && size && !screens && !ack && !textAck)
                    channelShape=streamerDataShape(bytes,size);
                if(!streamerShape.empty())
                    diagnostic(("streamer-data-"+streamerShape).c_str());
                if(!channelShape.empty())
                    diagnostic(("channel-data-"+labelText+"-"+channelShape).c_str());
                if(!screens && !ack && !textAck && !settingSync)return;
                if(auto queue=inbox.lock()){
                    std::lock_guard lock(queue->mutex);
                    if(queue->events.size()>124)return;
                    if(screens)queue->events.push_back({"quality-screens",{{"screen",screens->screenId},{"count",screens->count}},generation});
                    if(settingSync)queue->events.push_back({"capture-setting-sync",
                        {{"fields",settingSync->fields},
                         {"fps",settingSync->fps},{"quality",settingSync->quality},
                         {"chroma",settingSync->chroma},{"bitrate",settingSync->maxCustomBitrate},
                         {"width",settingSync->width},{"height",settingSync->height},
                         {"resolution_type",settingSync->resolutionType},
                         {"dpi_scale",settingSync->dpiScale},
                         {"pixel_width",settingSync->pixelWidth},
                         {"pixel_height",settingSync->pixelHeight},
                         {"orientation",settingSync->orientation}},generation});
                    if(screens && verboseDiagnostics()){
                        const auto summary=screenSourceWireSummary(bytes,size);
                        if(!summary.empty())queue->events.push_back({"screen-wire",summary,generation});
                    }
                    if(ack)queue->events.push_back({"quality-rpc-ack",{{"id",ack->requestId},{"capture",ack->capture},{"errors",ack->errors},{"entries",ack->entries},{"code",ack->errorCode},{"other",ack->otherFields}},generation});
                    if(textAck)queue->events.push_back({"text-input-ack",{{"id",textAck->requestId},{"status",textAck->status}},generation});
                    queue->cv.notify_one();
                }
            });
        }
    }
    setState("connecting", "正在建立音视频通道…");
    diagnostic("rtc-offer");
    peer_->setLocalDescription();
    for (const auto &e : earlySoac_)
        handleSoac(e);
    earlySoac_.clear();
}
void Client::handleSoac(const Json &packet) {
    if (!packet.contains("data") || !packet["data"].is_object())
        return;
    const auto &d = packet["data"];
    if (d.contains("ice_id")) {
        auto idString = [](const Json &id) { return id.is_string() ? id.get<std::string>() : id.dump(); };
        if (idString(d["ice_id"]) != idString(ice_))
            return;
    }
    const auto type = d.value("type", std::string{});
#ifdef UU_TWCC_OBSERVE_CLIENT
    if (type == "offer" || type == "answer" || type == "restart_ice") {
        stopFeedbackObservation();
        feedbackRemoteCaps_.reset();
        if (type != "answer") feedbackLocalCaps_.reset();
    }
#endif
    if(d.contains("sdp") && d["sdp"].is_string()) {
        // Reject incompatible HEVC parameters before transport can deliver RTP.
        // Only single-layer Main, SRST, no DONL is implemented in this version.
        if(hevc_) {
            std::istringstream codecLines(d["sdp"].get<std::string>());
            std::string item;bool inVideo=false,matched=false;
            while(std::getline(codecLines,item)) {
                if(!item.empty() && item.back()=='\r') item.pop_back();
                if(item.rfind("m=",0)==0) {
                    inVideo=item.rfind("m=video ",0)==0;
                    if(inVideo && item.rfind("m=video 0 ",0)==0)
                        throw std::runtime_error("电脑拒绝 H.265，请在 L3 改回 H.264");
                }
                if(!inVideo) continue;
                if(item=="a=rtpmap:96 H265/90000") matched=true;
                if(item.rfind("a=fmtp:96 ",0)==0) {
                    std::istringstream params(item.substr(10));std::string param;
                    while(std::getline(params,param,';')) {
                        param=trim(param);auto eq=param.find('=');
                        if(eq==std::string::npos) continue;
                        auto key=trim(param.substr(0,eq)),value=trim(param.substr(eq+1));
                        if((key=="sprop-max-don-diff" && value!="0") ||
                           (key=="profile-id" && value!="1") ||
                           (key=="tx-mode" && value!="SRST"))
                            throw std::runtime_error("电脑的 H.265 参数暂不支持，请改回 H.264");
                    }
                }
            }
            if(!matched) throw std::runtime_error("电脑未协商 H.265，请在 L3 改回 H.264");
            diagnostic("video-codec-negotiated-hevc-main");
        }
        rembAvailable_=false;
        std::istringstream feedbackLines(d["sdp"].get<std::string>());
        std::string line;bool video=false;
        while(std::getline(feedbackLines,line)) {
            if(!line.empty() && line.back()=='\r') line.pop_back();
            if(line.rfind("m=",0)==0) video=line.rfind("m=video ",0)==0;
            if(video && (line=="a=rtcp-fb:96 goog-remb" || line=="a=rtcp-fb:* goog-remb")) rembAvailable_=true;
        }
    }
    if (strictVideo_ && d.contains("sdp") && d["sdp"].is_string()) {
        // Bind RTX to the negotiated FID pair when the sender supplies it.
        std::istringstream lines(d["sdp"].get<std::string>());
        std::string line;
        bool inVideo = false;
        while (std::getline(lines, line)) {
            if (line.rfind("m=", 0) == 0) inVideo = line.rfind("m=video ", 0) == 0;
            if (inVideo && line.rfind("a=ssrc-group:FID ", 0) == 0) {
                std::istringstream pair(line.substr(17));
                uint32_t primary = 0, rtx = 0;
                if (pair >> primary >> rtx) strictVideo_->setSources(primary, rtx);
            }
        }
    }
    if (type == "answer")
        diagnostic("soac-answer-received");
    else if (type == "offer" || type == "restart_ice")
        diagnostic("soac-offer-received");
    const auto result = applyRemoteSignal(*peer_, d, pendingCandidates_);
    if(type=="candidate" && (result==RemoteSignal::CandidateApplied || result==RemoteSignal::CandidateQueued)) {
        const auto &source=d.contains("candidate") && d["candidate"].is_object()?d["candidate"]:d;
        recordIceCandidate("remote",result==RemoteSignal::CandidateApplied?"applied":"queued",
                           rtc::Candidate(source.at("candidate").get<std::string>(),"0"));
    }
    if(result==RemoteSignal::Description) {
#ifdef UU_TWCC_OBSERVE_CLIENT
        try {
            feedbackRemoteCaps_ = twcc::NegotiationCapabilities::fromSdp(
                d.at("sdp").get_ref<const std::string &>(),
                type == "answer" ? rtc::Description::Type::Answer : rtc::Description::Type::Offer);
            refreshFeedbackObservation();
        } catch (...) {
            feedbackRemoteCaps_.reset();
            diagnostic("twcc-observation-remote-unavailable");
        }
#endif
        if(auto description=peer_->remoteDescription())
            for(const auto &candidate:description->candidates()) recordIceCandidate("remote","description",candidate);
    }
    if (result == RemoteSignal::Closed)
        diagnostic("soac-after-transport-close");
    else if (result == RemoteSignal::CandidateQueued)
        diagnostic("soac-candidate-queued");
    else if (result == RemoteSignal::CandidateApplied)
        diagnostic("soac-candidate-applied");
    else if (result == RemoteSignal::Description) {
        diagnostic("soac-description-applied");
        if (!sdpDumpDone_ && diagEnabled() && peer_) {
#ifndef UU_TWCC_OBSERVE_CLIENT
            if (auto description = peer_->remoteDescription()) {
#ifdef _WIN32
                _mkdir("diag");
#else
                mkdir("sdmc:/switch/UURemote-ns/diag", 0777);
#endif
                std::ofstream file(
#ifdef __SWITCH__
                    "sdmc:/switch/UURemote-ns/diag/sdp-remote.txt"
#else
                    "diag/sdp-remote.txt"
#endif
                );
                if (file)
                    file << std::string(*description);
                sdpDumpDone_ = true;
            }
#else
            // Observation mode emits status only, never a raw-SDP diagnostic.
            sdpDumpDone_ = true;
            diagnostic("twcc-observation-no-raw-sdp-log");
#endif
        }
    }
}
void Client::input(const Json &data) {
    auto it = channels_.find("CONTROL_DATA_CHANNEL");
    const auto action = data.value("action", std::string{});
    if (it == channels_.end() || !it->second->isOpen()){
        if(action=="touch_native")++touchFailures_;
        gamepadConnected_=false;
        return;
    }
    if(action=="touch_native"){
        int type=data.at("type").get<int>();const auto &points=data.at("points");
        if((type==3 || type==4) && !nativeTouchActive_)return;
        if(type==2 && !nativeTouchActive_)type=1;
        if(type==2 && it->second->bufferedAmount()>65536){++touchFailures_;return;}
        const auto bytes=nativeTouchMessage(nextQualityRpc_++,type,points);
        try{
            // libdatachannel false may mean queued, not rejected. Preserve lifecycle.
            const bool immediate=it->second->send(reinterpret_cast<const std::byte *>(bytes.data()),bytes.size());
            if(!immediate && type!=2)diagnostic("native-touch-queued");
        }catch(...){++touchFailures_;return;}
        nativeTouchActive_=type<=2;touchPoints_=points.size();++touchPackets_;
        if(type!=2)diagnostic(type==1?"native-touch-start":type==3?"native-touch-end":"native-touch-cancel");
        return;
    }
    if (it->second->bufferedAmount() > 64 * 1024 &&
        (action == "mouse_move_absolute" || action == "mouse_scroll"))
        return;
    // The UI may request a pad before SCTP opens. Retry on the worker when
    // an actual update can be submitted; never send updates before setup.
    if(action=="gpd_update" && !gamepadConnected_){
        input({{"action","gpd_connect"},{"index",data.value("index",0)}});
        if(!gamepadConnected_)return;
    }
    const bool mouseButton=action=="mouse_press" || action=="mouse_release";
    const std::string owner=data.value("source",std::string("controls"));
    int button=0;
    if(mouseButton){
        button=data.at("button").get<int>();
        if(button<1 || button>3 || (owner!="controls" && owner!="phone_mouse"))return;
        const auto found=mouseOwners_.find(button);
        if(action=="mouse_press"){
            if(found!=mouseOwners_.end()){
                if(found->second.count(owner))return;
                found->second.insert(owner);return; // Already down for another input source.
            }
        }else{
            if(found==mouseOwners_.end() || !found->second.count(owner))return;
            if(found->second.size()>1){found->second.erase(owner);return;}
        }
    }
    auto wire=data;wire.erase("source"); // Input ownership is local, not part of the remote protocol.
    auto s = wire.dump();
    try {
        it->second->send(reinterpret_cast<const std::byte *>(s.data()), s.size());
    } catch (...) {
        return;
    }
    if(action=="gpd_connect")gamepadConnected_=true;
    else if(action=="gpd_disconnect")gamepadConnected_=false;
    if (action == "kbd_press")
        heldKeys_.insert(data.at("key").get<int>());
    else if (action == "kbd_release")
        heldKeys_.erase(data.at("key").get<int>());
    else if (action == "mouse_press"){
        heldButtons_.insert(button);mouseOwners_[button].insert(owner);
    }else if (action == "mouse_release"){
        heldButtons_.erase(button);mouseOwners_.erase(button);
    }
}
void Client::release() {
    if(nativeTouchActive_)input({{"action","touch_native"},{"type",4},{"points",Json::array()}});
    if(gamepadConnected_)input({{"action","gpd_update"},{"index",0},{"buttons",0},{"left_trigger",0},{"right_trigger",0},{"left_thumb_x",0},{"left_thumb_y",0},{"right_thumb_x",0},{"right_thumb_y",0}});
    auto keys = heldKeys_;
    const auto owners=mouseOwners_;
    for (int k : keys)
        input({{"action", "kbd_release"}, {"key", k}});
    for(const auto &[b,sources]:owners)for(const auto &source:sources)
        input({{"action","mouse_release"},{"button",b},{"source",source}});
    heldKeys_.clear();
    heldButtons_.clear();
    mouseOwners_.clear();
}
#ifdef UU_TWCC_OBSERVE_CLIENT
void Client::stopFeedbackObservation() {
    auto observation = std::move(feedbackObservation_);
    if (!observation) return;
    try {
        observation->stop();
        if (peer_ && peer_->getMediaHandler() == observation) peer_->setMediaHandler(nullptr);
        const auto counts = observation->observationCounters();
        diagnostic(("twcc-observation-accepted-" + std::to_string(counts.accepted) +
            "-missing-extension-" + std::to_string(counts.missingExtension) +
            "-malformed-" + std::to_string(counts.malformed) +
            "-missing-time-" + std::to_string(counts.missingTimestamp) +
            "-window-full-" + std::to_string(counts.windowFull)).c_str());
    } catch (...) {
        diagnostic("twcc-observation-stop-failed");
    }
}
void Client::refreshFeedbackObservation() {
    if (feedbackObservation_ || !peer_ || !feedbackLocalCaps_ || !feedbackRemoteCaps_ ||
        peer_->signalingState() != rtc::PeerConnection::SignalingState::Stable) return;
    try {
        // Reject stale queued signaling events. Compare only canonical allowed
        // capabilities against the CURRENT peer, never log/store its raw SDP.
        const auto local = peer_->localDescription(), remote = peer_->remoteDescription();
        if (!local || !remote) return;
        const auto currentLocal = twcc::NegotiationCapabilities::fromSdp(std::string(*local), local->type());
        const auto currentRemote = twcc::NegotiationCapabilities::fromSdp(std::string(*remote), remote->type());
        if (!currentLocal || !currentRemote || !(*currentLocal == *feedbackLocalCaps_) ||
            !(*currentRemote == *feedbackRemoteCaps_)) {
            diagnostic("twcc-observation-stale-capabilities"); return;
        }
        const auto result = twcc::inspectNegotiationCapabilities(*feedbackLocalCaps_, *feedbackRemoteCaps_);
        if (result.status != twcc::NegotiationStatus::Ready) {
            diagnostic(("twcc-observation-negotiation-status-" + std::to_string(int(result.status))).c_str());
            return;
        }
        if (peer_->getMediaHandler()) {
            diagnostic("twcc-observation-global-handler-occupied"); return;
        }
        // This mode does not advertise any capability, call startSending or
        // poll/commit reports. Timing fields are inert observer prerequisites.
        auto observation = std::make_shared<twcc::FeedbackAttachment>(result,
            twcc::FeedbackTiming{100000, 0, 2048});
        peer_->setMediaHandler(observation);
        feedbackObservation_ = std::move(observation);
        diagnostic("twcc-observation-attached-send-disabled");
    } catch (...) {
        stopFeedbackObservation();
        diagnostic("twcc-observation-unavailable");
    }
}
#endif
void Client::disconnect() {
    const auto disconnectStarted=monotonicMs();
    diagnostic("disconnect");

    // Only clear the scheduled reconnect time, not the armed flag.
    // reconnectArmed_ is set when user initiates connection and should persist
    // through disconnect to allow automatic reconnection on transient failures.
    // User-initiated cancel or fatal errors clear reconnectArmed_ separately.
    reconnectAt_ = 0;

    if(pendingText_)finishTextInput(3,"连接已断开，已取消等待中的粘贴");
#ifdef UU_TWCC_OBSERVE_CLIENT
    stopFeedbackObservation();
    feedbackLocalCaps_.reset(); feedbackRemoteCaps_.reset();
#endif
    pendingQuality_.reset();
    pendingVideoScale_.reset();videoScaleAttempted_=false;videoScaleVerified_=false;nextVideoScaleCheck_=0;
    setVideoScaleNotice("");
    {std::lock_guard lock(stateMutex_);state_.qualityPending=false;}
    connectedSince_=0;
    if (strictVideo_)
        strictVideo_->shutdown();
    release();
    diagnostic(("disconnect-stage-release-ms-"+std::to_string(monotonicMs()-disconnectStarted)).c_str());
    if(gamepadConnected_)input({{"action","gpd_disconnect"},{"index",0}});
    gamepadConnected_=false;nativeTouchActive_=false;touchPoints_=0;
    ++generation_;
    activeScreenId_=-1;
    iceState_ = rtc::PeerConnection::IceState::New;
    deadline_ = 0;
    lastKeyframeRequest_ = 0;
    qr_ = nullptr;
    earlySoac_.clear();
    pendingCandidates_.clear();
    acks_.clear();
    remainingAttachments_ = 0;
    nextAck_ = 1;
    for (auto &[name, ch] : channels_)
        ch->close();
    channels_.clear();
    if (peer_) {
        peer_->resetCallbacks();
        peer_->close();
        peer_.reset();
    }
    diagnostic(("disconnect-stage-peer-close-ms-"+std::to_string(monotonicMs()-disconnectStarted)).c_str());
    video_.reset();
    rembAvailable_=false;bitrateFeedbackSent_=false;lastBitrateAttempt_=0;
    turnBridges_.clear();
    strictVideo_.reset();
    videoAssembler_.reset();
    audio_.reset();
    videoStats_.reset();
    audioStats_.reset();
    pendingKeyframeAt_ = 0;
    lastKeyframeRecoveryMs_ = -1;
    keyframeRequests_ = 0;
    keyframeCountAtRequest_ = 0;
    {
        std::lock_guard lock(statsMutex_);
        statsCache_ = {};
    }
    socket_.reset();
    media_->reset();
    diagnostic(("disconnect-stage-media-reset-ms-"+std::to_string(monotonicMs()-disconnectStarted)).c_str());
    {
        std::lock_guard lock(stateMutex_);
        state_.qrUrl.clear();
        state_.streaming = false;
        state_.activeDeviceId.clear();
        state_.textChannelReady=false;
        state_.controlChannelReady=false;
        state_.settingAccepted=false;state_.settingApplied=false;state_.settingRejected=false;
        state_.settingErrorCode=0;state_.settingSource="none";
        state_.appliedFps=0;state_.appliedQuality=0;state_.appliedWidth=0;state_.appliedHeight=0;
        state_.appliedBitrateMbps=0;state_.appliedValid=false;state_.appliedBitrateKnown=false;
        state_.serverCapabilityValid=false;
        state_.serverDisplayInfo.clear();state_.serverCodecInfo.clear();
        state_.serverCaptureImpl.clear();state_.serverEncoderImpl.clear();state_.serverVideoTrack=-1;
    }
    diagnostic(("disconnect-total-ms-"+std::to_string(monotonicMs()-disconnectStarted)).c_str());
}
void Client::commitQuality(const QualityValues &q,bool hotCap,const char *notice) {
    if(hotCap && q.quality!=6 && (!video_ || !rembAvailable_ || q.cap<=0 || !video_->requestBitrate(unsigned(q.cap)*1000000u)))
        throw std::runtime_error("码率反馈更新失败");
    // The four v0.2 UI presets use exact-rate encoding. Keep successful normal
    // presets, while retaining session-only handling for high-bitrate probes.
    const bool fixedUiPreset=q.quality==6 && (q.cap==5 || q.cap==12 || q.cap==20 || q.cap==30);
    if(q.quality!=6 || fixedUiPreset)saveQuality(q.width,q.height,q.fps,q.quality,q.vsync,q.delay,q.cap,q.hevc);
    media_->setPlayoutDelay(q.delay);
    const bool sizeChanged=q.width!=streamWidth_ || q.height!=streamHeight_;
    streamWidth_=q.width;streamHeight_=q.height;streamFps_=q.fps;streamQuality_=q.quality;
    maxBitrateMbps_=q.cap;hevc_=q.hevc;
    if(sizeChanged){
        pendingVideoScale_.reset();videoScaleAttempted_=false;videoScaleVerified_=false;nextVideoScaleCheck_=0;
        setVideoScaleNotice("视频尺寸设置已变化，等待重新缩放");
    }
    if(hotCap && q.quality!=6) {bitrateFeedbackSent_=true;diagnostic("quality-remb-hot-updated");}
    {std::lock_guard lock(stateMutex_);
        state_.width=q.width;state_.height=q.height;state_.fps=q.fps;state_.quality=q.quality;
        state_.maxBitrateMbps=q.cap;state_.playoutDelayMs=q.delay;state_.vsync=q.vsync;state_.hevc=q.hevc;
        state_.qualityPending=false;state_.qualityNotice=notice;
    }
    qualityNoticeUntil_=monotonicMs()+8000;
    diagnostic(("quality-target-q"+std::to_string(q.quality)+"-cap"+std::to_string(q.cap)+
                "-feedback"+(q.quality==6?"off":"on")).c_str());
}
void Client::reconnectQuality(const QualityValues &q,const char *notice) {
    const auto device=connectedDeviceId_;commitQuality(q,false,notice);disconnect();
    setState("idle",notice);if(!device.empty())post("connect",{{"deviceId",device}});
}
void Client::finishQuality() {
    if(!pendingQuality_ || !pendingQuality_->acknowledged)return;
    const auto q=pendingQuality_->target;const auto media=media_->stats();
    if((q.width!=streamWidth_ || q.height!=streamHeight_) && (media.width!=q.width || media.height!=q.height))return;
    try {
        const char *notice=pendingQuality_->headerOnly
            ? "电脑已接收，等待实际状态"
            : "电脑已接受设置，等待实际参数确认";
        commitQuality(q,q.quality!=6 && q.cap!=maxBitrateMbps_,notice);
        diagnostic(pendingQuality_->headerOnly?"quality-rpc-header-only-committed":"quality-rpc-capture-committed");
        pendingQuality_.reset();
    }catch(...){fallbackQuality("在线设置未完成，正在重连应用…");}
}
void Client::fallbackQuality(const char *notice) {
    if(!pendingQuality_)return;
    const auto q=pendingQuality_->target;pendingQuality_.reset();++qualityFailed_;
    diagnostic("quality-rpc-fallback-reconnect");reconnectQuality(q,notice);
}
void Client::tickQuality() {
    if(pendingQuality_){
        finishQuality();
        if(pendingQuality_ && monotonicMs()>=pendingQuality_->deadline)
            fallbackQuality("在线设置超时，正在重连应用…");
    }
    if(qualityNoticeUntil_ && monotonicMs()>=qualityNoticeUntil_ && !pendingQuality_){
        std::lock_guard lock(stateMutex_);state_.qualityNotice.clear();qualityNoticeUntil_=0;
    }
}
void Client::setVideoScaleNotice(const std::string &notice) {
    std::lock_guard lock(stateMutex_);
    state_.videoScaleNotice=notice;
}
void Client::tickVideoScale() {
    const auto now=monotonicMs();
    if(now<nextVideoScaleCheck_)return;
    nextVideoScaleCheck_=now+250;
    const auto state=snapshot();
    if(!state.streaming || pendingQuality_)return;
    const auto channel=channels_.find("TEXT_DATA_CHANNEL");
    if(channel==channels_.end() || !channel->second->isOpen())return;
    if(activeScreenId_<0)return;

    if(pendingVideoScale_){
        const auto &request=*pendingVideoScale_;
        const auto media=media_->stats();
        if(request.acknowledged && media.epoch==request.mediaEpoch &&
           media.publishedTotal>request.publishedBefore && media.width>0 && media.height>0 &&
           media.width<=request.targetWidth && media.height<=request.targetHeight){
            videoScaleVerified_=true;pendingVideoScale_.reset();
            setVideoScaleNotice("实际视频尺寸已缩放到上限");
            diagnostic(("video-scale-verified-"+std::to_string(media.width)+"x"+
                        std::to_string(media.height)).c_str());
        }else if(now>=request.deadline){
            const bool acknowledged=request.acknowledged;
            pendingVideoScale_.reset();
            setVideoScaleNotice(acknowledged?"电脑已应答但视频尺寸未变化":"电脑未确认视频缩放，未自动重试");
            if(acknowledged)
                diagnostic(("video-scale-size-unconfirmed-actual-"+std::to_string(media.width)+"x"+
                            std::to_string(media.height)).c_str());
            else
                diagnostic("video-scale-ack-timeout");
        }
        return;
    }
    if(videoScaleAttempted_){
        if(videoScaleVerified_){
            const auto media=media_->stats();
            if(media.width>0 && media.height>0 &&
               (media.width>streamWidth_ || media.height>streamHeight_)){
                videoScaleVerified_=false;
                videoScaleAttempted_=false;
                nextVideoScaleCheck_=0;
                setVideoScaleNotice("实际视频已超出所选尺寸；正在重新请求缩放");
                diagnostic(("video-scale-output-exceeded-"+std::to_string(media.width)+"x"+
                            std::to_string(media.height)).c_str());
            }
        }
        return;
    }
    const auto media=media_->stats();
    if(media.width<=0 || media.height<=0 || media.width>16384 || media.height>16384)return;
    if(media.width<=streamWidth_ && media.height<=streamHeight_){
        videoScaleAttempted_=true;videoScaleVerified_=true;
        setVideoScaleNotice("视频尺寸已在目标范围内");
        diagnostic(("video-scale-already-within-"+std::to_string(media.width)+"x"+
                    std::to_string(media.height)).c_str());
        return;
    }
    if(channel->second->bufferedAmount()>65536)return;
    videoScaleAttempted_=true;videoScaleVerified_=false;
    const auto id=nextQualityRpc_++;
    try {
        const auto bytes=videoScaleRpc(id,media.width,media.height,streamWidth_,streamHeight_,
                                       streamFps_,streamQuality_,activeScreenId_,
                                       streamQuality_==6?maxBitrateMbps_:0);
        pendingVideoScale_=PendingVideoScale{id,now+8000,media.width,media.height,
                                             streamWidth_,streamHeight_};
        setVideoScaleNotice("正在请求把视频缩放到 "+std::to_string(streamWidth_)+"×"+
                            std::to_string(streamHeight_)+"，保留电脑桌面");
        const bool sent=channel->second->send(reinterpret_cast<const std::byte *>(bytes.data()),bytes.size());
        diagnostic(((sent?"video-scale-request-sent-src-":"video-scale-request-queued-src-")+
                   std::to_string(media.width)+"x"+std::to_string(media.height)+
                   "-dst-"+std::to_string(streamWidth_)+"x"+std::to_string(streamHeight_)).c_str());
    }catch(...){
        pendingVideoScale_.reset();
        setVideoScaleNotice("视频缩放请求未发送，未自动重试");
        diagnostic("video-scale-send-failed");
    }
}
void Client::applyBitrateCap() {
    // A configured custom encoder ceiling must not masquerade as a
    // fixed receiver bandwidth estimate. Keep normal RTCP reports/NACKs.
    if(streamQuality_==6 || !video_ || !video_->isOpen() || maxBitrateMbps_<=0 || bitrateFeedbackSent_) return;
    const auto now=monotonicMs();
    if(lastBitrateAttempt_ && now-lastBitrateAttempt_<1000) return;
    if(!rembAvailable_) {
        if(!lastBitrateAttempt_) diagnostic("bitrate-cap-not-negotiated");
        lastBitrateAttempt_=now;return;
    }
    lastBitrateAttempt_=now;
    try {
        // bps. The session repeats this stored feedback on sender reports
        // once the primary SSRC has been bound. Never send a zero REMB.
        bitrateFeedbackSent_=video_->requestBitrate(unsigned(maxBitrateMbps_)*1000000u);
        diagnostic(bitrateFeedbackSent_?"bitrate-cap-feedback-configured":"bitrate-cap-feedback-unavailable");
    } catch(...) {diagnostic("bitrate-cap-feedback-send-failed");}
}
void Client::refreshStats(int64_t now) {
    applyBitrateCap();
    lastStatsUpdate_ = now;
    StreamStatsSnapshot stats;
    if (peer_) {
        if (auto rtt = peer_->rtt())
            stats.rttMs = int(rtt->count());
        rtc::Candidate local, remote;
        if (peer_->getSelectedCandidatePair(&local, &remote)) {
            const auto identity=candidateIdentity(remote);
            const bool matchedRemoteRelay=!identity.empty() && remoteRelayEndpoints_.count(identity)>0;
            stats.transport = selectedPath(local, remote,matchedRemoteRelay);
            if(local.type()==rtc::Candidate::Type::Relayed && !turnBridges_.empty())
                stats.transport+=" · TURN上行 "+turnUplinkMode_;
            if(peer_->iceState()!=rtc::PeerConnection::IceState::Completed) stats.transport+=" · 探测中";
            stats.localEndpoint=endpoint(local);
            stats.remoteEndpoint=endpoint(remote);
            const auto summary="transport="+stats.transport+"\nlocal="+stats.localEndpoint+"\nremote="+stats.remoteEndpoint+"\n";
            if (summary!=lastSelectedPath_) {
                recordSelectedPath(summary);lastSelectedPath_=summary;
                if(diagEnabled()) {
                    auto &history=iceDiagnostics_["path_history"];
                    if(history.size()>=128) history.erase(history.begin());
                    history.push_back({{"time_ms",nowMs()},{"monotonic_ms",now},{"transport",stats.transport},{"local",stats.localEndpoint},
                        {"remote",stats.remoteEndpoint},{"matched_signaled_relay",matchedRemoteRelay},
                        {"ice_state",int(peer_->iceState())},{"rtt_ms",stats.rttMs}});
                    saveIceDiagnostics();
                }
                recordIceCandidate("local","selected",local);recordIceCandidate("remote","selected",remote);
            }
        }
    }
    if (videoStats_)
        stats.video = videoStats_->snapshot();
    if (audioStats_)
        stats.audio = audioStats_->snapshot();
    const auto media = media_->stats();
    stats.sampledMs=now;stats.decodedTotal=media.decodedTotal;stats.publishedTotal=media.publishedTotal;
    stats.mediaEpoch=media.epoch;stats.inputQueued=media.inputQueued;stats.hardwareFrame=media.hardwareFrame;
    stats.skipDue=media.skipDue;stats.skipBound=media.skipBound;stats.selectedLateMs=media.selectedLateMs;
    stats.cpuTransfers=media.cpuTransfers;stats.nativePublished=media.nativePublished;
    stats.codec = media.codec;
    stats.encoder = media.encoder;
    stats.decoder = media.decoder;
    stats.width = media.width;
    stats.height = media.height;
    stats.decodeFps = media.fps;
    stats.requestedFps = streamFps_;
    stats.maxBitrateMbps=maxBitrateMbps_;stats.bitrateFeedbackSent=bitrateFeedbackSent_;
    stats.touchPackets=touchPackets_;stats.touchFailures=touchFailures_;stats.touchPoints=touchPoints_;
    stats.qualitySent=qualitySent_;stats.qualityAcked=qualityAcked_;stats.qualityFailed=qualityFailed_;
    stats.qualityPending=bool(pendingQuality_);
    stats.activeScreenId=activeScreenId_;stats.qualityCode=lastQualityCode_;stats.qualityErrors=lastQualityErrors_;
    stats.decodeMs = media.decodeMs;
    stats.pacing = media.pacing;
    stats.playoutDelayMs = media.playoutDelayMs;
    stats.readyFrames = media.readyFrames;
    stats.presentationSkipped = media.presentationSkipped;
    stats.clockResets = media.clockResets;
    stats.missingTimestamps = media.missingTimestamps;
    stats.decodeErrors = media.decodeErrors;
    stats.droppedFrames = media.droppedFrames;
    stats.auMaxBytes=media.auMaxBytes;stats.auBytesTotal=media.auBytesTotal;
    stats.auCount=media.auCount;stats.auOversize=media.auOversize;
    stats.queueHighFrames=media.queueHighFrames;stats.queueHighBytes=media.queueHighBytes;
    stats.queueWaitLastMs=media.queueWaitLastMs;stats.queueWaitMaxMs=media.queueWaitMaxMs;
    stats.oldestQueueWaitMs=media.oldestQueueWaitMs;stats.decodeMaxMs=media.decodeMaxMs;
    stats.decodeGt8=media.decodeGt8;stats.decodeGt16=media.decodeGt16;stats.decodeGt33=media.decodeGt33;
    stats.lastDecodedAgeMs=media.lastDecodedAgeMs;
    stats.keyframeRequests = keyframeRequests_;
    stats.keyframeRecoveryMs = lastKeyframeRecoveryMs_;
    if (videoAssembler_) {
        stats.nackSent = videoAssembler_->nackCount();
        stats.rtxRecovered = videoAssembler_->rtxCount();
    }
    if (strictVideo_) {
        strictVideo_->setRtt(stats.rttMs);
        const auto receiver = strictVideo_->snapshot();
        stats.strictReceiver = true;
        stats.nackSent = receiver.nackMessages;
        stats.rtxRecovered = receiver.rtxRecovered;
        stats.receiverAccepted = receiver.accepted;
        stats.receiverDropped = receiver.dropped;
        stats.receiverWaitingIdr = receiver.waitingIdr;
        stats.receiverFastForwards = receiver.fastForwards;
        stats.receiverMeanWaitMs = receiver.meanWaitMs;
        stats.receiverRecentWaitMs = receiver.recentWaitMs;
        stats.receiverMaxWaitMs = receiver.maxWaitMs;
        stats.receiverPending=receiver.pendingFrames;stats.receiverBlockedMs=receiver.blockedMs;
        stats.nackRetryMs=receiver.nackRetryMs;stats.repairRttMs=receiver.repairRttMs;
        stats.nackRepeated=receiver.repeatedRequests;stats.receiverPli=receiver.pliRequests;
        stats.rxBufferedBytes=receiver.bufferedBytes;stats.rxBufferedHigh=receiver.bufferedHighBytes;
        stats.rxInputMessages=receiver.inputMessages;stats.rxInputBatches=receiver.inputBatches;
        stats.rxMaxBatch=receiver.maxBatch;stats.rxIncomingCalls=receiver.incomingCalls;
        stats.rxTickCalls=receiver.tickCalls;stats.rxIncomingTotalUs=receiver.incomingTotalUs;
        stats.rxIncomingMaxUs=receiver.incomingMaxUs;stats.rxTickTotalUs=receiver.tickTotalUs;
        stats.rxTickMaxUs=receiver.tickMaxUs;
        stats.rxMemoryBoundRetires=receiver.memoryBoundRetires;
        stats.rxMaxFrameBytes=receiver.maxFrameBytes;
        stats.repairPaced=receiver.repairPaced;stats.nackSequences=receiver.nackSequences;
        stats.nackBudgetDeferrals=receiver.budgetDeferrals;stats.nackExpiredScans=receiver.expiredRepairScans;
    }
    std::lock_guard lock(statsMutex_);
    statsCache_ = std::move(stats);
}
StreamStatsSnapshot Client::streamStats() {
    std::lock_guard lock(statsMutex_);
    return statsCache_;
}
} // namespace uu
