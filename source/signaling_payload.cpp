#include "signaling_payload.hpp"
#include <mbedtls/base64.h>
#include <zlib.h>
#include <stdexcept>

namespace uu {
void restoreSocketIoAttachments(Json &value,const std::vector<Bytes> &attachments) {
    size_t reconstructed=0;
    auto restore=[&](auto &&self,Json &item,unsigned depth)->void {
        if(depth>32)throw std::runtime_error("Signaling attachment nesting limit");
        if(item.is_object()) {
            const auto marker=item.find("_placeholder");
            if(marker!=item.end() && marker->is_boolean() && marker->get<bool>()) {
                const auto number=item.find("num");
                if(number==item.end() || !number->is_number_integer() || *number<0 ||
                   *number>=attachments.size())
                    throw std::runtime_error("Invalid signaling attachment reference");
                const auto &bytes=attachments[number->get<size_t>()];
                if(bytes.size()>maxSignalingAttachmentBytes-reconstructed)
                    throw std::runtime_error("Signaling attachment expansion limit");
                reconstructed+=bytes.size();
                item=Json::binary(bytes);
            } else for(auto &child:item)self(self,child,depth+1);
        } else if(item.is_array())for(auto &child:item)self(self,child,depth+1);
    };
    restore(restore,value,0);
}
std::string decodeSoacSdp(const Json &value,SdpEncoding encoding) {
    if(encoding==SdpEncoding::Plain) {
        const auto &text=value.get_ref<const std::string &>();
        if(text.size()>maxSignalingSdpBytes)throw std::runtime_error("Signaling SDP size limit");
        return text;
    }
    Bytes decoded;
    const uint8_t *bytes=nullptr;
    size_t size=0;
    if(encoding==SdpEncoding::GzipBase64) {
        const auto &text=value.get_ref<const std::string &>();
        if(text.size()>maxSignalingAttachmentBytes)throw std::runtime_error("Compressed SDP size limit");
        decoded.resize(text.size()/4*3+3);
        size_t written=0;
        if(mbedtls_base64_decode(decoded.data(),decoded.size(),&written,
                reinterpret_cast<const unsigned char *>(text.data()),text.size())!=0)
            throw std::runtime_error("Invalid compressed SDP base64");
        decoded.resize(written);bytes=decoded.data();size=decoded.size();
    } else if(encoding==SdpEncoding::GzipBinary) {
        const auto &binary=value.get_binary();bytes=binary.data();size=binary.size();
    } else throw std::logic_error("Missing validated SDP encoding");
    if(size>maxSignalingAttachmentBytes)throw std::runtime_error("Compressed SDP size limit");
    // Some older Engine.IO senders include the message-type byte in binary data.
    if(size>=3 && bytes[0]==4 && bytes[1]==0x1f && bytes[2]==0x8b){++bytes;--size;}
    if(size<2 || bytes[0]!=0x1f || bytes[1]!=0x8b)
        throw std::runtime_error("Invalid compressed SDP gzip header");
    z_stream stream{};
    if(inflateInit2(&stream,16+MAX_WBITS)!=Z_OK)
        throw std::runtime_error("Compressed SDP inflater unavailable");
    struct End {z_stream &stream;~End(){inflateEnd(&stream);}} end{stream};
    std::string output(maxSignalingSdpBytes+1,'\0');
    stream.next_in=const_cast<Bytef *>(bytes);stream.avail_in=static_cast<uInt>(size);
    stream.next_out=reinterpret_cast<Bytef *>(output.data());stream.avail_out=static_cast<uInt>(output.size());
    const auto result=inflate(&stream,Z_FINISH);
    if(result!=Z_STREAM_END || stream.avail_in || !stream.total_out || stream.total_out>maxSignalingSdpBytes)
        throw std::runtime_error("Invalid or oversized compressed SDP");
    output.resize(stream.total_out);
    return output;
}
Json controlRequest(const std::string &controlId,int width,int height,int fps,int quality) {
    const Json displays=Json::array({{{"id",0},{"width",width},{"height",height},{"fps",fps},
                                      {"hdr",-2},{"type",0},{"dpi_scale",100}}});
    Json codecs=Json::array();
#ifdef UU_SESSION_SIZE_CAP
    // This is the session's acceptable decode workload, not the device's
    // maximum hardware capability. Keep both host codec alternatives.
    const std::array<std::array<int,2>,1> sizes{{{width,height}}};
#else
    const std::array<std::array<int,2>,2> sizes{{{1280,720},{1920,1080}}};
#endif
    for(const auto &size:sizes)
        for(int codec:{1,2})codecs.push_back({{"video_codec",codec},{"codec_impl",1},{"bit_depth",8},
            {"chroma_sampling",1},{"max_width",size[0]},{"max_height",size[1]},
            {"width",size[0]},{"height",size[1]},{"max_frame_quality",quality},{"result",0}});
    // Retain the existing flat capability fields for older hosts while adding
    // the session wrapper and field names used by the native capability model.
    Json streamer={{"control_id",controlId},
        {"device_capability",{{"display_info",displays},{"video_codec_capability",codecs},{"ice_id",""}}},
        {"remote_display_info",displays},{"local_display_info",displays},{"frame_quality_capability",codecs}};
    return {{"app_control_id",controlId},{"app_data",{{"_placeholder",true},{"num",0}}},
            {"streamer_data",streamer.dump()}};
}
} // namespace uu
