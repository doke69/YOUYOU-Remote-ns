#pragma once
#include "protocol.hpp"
#include <optional>
namespace uu {
// Main.Message.simple_action belongs to CONTROL, not the clipboard/RPC TEXT channel.
inline constexpr const char *systemActionChannel="CONTROL_DATA_CHANNEL";
struct CaptureRpcAck {uint64_t requestId=0;unsigned errors=0,entries=0;int errorCode=0;bool capture=false;unsigned otherFields=0;};
struct ScreenSourceInfo {int screenId=-1;unsigned count=0;};
struct CaptureSettingSyncInfo {
    int fps=0,quality=0,chroma=0,resolutionType=0,dpiScale=0,orientation=0;
    int64_t maxCustomBitrate=0;
    int width=0,height=0,pixelWidth=0,pixelHeight=0;
};
std::optional<CaptureSettingSyncInfo> parseCaptureSettingSync(const uint8_t *data,size_t size) noexcept;
struct TextRpcAck {uint64_t requestId=0;unsigned status=0;};
Bytes clipboardTextRpc(uint64_t requestId,const std::string &text);
std::optional<TextRpcAck> parseTextRpcAck(const uint8_t *data,size_t size) noexcept;
std::optional<ScreenSourceInfo> parseScreenSources(const uint8_t *data,size_t size) noexcept;
// Diagnostic-only bounded protobuf field summary. It never includes payload
// strings; used to locate the host screen C7 width/height/pixel fields.
std::string screenSourceWireSummary(const uint8_t *data,size_t size) noexcept;
Bytes nativeTouchMessage(uint64_t seq,int type,const Json &points);
Bytes simpleActionMessage(uint64_t seq,unsigned action,std::optional<bool> enabled=std::nullopt);
Bytes captureSettingsRpc(uint64_t requestId,int width,int height,int fps,int quality,int screenId,int customMbps=0);
// Protocol-native settings: Main.RpcRequest field3/4 with their dedicated
// FpsSettingRequest / VideoQualitySettingReqeust bodies.
Bytes fpsSettingRpc(uint64_t requestId,int fps);
Bytes videoQualitySettingRpc(uint64_t requestId,int quality);
// Quality/fps/bitrate-only update. Deliberately omits resolution fields so a
// bitrate or frame-rate change cannot switch the host capture resolution.
Bytes qualityUpdateRpc(uint64_t requestId,int fps,int quality,int screenId,int customMbps=0);
// Full mobile-shaped capture request: source dimensions stay as observed, while
// the pixel dimensions bound the encoded video. Keeps fps/quality/bitrate.
Bytes videoScaleRpc(uint64_t requestId,int sourceWidth,int sourceHeight,
                    int outputWidth,int outputHeight,int fps,int quality,
                    int screenId,int customMbps=0);
std::optional<CaptureRpcAck> parseCaptureRpcAck(const uint8_t *data,size_t size) noexcept;
}
