#pragma once
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace uu {
// HID button bit positions match libnx. Keep normalization portable so the
// same policy serves both player slots and can be checked without a console.
enum class PadLayout { Full, LeftJoy, RightJoy };
struct PadSample {
    uint64_t held = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
};
enum class PadLegend { Switch, Xbox, PlayStation };
struct XboxButtons {uint32_t buttons=0;int leftTrigger=0,rightTrigger=0;};
inline int padTarget(int source,bool switchHabit,PadLayout layout=PadLayout::Full){
    // Source order is the normalized HID A/B/X/Y, sticks, shoulders,
    // triggers, start/back and D-pad. Only this final stage changes remote input.
    const int defaults[]={switchHabit?1:2,switchHabit?2:1,switchHabit?3:4,switchHabit?4:3,
        9,10,5,6,7,8,11,layout==PadLayout::LeftJoy?11:12,15,13,16,14};
    return defaults[source];
}
inline XboxButtons xboxButtons(uint64_t held,bool switchHabit,PadLayout layout=PadLayout::Full){
    constexpr uint32_t masks[]={0,4096,8192,16384,32768,256,512,0,0,64,128,16,32,1,2,4,8};
    XboxButtons result;
    for(int source=0;source<16;++source)if(held&(1ULL<<source)){
        const int target=padTarget(source,switchHabit,layout);
        result.buttons|=masks[target];
        if(target==7)result.leftTrigger=255;
        if(target==8)result.rightTrigger=255;
    }
    return result;
}

struct PadNavigation {
    uint64_t previous=0;
    uint64_t update(const PadSample &sample,bool enabled){
        uint64_t directions=0;
        if(enabled){
            if(sample.lx < -16384)directions|=1ULL<<12;
            if(sample.lx > 16384)directions|=1ULL<<14;
            if(sample.ly > 16384)directions|=1ULL<<13;
            if(sample.ly < -16384)directions|=1ULL<<15;
        }
        const auto pressed=directions&~previous;previous=directions;return pressed;
    }
};
inline PadSample normalizePad(PadSample raw, PadLayout layout) {
    if(layout == PadLayout::Full) return raw;
    const auto original = raw.held;
    const int originalX = raw.lx;
    raw.held = 0;
    if(layout == PadLayout::LeftJoy) {
        for(auto [from,to] : {std::pair{12,1}, {13,3}, {14,2}, {15,0}, {4,4}, {24,6}, {25,7}, {6,8}, {8,9}})
            if(original & (1ULL << from)) raw.held |= 1ULL << to;
        raw.lx = -raw.ly; raw.ly = originalX;
    } else {
        for(auto [from,to] : {std::pair{0,1}, {1,3}, {2,0}, {3,2}, {5,4}, {26,6}, {27,7}, {7,8}, {9,9}})
            if(original & (1ULL << from)) raw.held |= 1ULL << to;
        raw.lx = raw.ry; raw.ly = -raw.rx;
    }
    // Keep Plus/Minus physical identity. Minus is continuous remote input,
    // never a local shortcut, including on the left Joy-Con.
    raw.held |= original & ((1ULL << 10) | (1ULL << 11));
    raw.rx = raw.ry = 0;
    return raw;
}

struct PadMenuChord {
    bool pending = false, latched = false;
    // Normalized L + Plus, independently per player. 1 = remote Start tap,
    // 2 = local menu toggle. Consumed chords require both buttons released.
    int update(uint64_t held, uint64_t pressed, bool enabled) {
        constexpr uint64_t plus=1ULL<<10, chord=(1ULL<<6)|plus;
        if(!enabled) { pending=false;latched=(held&chord)!=0;return 0; }
        if(latched) { if(!(held&chord))latched=false;return 0; }
        if((held&chord)==chord && (pressed&chord)) {
            pending=false;latched=true;return 2;
        }
        if(pressed&plus)pending=true;
        if(pending && !(held&plus)) { pending=false;return 1; }
        return 0;
    }
};
} // namespace uu
