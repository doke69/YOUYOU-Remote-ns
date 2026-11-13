#include "client_snapshot.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <thread>

static thread_local bool countAllocations=false;
static thread_local size_t allocations=0;
void *operator new(size_t size) {
    if(countAllocations)++allocations;
    if(auto p=std::malloc(size?size:1))return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete(void *p,size_t) noexcept {std::free(p);}
#define CHECK(x) do {if(!(x)){std::cerr<<"failed line "<<__LINE__<<": " #x "\n";return 1;}}while(0)
#define FIELD(name, value) {uu::Snapshot copy=base,next=base;next.name=(value);CHECK(copy.updateFrom(next));CHECK(copy.name==next.name);CHECK(!copy.updateFrom(next));}

int main() {
    uu::Snapshot base;
    // Each member below is changed independently. The coverage test checks
    // this list against the declaration, including future additions.
    FIELD(qualitySavePending, !base.qualitySavePending);
    FIELD(qualitySaveError, base.qualitySaveError+" changed");
    FIELD(privacyPending, !base.privacyPending);
    FIELD(privacyKnown, !base.privacyKnown);
    FIELD(privacyMode, static_cast<uu::PrivacyMode>((unsigned(base.privacyMode)+1)%4));
    FIELD(privacyRequestedMode, static_cast<uu::PrivacyMode>((unsigned(base.privacyRequestedMode)+1)%4));
    FIELD(privacyNotice, base.privacyNotice+" changed");
    FIELD(cursorCapturePending, !base.cursorCapturePending);
    FIELD(cursorCaptureNotice, base.cursorCaptureNotice+" changed");
    FIELD(textPending, !base.textPending);
    FIELD(textRevision, base.textRevision+1);
    FIELD(textResult, base.textResult+1);
    FIELD(textNotice, base.textNotice+" changed");
    FIELD(activeDeviceId, base.activeDeviceId+" changed");
    FIELD(textChannelReady, !base.textChannelReady);
    FIELD(safetyNotice,"pending");
    FIELD(controlChannelReady, !base.controlChannelReady);
    FIELD(stage, base.stage+" changed");
    FIELD(detail, base.detail+" changed");
    FIELD(qrUrl, base.qrUrl+" changed");
    FIELD(nickname, base.nickname+" changed");
    FIELD(devicesRevision, base.devicesRevision+1);
    FIELD(authenticated, !base.authenticated);
    FIELD(streaming, !base.streaming);
    FIELD(width, base.width+1);
    FIELD(height, base.height+1);
    FIELD(fps, base.fps+1);
    FIELD(quality, base.quality+1);
    FIELD(maxBitrateMbps, base.maxBitrateMbps+1);
    FIELD(vsync, !base.vsync);
    FIELD(hevc, !base.hevc);
    FIELD(qualityPending, !base.qualityPending);
    FIELD(qualityNotice, base.qualityNotice+" changed");
    FIELD(routeBitrateNotice, base.routeBitrateNotice+" changed");
    FIELD(effectiveBitrateMbps, base.effectiveBitrateMbps+1);
    FIELD(videoScaleNotice, base.videoScaleNotice+" changed");
    FIELD(playoutDelayMs, base.playoutDelayMs+1);
    FIELD(appliedFps, base.appliedFps+1);
    FIELD(appliedQuality, base.appliedQuality+1);
    FIELD(appliedWidth, base.appliedWidth+1);
    FIELD(appliedHeight, base.appliedHeight+1);
    FIELD(appliedBitrateMbps, base.appliedBitrateMbps+1);
    FIELD(appliedValid, !base.appliedValid);
    FIELD(appliedBitrateKnown, !base.appliedBitrateKnown);
    FIELD(appliedFpsKnown, !base.appliedFpsKnown);
    FIELD(appliedQualityKnown, !base.appliedQualityKnown);
    FIELD(appliedWidthKnown, !base.appliedWidthKnown);
    FIELD(appliedHeightKnown, !base.appliedHeightKnown);
    FIELD(settingAccepted, !base.settingAccepted);
    FIELD(settingApplied, !base.settingApplied);
    FIELD(settingRejected, !base.settingRejected);
    FIELD(settingApplyPending, !base.settingApplyPending);
    FIELD(settingErrorCode, base.settingErrorCode+1);
    FIELD(settingSource, base.settingSource+" changed");
    FIELD(serverCapabilityValid, !base.serverCapabilityValid);
    FIELD(serverDisplayInfo, base.serverDisplayInfo+" changed");
    FIELD(serverCodecInfo, base.serverCodecInfo+" changed");
    FIELD(serverCaptureImpl, base.serverCaptureImpl+" changed");
    FIELD(serverEncoderImpl, base.serverEncoderImpl+" changed");
    FIELD(serverVideoTrack, base.serverVideoTrack+1);
    FIELD(loginMode, base.loginMode+" changed");
    FIELD(smsRetryAt, base.smsRetryAt+1);

    {
        uu::Snapshot copy=base,next=base;
        next.devices=uu::Json::array({{{"device_id","first"},{"alias","First computer"}}});
        ++next.devicesRevision;
        CHECK(copy.updateFrom(next) && copy.devices==next.devices);
        next.devices[0]["alias"]="Renamed computer";++next.devicesRevision;
        CHECK(copy.updateFrom(next) && copy.devices==next.devices);
        next.devices=uu::Json::array();++next.devicesRevision;
        CHECK(copy.updateFrom(next) && copy.devices.empty());
        CHECK(!copy.updateFrom(next));
    }
    uu::Snapshot latest,copy;
    latest.streaming=true;
    latest.detail="Synthetic connected desktop with a long status message";
    latest.serverDisplayInfo="Synthetic capabilities and display description";
    latest.devices=uu::Json::array({{{"device_id","synthetic-long-device-id-00001"},{"alias","Synthetic desktop for allocation counting"}}});
    latest.devicesRevision=1;
    CHECK(copy.updateFrom(latest));
    allocations=0;countAllocations=true;bool changed=false;
    for(int i=0;i<20000;++i)changed|=copy.updateFrom(latest);
    countAllocations=false;
    CHECK(!changed && allocations==0);
    // The client retains its existing state mutex around updateFrom. Exercise
    // coherent multi-field publication without dropping updates by a timer.
    std::mutex mutex;uu::Snapshot published,view;published.width=published.height=0;
    std::atomic<bool> done{false};bool coherent=true;unsigned reads=0;
    std::thread writer([&]{for(int i=1;i<=2000;++i){std::lock_guard lock(mutex);published.width=i;published.height=-i;published.textRevision=i;}done=true;});
    do {
        {std::lock_guard lock(mutex);view.updateFrom(published);}
        coherent&=view.width==-view.height && uint64_t(view.width)==view.textRevision;
        ++reads;
    }while(!done.load());
    writer.join();{std::lock_guard lock(mutex);view.updateFrom(published);}
    CHECK(coherent && view.width==2000 && view.height==-2000 && view.textRevision==2000);
    std::cout<<"PASS: independent state members, device revision/rename/reset; 20000 unchanged reads, "<<allocations<<" allocations; coherent concurrent snapshots ("<<reads<<" reads)\n";
}
