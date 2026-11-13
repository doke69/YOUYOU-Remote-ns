#pragma once
#include "auth.hpp"
#include "client_snapshot.hpp"
#include "runtime/latest_snapshot_worker.hpp"
#include "control_stage_timing.hpp"
#include "config_store.hpp"
#include "input_admission.hpp"
#include "media.hpp"
#include "quality_confirmation.hpp"
#include "quality_rpc.hpp"
#include "route_bitrate_policy.hpp"
#include "remb_feedback.hpp"
#include "stream_stats.hpp"
#include "transport.hpp"
#include "video_assembler.hpp"
#include "strict_video_receiver.hpp"
#include <array>
#include <functional>
#include <rtc/rtc.hpp>
#include <set>
#include <optional>
#include "transport_feedback_negotiation.hpp"
#ifdef UU_RBE_OBSERVE_CLIENT
#include "receive_bandwidth_observer.hpp"
#endif
#ifdef UU_TWCC_OBSERVE_CLIENT
#include "transport_feedback_attachment.hpp"
#endif

namespace uu {
class TurnStreamBridge;
class Client {
    struct Event {
        std::string type;
        Json data;
        uint64_t generation = 0;
        int64_t queuedAtMs = 0;
        InputMotionKey motion;
    };
    struct Inbox {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Event> events;
        InputAdmission inputAdmission;
        size_t peerCallbackHighWater=0,dataChannelHighWater=0;
        uint64_t peerCallbackRejected=0,dataChannelRejected=0;
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
    std::optional<twcc::NegotiationCapabilities> feedbackLocalCaps_, feedbackRemoteCaps_;
    std::optional<twcc::FeedbackCapabilitiesResult> feedbackCapabilities_;
    void invalidateFeedbackCapabilities();
    void refreshFeedbackCapabilities();
#ifdef UU_RBE_OBSERVE_CLIENT
    std::shared_ptr<rbe::ReceiveBandwidthObserver> receiveBandwidthObservation_;
    int64_t receiveBandwidthLogAt_ = 0;
    void stopReceiveBandwidthObservation();
    void refreshReceiveBandwidthObservation();
#ifdef UU_RBE_CLIENT_SEND
    bool receiveBandwidthSendAttempted_ = false, receiveBandwidthSuppressLegacy_ = false;
    void pollReceiveBandwidthFeedback(int64_t now);
#endif
#endif
#ifdef UU_TWCC_OBSERVE_CLIENT
    std::shared_ptr<twcc::FeedbackAttachment> feedbackObservation_;
    void stopFeedbackObservation();
    void refreshFeedbackObservation();
#ifdef UU_TWCC_CLIENT_SEND
    bool feedbackSendAttempted_ = false, feedbackSuppressRemb_ = false;
    int64_t feedbackSendLogAt_ = 0;
    void pollFeedbackSending(int64_t now);
#endif
#endif
    std::vector<std::shared_ptr<TurnStreamBridge>> turnBridges_;
    std::set<const TurnStreamBridge *> reportedTurnBridgeFailures_;
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
    struct PendingSafetyAction {
        unsigned action;
        std::optional<bool> enabled;
        int64_t deadline;
        bool quiet;
    };
    // Lock, mute/unmute, then the restoration policy. A newer choice replaces its slot.
    std::array<std::optional<PendingSafetyAction>,3> safetyActions_;
    int64_t nextSafetyAttempt_=0;
    void tickSafetyActions();
    std::map<int,std::set<std::string>> mouseOwners_;
    struct PendingText {uint64_t id;int64_t deadline;};
    std::optional<PendingText> pendingText_;
    void finishTextInput(int result,const char *notice);
    std::array<bool,2> gamepadConnected_{};
    struct PendingPadMotion {
        Json data;
        InputMotionKey key;
        uint64_t sequence;
    };
    std::array<InputMotionKey,2> submittedPadMotion_{};
    std::array<std::optional<PendingPadMotion>,2> pendingPadMotion_{};
    uint64_t padMotionDeferred_=0, padMotionReplaced_=0;
    bool nativeTouchActive_=false;
    uint64_t touchPackets_=0,touchFailures_=0;unsigned touchPoints_=0;
    unsigned soacEvents_ = 0, soacRejected_ = 0;
    bool remoteDescriptionApplied_ = false;
    std::deque<Json> earlySoac_;
    std::deque<rtc::Candidate> pendingCandidates_;
    SioPacket pending_;
    int remainingAttachments_ = 0;
    std::vector<Bytes> pendingAttachments_;
    size_t pendingAttachmentBytes_=0;
    uint64_t signalPacketCount_=0,signalBinaryFrameCount_=0;
    int64_t deadline_ = 0, connectionAttemptStartedAt_=0, lastPacket_ = 0, lastKeyframeRequest_ = 0, heartbeatTimeout_ = 45000,
            qrExpires_ = 0, qrNext_ = 0;
    // Stream options sent to the computer (config.ini overridable for on-device
    // experiments: width/height/fps/quality and keyframe request interval).
    int streamWidth_ = 1280, streamHeight_ = 720, streamFps_ = 30, streamQuality_ = 3;
    int keyframeIntervalMs_ = 0;
    int maxBitrateMbps_ = 0;
    BitrateRoute bitrateRoute_=BitrateRoute::Unknown;
    int lastEncoderQuality_=0,lastEncoderCap_=-1;
    struct RouteBitrateRequest {uint64_t id;int quality,cap;int64_t deadline;};
    std::optional<RouteBitrateRequest> routeBitrateRequest_;
    bool routeBitrateUpdateFailed_=false;
    void refreshRouteBitrate();
    bool hevc_ = false;
    bool rembAvailable_ = false;
    RembFeedbackGate rembGate_;
    bool rembNotNegotiatedLogged_ = false, rembFirstRtpWaitLogged_ = false;
    // One-shot per-session UU_REMB_FEEDBACK A/B marker; reset on disconnect.
    bool rembExperimentLogged_ = false;
    uint32_t localRtcpSsrc_ = 0;
    std::string localRtcpCname_;
    void applyBitrateCap(const NetworkStats *sample = nullptr);
    using QualityValues=QualityConfigValues;
    std::unique_ptr<QualityConfigWriter> qualityConfigWriter_;
    uint64_t qualitySaveOrder_=0,qualitySaveObserved_=0;
    void pollQualitySave();
    enum class QualityRpcKind {QualityOnly,Capture,Fps,VideoQuality};
    struct PendingQuality {QualityValues target;uint64_t id;int64_t deadline;bool acknowledged=false;bool headerOnly=false;int screenId=-1;QualityRpcKind kind=QualityRpcKind::QualityOnly;bool retried=false;bool bitrateOnly=false;};
    std::optional<PendingQuality> pendingQuality_;
    struct PendingQualityApply {QualityValues target;uint64_t id;int64_t deadline;};
    std::optional<PendingQualityApply> pendingQualityApply_;
    QualityApplyTracker qualityApply_;
    bool fpsSettingSupported_=true,qualitySettingSupported_=true;
    uint64_t nextQualityRpc_=1,qualitySent_=0,qualityAcked_=0,qualityFailed_=0;
    uint64_t qualityAckKeyframeAttempted_=0,qualityAckKeyframeSkipped_=0;
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
    int64_t nextVideoScaleCheck_=0,videoScaleStableSince_=0,videoScaleRetryAt_=0;
    int videoScaleObservedWidth_=0,videoScaleObservedHeight_=0;
    unsigned videoScaleAttempts_=0;
    void tickVideoScale();
    void setVideoScaleNotice(const std::string &);
    std::string connectedDeviceId_;
    bool autoReconnect_=true,reconnectArmed_=false;
    int reconnectAttempts_=0,reconnectLimit_=5;
    int64_t reconnectAt_=0,connectedSince_=0;
    std::string lastSelectedPath_;
    struct PathQueryJob {
        uint64_t order,generation;
        std::shared_ptr<rtc::PeerConnection> peer;
        size_t retainedBytes() const noexcept {return sizeof(PathQueryJob);}
    };
    struct PathQueryResult {
        uint64_t generation=0;
        bool selected=false;
        rtc::Candidate local,remote;
        int64_t completedMs=0;
        uint64_t queryUs=0;
    };
    struct PathQueryState {std::mutex mutex;PathQueryResult result;};
    std::shared_ptr<PathQueryState> pathQueryState_;
    std::unique_ptr<runtime::LatestSnapshotWorker<PathQueryJob>> pathQueryWorker_;
    uint64_t pathQueryOrder_=0,pathQueryFailures_=0;
    Json iceDiagnostics_;
    std::set<std::string> remoteRelayEndpoints_;
    void recordIceCandidate(const char *direction, const char *stage, rtc::Candidate candidate);
    void saveIceDiagnostics();
    struct PendingPrivacy {uint64_t id;int64_t deadline;PrivacyMode mode;bool stateNotified=false;};
    std::optional<PendingPrivacy> pendingPrivacy_;
    struct PendingCursorCapture {uint64_t id;int64_t deadline;};
    std::optional<PendingCursorCapture> pendingCursorCapture_;
    bool autoCursorCaptureHandled_=false;
    bool rtxEnabled_ = false;
    bool sdpDumpDone_ = false;
    Json qr_;
    int qrStatus_ = 1;
    SmsAuth sms_;
    std::shared_ptr<RtpTrackStats> videoStats_, audioStats_;
    std::shared_ptr<VideoSupplyTrace> videoSupplyTrace_;
    std::shared_ptr<VideoRepairTrace> videoRepairTrace_;
    std::shared_ptr<VideoAssembler> videoAssembler_;
    std::shared_ptr<StrictVideoReceiver> strictVideo_;
    std::mutex statsMutex_;
    StreamStatsSnapshot statsCache_;
    int64_t lastStatsUpdate_ = 0, pipelineLogAt_ = 0, pendingKeyframeAt_ = 0, lastKeyframeRecoveryMs_ = -1;
    uint64_t keyframeRequests_ = 0, keyframeCountAtRequest_ = 0;
    ControlStageTiming controlStages_;
    struct InputTiming {
        uint64_t samples=0, waitTotalMs=0, waitMaxMs=0, handleTotalUs=0, handleMaxUs=0;
        uint64_t receiverTickMaxUs=0, feedbackMaxUs=0;
        uint64_t controlBufferedMax=0;
        int64_t reportedAtMs=0;
    } inputTiming_;
    bool firstVideoRtpLogged_=false,firstAudioRtpLogged_=false;
    bool firstVideoAccessUnitLogged_=false,firstVideoFramePublishedLogged_=false;
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
    void logEventQueueSnapshot(const char *reason);
    void refresh();
    void disconnect();
    void input(const Json &data, InputMotionKey motion = {}, uint64_t sequence = 0);
    void flushPadMotion();
    bool sendControlInput(rtc::DataChannel &channel, const std::byte *data, size_t size,
                          const std::string &action, uint64_t sequence, size_t buffered);
    void release();
    void pollQr();
    void completeLogin(const Json &response);
    void post(std::string type, Json data = nullptr, uint64_t generation = 0);
    bool isStreaming();

  public:
    Client(std::string key, std::string client, std::shared_ptr<Media> media);
    ~Client();
    Snapshot snapshot();
    void refreshSnapshot(Snapshot &snapshot);
    void command(const std::string &name, Json data = nullptr);
    /// Latest network/media statistics for the on-screen network panel.
    StreamStatsSnapshot streamStats();
};
} // namespace uu
