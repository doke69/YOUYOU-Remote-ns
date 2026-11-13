#pragma once
#include "quality_rpc.hpp"
#include <tuple>

namespace uu {
struct Snapshot {
    bool qualitySavePending=false;
    std::string qualitySaveError;
    bool privacyPending=false,privacyKnown=false;
    PrivacyMode privacyMode=PrivacyMode::Off,privacyRequestedMode=PrivacyMode::Unknown;
    std::string privacyNotice;
    bool cursorCapturePending=false;
    std::string cursorCaptureNotice;
    bool textPending=false;
    uint64_t textRevision=0;
    int textResult=0; // 0=idle, 1=pending, 2=paste submitted, 3=failed/cancelled.
    std::string textNotice;
    std::string activeDeviceId; // Session identity only; never a login credential.
    bool textChannelReady=false;
    bool controlChannelReady=false;
    std::string safetyNotice;
    std::string stage = "idle", detail = "按 A 扫码登录，或按 X 使用短信验证码", qrUrl, nickname;
    Json devices = Json::array();
    uint64_t devicesRevision = 0;
    bool authenticated = false, streaming = false;
    int width = 1280, height = 720, fps = 30;
    int quality = 3, maxBitrateMbps = 0;
    bool vsync = false, hevc = false, qualityPending=false;
    std::string qualityNotice;
    std::string routeBitrateNotice;
    int effectiveBitrateMbps=20;
    std::string videoScaleNotice;
    int playoutDelayMs = 0;
    // Actual host capture state, when CaptureSettingSync is available.
    int appliedFps = 0, appliedQuality = 0, appliedWidth = 0, appliedHeight = 0;
    int appliedBitrateMbps = 0;
    bool appliedValid = false, appliedBitrateKnown = false;
    bool appliedFpsKnown = false, appliedQualityKnown = false;
    bool appliedWidthKnown = false, appliedHeightKnown = false;
    // Unified setting result: ACK is accepted; only CaptureSettingSync/media
    // can promote a setting to applied. Apply confirmation is tracked
    // independently from the RPC pending state.
    bool settingAccepted=false, settingApplied=false, settingRejected=false;
    bool settingApplyPending=false;
    int settingErrorCode=0;
    std::string settingSource;
    // Parsed forward_setting capability summary (safe, bounded strings only).
    bool serverCapabilityValid=false;
    std::string serverDisplayInfo,serverCodecInfo,serverCaptureImpl,serverEncoderImpl;
    int serverVideoTrack=-1;
    std::string loginMode = "qr";
    int64_t smsRetryAt = 0;
    // UI owns this copy. Device JSON changes are represented by devicesRevision.
    // Compare typed values under the client's state lock, then copy only on change.
    bool updateFrom(const Snapshot &latest) {
        if (std::tie(
            qualitySavePending, qualitySaveError, privacyPending, privacyKnown,
            privacyMode, privacyRequestedMode, privacyNotice, cursorCapturePending, cursorCaptureNotice, textPending,
            textRevision, textResult, textNotice, activeDeviceId,
            textChannelReady, controlChannelReady, safetyNotice, stage, detail,
            qrUrl, nickname, devicesRevision, authenticated,
            streaming, width, height, fps,
            quality, maxBitrateMbps, vsync, hevc,
            qualityPending, qualityNotice, routeBitrateNotice, effectiveBitrateMbps,
            videoScaleNotice, playoutDelayMs, appliedFps, appliedQuality,
            appliedWidth, appliedHeight, appliedBitrateMbps, appliedValid,
            appliedBitrateKnown, appliedFpsKnown, appliedQualityKnown, appliedWidthKnown,
            appliedHeightKnown, settingAccepted, settingApplied, settingRejected,
            settingApplyPending, settingErrorCode, settingSource, serverCapabilityValid,
            serverDisplayInfo, serverCodecInfo, serverCaptureImpl, serverEncoderImpl,
            serverVideoTrack, loginMode, smsRetryAt) == std::tie(
            latest.qualitySavePending, latest.qualitySaveError, latest.privacyPending, latest.privacyKnown,
            latest.privacyMode, latest.privacyRequestedMode, latest.privacyNotice, latest.cursorCapturePending, latest.cursorCaptureNotice, latest.textPending,
            latest.textRevision, latest.textResult, latest.textNotice, latest.activeDeviceId,
            latest.textChannelReady, latest.controlChannelReady, latest.safetyNotice, latest.stage, latest.detail,
            latest.qrUrl, latest.nickname, latest.devicesRevision, latest.authenticated,
            latest.streaming, latest.width, latest.height, latest.fps,
            latest.quality, latest.maxBitrateMbps, latest.vsync, latest.hevc,
            latest.qualityPending, latest.qualityNotice, latest.routeBitrateNotice, latest.effectiveBitrateMbps,
            latest.videoScaleNotice, latest.playoutDelayMs, latest.appliedFps, latest.appliedQuality,
            latest.appliedWidth, latest.appliedHeight, latest.appliedBitrateMbps, latest.appliedValid,
            latest.appliedBitrateKnown, latest.appliedFpsKnown, latest.appliedQualityKnown, latest.appliedWidthKnown,
            latest.appliedHeightKnown, latest.settingAccepted, latest.settingApplied, latest.settingRejected,
            latest.settingApplyPending, latest.settingErrorCode, latest.settingSource, latest.serverCapabilityValid,
            latest.serverDisplayInfo, latest.serverCodecInfo, latest.serverCaptureImpl, latest.serverEncoderImpl,
            latest.serverVideoTrack, latest.loginMode, latest.smsRetryAt)) return false;
        *this = latest;
        return true;
    }
};
} // namespace uu
