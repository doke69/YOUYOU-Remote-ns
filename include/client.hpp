#pragma once
#include "auth.hpp"
#include "media.hpp"
#include "stream_stats.hpp"
#include "transport.hpp"
#include "video_assembler.hpp"
#include "strict_video_receiver.hpp"
#include <functional>
#include <rtc/rtc.hpp>
#include <set>
#include <optional>
#ifdef UU_TWCC_OBSERVE_CLIENT
#include "transport_feedback_attachment.hpp"
#endif

namespace uu {
class TurnStreamBridge;
struct Snapshot {
    bool textPending=false;
    uint64_t textRevision=0;
    int textResult=0; // 0=idle, 1=pending, 2=paste submitted, 3=failed/cancelled.
    std::string textNotice;
    std::string activeDeviceId; // Session identity only; never a login credential.
    bool textChannelReady=false;
    bool controlChannelReady=false;
    std::string stage = "idle", detail = "按 A 扫码登录，或按 X 使用短信验证码", qrUrl, nickname;
    Json devices = Json::array();
    bool authenticated = false, streaming = false;
    int width = 1280, height = 720, fps = 30;
    int quality = 3, maxBitrateMbps = 0;
    bool vsync = true, hevc = false, qualityPending=false;
    std::string qualityNotice;
    std::string videoScaleNotice;
    int playoutDelayMs = 35;
    // Actual host capture state, when CaptureSettingSync is available.
    int appliedFps = 0, appliedQuality = 0, appliedWidth = 0, appliedHeight = 0;
    int appliedBitrateMbps = 0;
    bool appliedValid = false, appliedBitrateKnown = false;
    // Unified setting result: ACK is accepted; only CaptureSettingSync/media
    // can promote a setting to applied.
    bool settingAccepted=false, settingApplied=false, settingRejected=false;
    int settingErrorCode=0;
    std::string settingSource;
    // Parsed forward_setting capability summary (safe, bounded strings only).
    bool serverCapabilityValid=false;
    std::string serverDisplayInfo,serverCodecInfo,serverCaptureImpl,serverEncoderImpl;
    int serverVideoTrack=-1;
    std::string loginMode = "qr";
    int64_t smsRetryAt = 0;
};
class Client {
    struct Event {
        std::string type;
        Json data;
        uint64_t generation = 0;
    };
    struct Inbox {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Event> events;
    };
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    std::shared_ptr<Media> media_;
    std::atomic<bool> stop_{false}, cancel_{false};
    std::thread worker_;
    std::mutex stateMutex_;
    Snapshot state_;
    std::unique_ptr<Api> api_;
    std::unique_ptr<SignalSocket> socket_;
    std::shared_ptr<rtc::PeerConnection> peer_;
#ifdef UU_TWCC_OBSERVE_CLIENT
    std::optional<twcc::NegotiationCapabilities> feedbackLocalCaps_, feedbackRemoteCaps_;
    std::shared_ptr<twcc::FeedbackAttachment> feedbackObservation_;
    void stopFeedbackObservation();
    void refreshFeedbackObservation();
#endif
    std::vector<std::shared_ptr<TurnStreamBridge>> turnBridges_;
    std::string turnUplinkMode_="udp";
    rtc::PeerConnection::IceState iceState_ = rtc::PeerConnection::IceState::New;
    std::map<std::string, std::shared_ptr<rtc::DataChannel>> channels_;
    std::shared_ptr<rtc::Track> video_, audio_;
    Json remote_, ice_;
    std::string control_;
    uint64_t generation_ = 1;
    int nextAck_ = 1;
    std::map<int, std::string> acks_;
    std::set<int> heldKeys_, heldButtons_;
    std::map<int,std::set<std::string>> mouseOwners_;
    struct PendingText {uint64_t id;int64_t deadline;};
    std::optional<PendingText> pendingText_;
    void finishTextInput(int result,const char *notice);
    bool gamepadConnected_=false,nativeTouchActive_=false;
    uint64_t touchPackets_=0,touchFailures_=0;unsigned touchPoints_=0;
    std::deque<Json> earlySoac_;
    std::deque<rtc::Candidate> pendingCandidates_;
    SioPacket pending_;
    int remainingAttachments_ = 0;
    int64_t deadline_ = 0, lastPacket_ = 0, lastKeyframeRequest_ = 0, heartbeatTimeout_ = 45000,
            qrExpires_ = 0, qrNext_ = 0;
    // Stream options sent to the computer (config.ini overridable for on-device
    // experiments: width/height/fps/quality and keyframe request interval).
    int streamWidth_ = 1280, streamHeight_ = 720, streamFps_ = 30, streamQuality_ = 3;
    int keyframeIntervalMs_ = 4000;
    int maxBitrateMbps_ = 0;
    bool hevc_ = false;
    bool rembAvailable_ = false, bitrateFeedbackSent_ = false;
    int64_t lastBitrateAttempt_ = 0;
    void applyBitrateCap();
    struct QualityValues {int width,height,fps,quality,cap,delay;bool vsync,hevc;};
    enum class QualityRpcKind {QualityOnly,Capture,Fps,VideoQuality};
    struct PendingQuality {QualityValues target;uint64_t id;int64_t deadline;bool acknowledged=false;bool headerOnly=false;int screenId=-1;QualityRpcKind kind=QualityRpcKind::QualityOnly;bool retried=false;};
    std::optional<PendingQuality> pendingQuality_;
    bool fpsSettingSupported_=true,qualitySettingSupported_=true;
    uint64_t nextQualityRpc_=1,qualitySent_=0,qualityAcked_=0,qualityFailed_=0;
    int activeScreenId_=-1,lastQualityCode_=0;unsigned lastQualityErrors_=0;
    int64_t qualityNoticeUntil_=0;
    void commitQuality(const QualityValues &,bool hotCap,const char *notice);
    void reconnectQuality(const QualityValues &,const char *notice);
    void finishQuality();
    void fallbackQuality(const char *notice);
    void tickQuality();
    struct PendingVideoScale {uint64_t id;int64_t deadline;int sourceWidth,sourceHeight,targetWidth,targetHeight;bool acknowledged=false;uint64_t mediaEpoch=0,publishedBefore=0;};
    std::optional<PendingVideoScale> pendingVideoScale_;
    bool videoScaleAttempted_=false,videoScaleVerified_=false;
    int64_t nextVideoScaleCheck_=0;
    void tickVideoScale();
    void setVideoScaleNotice(const std::string &);
    std::string connectedDeviceId_;
    bool autoReconnect_=true,reconnectArmed_=false;
    int reconnectAttempts_=0,reconnectLimit_=5;
    int64_t reconnectAt_=0,connectedSince_=0;
    std::string lastSelectedPath_;
    Json iceDiagnostics_;
    std::set<std::string> remoteRelayEndpoints_;
    void recordIceCandidate(const char *direction, const char *stage, rtc::Candidate candidate);
    void saveIceDiagnostics();
    bool rtxEnabled_ = false;
    bool sdpDumpDone_ = false;
    Json qr_;
    int qrStatus_ = 1;
    SmsAuth sms_;
    std::shared_ptr<RtpTrackStats> videoStats_, audioStats_;
    std::shared_ptr<VideoAssembler> videoAssembler_;
    std::shared_ptr<StrictVideoReceiver> strictVideo_;
    std::mutex statsMutex_;
    StreamStatsSnapshot statsCache_;
    int64_t lastStatsUpdate_ = 0, pendingKeyframeAt_ = 0, lastKeyframeRecoveryMs_ = -1;
    uint64_t keyframeRequests_ = 0, keyframeCountAtRequest_ = 0;
    void refreshStats(int64_t now);
    void run();
    void drainEvents();
    void handle(const Event &event);
    void packet(const WsMessage &message);
    void dispatch(const SioPacket &packet);
    void beginPeer(const Json &control);
    void handleSoac(const Json &data);
    void emit(const std::string &name, const Json &data, const std::string &ack = "",
              const Bytes &attachment = {});
    void setState(const std::string &stage, const std::string &detail);
    void refresh();
    void disconnect();
    void input(const Json &data);
    void release();
    void pollQr();
    void completeLogin(const Json &response);
    void post(std::string type, Json data = nullptr, uint64_t generation = 0);

  public:
    Client(std::string key, std::string client, std::shared_ptr<Media> media);
    ~Client();
    Snapshot snapshot();
    void command(const std::string &name, Json data = nullptr);
    /// Latest network/media statistics for the on-screen network panel.
    StreamStatsSnapshot streamStats();
};
} // namespace uu
