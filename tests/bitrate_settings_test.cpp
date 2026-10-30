#include "protocol.hpp"
#include "quality_rpc.hpp"
#include "quality_confirmation.hpp"
#include "quality_presets.hpp"
#include "config_migration.hpp"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
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
static bool hasKey(const uu::Bytes &b,unsigned key) {
    for(auto &f:fields(b))if(f.key==key)return true;
    return false;
}
int main() {
    try {
        for(auto mode:{uu::PrivacyMode::Off,uu::PrivacyMode::Normal,uu::PrivacyMode::ScreenOff}){
            const auto packet=uu::privacyScreenRpc(17,mode);
            const auto rpc=get(packet,21,2).bytes;
            require(get(get(rpc,1,2).bytes,1,0).value==17);
            const auto body=get(rpc,7,2).bytes;
            require(get(body,1,0).value==unsigned(mode!=uu::PrivacyMode::Off));
            require(get(body,2,0).value==unsigned(mode));
        }
        for(unsigned invalid:{0u,4u}){
            bool rejected=false;
            try{uu::privacyScreenRpc(17,static_cast<uu::PrivacyMode>(invalid));}
            catch(const std::invalid_argument &){rejected=true;}
            require(rejected);
        }
        // Main22 -> RpcResponse3. Success code 0 is omitted by the host's
        // protobuf encoder; a header without the privacy body is not success.
        uu::Bytes privacyOk={0xb2,0x01,0x06,0x0a,0x02,0x08,0x11,0x1a,0x00};
        auto privacyAck=uu::parsePrivacyRpcAck(privacyOk.data(),privacyOk.size());
        require(privacyAck && privacyAck->requestId==17 && privacyAck->errorCode==0);
        require(privacyAck->currentMode==uu::PrivacyMode::Unknown);
        // Requested enhanced=3 but actual normal=2 must remain distinguishable.
        uu::Bytes fallback={0xb2,0x01,0x0a,0x0a,0x02,0x08,0x11,0x1a,0x04,0x10,0x03,0x18,0x02};
        privacyAck=uu::parsePrivacyRpcAck(fallback.data(),fallback.size());
        require(privacyAck && privacyAck->requestedMode==uu::PrivacyMode::ScreenOff &&
                privacyAck->currentMode==uu::PrivacyMode::Normal);
        for(unsigned mode:{1u,2u,3u}){
            auto ack=fallback;ack[10]=uint8_t(mode);ack[12]=uint8_t(mode);
            privacyAck=uu::parsePrivacyRpcAck(ack.data(),ack.size());
            require(privacyAck && unsigned(privacyAck->requestedMode)==mode && unsigned(privacyAck->currentMode)==mode);
        }
        auto badMode=fallback;badMode[12]=4;
        require(!uu::parsePrivacyRpcAck(badMode.data(),badMode.size()));
        auto duplicate=fallback;duplicate[2]+=2;duplicate[8]+=2;duplicate.insert(duplicate.end(),{0x18,0x03});
        require(!uu::parsePrivacyRpcAck(duplicate.data(),duplicate.size()));
        auto badWire=fallback;badWire[11]=0x1a;
        require(!uu::parsePrivacyRpcAck(badWire.data(),badWire.size()));
        uu::Bytes privacyReject={0xb2,0x01,0x08,0x0a,0x02,0x08,0x11,0x1a,0x02,0x08,0x05};
        privacyAck=uu::parsePrivacyRpcAck(privacyReject.data(),privacyReject.size());
        require(privacyAck && privacyAck->errorCode==5);
        require(!uu::parsePrivacyRpcAck(privacyReject.data(),privacyReject.size()-1));
        uu::Bytes privacyHeaderOnly={0xb2,0x01,0x04,0x0a,0x02,0x08,0x11};
        require(!uu::parsePrivacyRpcAck(privacyHeaderOnly.data(),privacyHeaderOnly.size()));
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
        // Partial sync without field 4 must remain visibly missing: callers may
        // not infer a confirmed bitrate from fps/quality/size presence.
        uu::Bytes partial={0xca,0x01,0x02,0x08,0x02};
        auto partialState=uu::parseCaptureSettingSync(partial.data(),partial.size());
        require(partialState && (partialState->fields&1u) && !(partialState->fields&16u));
        require(partialState->fps==2 && partialState->maxCustomBitrate==0);
        // Explicit resolution/quality/fps contract in the actual ConnectOptions
        // wire: capture field 4=4 (ChooseType_RESOLUTION), field 10=0 (no
        // automatic frame-quality mode), field 11=fps; decoder field 2 is the
        // codec capability and fields 3/4 are the selected size.
        for(int fps:{30,60}) for(bool hevc:{false,true}) for(auto size:{std::pair<int,int>{960,540},
                                                                    std::pair<int,int>{1280,720},
                                                                    std::pair<int,int>{1920,1080}}) {
            const auto initial=uu::connectOptions(size.first,size.second,fps,3,0,hevc);
            const auto capture=get(initial,3,2).bytes;
            require(get(capture,1,0).value==uint64_t(fps==30?1:2));
            require(get(capture,2,0).value==3);
            require(get(capture,4,0).value==4);
            require(!hasKey(capture,8));
            require(get(capture,10,0).value==0);
            require(get(capture,11,0).value==uint64_t(fps));
            const auto decoder=get(initial,4,2).bytes;
            require(get(decoder,1,0).value==uint64_t(fps));
            require(get(decoder,2,0).value==uint64_t(hevc?2:1));
            require(get(decoder,3,0).value==uint64_t(size.first));
            require(get(decoder,4,0).value==uint64_t(size.second));
        }
        // Native presets must carry quality 1..4 and no Custom field 8 on
        // initial ConnectOptions or online capture/quality requests.
        for(int i=0;i<uu::qualityPresetCount;++i){
            const int quality=uu::qualityPresets[i].quality;
            require(quality==i+1 && uu::qualityPresets[i].name && uu::qualityPresets[i].legacyMbps);
            const auto initial=uu::connectOptions(1280,720,60,quality,0,false);
            const auto initialCapture=get(initial,3,2).bytes;
            require(get(initialCapture,2,0).value==uint64_t(quality));
            require(!hasKey(initialCapture,8));
            for(auto rpc:{uu::qualityUpdateRpc(7,60,quality,0,0),
                          uu::captureSettingsRpc(8,1280,720,60,quality,0,0)}) {
                const auto request=get(rpc,21,2).bytes;
                const auto setting=get(request,2,2).bytes;
                require(get(setting,2,0).value==uint64_t(quality));
                require(!hasKey(setting,8));
            }
            const auto native=uu::videoQualitySettingRpc(9,quality);
            const auto nativeRequest=get(native,21,2).bytes;
            const auto nativeBody=get(nativeRequest,4,2).bytes;
            require(get(nativeBody,1,0).value==uint64_t(quality));
            require(!hasKey(nativeBody,8));
        }
        // A loaded native preset preserves its local REMB cap when only another
        // setting changes; explicit preset selection passes 0. Neither writes a
        // Custom field 8 in preset mode.
        {
            const auto preserved=uu::qualityPresetRequest(2,20);
            require(preserved.quality==3 && preserved.maxBitrateMbps==20);
            const auto explicitChoice=uu::qualityPresetRequest(2,0);
            require(explicitChoice.quality==3 && explicitChoice.maxBitrateMbps==0);
        }
        // v2->v3 migration preserves custom limits even when they match an
        // old preset, because the original user choice cannot be distinguished.
        using Lines=std::vector<std::string>;
        auto lineValue=[](const Lines &lines,const std::string &key)->std::string{
            for(const auto &line:lines){
                const auto equals=line.find('=');
                if(equals!=std::string::npos && line.substr(0,equals)==key)return line.substr(equals+1);
            }
            return {};
        };
        for(int i=0;i<uu::qualityPresetCount;++i){
            Lines lines={"width=1280","quality=6","max_bitrate_mbps="+std::to_string(uu::qualityPresets[i].legacyMbps),"video_codec=hevc"};
            auto migration=uu::migrateQualityConfigLines(lines);
            require(!migration.changed && migration.legacyCustom);
            require(lineValue(lines,"quality")=="6");
            require(lineValue(lines,"max_bitrate_mbps")==std::to_string(uu::qualityPresets[i].legacyMbps));
            require(lineValue(lines,"video_codec")=="hevc");
            auto again=uu::migrateQualityConfigLines(lines);
            require(!again.changed && again.legacyCustom);
        }
        {
            Lines lines={"quality=6","max_bitrate_mbps=25","width=1280"};
            auto migration=uu::migrateQualityConfigLines(lines);
            require(!migration.changed && migration.legacyCustom);
            require(lineValue(lines,"quality")=="6" && lineValue(lines,"max_bitrate_mbps")=="25");
            Lines preset={"quality=1","max_bitrate_mbps=0"};
            auto direct=uu::migrateQualityConfigLines(preset);
            require(!direct.changed && !direct.legacyCustom);
            require(lineValue(preset,"quality")=="1");
        }
        {
            Lines lines={"quality=6 # old UI","max_bitrate_mbps=20 # cap","video_codec=hevc # keep","width=1280"};
            auto migration=uu::migrateQualityConfigLines(lines);
            require(!migration.changed && migration.legacyCustom);
            require(lineValue(lines,"quality")=="6 # old UI");
            require(lineValue(lines,"max_bitrate_mbps")=="20 # cap");
            require(lineValue(lines,"video_codec")=="hevc # keep");
            auto again=uu::migrateQualityConfigLines(lines);
            require(!again.changed && again.legacyCustom);
        }
        {
            Lines lines={"quality=6","quality=3","max_bitrate_mbps=20"};
            auto migration=uu::migrateQualityConfigLines(lines);
            require(!migration.changed && migration.duplicateKeys && migration.unresolved);
            require(lineValue(lines,"quality")=="6");
            require(lineValue(lines,"max_bitrate_mbps")=="20");
        }
        {
            Lines lines={"quality=6","max_bitrate_mbps=0"};
            auto migration=uu::migrateQualityConfigLines(lines);
            require(!migration.changed && migration.legacyCustom && migration.invalidCustom);
            require(lineValue(lines,"quality")=="6" && lineValue(lines,"max_bitrate_mbps")=="0");
        }
        // Temp file fixture: migrate, persist, reload, and verify idempotence
        // and unrelated/mixed-case content preservation.
        {
            const std::string path="quality-migration-fixture.ini";
            {
                std::ofstream out(path,std::ios::trunc);
                out << "config_version=2\nwidth=1280\nquality=6 # old UI\n"
                       "max_bitrate_mbps=20 # cap\nvideo_codec=hevc\n";
            }
            auto readLines=[&]{
                Lines result;std::ifstream in(path);std::string line;
                while(std::getline(in,line))result.push_back(line);
                return result;
            };
            auto lines=readLines();
            auto migration=uu::migrateQualityConfigLines(lines);
            require(!migration.changed && migration.legacyCustom);
            {
                std::ofstream out(path,std::ios::trunc);
                for(const auto &line:lines)out<<line<<'\n';
            }
            auto reloaded=readLines();
            require(lineValue(reloaded,"quality")=="6 # old UI");
            require(lineValue(reloaded,"max_bitrate_mbps")=="20 # cap");
            require(lineValue(reloaded,"width")=="1280");
            auto again=uu::migrateQualityConfigLines(reloaded);
            require(!again.changed && again.legacyCustom);
            std::remove(path.c_str());
        }
        // The same production tracker used by Client: ACK must not erase
        // evidence, and split syncs accumulate into one match.
        const uu::QualityApplicationTarget custom{1280,720,60,6,20};
        uu::QualityApplyTracker tracker;
        tracker.acknowledge();
        tracker.armConfirmation(1);
        tracker.evidence.observeFps(60);
        tracker.evidence.observeQuality(6);
        require(!tracker.evaluate(custom) && tracker.pending);
        tracker.evidence.observeWidth(1280);
        tracker.evidence.observeHeight(720);
        require(!tracker.evaluate(custom) && tracker.pending); // bitrate still absent
        tracker.evidence.observeBitrate(20);
        require(tracker.evaluate(custom) && !tracker.pending);

        tracker.beginRequest();
        tracker.evidence.observeFps(60);
        tracker.evidence.observeQuality(6);
        tracker.evidence.observeSize(1280,720);
        tracker.evidence.observeBitrate(20);
        require(tracker.evaluate(custom));
        tracker.acknowledge(); // late ACK: preserve the already-matched evidence
        require(tracker.evaluate(custom));

        tracker.beginRequest();
        tracker.acknowledge();
        tracker.armConfirmation(2);
        require(!tracker.evaluate(custom)); // ACK-only cannot confirm
        tracker.timeout();
        require(!tracker.applied && !tracker.pending);

        tracker.beginRequest();
        tracker.evidence.observeFps(60);
        tracker.evidence.observeQuality(6);
        tracker.evidence.observeSize(1280,720);
        require(!tracker.evaluate(custom)); // missing bitrate stays unknown
        tracker.evidence.observeBitrate(12);
        require(!tracker.evaluate(custom)); // different value stays unconfirmed
        tracker.evidence.observeBitrate(20);
        require(tracker.evaluate(custom));

        tracker.beginRequest();
        require(!tracker.evidence.anyKnown() && !tracker.accepted && !tracker.applied && !tracker.pending);

        // Preset mode sends no Custom bitrate even if a local cap is present.
        const uu::QualityApplicationTarget preset{1280,720,60,3,20};
        tracker.beginRequest();
        tracker.evidence.observeFps(60);
        tracker.evidence.observeQuality(3);
        tracker.evidence.observeSize(1280,720);
        require(tracker.evaluate(preset));
        std::cout<<"PASS: native preset wire/cap-preserve; legacy migration/comments/duplicates/fixture; Custom wire; sync; tracker\n";
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
