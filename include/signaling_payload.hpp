#pragma once
#include "protocol.hpp"

namespace uu {
enum class SdpEncoding { None, Plain, GzipBase64, GzipBinary };
constexpr size_t maxSignalingAttachmentBytes=2*1024*1024;
constexpr size_t maxSignalingSdpBytes=1024*1024;
// Reconstruct a complete Socket.IO packet before interpreting its protocol.
void restoreSocketIoAttachments(Json &value,const std::vector<Bytes> &attachments);
// Encoding/type has been validated by inspectSoac; this boundary checks size,
// base64/gzip integrity and produces the plain SDP accepted by the RTC SDK.
std::string decodeSoacSdp(const Json &value,SdpEncoding encoding);
Json controlRequest(const std::string &controlId,int width,int height,int fps,int quality);
} // namespace uu
