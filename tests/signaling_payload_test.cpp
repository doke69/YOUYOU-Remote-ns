#include "signaling_envelope.hpp"
#include <zlib.h>
#include <iostream>
#include <stdexcept>

void require(bool ok,const char *label){if(!ok)throw std::runtime_error(label);}
template<class F> void rejects(F fn,const char *label){
    bool rejected=false;
    try{fn();}catch(const std::exception &){rejected=true;}
    require(rejected,label);
}
uu::Bytes gzip(const std::string &text) {
    z_stream stream{};
    require(deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,16+MAX_WBITS,8,Z_DEFAULT_STRATEGY)==Z_OK,"gzip init");
    struct End {z_stream &stream;~End(){deflateEnd(&stream);}} end{stream};
    uu::Bytes output(deflateBound(&stream,static_cast<uLong>(text.size())));
    stream.next_in=reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));stream.avail_in=static_cast<uInt>(text.size());
    stream.next_out=output.data();stream.avail_out=static_cast<uInt>(output.size());
    require(deflate(&stream,Z_FINISH)==Z_STREAM_END,"gzip fixture");
    output.resize(stream.total_out);return output;
}
std::string normalized(uu::Json packet) {
    uu::SdpEncoding encoding;
    require(uu::inspectSoac(packet,"session",&encoding)==uu::SoacStatus::Accepted,"envelope accepted");
    const auto &payload=packet.at("data");
    return uu::decodeSoacSdp(payload.at(encoding==uu::SdpEncoding::Plain?"sdp":"gzip_sdp"),encoding);
}
int main() {
    const std::string sdp="v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=synthetic\r\nt=0 0\r\n";
    const auto compressed=gzip(sdp);
    const uu::Json plain={{"data",{{"ice_id","session"},{"type","answer"},{"sdp",sdp}}}};
    require(normalized(plain)==sdp,"plain answer retained");
    uu::Json zipped={{"data",{{"ice_id","session"},{"type","answer"},
        {"gzip_sdp",uu::base64(compressed.data(),compressed.size())}}}};
    require(normalized(zipped)==sdp,"base64 gzip answer");
    zipped["data"]["gzip_sdp"]=uu::Json::binary(compressed);
    require(normalized(zipped)==sdp,"binary gzip answer");
    auto prefixed=compressed;prefixed.insert(prefixed.begin(),4);
    zipped["data"]["gzip_sdp"]=uu::Json::binary(prefixed);
    require(normalized(zipped)==sdp,"legacy EIO binary message prefix");
    for(const char *wire:{
        "451-[\"soac\",{\"data\":{\"ice_id\":\"session\",\"type\":\"answer\",\"gzip_sdp\":{\"_placeholder\":true,\"num\":0}}}]",
        "461-12[\"success\",{\"data\":{\"ice_id\":\"session\",\"type\":\"answer\",\"gzip_sdp\":{\"_placeholder\":true,\"num\":0}}}]"}) {
        auto packet=uu::parseSocketIo(wire);
        require(packet.attachments==1,"attachment count");
        uu::restoreSocketIoAttachments(packet.data,{compressed});
        require(normalized(packet.data[1])==sdp,"full Socket.IO packet to normalized SDP");
        if(packet.type==6)require(packet.id==12,"binary ACK id retained");
    }
    auto foreign=plain;foreign["data"]["ice_id"]="foreign";
    require(uu::inspectSoac(foreign,"session")==uu::SoacStatus::SessionMismatch,"foreign session remains isolated");
    auto malformed=uu::Json{{"_placeholder",true},{"num",-1}};
    rejects([&]{uu::restoreSocketIoAttachments(malformed,{compressed});},"negative attachment index");
    malformed={{"_placeholder",true},{"num",1}};
    rejects([&]{uu::restoreSocketIoAttachments(malformed,{compressed});},"missing attachment");
    malformed={{"_placeholder",true}};
    rejects([&]{uu::restoreSocketIoAttachments(malformed,{compressed});},"missing index");
    auto many=uu::Json::array();for(int i=0;i<3;++i)many.push_back({{"_placeholder",true},{"num",0}});
    rejects([&]{uu::restoreSocketIoAttachments(many,{uu::Bytes(uu::maxSignalingSdpBytes,1)});},"repeated reference expansion bound");
    zipped["data"]["gzip_sdp"]="invalid!";
    rejects([&]{normalized(zipped);},"bad base64");
    auto corrupt=compressed;corrupt[corrupt.size()-8]^=1;
    zipped["data"]["gzip_sdp"]=uu::Json::binary(corrupt);
    rejects([&]{normalized(zipped);},"bad gzip CRC");
    corrupt=compressed;corrupt.resize(corrupt.size()-4);
    zipped["data"]["gzip_sdp"]=uu::Json::binary(corrupt);
    rejects([&]{normalized(zipped);},"truncated gzip");
    zipped["data"]["gzip_sdp"]=uu::Json::binary(gzip(std::string(uu::maxSignalingSdpBytes+1,'x')));
    rejects([&]{normalized(zipped);},"gzip expansion bound");
    auto large=plain;large["data"]["sdp"]=std::string(uu::maxSignalingSdpBytes+1,'x');
    rejects([&]{normalized(large);},"plain SDP bound");
    const auto request=uu::controlRequest("control-session",1280,720,60,3);
    const auto streamer=uu::Json::parse(request.at("streamer_data").get<std::string>());
    require(request.at("app_control_id")==streamer.at("control_id"),"control id matches inside and outside");
    require(!request.contains("ice_id") && streamer.at("device_capability").at("ice_id")=="","server owns ICE identity");
    require(streamer.at("device_capability").at("display_info")[0].at("width")==1280,"resolution retained");
    require(streamer.at("device_capability").at("video_codec_capability")==streamer.at("frame_quality_capability"),"legacy and wrapped codec capabilities agree");
    require(streamer.at("device_capability").at("display_info")==streamer.at("local_display_info"),"legacy and wrapped displays agree");
    for(const auto &size:std::array<std::array<int,2>,3>{{{960,540},{1280,720},{1920,1080}}})
    for(int quality:{1,6})for(int fps:{30,60}) {
        const auto req=uu::controlRequest("test-session",size[0],size[1],fps,quality);
        const auto cap=uu::Json::parse(req.at("streamer_data").get<std::string>());
        const auto &entries=cap.at("frame_quality_capability");
        require(cap.at("device_capability").at("video_codec_capability")==entries,"all capability paths agree");
        require(cap.at("remote_display_info")==cap.at("local_display_info"),"display paths agree");
        require(cap.at("local_display_info")[0].at("width")==size[0] && cap.at("local_display_info")[0].at("height")==size[1],"selected display dimensions");
#ifdef UU_SESSION_SIZE_CAP
        require(entries.size()==2,"one entry per codec for selected session");
        for(const auto &entry:entries)require(entry.at("max_width")==size[0] && entry.at("max_height")==size[1] && entry.at("width")==size[0] && entry.at("height")==size[1],"no larger session capability");
#else
        require(entries.size()==4,"legacy hardware capability list retained");
#endif
        bool h264=false,h265=false;
        for(const auto &entry:entries) {
            h264|=entry.at("video_codec")==1;h265|=entry.at("video_codec")==2;
            require(entry.at("max_frame_quality")==quality && entry.at("bit_depth")==8 && entry.at("chroma_sampling")==1 && entry.at("result")==0,"quality and format retained");
        }
        require(h264 && h265,"codec alternatives retained");
        require(cap.at("local_display_info")[0].at("fps")==fps,"requested frame rate retained");
    }
    std::cout<<"PASS: plain/compressed/binary SDP, attachment references, corruption/size limits, control-session schema\n";
}
