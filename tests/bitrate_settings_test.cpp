#include "protocol.hpp"
#include "quality_rpc.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

// Independent protobuf wire reader: verify the bytes actually passed to the
// transport, not the menu value or a reconstructed diagnostic string.
struct Field { unsigned key,wire; uint64_t value; uu::Bytes bytes; };
static void require(bool value) { if(!value)throw std::runtime_error("bitrate wire mismatch"); }
static uint64_t varint(const uu::Bytes &b,size_t &i) {
    uint64_t out=0;
    for(unsigned shift=0;shift<64;shift+=7) {
        require(i<b.size());auto v=b[i++];out|=uint64_t(v&127)<<shift;
        if(!(v&128))return out;
    }
    throw std::runtime_error("invalid wire varint");
}
static std::vector<Field> fields(const uu::Bytes &b) {
    std::vector<Field> out;size_t i=0;
    while(i<b.size()) {
        auto tag=varint(b,i);Field f{unsigned(tag>>3),unsigned(tag&7),0,{}};
        if(f.wire==0)f.value=varint(b,i);
        else if(f.wire==2) {
            auto n=varint(b,i);require(n<=b.size()-i);
            f.bytes.assign(b.begin()+i,b.begin()+i+size_t(n));i+=size_t(n);
        } else throw std::runtime_error("unexpected wire type");
        out.push_back(std::move(f));
    }
    return out;
}
static Field get(const uu::Bytes &b,unsigned key,unsigned wire) {
    Field found{};unsigned count=0;
    for(auto &f:fields(b))if(f.key==key){found=f;++count;}
    require(count==1 && found.wire==wire);return found;
}
int main() {
    try {
        for(int mbps:{5,12,20,30,100})for(bool hevc:{false,true}) {
            const auto bps=uint64_t(mbps)*1000000;
            auto initial=uu::connectOptions(1280,720,60,6,mbps,hevc);
            auto capture=get(initial,3,2).bytes;
            require(get(capture,2,0).value==6);
            require(get(capture,8,0).value==bps);
            for(auto rpc:{uu::qualityUpdateRpc(7,60,6,0,mbps),
                          uu::captureSettingsRpc(8,1280,720,60,6,0,mbps)}) {
                auto request=get(rpc,21,2).bytes;
                auto setting=get(request,2,2).bytes;
                require(get(setting,2,0).value==6);
                require(get(setting,8,0).value==bps);
            }
        }
        // A host sync is separate evidence from an ACK: 20 Mbps -> field 4.
        // Main field 25, body field 4 varint 20,000,000.
        uu::Bytes sync={0xca,0x01,0x05,0x20,0x80,0xda,0xc4,0x09};
        auto state=uu::parseCaptureSettingSync(sync.data(),sync.size());
        require(state && (state->fields&16u) && state->maxCustomBitrate==20000000);
        std::cout<<"PASS: H264/H265 initial and online 5/12/20/30/100 Mbps wire units; host sync\n";
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
