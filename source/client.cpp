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
    static uint32_t read32(const uint8_t *p) {
        return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
    }
  public:
    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override {
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
const char *transportLabel(const rtc::Candidate &local, const rtc::Candidate &remote) {
    using Type = rtc::Candidate::Type;
    if (local.type() == Type::Relayed || remote.type() == Type::Relayed)
        return "TURN 中继";
    if (local.type() == Type::Host && remote.type() == Type::Host)
        return "局域网直连";
    return "P2P 直连";
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
    std::string line, retained;
    while (std::getline(input, line)) {
        const auto key = trim(line.substr(0, line.find('=')));
        if (key != "width" && key != "height" && key != "fps" && key != "quality" && key != "vsync" &&
            key != "playout_delay_ms" && key != "frame_pacing" && key != "max_bitrate_mbps" && key != "video_codec") retained += line + '\n';
    }
    input.close();
    std::ofstream output(path + ".quality-tmp", std::ios::trunc);
    output << retained << "width=" << width << "\nheight=" << height << "\nfps=" << fps << "\nquality=" << quality << '\n';
    output << "vsync=" << (vsync ? 1 : 0) << '\n';
    output << "frame_pacing=1\nplayout_delay_ms=" << playoutDelay << '\n';
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
                } else {reconnectAt_=0;setState("error",failure);}
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
            diagnostic(("quality-current-screen-"+std::to_string(screen)+"-count-"+std::to_string(data.at("count").get<unsigned>())).c_str());
            if(pendingQuality_ && pendingQuality_->screenId!=screen)
                fallbackQuality("电脑屏幕已变化，正在重新连接…");
        }
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
        const auto it=channels_.find("TEXT_DATA_CHANNEL");
        if(it==channels_.end() || !it->second->isOpen() || it->second->bufferedAmount()>65536){
            reconnectQuality(target,"设置通道尚未就绪，正在重连应用…");return;
        }
        if(activeScreenId_<0){
            diagnostic("quality-current-screen-unavailable");
            reconnectQuality(target,"尚未收到电脑屏幕信息，正在重连应用…");return;
        }
        const auto id=nextQualityRpc_++;
        const auto bytes=captureSettingsRpc(id,width,height,fps,quality,activeScreenId_,quality==6?maxBitrate:0);
        pendingQuality_=PendingQuality{target,id,monotonicMs()+5000,false,false,activeScreenId_};
        {std::lock_guard lock(stateMutex_);state_.qualityPending=true;state_.qualityNotice="正在等待电脑确认画质…";}
        try{
            if(!it->second->send(reinterpret_cast<const std::byte *>(bytes.data()),bytes.size())){
                fallbackQuality("设置发送受阻，正在重连应用…");return;
            }
            ++qualitySent_;diagnostic(("quality-rpc-sent-"+std::to_string(id)).c_str());
        }catch(...){fallbackQuality("设置发送失败，正在重连应用…");}
    } else if(e=="quality-rpc-ack") {
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
            fallbackQuality("电脑未确认在线设置，正在重连应用…");return;
        }
        // Original CaptureSettingResp leaves errors empty when the optional
        // capture body is absent. A matching header-only reply is an RPC
        // acknowledgment, not evidence of actual encoder FPS or quality.
        pendingQuality_->headerOnly=!capture;
        ++qualityAcked_;pendingQuality_->acknowledged=true;pendingQuality_->deadline=monotonicMs()+5000;
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
        const auto channel=channels_.find("TEXT_DATA_CHANNEL");
        if(!snapshot().streaming || channel==channels_.end() || !channel->second->isOpen() || channel->second->bufferedAmount()>65536){
            inform("电脑控制通道尚未就绪，未发送操作");return;
        }
        const auto action=data.at("action").get<unsigned>();
        const auto enabled=data.contains("enabled")?std::optional<bool>(data.at("enabled").get<bool>()):std::nullopt;
        const auto bytes=simpleActionMessage(nextQualityRpc_++,action,enabled);
        try {
            if(channel->second->send(reinterpret_cast<const std::byte *>(bytes.data()),bytes.size()))inform("操作请求已发送，请确认电脑端结果");
            else inform("操作请求已排队，等待电脑端处理");
        }catch(...){inform("电脑控制操作发送失败");}
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
        if (event == "room") {
            auto publisher = value.at("publisher");
            remote_ = publisher.is_object() ? publisher.value("client_id", publisher.value("id", Json{}))
                                            : publisher;
            if ((!remote_.is_string() && !remote_.is_number()) || remote_ == "")
                throw std::runtime_error("房间内没有在线电脑");
            control_ = uuid();
            auto options = connectOptions(streamWidth_, streamHeight_, streamFps_, streamQuality_,streamQuality_==6?maxBitrateMbps_:0);
            options.insert(options.begin(), 4);
            diagnostic(("stream-options-" + std::to_string(streamWidth_) + "x" +
                        std::to_string(streamHeight_) + "-" + std::to_string(streamFps_) + "fps-q" +
                        std::to_string(streamQuality_) + "-cap" +
                        (streamQuality_==6?std::to_string(maxBitrateMbps_):std::string("auto")) +
                        "-kf" + std::to_string(keyframeIntervalMs_))
                           .c_str());
            setState("negotiating", "正在申请远控权限…");
            // Preserve room ICE configuration if control does not repeat it.
            ice_ = value.value("iceServers", Json::array());
            emit("control",
                 {{"app_data", {{"_placeholder", true}, {"num", 0}}},
                  {"streamer_data", ""},
                  {"app_control_id", control_}},
                 "control", options);
        } else if (event == "control")
            beginPeer(value);
    } else if (p.type == 2 || p.type == 5) {
        if (!p.data.is_array() || p.data.empty() || !p.data[0].is_string())
            throw std::runtime_error("Invalid signaling event");
        auto event = p.data[0].get<std::string>();
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
        if(std::string(label)=="TEXT_DATA_CHANNEL"){
            std::weak_ptr<Inbox> inbox=inbox_;const auto generation=generation_;
            auto stateChanged=[inbox,generation](bool ready){
                if(auto queue=inbox.lock()){
                    std::lock_guard lock(queue->mutex);
                    queue->events.push_back({"text-channel-state",ready,generation});queue->cv.notify_one();
                }
            };
            channel->onOpen([stateChanged]{stateChanged(true);});
            channel->onClosed([stateChanged]{stateChanged(false);});
        }
        {
            const bool acceptRpc=std::string(label)=="TEXT_DATA_CHANNEL" || std::string(label)=="FILE_DATA_CHANNEL";
            std::weak_ptr<Inbox> inbox=inbox_;const auto generation=generation_;
            channel->onMessage([inbox,generation,acceptRpc](rtc::message_variant message) {
                const uint8_t *bytes=nullptr;size_t size=0;
                if(auto *binary=std::get_if<rtc::binary>(&message)){bytes=reinterpret_cast<const uint8_t *>(binary->data());size=binary->size();}
                else if(auto *text=std::get_if<std::string>(&message)){bytes=reinterpret_cast<const uint8_t *>(text->data());size=text->size();}
                const auto screens=parseScreenSources(bytes,size);
                const auto ack=acceptRpc?parseCaptureRpcAck(bytes,size):std::optional<CaptureRpcAck>{};
                const auto textAck=acceptRpc?parseTextRpcAck(bytes,size):std::optional<TextRpcAck>{};
                if(!screens && !ack && !textAck)return;
                if(auto queue=inbox.lock()){
                    std::lock_guard lock(queue->mutex);
                    if(queue->events.size()>124)return;
                    if(screens)queue->events.push_back({"quality-screens",{{"screen",screens->screenId},{"count",screens->count}},generation});
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
    diagnostic("disconnect");
    if(pendingText_)finishTextInput(3,"连接已断开，已取消等待中的粘贴");
#ifdef UU_TWCC_OBSERVE_CLIENT
    stopFeedbackObservation();
    feedbackLocalCaps_.reset(); feedbackRemoteCaps_.reset();
#endif
    pendingQuality_.reset();
    {std::lock_guard lock(stateMutex_);state_.qualityPending=false;}
    connectedSince_=0;
    if (strictVideo_)
        strictVideo_->shutdown();
    release();
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
    {
        std::lock_guard lock(stateMutex_);
        state_.qrUrl.clear();
        state_.streaming = false;
        state_.activeDeviceId.clear();
        state_.textChannelReady=false;
    }
}
void Client::commitQuality(const QualityValues &q,bool hotCap,const char *notice) {
    if(hotCap && q.quality!=6 && (!video_ || !rembAvailable_ || q.cap<=0 || !video_->requestBitrate(unsigned(q.cap)*1000000u)))
        throw std::runtime_error("码率反馈更新失败");
    // The four v0.2 UI presets use exact-rate encoding. Keep successful normal
    // presets, while retaining session-only handling for high-bitrate probes.
    const bool fixedUiPreset=q.quality==6 && (q.cap==5 || q.cap==12 || q.cap==20 || q.cap==30);
    if(q.quality!=6 || fixedUiPreset)saveQuality(q.width,q.height,q.fps,q.quality,q.vsync,q.delay,q.cap,q.hevc);
    media_->setPlayoutDelay(q.delay);
    streamWidth_=q.width;streamHeight_=q.height;streamFps_=q.fps;streamQuality_=q.quality;
    maxBitrateMbps_=q.cap;hevc_=q.hevc;
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
        commitQuality(q,q.quality!=6 && q.cap!=maxBitrateMbps_,"电脑已应答画质设置");
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
        if(pendingQuality_ && monotonicMs()>=pendingQuality_->deadline)fallbackQuality("在线设置超时，正在重连应用…");
    }
    if(qualityNoticeUntil_ && monotonicMs()>=qualityNoticeUntil_ && !pendingQuality_){
        std::lock_guard lock(stateMutex_);state_.qualityNotice.clear();qualityNoticeUntil_=0;
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
