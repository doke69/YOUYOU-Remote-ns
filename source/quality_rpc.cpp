#include "quality_rpc.hpp"
#include <limits>
#include <cstring>
#include <cmath>
#include <set>
#include <stdexcept>
namespace uu {
namespace {
void var(Bytes &b,uint64_t value) {while(value>127){b.push_back(uint8_t(value)|128);value>>=7;}b.push_back(uint8_t(value));}
void integer(Bytes &b,unsigned key,uint64_t value){var(b,key*8);var(b,value);}
void message(Bytes &b,unsigned key,const Bytes &value){var(b,key*8+2);var(b,value.size());b.insert(b.end(),value.begin(),value.end());}
struct Field {unsigned key=0,wire=0;uint64_t value=0;const uint8_t *data=nullptr;size_t size=0;};
class Reader {
    const uint8_t *p,*end;unsigned fields=0;bool valid=true;
    bool varint(uint64_t &value){value=0;for(unsigned i=0;i<10;++i){if(p==end)return valid=false;const auto byte=*p++;if(i==9 && byte>1)return valid=false;value|=uint64_t(byte&127)<<(i*7);if(!(byte&128))return true;}return valid=false;}
  public:
    Reader(const uint8_t *data,size_t size):p(data),end(data+size){}
    bool next(Field &f){
        if(!valid || p==end)return false;
        if(++fields>128)return valid=false;
        uint64_t tag=0;if(!varint(tag) || !(tag>>3) || (tag>>3)>0x1fffffff)return valid=false;
        f={};f.key=unsigned(tag>>3);f.wire=unsigned(tag&7);
        if(f.wire==0)return varint(f.value);
        uint64_t length=0;
        if(f.wire==2){if(!varint(length))return false;}else if(f.wire==1)length=8;else if(f.wire==5)length=4;else return valid=false;
        if(length>uint64_t(end-p))return valid=false;
        f.data=p;f.size=size_t(length);p+=length;return true;
    }
    bool good()const{return valid && p==end;}
};
}
// APK Main.Message field 3 / General.SimpleAction field 1; enum values
// recovered from cg.d1. These are one-way commands, not acknowledged RPCs.
// Verified mobile Wire adapters K3/C3/X6/b7: Main 20 -> Input 1 -> Touch.
Bytes nativeTouchMessage(uint64_t seq,int type,const Json &points){
    if(!seq || type<1 || type>4 || !points.is_array() || points.size()>10 || ((type<=2)==points.empty()))
        throw std::invalid_argument("Invalid native touch snapshot");
    Bytes touch,input,out;integer(touch,1,type);std::set<int> ids;
    auto real=[&](Bytes &dst,unsigned tag,float value){uint32_t bits;std::memcpy(&bits,&value,4);var(dst,tag*8+5);for(int i=0;i<4;++i)dst.push_back(uint8_t(bits>>(i*8)));};
    for(const auto &p:points){
        const int id=p.at("id").get<int>();const float x=p.at("x").get<float>(),y=p.at("y").get<float>();
        if(id<0 || id>9 || !ids.insert(id).second || !std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1)
            throw std::invalid_argument("Invalid native touch point");
        Bytes point;integer(point,1,id);real(point,2,x);real(point,3,y);real(point,4,2.f);real(point,5,2.f);
        message(touch,2,point);
    }
    message(input,1,touch);integer(out,1,seq);integer(out,2,nowMs());message(out,20,input);return out;
}
Bytes simpleActionMessage(uint64_t seq,unsigned action,std::optional<bool> enabled) {
    if(!seq || !(action==2 || action==9 || action==10 || action==12 || action==13 ||
        action==14 || action==16 || action==17 || action==18 || action==19) || (action==19)!=enabled.has_value())
        throw std::invalid_argument("Invalid system action");
    Bytes body,out;integer(body,1,action);
    // Mobile AutoLockScreen.h: true takes const 2, false branches to const 1.
    // Confirmed independently by the Linux host's SET_AUTO_LOCK decoder.
    if(enabled)integer(body,5,*enabled?2:1);
    integer(out,1,seq);integer(out,2,nowMs());message(out,3,body);return out;
}
Bytes clipboardTextRpc(uint64_t id,const std::string &text){
    if(!id || text.empty() || text.size()>2048 || text.find('\0')!=std::string::npos)
        throw std::invalid_argument("Invalid text input size");
    // Validate UTF-8 before encoding a protobuf string; do not log its contents.
    for(size_t i=0;i<text.size();){
        const unsigned c=uint8_t(text[i++]);if(c<0x80)continue;
        unsigned n=0,cp=0,min=0;
        if(c>=0xc2 && c<=0xdf){n=1;cp=c&31;min=0x80;}
        else if(c>=0xe0 && c<=0xef){n=2;cp=c&15;min=0x800;}
        else if(c>=0xf0 && c<=0xf4){n=3;cp=c&7;min=0x10000;}
        else throw std::invalid_argument("Invalid text encoding");
        if(i+n>text.size())throw std::invalid_argument("Truncated text encoding");
        while(n--){const unsigned b=uint8_t(text[i++]);if((b&0xc0)!=0x80)throw std::invalid_argument("Invalid text encoding");cp=(cp<<6)|(b&63);}
        if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))throw std::invalid_argument("Invalid text codepoint");
    }
    // Mobile ClipboardTextChangeReq: Main21 -> RpcRequest10 -> {type1=1,text2}.
    Bytes change,header,rpc,out;integer(change,1,1);
    message(change,2,Bytes(text.begin(),text.end()));integer(header,1,id);
    message(rpc,1,header);message(rpc,10,change);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}
std::optional<TextRpcAck> parseTextRpcAck(const uint8_t *data,size_t size)noexcept{
    if(!data || !size || size>65536)return {};
    TextRpcAck out;bool response=false,header=false,id=false,body=false,status=false;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==22){
        if(response || f.wire!=2)return {};response=true;
        Reader rpc(f.data,f.size);Field r;
        while(rpc.next(r)){
            if(r.key==1){
                if(header || r.wire!=2)return {};header=true;
                Reader h(r.data,r.size);Field v;
                while(h.next(v))if(v.key==1){if(id || v.wire!=0 || !v.value)return {};id=true;out.requestId=v.value;}
                if(!h.good())return {};
            }else if(r.key==6){
                if(body || r.wire!=2)return {};body=true;
                Reader b(r.data,r.size);Field v;
                while(b.next(v))if(v.key==1){if(status || v.wire!=0 || v.value>2)return {};status=true;out.status=unsigned(v.value);}
                if(!b.good())return {};
            }
        }
        if(!rpc.good())return {};
    }
    // CB_UNSPECIFIED=0 is never treated as success. CB_SUCCEEDED=1, FAILED=2.
    if(!outer.good() || !response || !header || !id || !body)return {};
    return out;
}
Bytes fpsSettingRpc(uint64_t id,int fps){
    if(!id || (fps!=30&&fps!=60))throw std::invalid_argument("Invalid fps setting");
    Bytes body,header,rpc,out;
    integer(body,1,fps==30?1:2); // gv_pb.FPS: FPS_30=1, FPS_60=2
    integer(header,1,id);message(rpc,1,header);message(rpc,3,body);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}
Bytes videoQualitySettingRpc(uint64_t id,int quality){
    if(!id || quality<1 || quality>6)throw std::invalid_argument("Invalid quality setting");
    Bytes body,header,rpc,out;
    integer(body,1,quality); // gv_pb.VideoQuality
    integer(header,1,id);message(rpc,1,header);message(rpc,4,body);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}
Bytes captureSettingsRpc(uint64_t id,int w,int h,int fps,int quality,int screenId,int customMbps){
    if(!id || screenId<0 || !((w==960&&h==540)||(w==1280&&h==720)||(w==1920&&h==1080)) ||
       (fps!=30&&fps!=60) || quality<1 || quality>6 || customMbps<0 || customMbps>100 ||
       (quality==6 && customMbps==0) || (quality!=6 && customMbps!=0))throw std::invalid_argument("Invalid capture settings");
    Bytes capture,header,rpc,out;
    integer(capture,1,fps==30?1:2);integer(capture,2,quality);integer(capture,3,1);
    // Preserve the 0.3.25 capture pacing fields; these keep the host frame
    // cadence regular and avoid batching in the decoder queue.
    integer(capture,12,0);
    integer(capture,18,uint64_t(fps));
    // Connect auto-selection (-1) is not a valid online target. Use the
    // current ID advertised in Main.screens and validated against its list.
    integer(capture,4,unsigned(screenId));integer(capture,5,w);integer(capture,6,h);
    // TV wire field 8; mobile custom-state -> capture uses bits/second.
    // Keep chroma settings unchanged and do not treat this request as a
    // guarantee that a static scene will generate the requested rate.
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes qualityUpdateRpc(uint64_t id,int fps,int quality,int screenId,int customMbps){
    if(!id || screenId<0 || (fps!=30&&fps!=60) || quality<1 || quality>6 ||
       customMbps<0 || customMbps>100 || (quality==6&&customMbps==0) ||
       (quality!=6&&customMbps!=0))throw std::invalid_argument("Invalid quality update");
    Bytes capture,header,rpc,out;
    integer(capture,1,fps==30?1:2);integer(capture,2,quality);integer(capture,3,1);
    integer(capture,12,0);
    integer(capture,18,uint64_t(fps));
    integer(capture,4,unsigned(screenId));
    // No resolution/max-scale/pixel fields: this request must only update the
    // encoder controls and leave the host capture resolution unchanged.
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes videoScaleRpc(uint64_t id,int sourceWidth,int sourceHeight,
                    int outputWidth,int outputHeight,int fps,int quality,
                    int screenId,int customMbps){
    const bool outputSupported=(outputWidth==960&&outputHeight==540) ||
        (outputWidth==1280&&outputHeight==720) || (outputWidth==1920&&outputHeight==1080);
    if(!id || sourceWidth<1 || sourceHeight<1 || sourceWidth>16384 || sourceHeight>16384 ||
       !outputSupported || screenId<0 || (fps!=30&&fps!=60) || quality<1 || quality>6 ||
       customMbps<0 || customMbps>100 || (quality==6&&customMbps==0) ||
       (quality!=6&&customMbps!=0))
        throw std::invalid_argument("Invalid video scale request");
    Bytes capture,header,rpc,out;
    integer(capture,1,fps==30?1:2);integer(capture,2,quality);integer(capture,3,1);
    integer(capture,12,0);
    integer(capture,18,uint64_t(fps));
    integer(capture,4,unsigned(screenId));
    // Source/desktop dimensions are intentionally the observed media size, not
    // the selected stream size. The pixel pair is the mobile-style encoded
    // output bound; max_scale is kept as the same bound for hosts that use it.
    integer(capture,5,unsigned(sourceWidth));integer(capture,6,unsigned(sourceHeight));
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    integer(capture,10,3);
    integer(capture,14,unsigned(outputWidth));integer(capture,15,unsigned(outputHeight));
    integer(capture,16,unsigned(outputWidth));integer(capture,17,unsigned(outputHeight));
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

namespace {
std::string wireSummary(const uint8_t *data,size_t size,unsigned depth) {
    Reader reader(data,size);Field field;std::string out;unsigned seen=0;
    while(reader.next(field) && ++seen<=128 && out.size()<600) {
        out+=std::to_string(field.key);
        if(field.wire==0) out+="="+std::to_string(field.value);
        else if(field.wire==2) {
            out+="["+std::to_string(field.size)+"]";
            if(depth && field.size && field.size<=8192) {
                const auto nested=wireSummary(field.data,field.size,depth-1);
                if(!nested.empty()) out+="{"+nested+"}";
            }
        } else out+="?";
        out+=",";
    }
    return out;
}
}
std::string screenSourceWireSummary(const uint8_t *data,size_t size) noexcept {
    if(!data || !size || size>65536) return {};
    try { return wireSummary(data,size,2); } catch(...) { return {}; }
}
std::optional<ScreenSourceInfo> parseScreenSources(const uint8_t *data,size_t size)noexcept{    if(!data || !size || size>65536)return {};
    bool found=false;ScreenSourceInfo result;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==7){
        if(found || f.wire!=2)return {};found=true;
        std::array<int,32> ids{};unsigned count=0;
        int current=0;bool currentPresent=false;
        Reader sources(f.data,f.size);Field source;
        while(sources.next(source)){
            if(source.key==1){
                if(source.wire!=2 || count==ids.size())return {};
                int id=0;bool idPresent=false;Reader screen(source.data,source.size);Field prop;
                while(screen.next(prop))if(prop.key==1){
                    if(idPresent || prop.wire!=0 || prop.value>uint64_t(INT32_MAX))return {};
                    idPresent=true;id=int(prop.value);
                }
                if(!screen.good())return {};
                for(unsigned i=0;i<count;++i)if(ids[i]==id)return {};
                ids[count++]=id;
            }else if(source.key==2){
                if(currentPresent || source.wire!=0)return {};
                currentPresent=true;
                current=source.value<=uint64_t(INT32_MAX)?int(source.value):-1;
            }
        }
        if(!sources.good())return {};
        result.count=count;
        // Protobuf's omitted int32 ID is zero; it is accepted only when the
        // complete advertised list contains that same ID. Never guess a screen.
        for(unsigned i=0;i<count;++i)if(current>=0 && ids[i]==current)result.screenId=current;
    }
    if(!outer.good() || !found)return {};
    return result;
}

std::optional<CaptureSettingSyncInfo> parseCaptureSettingSync(const uint8_t *data,size_t size)noexcept{
    if(!data || !size || size>65536)return {};
    bool found=false;CaptureSettingSyncInfo result;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==25){
        if(found || f.wire!=2)return {};found=true;
        Reader body(f.data,f.size);Field v;
        while(body.next(v)){
            if(v.wire!=0)continue;
            switch(v.key){
                case 1: if(v.value>2)return {}; result.fps=int(v.value);result.fields|=1u;break;
                case 2: if(v.value>6)return {}; result.quality=int(v.value);result.fields|=2u;break;
                case 3: if(v.value>4)return {}; result.chroma=int(v.value);break;
                case 4: if(v.value>uint64_t(std::numeric_limits<int64_t>::max()))return {}; result.maxCustomBitrate=int64_t(v.value);result.fields|=16u;break;
                case 5: if(v.value>16384)return {}; result.width=int(v.value);result.fields|=4u;break;
                case 6: if(v.value>16384)return {}; result.height=int(v.value);result.fields|=8u;break;
                case 7: if(v.value>4)return {}; result.resolutionType=int(v.value);break;
                case 8: result.dpiScale=int(v.value);break;
                case 9: if(v.value>16384)return {}; result.pixelWidth=int(v.value);break;
                case 10: if(v.value>16384)return {}; result.pixelHeight=int(v.value);break;
                case 11: result.orientation=int(v.value);break;
                default: break;
            }
        }
        if(!body.good())return {};
    }
    if(!outer.good() || !found)return {};
    if(!result.fields)return {};
    return result;
}

std::optional<CaptureRpcAck> parseCaptureRpcAck(const uint8_t *data,size_t size)noexcept{
    if(!data || !size || size>65536)return {};
    CaptureRpcAck result;bool response=false,header=false,id=false;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==22){
        if(response || f.wire!=2)return {};response=true;
        Reader rpc(f.data,f.size);Field r;
        while(rpc.next(r)){
            if(r.key==1){
                if(header || r.wire!=2)return {};header=true;
                Reader head(r.data,r.size);Field h;
                while(head.next(h)){if(h.key==1){if(id || h.wire!=0)return {};id=true;result.requestId=h.value;}else ++result.otherFields;}
                if(!head.good())return {};
            }else if(r.key==2){
                if(result.capture || r.wire!=2)return {};result.capture=true;
                Reader capture(r.data,r.size);Field e;
                while(capture.next(e))if(e.key==1){
                    if(e.wire!=2 || ++result.entries>64)return {};
                    Reader error(e.data,e.size);Field v;bool code=false;int entryCode=0;
                    while(error.next(v))if(v.key==1){
                        if(code || v.wire!=0)return {};code=true;
                        entryCode=int(int32_t(uint32_t(v.value)));
                    }
                    if(!error.good())return {};
                    // The repeated Error container also carries code-zero
                    // result records. Check every record, not container size
                    // or only the first record. Proto3's omitted int32 is zero.
                    if(entryCode!=0){
                        if(!result.errors)result.errorCode=entryCode;
                        ++result.errors;
                    }
                }
                if(!capture.good())return {};
            }else ++result.otherFields;
        }
        if(!rpc.good())return {};
    }
    if(!outer.good() || !response || !header || !id || !result.requestId)return {};
    return result;
}
}
