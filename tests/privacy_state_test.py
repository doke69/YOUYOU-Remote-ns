"""Compile production privacy branches with mocked transport and clock.

Usage: python tests/privacy_state_test.py COMPILER OUTPUT_DIRECTORY
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[2]).absolute()
out.mkdir(parents=True, exist_ok=True)
client = (root / 'source/client.cpp').read_text(encoding='utf-8')
header = (root / 'include/client.hpp').read_text(encoding='utf-8')
ui = (root / 'source/live_frontend.inc').read_text(encoding='utf-8')
request = client.split('} else if(e=="privacy-screen") {', 1)[1].split('} else if(e=="privacy-screen-ack")', 1)[0]
ack = client.split('} else if(e=="privacy-screen-ack") {', 1)[1].split('} else if(e=="privacy-screen-state")', 1)[0]
notify = client.split('} else if(e=="privacy-screen-state") {', 1)[1].split('} else if(e=="text-channel-state")', 1)[0]
cursor_request = client.split('} else if(e=="cursor-capture") {', 1)[1].split('} else if(e=="privacy-screen")', 1)[0]
cursor_ack = client.split('} else if(e=="quality-rpc-ack") {', 1)[1].split('        if(routeBitrateRequest_', 1)[0]
auto_cursor = client[client.index('            // Re-enable host cursor capture'):client.index('            if(pendingPrivacy_ &&')]
timeout = client[client.index('            if(pendingPrivacy_ &&'):client.index('            if(pendingText_ && monotonicMs()')]
disconnect = client.split('void Client::disconnect() {', 1)[1]
disconnect = disconnect[disconnect.index('    pendingPrivacy_.reset();'):disconnect.index('    connectedSince_=0;')]
pending = header[header.index('    struct PendingPrivacy'):header.index('    bool rtxEnabled_')]
action = ui[ui.index('        case 755:'):ui.index('        case 759:')]
source = r'''
#include "protocol.hpp"
#include "client_snapshot.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
using namespace uu;
static int64_t clockMs=1;
namespace uu {int64_t monotonicMs(){return clockMs;}}
void diagnostic(const char*){}
// Transport boundary only. Packet serialization has separate protocol tests.
namespace uu {Bytes privacyScreenRpc(uint64_t,PrivacyMode){return {};}}
namespace uu {Bytes cursorCaptureRpc(uint64_t){return {};}}
struct Channel {
    bool fail=false;
    bool open=true;
    size_t buffered=0;
    unsigned sends=0;
    bool isOpen()const{return open;}
    size_t bufferedAmount()const{return buffered;}
    void send(const std::byte*,size_t){if(fail)throw std::runtime_error("send failed");++sends;}
};
struct Harness {
    Snapshot state_;
    std::mutex stateMutex_;
    std::string connectedDeviceId_="pc";
    uint64_t nextQualityRpc_=1;
    int qualityAckFallthrough=0;
    bool firstVideoFramePublishedLogged_=false,cancel_=false;
    uint64_t generation_=1;
    struct Event {std::string type;Json data;uint64_t generation;};
    bool isStreaming()const{return state_.streaming;}
    void handle(const Event&e){assert(e.type=="cursor-capture" && e.generation==generation_);cursorRequest(e.data);}
    std::map<std::string,std::shared_ptr<Channel>> channels_{{"TEXT_DATA_CHANNEL",std::make_shared<Channel>()}};
    Snapshot snapshot()const{return state_;}
'''+pending+'\nvoid autoTick(){'+auto_cursor+'}\nvoid cursorRequest(const Json &data){'+cursor_request+'}\nvoid cursorAck(const Json &data){'+cursor_ack+'++qualityAckFallthrough;}\nvoid request(const Json &data){'+request+'}\nvoid ack(const Json &data){'+ack+'}\nvoid notify(const Json &data){'+notify+'}\nvoid tick(){'+timeout+'}\nvoid disconnect(){'+disconnect+r'''}
};
struct UI {
    std::optional<int> lastPrivacyMode;
    struct Calls {std::vector<Json> values;void command(const char*,Json data){values.push_back(data);}} client;
    void action(int a,const Snapshot&s){switch(a){
'''+action+r'''
    }}
};
int main(){
    Harness h;
    h.state_.streaming=true;
    const Json normal={{"deviceId","pc"},{"mode",2}};
    const Json screenOff={{"deviceId","pc"},{"mode",3}};
    h.request(normal);
    assert(h.state_.privacyPending && h.pendingPrivacy_->id==1);
    h.request(screenOff); // while pending, do not replace the in-flight request
    assert(h.pendingPrivacy_->id==1);
    clockMs=8002;h.tick();
    assert(!h.state_.privacyPending && !h.state_.privacyKnown);
    assert(h.state_.privacyRequestedMode==PrivacyMode::Unknown);
    assert(h.pendingPrivacy_ && h.pendingPrivacy_->deadline==0);
    h.tick(); // timeout only once, correlation survives
    h.ack({{"id",1},{"code",0},{"requested_mode",2},{"current_mode",2}});
    assert(h.state_.privacyKnown && h.state_.privacyMode==PrivacyMode::Normal && !h.pendingPrivacy_);
    h.request(screenOff); // id 2
    clockMs+=8001;h.tick();
    h.request(normal); // id 3 supersedes timed-out id 2
    h.ack({{"id",2},{"code",0},{"requested_mode",3},{"current_mode",3}});
    assert(h.pendingPrivacy_->id==3 && h.state_.privacyPending && !h.state_.privacyKnown);
    h.ack({{"id",3},{"code",0},{"requested_mode",2},{"current_mode",2}});
    assert(h.state_.privacyKnown && h.state_.privacyMode==PrivacyMode::Normal);
    h.ack({{"id",2},{"code",0},{"requested_mode",3},{"current_mode",3}});
    assert(h.state_.privacyMode==PrivacyMode::Normal);
    h.request(screenOff); // id 4
    h.disconnect();
    h.ack({{"id",4},{"code",0},{"requested_mode",3},{"current_mode",3}});
    assert(!h.pendingPrivacy_ && !h.state_.privacyKnown && !h.state_.privacyPending);
    h.request(screenOff); // id 5, new session
    h.ack({{"id",4},{"code",0},{"requested_mode",3},{"current_mode",3}});
    assert(h.pendingPrivacy_->id==5 && !h.state_.privacyKnown);
    h.ack({{"id",5},{"code",0},{"requested_mode",3},{"current_mode",2}});
    assert(h.state_.privacyMode==PrivacyMode::Normal); // host fallback wins
    h.request(normal); // id 6, ACK without an actual mode
    h.ack({{"id",6},{"code",0},{"requested_mode",2},{"current_mode",0}});
    assert(!h.state_.privacyKnown && h.state_.privacyRequestedMode==PrivacyMode::Unknown);
    h.channels_.at("TEXT_DATA_CHANNEL")->fail=true;
    h.request(normal);
    assert(!h.pendingPrivacy_ && !h.state_.privacyPending && !h.state_.privacyKnown);
    h.notify({{"code",0},{"requested_mode",3},{"current_mode",2}});
    assert(h.state_.privacyKnown && h.state_.privacyMode==PrivacyMode::Normal);
    h.channels_.at("TEXT_DATA_CHANNEL")->fail=false;
    h.request(screenOff);
    const auto notifyRequest=h.pendingPrivacy_->id;
    h.notify({{"code",0},{"requested_mode",3},{"current_mode",2}});
    const auto notifiedNotice=h.state_.privacyNotice;
    assert(h.state_.privacyPending); // no request ID: does not complete the RPC
    h.ack({{"id",notifyRequest},{"code",0},{"requested_mode",3},{"current_mode",3}});
    assert(!h.state_.privacyPending && h.state_.privacyMode==PrivacyMode::Normal && h.state_.privacyNotice==notifiedNotice);
    h.request(screenOff);
    h.notify({{"code",0},{"requested_mode",3},{"current_mode",2}});
    clockMs+=8001;h.tick();
    assert(!h.state_.privacyPending && h.state_.privacyKnown && h.state_.privacyMode==PrivacyMode::Normal);
    h.disconnect();
    assert(!h.state_.privacyKnown);
    h.notify({{"code",0},{"requested_mode",3},{"current_mode",3}});
    assert(h.state_.privacyKnown && h.state_.privacyMode==PrivacyMode::ScreenOff);
    h.notify({{"code",0},{"requested_mode",0},{"current_mode",0}});
    assert(!h.state_.privacyKnown);

    UI ui;
    Snapshot s;s.streaming=true;s.textChannelReady=true;s.activeDeviceId="pc";
    for(int history:{1,2,3})for(int requested:{0,1,2,3}){
        ui.lastPrivacyMode=history;s.privacyRequestedMode=static_cast<PrivacyMode>(requested);
        for(int action:{755,756}){
            ui.action(action,s);
            assert(ui.client.values.back().at("mode")== (action==755?2:3));
        }
    }
    s.privacyKnown=true;s.privacyMode=PrivacyMode::Normal;
    ui.action(755,s);assert(ui.client.values.back().at("mode")==1);
    ui.action(756,s);assert(ui.client.values.back().at("mode")==3);
    s.privacyMode=PrivacyMode::ScreenOff;
    ui.action(756,s);assert(ui.client.values.back().at("mode")==1);
    const auto count=ui.client.values.size();
    s.privacyPending=true;ui.action(755,s);
    s.privacyPending=false;s.textChannelReady=false;ui.action(755,s);
    s.textChannelReady=true;s.streaming=false;ui.action(756,s);
    assert(ui.client.values.size()==count);
    Harness cursor;
    cursor.state_.streaming=true;
    cursor.cursorRequest({{"deviceId","other"}});
    assert(!cursor.pendingCursorCapture_);
    cursor.cursorRequest({{"deviceId","pc"}});
    assert(cursor.state_.cursorCapturePending && cursor.pendingCursorCapture_->id==1);
    cursor.cursorRequest({{"deviceId","pc"}});
    assert(cursor.channels_.at("TEXT_DATA_CHANNEL")->sends==1);
    cursor.cursorAck({{"id",1},{"capture",true},{"errors",0},{"other",0}});
    assert(!cursor.state_.cursorCapturePending && !cursor.pendingCursorCapture_ && cursor.qualityAckFallthrough==0);
    cursor.cursorRequest({{"deviceId","pc"}});
    cursor.cursorAck({{"id",2},{"capture",false},{"errors",0},{"other",0}});
    const auto unconfirmed=cursor.state_.cursorCaptureNotice;
    cursor.cursorRequest({{"deviceId","pc"}});
    cursor.cursorAck({{"id",3},{"capture",true},{"errors",1},{"code",5},{"other",0}});
    assert(cursor.state_.cursorCaptureNotice!=unconfirmed && cursor.qualityAckFallthrough==0);
    cursor.cursorRequest({{"deviceId","pc"}});
    clockMs+=8001;cursor.tick();
    assert(!cursor.state_.cursorCapturePending && !cursor.pendingCursorCapture_);
    cursor.cursorRequest({{"deviceId","pc"}});
    cursor.disconnect();
    assert(!cursor.pendingCursorCapture_ && !cursor.state_.cursorCapturePending && cursor.state_.cursorCaptureNotice.empty());
    cursor.channels_.at("TEXT_DATA_CHANNEL")->fail=true;
    cursor.cursorRequest({{"deviceId","pc"}});
    assert(!cursor.state_.cursorCapturePending && !cursor.pendingCursorCapture_);
    Harness automatic;
    automatic.state_.streaming=true;
    auto ch=automatic.channels_.at("TEXT_DATA_CHANNEL");
    automatic.autoTick();assert(ch->sends==0); // wait for this session's first frame
    automatic.firstVideoFramePublishedLogged_=true;ch->open=false;
    automatic.autoTick();assert(ch->sends==0);
    ch->open=true;ch->buffered=65537;automatic.autoTick();assert(ch->sends==0);
    ch->buffered=0;automatic.cancel_=true;automatic.autoTick();assert(ch->sends==0);
    automatic.cancel_=false;automatic.state_.streaming=false;automatic.autoTick();assert(ch->sends==0);
    automatic.state_.streaming=true;automatic.autoTick();assert(ch->sends==1 && automatic.pendingCursorCapture_);
    for(int i=0;i<5;++i)automatic.autoTick();assert(ch->sends==1);
    const auto autoId=automatic.pendingCursorCapture_->id;
    automatic.cursorAck({{"id",autoId},{"capture",true},{"errors",0},{"other",0}});
    automatic.autoTick();assert(ch->sends==1 && automatic.qualityAckFallthrough==0);
    automatic.disconnect();assert(!automatic.autoCursorCaptureHandled_);
    ++automatic.generation_;automatic.firstVideoFramePublishedLogged_=false;
    automatic.autoTick();assert(ch->sends==1);
    automatic.firstVideoFramePublishedLogged_=true;automatic.autoTick();assert(ch->sends==2);
    clockMs+=8001;automatic.tick();automatic.autoTick();assert(ch->sends==2); // no retry loop
    Harness manual;
    manual.state_.streaming=true;manual.cursorRequest({{"deviceId","pc"}});
    manual.cursorAck({{"id",1},{"capture",true},{"errors",0},{"other",0}});
    manual.firstVideoFramePublishedLogged_=true;manual.autoTick();
    assert(manual.channels_.at("TEXT_DATA_CHANNEL")->sends==1); // manual already satisfied it
    Harness privacyFirst;
    privacyFirst.state_.streaming=true;privacyFirst.firstVideoFramePublishedLogged_=true;
    privacyFirst.request({{"deviceId","pc"},{"mode",3}});privacyFirst.autoTick();
    assert(!privacyFirst.pendingCursorCapture_);
    privacyFirst.ack({{"id",1},{"code",0},{"requested_mode",3},{"current_mode",3}});
    privacyFirst.autoTick();assert(privacyFirst.pendingCursorCapture_);
    Harness failed;
    failed.state_.streaming=true;failed.firstVideoFramePublishedLogged_=true;
    failed.channels_.at("TEXT_DATA_CHANNEL")->fail=true;failed.autoTick();
    failed.channels_.at("TEXT_DATA_CHANNEL")->fail=false;failed.autoTick();
    assert(failed.channels_.at("TEXT_DATA_CHANNEL")->sends==0 && failed.autoCursorCaptureHandled_);
    failed.cursorRequest({{"deviceId","pc"}});failed.autoTick();
    assert(failed.channels_.at("TEXT_DATA_CHANNEL")->sends==1); // manual retry retained
    std::cout<<"PASS privacy/cursor production branches: state sync, first-frame/channel gates, once per connection, manual request, timeout and failure\n";
}
'''
cpp = out / 'privacy-state.cpp'
cpp.write_text(source, encoding='utf-8')
exe = out / 'privacy-state.exe'
subprocess.run([sys.argv[1], '-std=c++17', '-O0', '-UNDEBUG',
                '-I'+str(root/'include'), '-I'+str(root/'external/json/include'),
                str(cpp), '-o', str(exe)], check=True,
               env=dict(os.environ, TEMP=str(out), TMP=str(out), TMPDIR=str(out)))
subprocess.run([str(exe)], check=True)
