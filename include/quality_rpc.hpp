#pragma once
#include "protocol.hpp"
#include <optional>
namespace uu {
struct CaptureRpcAck {uint64_t requestId=0;unsigned errors=0,entries=0;int errorCode=0;bool capture=false;unsigned otherFields=0;};
struct ScreenSourceInfo {int screenId=-1;unsigned count=0;};
struct TextRpcAck {uint64_t requestId=0;unsigned status=0;};
Bytes clipboardTextRpc(uint64_t requestId,const std::string &text);
std::optional<TextRpcAck> parseTextRpcAck(const uint8_t *data,size_t size) noexcept;
std::optional<ScreenSourceInfo> parseScreenSources(const uint8_t *data,size_t size) noexcept;
Bytes nativeTouchMessage(uint64_t seq,int type,const Json &points);
Bytes simpleActionMessage(uint64_t seq,unsigned action,std::optional<bool> enabled=std::nullopt);
Bytes captureSettingsRpc(uint64_t requestId,int width,int height,int fps,int quality,int screenId,int customMbps=0);
std::optional<CaptureRpcAck> parseCaptureRpcAck(const uint8_t *data,size_t size) noexcept;
}
