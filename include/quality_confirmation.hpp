#pragma once
#include <cstdint>

namespace uu {

// Target values actually requested from the host.
struct QualityApplicationTarget {
    int width=0;
    int height=0;
    int fps=0;
    int quality=0;
    int bitrateMbps=0;
};

// Actual values observed from CaptureSettingSync during the current request.
// Presence is tracked separately so a missing field never inherits a match
// from another field.
struct QualityEvidence {
    bool fpsKnown=false, qualityKnown=false, widthKnown=false, heightKnown=false;
    bool bitrateKnown=false;
    int fps=0, quality=0, width=0, height=0, bitrateMbps=0;

    void clear() noexcept { *this = QualityEvidence{}; }
    void observeFps(int value) noexcept { fps=value; fpsKnown=true; }
    void observeQuality(int value) noexcept { quality=value; qualityKnown=true; }
    void observeWidth(int value) noexcept { width=value; widthKnown=true; }
    void observeHeight(int value) noexcept { height=value; heightKnown=true; }
    void observeSize(int w,int h) noexcept { observeWidth(w); observeHeight(h); }
    void observeBitrate(int value) noexcept { bitrateMbps=value; bitrateKnown=true; }
    bool anyKnown() const noexcept {
        return fpsKnown || qualityKnown || widthKnown || heightKnown || bitrateKnown;
    }
};

// A complete match requires the requested capture fields. A Custom bitrate is
// required only when the request actually carries Custom mode and a positive
// limit; preset mode may carry a local cap that is not sent to the host.
inline bool qualityApplicationMatches(const QualityApplicationTarget &target,
                                      const QualityEvidence &evidence) noexcept {
    if(!evidence.fpsKnown || !evidence.qualityKnown ||
       !evidence.widthKnown || !evidence.heightKnown)
        return false;
    if(evidence.fps!=target.fps || evidence.quality!=target.quality ||
       evidence.width!=target.width || evidence.height!=target.height)
        return false;
    if(target.quality==6 && target.bitrateMbps>0 &&
       (!evidence.bitrateKnown || evidence.bitrateMbps!=target.bitrateMbps))
        return false;
    return true;
}

// Focused state for one quality request. It deliberately has no generic event
// bus: beginRequest() is the only reset boundary, ACK never erases evidence,
// and evaluate() is the single match rule shared by sync and finishQuality().
struct QualityApplyTracker {
    QualityEvidence evidence;
    bool accepted=false;
    bool applied=false;
    bool pending=false;
    uint64_t pendingRequestId=0;

    void beginRequest() noexcept {
        evidence.clear();
        accepted=false;
        applied=false;
        pending=false;
        pendingRequestId=0;
    }
    void accept() noexcept { accepted=true; }
    void acknowledge() noexcept { accepted=true; }
    void armConfirmation(uint64_t requestId) noexcept {
        pending=true;
        pendingRequestId=requestId;
    }
    bool evaluate(const QualityApplicationTarget &target) noexcept {
        applied=qualityApplicationMatches(target,evidence);
        if(applied){pending=false;pendingRequestId=0;}
        return applied;
    }
    void timeout() noexcept {
        applied=false;
        pending=false;
        pendingRequestId=0;
    }
};

} // namespace uu
