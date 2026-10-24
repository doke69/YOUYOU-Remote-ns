#include "background.hpp"
#include "client.hpp"
#include "config_store.hpp"
#include "cpu_diagnostics.hpp"
#include "diagnostics.hpp"
#include "device_selection.hpp"
#include "live_telemetry.hpp"
#include "native_display.hpp"
#include "qrcodegen.hpp"
#include "rtc_diagnostics.hpp"
#include "runtime_profile.hpp"
#include <SDL_ttf.h>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <list>
#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <vector>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Visual thesis: quiet dark workspace, large readable type, a single mint focus.
// Content: account and connection status, computer list, QR/desktop workspace, controls.
// Interaction: eased focus movement, a restrained busy pulse, toggleable stream overlay.
namespace {
#ifdef __SWITCH__
// Console profile: used to adapt the UI/output to Switch vs Switch Lite and to
// record the environment in connection.log for on-device diagnosis.
SetSysProductModel productModel = SetSysProductModel_Invalid;
bool setsysReady = false;
std::string productModelText = "Switch";
std::string firmwareText;
const char *operationModeLabel() {
    return appletGetOperationMode() == AppletOperationMode_Console ? "TV" : "掌机";
}
std::string deviceSummary() {
    return productModelText + " · " + operationModeLabel() +
           (firmwareText.empty() ? std::string{} : " · 系统 " + firmwareText);
}
#endif
constexpr SDL_Color white{238, 243, 246, 255}, muted{142, 158, 169, 255}, accent{102, 231, 197, 255};
void color(SDL_Renderer *r, SDL_Color c) {
    uu::display::setColor(r, c.r, c.g, c.b, c.a);
}
void rect(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c) {
    color(r, c);
    SDL_Rect a{x, y, w, h};
    uu::display::fillRect(r, &a);
}
namespace tvui { void rounded(SDL_Renderer*,SDL_Rect,SDL_Color,int); }
// Bounded LRU: avoid rasterizing CJK and allocating GPU textures every frame.
struct TextEntry {
    SDL_Renderer *renderer; TTF_Font *font; std::string value; Uint32 tint;
    SDL_Texture *texture; int width, height; size_t bytes;
    int inkTop=0,inkHeight=0;
};
std::list<TextEntry> textCache;
size_t textCacheBytes=0;
void clearTextCache() {
    for(auto &entry:textCache) uu::display::destroyTexture(entry.texture);
    textCache.clear();textCacheBytes=0;
}
const TextEntry *cachedText(SDL_Renderer *r,TTF_Font *f,const std::string &s,SDL_Color c) {
    if(s.empty()) return nullptr;
    const Uint32 tint=(Uint32(c.r)<<24)|(Uint32(c.g)<<16)|(Uint32(c.b)<<8)|c.a;
    auto entry=std::find_if(textCache.begin(),textCache.end(),[&](const TextEntry &e){
        return e.renderer==r && e.font==f && e.tint==tint && e.value==s;
    });
    if(entry==textCache.end()) {
        auto *surface=TTF_RenderUTF8_Blended(f,s.c_str(),c);
        if(!surface) return nullptr;
        int inkTop=0,inkHeight=surface->h;
        const bool mustLock=SDL_MUSTLOCK(surface);
        if(surface->format->BytesPerPixel==4 && surface->format->Amask &&
           (!mustLock || SDL_LockSurface(surface)==0)) {
            auto inkRow=[&](int y) {
                const auto *pixels=reinterpret_cast<const Uint32*>(
                    static_cast<const Uint8*>(surface->pixels)+y*surface->pitch);
                for(int x=0;x<surface->w;++x)
                    if(pixels[x]&surface->format->Amask)return true;
                return false;
            };
            int bottom=surface->h;
            while(inkTop<bottom && !inkRow(inkTop))++inkTop;
            while(bottom>inkTop && !inkRow(bottom-1))--bottom;
            inkHeight=bottom-inkTop;
            if(mustLock)SDL_UnlockSurface(surface);
        }
        auto *texture=uu::display::textureFromSurface(r,surface);
        const int w=surface->w,h=surface->h;SDL_FreeSurface(surface);
        if(!texture) return nullptr;
        const size_t bytes=size_t(w)*h*4;
        while(!textCache.empty() && (textCache.size()>=192 || textCacheBytes+bytes>8u*1024u*1024u)) {
            textCacheBytes-=textCache.back().bytes;
            uu::display::destroyTexture(textCache.back().texture);textCache.pop_back();
        }
        textCache.push_front({r,f,s,tint,texture,w,h,bytes,inkTop,inkHeight});textCacheBytes+=bytes;
    } else textCache.splice(textCache.begin(),textCache,entry);
    return &textCache.front();
}
void text(SDL_Renderer *r, TTF_Font *f, const std::string &s, int x, int y, SDL_Color c = white,
          int maxWidth = 1200) {
    const auto *entry=cachedText(r,f,s,c);if(!entry)return;
    const auto &e=*entry;
    SDL_Rect src{0,0,std::min(e.width,maxWidth),e.height};
    SDL_Rect dst{x,y,src.w,src.h};uu::display::copy(r,e.texture,&src,&dst);
}
void statusText(SDL_Renderer *renderer, TTF_Font *font, const std::string &message, SDL_Color tint) {
    auto *surface = TTF_RenderUTF8_Blended_Wrapped(font, message.substr(0, 512).c_str(), tint, 1110);
    if (!surface)
        return;
    auto *texture = uu::display::textureFromSurface(renderer, surface);
    SDL_Rect source{0, 0, surface->w, std::min(surface->h, 64)}, destination{82, 580, source.w, source.h};
    if (texture)
        uu::display::copy(renderer, texture, &source, &destination);
    uu::display::destroyTexture(texture);
    SDL_FreeSurface(surface);
}
std::string formatMbps(double value) {
    if (value < 0)
        return "—";
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f Mbps", value);
    return buffer;
}
std::string formatPercent(double value) {
    if (value < 0)
        return "—";
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f%%", value);
    return buffer;
}
std::string formatMillis(double value) {
    if (value < 0)
        return "—";
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f ms", value);
    return buffer;
}
// Network information panel (latency / codec / packet loss / decoder / encoder).
void statsPanel(SDL_Renderer *renderer, TTF_Font *font, const uu::StreamStatsSnapshot &s, bool persistent) {
    constexpr int x = 40, y = 92, width = 1160, height = 448;
    rect(renderer, x, y, width, height, {10, 15, 22, 232});
    rect(renderer, x, y, width, 2, accent);
    text(renderer, font, "网络信息（B 关闭）", x + 20, y + 12, accent);
    rect(renderer,940,98,240,38,{49,53,60,255});
    text(renderer,font,persistent?"A 常驻信息：开":"A 常驻信息：关",950,103,white,220);
    const int lineX = x + 20, labelWidth = 96;
    int lineY = y + 44;
    auto line = [&](const char *label, const std::string &value, SDL_Color color = white) {
        text(renderer, font, label, lineX, lineY, muted);
        text(renderer, font, value, lineX + labelWidth, lineY, color, width - labelWidth - 40);
        lineY += 30;
    };
    std::string network = "RTT " + (s.rttMs >= 0 ? std::to_string(s.rttMs) + " ms" : "—") + "  ·  " +
                          s.transport;
    line("网络", network);
    line("本地地址",s.localEndpoint);
    line("远端地址",s.remoteEndpoint);
    auto fpsText=[](double fps) {
        if(fps<0) return std::string("—");
        char value[32]; std::snprintf(value,sizeof value,"%.1f",fps); return std::string(value);
    };
    line("帧率", "目标 "+std::to_string(s.requestedFps)+"  ·  提交显示 "+fpsText(s.presentFps)+
        "  ·  解码 "+fpsText(s.decodeFps)+" FPS");
    line("播放调度",s.pacing ? "缓冲 "+std::to_string(s.readyFrames)+" 帧 / "+std::to_string(s.playoutDelayMs)+
         " ms  ·  跳过 "+std::to_string(s.presentationSkipped)+"  ·  缺时间戳 "+std::to_string(s.missingTimestamps) : "关闭");
    std::string video = "初传缺口 " + formatPercent(s.video.lossPercent) + "  ·  抖动 " +
                        formatMillis(s.video.jitterMs) + "  ·  码率 " + formatMbps(s.video.bitrateMbps);
    line("视频", video);
    line("重传", "NACK " + std::to_string(s.nackSent) + " 次  ·  RTX 回填 " +
                     std::to_string(s.rtxRecovered) + " 包");
    if (s.strictReceiver) {
        const auto total = s.receiverAccepted + s.receiverDropped;
        line("接收帧", "交付 " + std::to_string(s.receiverAccepted) + "  ·  丢弃 " +
            std::to_string(s.receiverDropped) + "（" + formatPercent(total ? double(s.receiverDropped)*100/total : -1) +
            "，含待 IDR " + std::to_string(s.receiverWaitingIdr) + "）");
        line("组帧等待", "最近 " + std::to_string(s.receiverRecentWaitMs) + " ms  ·  平均 " +
            formatMillis(s.receiverMeanWaitMs) + "  ·  IDR 跳转 " + std::to_string(s.receiverFastForwards));
    }
    std::string codec = s.codec;
    if (s.width > 0)
        codec += "  ·  " + std::to_string(s.width) + "x" + std::to_string(s.height);
    line("编码", codec + "  ·  " + s.encoder);
    std::string decode = s.decoder;
    if (s.decodeMs > 0)
        decode += "  ·  " + formatMillis(s.decodeMs);
    decode += "  ·  解码错误 " + std::to_string(s.decodeErrors) + "  ·  丢帧 " +
              std::to_string(s.droppedFrames);
    line("解码", decode);
    std::string keys = "请求 " + std::to_string(s.keyframeRequests) + " 次  ·  最近恢复 " +
                       (s.keyframeRecoveryMs >= 0 ? std::to_string(s.keyframeRecoveryMs) + " ms" : "—");
    line("关键帧", keys);
    std::string audio = "Opus  ·  码率 " + formatMbps(s.audio.bitrateMbps) + "  ·  丢包 " +
                        formatPercent(s.audio.lossPercent);
    line("音频", audio);
}
// Compact bottom-left overlay with a subdued backdrop:
// session time, transport, fps, realtime video Mbps, RTT, frame time, loss,
// configured video bitrate.
void compactNetworkInfo(SDL_Renderer *renderer,TTF_Font *font,const uu::StreamStatsSnapshot &s,
                        Uint32 elapsedSeconds) {
    const SDL_Color normal{235,235,235,240},warning{253,138,0,235},bad{250,75,90,235};
    auto number=[](double v){if(v<0)return std::string("--");char out[32];std::snprintf(out,sizeof out,"%.1f",v);return std::string(out);};
    auto duration=[&](Uint32 seconds){char out[32];std::snprintf(out,sizeof out,"%02u:%02u:%02u",
        unsigned(seconds/3600),unsigned((seconds/60)%60),unsigned(seconds%60));return std::string(out);};
    std::string path="UDP";
    if(s.transport.find("私网地址直连")!=std::string::npos || s.transport.find("局域网直连")!=std::string::npos)path="UDP LAN";
    else if(s.transport.find("TURN")!=std::string::npos || s.transport.find("远端中继")!=std::string::npos)path="TURN";
    else if(s.transport.find("P2P")!=std::string::npos || s.transport.find("公网")!=std::string::npos)path="UDP WAN";
    struct Row{std::string text;SDL_Color color;};
    const Row rows[8]={
        {duration(elapsedSeconds),normal},
        {path,normal},
        {number(s.presentFps)+" fps",normal},
        {formatMbps(s.video.bitrateMbps),normal},
        {s.rttMs>=0?std::to_string(s.rttMs)+" ms":std::string("-- ms"),normal},
        {(s.decodeMs>0?number(s.decodeMs):std::string("--"))+" ms frm.",normal},
        {formatPercent(s.video.lossPercent)+" loss",
            s.video.lossPercent>=10?bad:s.video.lossPercent>=5?warning:normal},
        {s.maxBitrateMbps>0?std::to_string(s.maxBitrateMbps)+" Mbps":std::string("-- Mbps"),normal},
    };
    std::string display[8];int maxWidth=0;
    for(int i=0;i<8;++i){
        display[i]=rows[i].text;int w=0,h=0;if(TTF_SizeUTF8(font,display[i].c_str(),&w,&h)!=0)continue;
        while(w>300 && !display[i].empty()){
            size_t n=display[i].size()-1;while(n>0 && (static_cast<unsigned char>(display[i][n])&0xc0)==0x80)--n;
            display[i].resize(n);TTF_SizeUTF8(font,display[i].c_str(),&w,&h);
        }
        maxWidth=std::max(maxWidth,w);
    }
    const int lineHeight=std::max(24,TTF_FontHeight(font)+2);
    constexpr int lineCount=8,padX=12,padY=10;
    const int panelW=std::min(340,maxWidth+padX*2),panelH=lineCount*lineHeight+padY*2;
    const int panelX=16,panelY=720-16-panelH;
    tvui::rounded(renderer,{panelX,panelY,panelW,panelH},SDL_Color{0,0,0,115},5);
    for(int i=0;i<8;++i)
        text(renderer,font,display[i],panelX+padX,panelY+padY+i*lineHeight,rows[i].color,panelW-padX*2);
}
std::optional<std::string> numericInput(const char *title, const std::string &value, int minimum,
                                        int maximum) {
#ifdef __SWITCH__
    SwkbdConfig keyboard{};
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return std::nullopt;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&keyboard, title);
    swkbdConfigSetInitialText(&keyboard, value.c_str());
    swkbdConfigSetStringLenMin(&keyboard, minimum);
    swkbdConfigSetStringLenMax(&keyboard, maximum);
    swkbdConfigSetOkButtonText(&keyboard, "确定");
    std::array<char, 64> buffer{};
    auto result = swkbdShow(&keyboard, buffer.data(), buffer.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result))
        return std::nullopt;
    return std::string(buffer.data());
#else
    (void)title;
    (void)value;
    (void)minimum;
    (void)maximum;
    return std::nullopt;
#endif
}
std::optional<std::string> textInput(const std::string &value) {
#ifdef __SWITCH__
    // One explicit native editor, never SDL's inline keyboard plus our own UI.
    SDL_StopTextInput();
    SwkbdConfig keyboard{};
    if(R_FAILED(swkbdCreate(&keyboard,0)))return std::nullopt;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard,SwkbdType_Normal);
    swkbdConfigSetHeaderText(&keyboard,"输入发送到电脑的文字");
    swkbdConfigSetInitialText(&keyboard,value.c_str());
    swkbdConfigSetStringLenMax(&keyboard,512);
    swkbdConfigSetOkButtonText(&keyboard,"完成");
    std::array<char,2049> buffer{};
    const auto result=swkbdShow(&keyboard,buffer.data(),buffer.size());
    swkbdClose(&keyboard);
    if(R_FAILED(result))return std::nullopt;
    return std::string(buffer.data());
#else
    (void)value;return std::nullopt;
#endif
}
struct SmsForm {
    std::string country = "86", mobile, code;
    int selected = 1;
    uu::Json credentials() const {
        return {{"countryCode", country}, {"mobile", mobile}, {"code", code}};
    }
};
struct Fonts {
    // Keep both files in memory for the complete lifetime of all font faces.
    // FreeType seeks/reads its stream during layout as well as rasterization.
    std::vector<unsigned char> regularBytes, boldBytes;
    TTF_Font *normal = nullptr, *small = nullptr, *large = nullptr, *hint = nullptr,
             *brand = nullptr;
    Fonts() {
#ifdef __SWITCH__
        const std::string root="romfs:/fonts/";
#else
        // Packaged desktop builds use RomFS; source-tree previews use assets.
        const std::string root=std::ifstream("romfs/fonts/NotoSansSC-Regular.otf").good()
                                   ? "romfs/fonts/" : "assets/fonts/";
#endif
        auto readFont=[](const std::string &path) {
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(!file)throw std::runtime_error("Cannot open bundled font");
            const auto length=file.tellg();
            if(length<=0 || length>32*1024*1024)
                throw std::runtime_error("Invalid bundled font size");
            std::vector<unsigned char> bytes(static_cast<size_t>(length));
            file.seekg(0);
            if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))
                throw std::runtime_error("Cannot read bundled font");
            return bytes;
        };
        regularBytes=readFont(root+"NotoSansSC-Regular.otf");
        boldBytes=readFont(root+"NotoSansSC-Bold.otf");
        auto load = [&](int size) {
            return TTF_OpenFontRW(SDL_RWFromConstMem(regularBytes.data(),int(regularBytes.size())),1,size);
        };
        normal = load(26);
        small = load(21);
        large = load(48);
        hint = load(16);
        brand = TTF_OpenFontRW(SDL_RWFromConstMem(boldBytes.data(),int(boldBytes.size())),1,34);
        if (!normal || !small || !large || !hint || !brand) {
            const std::string error=TTF_GetError();
            close();
            throw std::runtime_error("Cannot load bundled Noto Sans SC font: "+error);
        }
    }
    void close() {
        for(auto **font:{&normal,&small,&large,&hint,&brand}) {
            if(*font)TTF_CloseFont(*font);
            *font=nullptr;
        }
    }
    ~Fonts() { close(); }
    Fonts(const Fonts&)=delete;
    Fonts& operator=(const Fonts&)=delete;
};
std::string read(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
#include "tv_frontend.inc"
#include "live_frontend.inc"
#include "touch_gestures.inc"
#include "phone_mouse.inc"
#include "native_touch.inc"

std::optional<uu::Json> qualityMenu(SDL_Renderer *renderer, Fonts &fonts, const uu::Snapshot &state) {
    const char *resolutionNames[] = {"960 × 540", "1280 × 720", "1920 × 1080"};
    const int widths[] = {960,1280,1920}, heights[] = {540,720,1080};
    const char *qualityNames[] = {"流畅", "普通", "高清", "蓝光", "自动"};
    int resolution = state.width == 960 ? 0 : state.width == 1920 ? 2 : 1;
    int fps = state.fps == 30 ? 0 : 1, quality = std::clamp(state.quality-1,0,4), selected=0;
    bool vsync=state.vsync;
    int playoutDelay=state.playoutDelayMs;
    int maxBitrate=state.maxBitrateMbps;
    bool hevc=state.hevc;
    const int delayOptions[]={0,16,35,50,100};
    std::string displayError;
#ifdef __SWITCH__
    PadState menuPad;
    padInitializeDefault(&menuPad);
#endif
    bool touched = false;
    while (true) {
        bool up=false,down=false,left=false,right=false,apply=false,back=false,reset=false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) return std::nullopt;
            if (event.type == SDL_KEYDOWN) {
                const auto key = event.key.keysym.sym;
                up |= key == SDLK_UP; down |= key == SDLK_DOWN;
                left |= key == SDLK_LEFT; right |= key == SDLK_RIGHT;
                apply |= key == SDLK_RETURN; back |= key == SDLK_ESCAPE; reset |= key == SDLK_x;
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                const int y=event.button.y;
                if (y>=200 && y<515) {selected=(y-200)/45;right=true;}
                if (y>=590 && y<655) apply=true;
            }
        }
#ifdef __SWITCH__
        if (!appletMainLoop()) return std::nullopt;
        padUpdate(&menuPad);
        const auto pressed = padGetButtonsDown(&menuPad);
        up |= pressed & HidNpadButton_Up; down |= pressed & HidNpadButton_Down;
        left |= pressed & HidNpadButton_Left; right |= pressed & HidNpadButton_Right;
        apply |= pressed & HidNpadButton_A; back |= pressed & HidNpadButton_B;
        reset |= pressed & HidNpadButton_X;
        HidTouchScreenState touch{};
        hidGetTouchScreenStates(&touch,1);
        if (touch.count && !touched) {
            const int y=touch.touches[0].y;
            if (y>=200 && y<515) {selected=(y-200)/45;right=true;}
            if (y>=590 && y<655) apply=true;
        }
        touched=touch.count!=0;
#endif
        if (back) return std::nullopt;
        if (up) selected=(selected+6)%7;
        if (down) selected=(selected+1)%7;
        const int delta=right ? 1 : left ? -1 : 0;
        if (delta) {
            if(selected==0) resolution=(resolution+delta+3)%3;
            if(selected==1) fps=1-fps;
            if(selected==2) quality=(quality+delta+5)%5;
            if(selected==3) vsync=!vsync;
            if(selected==6) hevc=!hevc;
            if(selected==5) maxBitrate=std::clamp(maxBitrate+delta,0,50);
            if(selected==4) {
                int index=0;
                while(index<4 && delayOptions[index]<playoutDelay) ++index;
                playoutDelay=delayOptions[(index+delta+5)%5];
            }
        }
        if (reset) {resolution=1;fps=1;quality=2;vsync=true;playoutDelay=0;maxBitrate=0;hevc=false;}
        if (apply) {
            if (vsync!=state.vsync && uu::display::vsync(renderer,vsync?1:0)!=0) {
                displayError="当前显示后端不支持切换垂直同步，请保留原设置。";
            } else {
                return uu::Json{{"width",widths[resolution]},{"height",heights[resolution]},
                                {"fps",fps?60:30},{"quality",quality+1},{"vsync",vsync},
                                {"playout_delay_ms",playoutDelay},{"max_bitrate_mbps",maxBitrate},{"hevc",hevc}};
            }
        }
        color(renderer,{13,19,25,255});uu::display::clear(renderer);
        text(renderer,fonts.large,"画质与显示",80,65);
        text(renderer,fonts.small,"↑ ↓ 选择   ← → 调整   A 保存   B 返回   X 恢复 720p60 高清",80,150,muted);
        const char *labels[]={"分辨率","目标帧率","画质","垂直同步","播放缓冲","码率上限","视频编码"};
        const std::string values[]={resolutionNames[resolution],fps?"60 FPS":"30 FPS",qualityNames[quality],vsync?"开启":"关闭",
            playoutDelay==0?"低延迟（0 ms）":std::to_string(playoutDelay)+" ms",maxBitrate?std::to_string(maxBitrate)+" Mbps":"自动",hevc?"H.265（实验）":"H.264"};
        for(int row=0;row<7;++row) {
            rect(renderer,80,200+row*45,1120,40,row==selected?SDL_Color{32,60,57,255}:SDL_Color{28,40,49,255});
            text(renderer,fonts.normal,labels[row],104,205+row*45,muted);
            text(renderer,fonts.normal,"〈  "+values[row]+"  〉",650,205+row*45,white);
        }
        text(renderer,fonts.small,"码率上限范围 1–50 Mbps；自动由电脑调配，实际码率随画面和网络变化。",80,520,muted);
        text(renderer,fonts.small,displayError.empty()?"画质优先在线更新；编码切换、关闭上限或不兼容时会重连。":displayError,80,553,
             displayError.empty()?muted:SDL_Color{255,157,141,255});
        rect(renderer,80,590,1120,65,accent);
        text(renderer,fonts.normal,"A  保存设置",110,608,{13,19,25,255});
        uu::display::present(renderer);SDL_Delay(16);
    }
}
struct Controls {
    bool confirm = false, back = false, refresh = false, logout = false, up = false, down = false,
         help = false, disconnect = false, exit = false, stats = false, quality = false, keyboard=false,left=false,right=false;
    float mx = 0, my = 0, scroll = 0;
    uint64_t held = 0;
};
#ifdef __SWITCH__
struct FocusHook {
    AppletHookCookie cookie{};
    explicit FocusHook(uu::Client &client) {
        appletHook(
            &cookie,
            [](AppletHookType type, void *data) {
                if ((type == AppletHookType_OnFocusState &&
                     appletGetFocusState() != AppletFocusState_InFocus) ||
                    type == AppletHookType_OnExitRequest) {
                    static_cast<uu::Client *>(data)->command("release");
                }
            },
            &client);
    }
    ~FocusHook() {
        appletUnhook(&cookie);
    }
};
#endif
} // namespace

int main(int, char **) {
#ifdef __SWITCH__
    if (appletGetAppletType() != AppletType_Application &&
        appletGetAppletType() != AppletType_SystemApplication) {
        consoleInit(nullptr);
        printf("UU Remote NS requires full application mode.\nHold R while launching a game, then start this "
               "NRO.\nPress + to exit.\n");
        PadState p;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&p);
        while (appletMainLoop()) {
            padUpdate(&p);
            if (padGetButtonsDown(&p) & HidNpadButton_Plus)
                break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
        return 0;
    }
    // Reserve the BSD backing pool before sockets are created. Raising only
    // SO_RCVBUF later failed with ENOBUFS and left the video socket at 0xA500.
    // Keep TCP, session count and efficiency defaults; libnx sizes the backing
    // transfer memory from this configuration (about 10MiB with current SDK).
    SocketInitConfig streamSockets = *socketGetDefaultInitConfig();
    streamSockets.udp_rx_buf_size = 1u << 20;
    streamSockets.udp_tx_buf_size = 1u << 20;
    Result socketResult = socketInitialize(&streamSockets);
    if (R_FAILED(socketResult)) {
        uu::diagnostic(("bsd-stream-pool-init-failed-" + std::to_string(socketResult)).c_str());
        // A bounded fallback keeps login usable if the larger pool cannot be
        // allocated. Actual per-socket telemetry will expose the smaller pool.
        socketResult = socketInitializeDefault();
        if (R_SUCCEEDED(socketResult)) uu::diagnostic("bsd-stream-pool-default-fallback");
    } else uu::diagnostic("bsd-stream-pool-udp-rx-tx-1048576");
    nifmInitialize(NifmServiceType_User);
    sslInitialize(0);
    romfsInit();
    plInitialize(PlServiceType_User);
    setsysReady = R_SUCCEEDED(setsysInitialize());
    if (setsysReady) {
        if (R_FAILED(setsysGetProductModel(&productModel)))
            productModel = SetSysProductModel_Invalid;
        SetSysFirmwareVersion version{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&version))) {
            char text[32];
            std::snprintf(text, sizeof text, "%u.%u.%u", unsigned(version.major),
                          unsigned(version.minor), unsigned(version.micro));
            firmwareText = text;
        }
    }
    switch (productModel) {
    case SetSysProductModel_Iowa:
        productModelText = "Switch V2";
        break;
    case SetSysProductModel_Hoag:
        productModelText = "Switch Lite";
        break;
    case SetSysProductModel_Aula:
        productModelText = "Switch OLED";
        break;
    default:
        productModelText = "Switch";
        break;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    appletSetMediaPlaybackState(true);
#endif
    int result = 0;
    try {
#ifdef __SWITCH__
        if (R_FAILED(socketResult))
            throw std::runtime_error("Network initialization failed (code " + std::to_string(socketResult) + ")");
#endif
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
            throw std::runtime_error(SDL_GetError());
        if (TTF_Init() != 0)
            throw std::runtime_error(TTF_GetError());
        curl_global_init(CURL_GLOBAL_DEFAULT);
        uu::ensureDefaultConfig();
        uu::ConfigStore::instance().reload();
        {
#ifdef __SWITCH__
            // Docked/TV mode uses a 1080p window; handheld (and Switch Lite,
            // which is always handheld) uses the native 720p panel. The
            // SDL_WINDOW_RESIZABLE flag lets the devkitPro SDL2 driver resize
            // the window automatically when the console is docked/undocked.
            // The renderer keeps a 1280x720 logical size so all UI coordinates,
            // touch mapping and letterbox math stay unchanged.
            const bool tvMode = appletGetOperationMode() == AppletOperationMode_Console;
            const int windowWidth = tvMode ? 1920 : 1280;
            const int windowHeight = tvMode ? 1080 : 720;
            const Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#else
            const int windowWidth = 1280;
            const int windowHeight = 720;
            const Uint32 windowFlags = SDL_WINDOW_SHOWN;
#endif
            // Switch SDL2 creates an EGL surface even for a plain window.
            // A Deko swapchain must exclusively own NWindow, so do not create
            // an SDL window in native mode. Fonts use an offscreen renderer.
            const bool useNativeDisplay=uu::display::nativeRequested();
            uu::diagnostic(useNativeDisplay?"display-select-native":"display-select-sdl");
            SDL_Window *window = nullptr;
            if (!useNativeDisplay) {
                window = SDL_CreateWindow("UU Remote NS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          windowWidth, windowHeight, windowFlags);
                if (!window) throw std::runtime_error(SDL_GetError());
            }
            auto *renderer = uu::display::createRenderer(window,windowWidth,windowHeight);
            if (!renderer)
                throw std::runtime_error(SDL_GetError());
            SDL_RenderSetLogicalSize(renderer, 1280, 720);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            Fonts fonts;
#ifdef __SWITCH__
            std::string key = read("romfs:/signing.key");
            const std::string profile = "sdmc:/switch/UURemote-ns/client-id";
            mkdir("sdmc:/switch/UURemote-ns", 0777);
#ifdef __SWITCH__
            {
                u32 address = 0;
                if (R_SUCCEEDED(nifmGetCurrentIpAddress(&address))) {
                    char text[48];
                    std::snprintf(text, sizeof text, "ip-%u.%u.%u.%u", address & 0xFF,
                                  (address >> 8) & 0xFF, (address >> 16) & 0xFF, (address >> 24) & 0xFF);
                    uu::diagnostic(text);
                }
                int probe = socket(AF_INET, SOCK_DGRAM, 0);
                if (probe >= 0) {
                    int value = 0;
                    socklen_t length = sizeof value;
                    if (getsockopt(probe, SOL_SOCKET, SO_RCVBUF, &value, &length) == 0)
                        uu::diagnostic(("bsd-rcvbuf-default-" + std::to_string(value)).c_str());
                    for (int wanted : {1 << 20, 8 << 20}) {
                        int size = wanted;
                        if (setsockopt(probe, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) != 0)
                            uu::diagnostic(("bsd-rcvbuf-setfail-" + std::to_string(wanted)).c_str());
                        value = 0;
                        length = sizeof value;
                        if (getsockopt(probe, SOL_SOCKET, SO_RCVBUF, &value, &length) == 0)
                            uu::diagnostic(("bsd-rcvbuf-set-" + std::to_string(wanted) + "-" +
                                            std::to_string(value))
                                               .c_str());
                    }
                    close(probe);
                }
            }
#endif
#else
            std::string key = read("romfs/signing.key");
            const std::string profile = "client-id";
#endif
            if (key.size() != 24)
                throw std::runtime_error("Missing signing.key: run scripts/extract_key.py and rebuild");
            uu::diagnostic("app-start-" UU_VERSION);
            uu::diagnostic("ui-font-source-memory");
#ifdef UU_SRTP_MBEDTLS
            uu::diagnostic("srtp-payload-backend-mbedtls");
#ifdef UU_SWITCH_BATCH_RX
            uu::diagnostic("udp-receive-mode-batch-experimental");
#else
            uu::diagnostic("udp-receive-mode-single");
#endif
#else
            uu::diagnostic("srtp-payload-backend-portable");
#endif
#ifdef __SWITCH__
            uu::diagnostic(("console-" + productModelText + "-" + operationModeLabel() + "-fw" +
                            (firmwareText.empty() ? std::string("?") : firmwareText))
                               .c_str());
#endif
            std::set_terminate(uu::terminateWithDiagnostics);
            rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel, const std::string &message) {
                const auto event = uu::rtcLogEvent(message);
                if (!event.empty())
                    uu::diagnostic(event.c_str());
            });
            std::string id = read(profile);
            if (id.size() != 36) {
                id = uu::uuid();
                std::ofstream(profile) << id;
            }
            auto media = std::make_shared<uu::Media>();
            {
                uu::Client client(key, id, media);
                SDL_RendererInfo rendererInfo{};
                SDL_GetRendererInfo(renderer,&rendererInfo);
                bool nativeNv12=false;
                for(Uint32 i=0;i<rendererInfo.num_texture_formats;++i)
                    nativeNv12|=rendererInfo.texture_formats[i]==SDL_PIXELFORMAT_NV12;
                if(nativeNv12) {
                    auto *probe=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_NV12,SDL_TEXTUREACCESS_STREAMING,2,2);
                    nativeNv12=probe!=nullptr;
                    if(probe) uu::display::destroyTexture(probe);
                }
                media->setYuvOutput(nativeNv12);
                media->setNativeOutput(uu::display::active());
                uu::diagnostic(nativeNv12?"display-native-nv12-enabled":"display-native-nv12-unavailable-rgba");
                uu::diagnostic(uu::display::active()?"display-native-output=1":"display-native-output=0");
                uu::LiveTelemetry telemetry;
                Uint32 lastCpuLedgerSample=0;
                Uint32 texFormat=0;
                bool actualVsync=(rendererInfo.flags & SDL_RENDERER_PRESENTVSYNC)!=0;
                const bool requestedVsync=client.snapshot().vsync;
                if(uu::display::vsync(renderer,requestedVsync?1:0)==0) actualVsync=requestedVsync;
                uu::diagnostic(actualVsync?"display-vsync-on":"display-vsync-off");
#ifdef __SWITCH__
                FocusHook focusHook(client);
#endif
                bool running = true, overlay = false, wasStreaming = false, touchHeld = false,
                     statsVisible = false, persistentNetwork = false;
                int selected = 0, texW = 0, texH = 0;
                uint64_t serial = 0, lastHeld = 0;
                // One transient NV12 import failure must not permanently switch
                // the session to the CPU conversion path. Retry a few frames,
                // then latch the existing fallback for this session.
                unsigned nativeImportFailures = 0;
                bool nativeFallback = false;
                unsigned lastArrowMask=0;
                bool nativePadRequested=false,plusPending=false,startPulse=false,minusPending=false,selectPulse=false;
                Uint32 nativePadSentAt=0,plusAt=0,minusAt=0;std::string lastNativePad;
                bool homePlusPending=false;Uint32 homePlusAt=0;
                auto padPacket=[&](uint32_t buttons,int lx,int ly,int rx,int ry,int lt,int rt){
                    return uu::Json{{"action","gpd_update"},{"index",0},{"buttons",buttons},{"left_trigger",lt},{"right_trigger",rt},
                        {"left_thumb_x",lx},{"left_thumb_y",ly},{"right_thumb_x",rx},{"right_thumb_y",ry}};
                };
                StreamTouch streamTouch;NativeTouch nativeTouch;PhoneMouse phoneMouse;
                float cursorX = .5f, cursorY = .5f, focusY = 230;
                Uint32 previous = SDL_GetTicks(), lastScroll = 0, lastPointer = 0;
                Uint32 presentWindow=SDL_GetTicks();
                Uint32 streamingSince=0;
                uint64_t presentedFrames=0;
                double presentFps=-1;
                const double counterToUs=1000000.0/SDL_GetPerformanceFrequency();
                // SDL swap completion is an estimate, not a hardware scanout
                // timestamp. Only trust a stable observed refresh cadence.
                uint64_t previousSwap=0;
                double refreshPeriodMs=0,leadSumMs=0;
                unsigned irregularSwaps=0;
                unsigned stableSwaps=0;
                uint64_t leadSamples=0;
                double cpuUs=0,uploadUs=0,presentUs=0,maxCpuUs=0;
                uint64_t renderLoops=0,uploadCount=0,slowLoops=0;
                uint64_t lastDecoded=0,lastPublished=0,lastSelected=0,lastEmpty=0,lastNoReady=0,lastFuture=0;
                uu::StreamStatsSnapshot panelStats; Uint32 panelStatsAt=0;
                SDL_Texture *video = nullptr;
                std::string qrUrl;
                std::unique_ptr<qrcodegen::QrCode> qr;
                std::unique_ptr<SDL_Texture,decltype(&uu::display::destroyTexture)>
                    qrTexture(nullptr,uu::display::destroyTexture);
                SmsForm sms;
                SDL_Rect viewport{0, 0, 1280, 720};
                auto send = [&](const uu::Json &data) { client.command("input", data); };
                tvui::LiveFrontend liveUi(client,media,renderer);
                // Restore a validated local session first; scan only when absent/revoked.
                client.command("startup");
                while (running) {
                    const auto loopStarted=SDL_GetPerformanceCounter();
#ifdef __SWITCH__
                    if (!appletMainLoop())
                        break;
#endif
                    Uint32 now = SDL_GetTicks();
                    float dt = std::min((now - previous) / 1000.f, .05f);
                    previous = now;
                    auto state = client.snapshot();
                    liveUi.sync(state);
                    if(!state.qualityPending && actualVsync!=state.vsync && uu::display::vsync(renderer,state.vsync?1:0)==0)
                        actualVsync=state.vsync;
                    selected = std::clamp(selected, 0, std::max(0, int(state.devices.size()) - 1));
                    Controls c;
                    SDL_Event event;
                    while (SDL_PollEvent(&event)) {
                        if (event.type == SDL_QUIT)
                            running = false;
                        liveUi.event(event);
                        if (event.type == SDL_WINDOWEVENT &&
                            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                            client.command("release");
                            streamTouch.cancel(send);nativeTouch.cancel(send);phoneMouse.cancel(send);minusPending=false;homePlusPending=false;
                            touchHeld = false;
                        }
                        if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                            auto k = event.key.keysym.sym;
                            c.confirm |= k == SDLK_RETURN;
                            c.back |= k == SDLK_ESCAPE;
                            c.refresh |= k == SDLK_y;
                            c.logout |= k == SDLK_x;
                            c.up |= k == SDLK_UP;
                            c.down |= k == SDLK_DOWN;
                            c.left |= k==SDLK_LEFT;c.right |= k==SDLK_RIGHT;
                            c.help |= k == SDLK_F1;
                            c.disconnect |= k == SDLK_F2;
                            c.stats |= k == SDLK_F3 || k == SDLK_F5;
                            c.quality |= k == SDLK_F4;
                            c.exit |= k == SDLK_F10;
                        }
#ifndef __SWITCH__
                        if (state.streaming && !liveUi.captures() &&
                            (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN ||
                             event.type == SDL_MOUSEBUTTONUP)) {
                            if (event.type == SDL_MOUSEMOTION) {
                                cursorX =
                                    std::clamp((event.motion.x - viewport.x) / float(viewport.w), 0.f, 1.f);
                                cursorY =
                                    std::clamp((event.motion.y - viewport.y) / float(viewport.h), 0.f, 1.f);
                                send({{"action", "mouse_move_absolute"},
                                      {"abs_x", cursorX},
                                      {"abs_y", cursorY}});
                            } else
                                send({{"action",
                                       event.type == SDL_MOUSEBUTTONDOWN ? "mouse_press" : "mouse_release"},
                                      {"button", event.button.button == SDL_BUTTON_LEFT ? 1 : 2}});
                        }
#endif
                    }
#ifdef __SWITCH__
                    padUpdate(&pad);
                    uint64_t pressed = padGetButtonsDown(&pad);
                    c.held = padGetButtons(&pad);
                    const bool minusHeld=(c.held&HidNpadButton_Minus)!=0;selectPulse=false;
                    if(state.streaming && !liveUi.active()){
                        if(pressed&HidNpadButton_Minus){minusPending=true;minusAt=now;}
                        if(minusPending && minusHeld && now-minusAt>=650){liveUi.toggleInputMode();minusPending=false;plusPending=startPulse=false;}
                        if(minusPending && !minusHeld){selectPulse=true;minusPending=false;}
                        c.held&=~uint64_t(HidNpadButton_Minus);
                        if(selectPulse && !liveUi.gamepadMode())c.held|=HidNpadButton_Minus;
                    }else minusPending=false;
                    c.confirm |= pressed & HidNpadButton_A;
                    c.back |= pressed & HidNpadButton_B;
                    c.refresh |= pressed & HidNpadButton_Y;
                    c.logout |= pressed & HidNpadButton_X;
                    c.up |= pressed & HidNpadButton_Up;
                    c.down |= pressed & HidNpadButton_Down;
                    c.left|=pressed&HidNpadButton_Left;c.right|=pressed&HidNpadButton_Right;
                    // Exit only from the home page and only after a fresh hold.
                    // Leave the normal scope so Client/Media/GPU cleanup runs
                    // before libnx returns control to the launching loader.
                    const bool homeExitAllowed=state.authenticated && !state.streaming && liveUi.atHome();
                    if(homeExitAllowed) {
                        if(pressed&HidNpadButton_Plus){homePlusPending=true;homePlusAt=now;}
                        if(!(c.held&HidNpadButton_Plus))homePlusPending=false;
                        if(homePlusPending && Uint32(now-homePlusAt)>=850) {
                            uu::diagnostic("home-hold-plus-normal-exit");
                            running=false;
                            break;
                        }
                    } else homePlusPending=false;
                    if(!homeExitAllowed && !liveUi.gamepadMode())c.help |= pressed & HidNpadButton_Plus;
                    else if(state.streaming){
                        if(pressed&HidNpadButton_Plus){plusPending=true;plusAt=now;}
                        if(plusPending && (c.held&HidNpadButton_Plus) && now-plusAt>=600){c.help=true;plusPending=false;}
                        if(plusPending && !(c.held&HidNpadButton_Plus)){startPulse=true;plusPending=false;}
                    }
                    // R3 belongs to the active keyboard mapping; network details are in More.
                    if(!liveUi.gamepadMode())c.keyboard |= pressed & HidNpadButton_StickL;
                    // Minus is mapped to a remote key; disconnect is confirmed in the toolbar.
                    c.exit |= !state.authenticated && !state.streaming && (pressed & HidNpadButton_Plus);
                    auto stick = padGetStickPos(&pad, 0), right = padGetStickPos(&pad, 1);
                    auto axis = [](int v) {
                        float n = v / 32767.f;
                        return std::abs(n) > .15f ? std::copysign((std::abs(n) - .15f) / .85f, n) : 0.f;
                    };
                    c.mx = liveUi.mapping()==1?axis(stick.x):axis(right.x);
                    c.my = liveUi.mapping()==1?-axis(stick.y):-axis(right.y);
                    c.scroll = liveUi.mapping()==1?(float(bool(c.held&HidNpadButton_ZL))-float(bool(c.held&HidNpadButton_ZR))):axis(stick.y);
#endif
                    bool busy = state.stage != "idle" && state.stage != "error" && state.stage != "login" &&
                                state.stage != "sms";
                    bool menuTouchHeld=false;
#ifdef __SWITCH__
                    HidTouchScreenState menuTouch{};hidGetTouchScreenStates(&menuTouch,1);
                    menuTouchHeld=menuTouch.count!=0;
                    if(liveUi.active())liveUi.pointer(menuTouchHeld,menuTouchHeld?int(menuTouch.touches[0].x):0,menuTouchHeld?int(menuTouch.touches[0].y):0);
#endif
                    bool captureMenuInput=false;
                    if(state.authenticated){
                        tvui::LiveInput input;input.confirm=c.confirm;input.cancel=c.back;input.up=c.up;input.down=c.down;input.left=c.left;input.right=c.right;
                        input.toolbar=c.help;input.network=c.stats;input.qualityMenu=c.quality;input.keyboard=c.keyboard;input.disconnect=c.disconnect;input.held=c.held;
                        captureMenuInput=liveUi.input(input,state,menuTouchHeld);
                        if(captureMenuInput){c.confirm=c.back=c.refresh=c.logout=c.up=c.down=c.help=c.stats=c.quality=c.keyboard=c.disconnect=false;c.mx=c.my=c.scroll=0;c.held=0;}
                        std::string draft;
                        if(liveUi.takeKeyboardEdit(draft)){
                            streamTouch.cancel(send);nativeTouch.cancel(send);phoneMouse.cancel(send);
                            client.command("release");
#ifdef __SWITCH__
                            liveUi.finishKeyboardEdit(textInput(draft));
#else
                            // Desktop host uses its IME in the single draft field.
                            SDL_StartTextInput();
#endif
                            previous=SDL_GetTicks();continue;
                        }
                        c.exit=false;
                    }
                    persistentNetwork=liveUi.networkOverlay;
                    if (c.exit)
                        running = false;
                    if (state.streaming) {
                        const bool captureNetworkInput=captureMenuInput;
                        if (c.disconnect) {
                            client.command("disconnect");
                            continue;
                        }
                        if(liveUi.gamepadMode()) {
#ifdef __SWITCH__
                            if(!nativePadRequested){send({{"action","gpd_connect"},{"index",0}});nativePadRequested=true;lastNativePad.clear();}
                            uint32_t mask=0;
                            const auto physical=captureMenuInput?uint64_t(0):c.held;
                            for(auto [button,bit]:std::initializer_list<std::pair<uint64_t,uint32_t>>{
                                {HidNpadButton_Up,1},{HidNpadButton_Down,2},{HidNpadButton_Left,4},{HidNpadButton_Right,8},
                                {HidNpadButton_StickL,64},{HidNpadButton_StickR,128},
                                {HidNpadButton_L,256},{HidNpadButton_R,512},
                                {HidNpadButton_B,liveUi.xboxByLetter()?8192:4096},{HidNpadButton_A,liveUi.xboxByLetter()?4096:8192},{HidNpadButton_Y,liveUi.xboxByLetter()?32768:16384},{HidNpadButton_X,liveUi.xboxByLetter()?16384:32768}})if(physical&button)mask|=bit;
                            if(startPulse && !captureMenuInput)mask|=16;
                            if(selectPulse && !captureMenuInput)mask|=32;
                            auto nativeAxis=[&](int v){return captureMenuInput?0:int(std::round(axis(v)*32767));};
                            const auto packet=padPacket(mask,nativeAxis(stick.x),nativeAxis(stick.y),nativeAxis(right.x),nativeAxis(right.y),physical&HidNpadButton_ZL?255:0,physical&HidNpadButton_ZR?255:0);
                            const auto encoded=packet.dump();
                            if(encoded!=lastNativePad || now-nativePadSentAt>=500){send(packet);lastNativePad=encoded;nativePadSentAt=now;}
                            startPulse=false;
#endif
                            c.held=0;c.mx=c.my=c.scroll=0;
                        }else if(nativePadRequested){send(padPacket(0,0,0,0,0,0,0));send({{"action","gpd_disconnect"},{"index",0}});nativePadRequested=false;}
                        cursorX = std::clamp(cursorX + c.mx * dt * liveUi.pointerSpeed(), 0.f, 1.f);
                        cursorY = std::clamp(cursorY + c.my * dt * liveUi.pointerSpeed(), 0.f, 1.f);
                        if ((c.mx || c.my) && now - lastPointer >= 16) {
                            send({{"action", "mouse_move_absolute"}, {"abs_x", cursorX}, {"abs_y", cursorY}});
                            lastPointer = now;
                        }
                        if (c.scroll && now - lastScroll > 80) {
                            send({{"action", "mouse_scroll"},
                                  {"delta_x", 0},
                                  {"delta_y", int(c.scroll * 120)}});
                            lastScroll = now;
                        }
#ifdef __SWITCH__
                        for (auto [button, keycode] :
                             std::initializer_list<std::pair<uint64_t, int>>{{HidNpadButton_A, 13},{HidNpadButton_B, 27},
                                {HidNpadButton_X, liveUi.mapping()==1?0:32},{HidNpadButton_Y,liveUi.mapping()==1?0:8},
                                {HidNpadButton_L,liveUi.mapping()==1?18:17},{HidNpadButton_R,liveUi.mapping()==1?9:18},
                                {HidNpadButton_Up,liveUi.mapping()==1?17:38},{HidNpadButton_Down,liveUi.mapping()==1?16:40},
                                {HidNpadButton_Left,liveUi.mapping()==1?8:37},{HidNpadButton_Right,liveUi.mapping()==1?46:39},
                                {HidNpadButton_Minus,liveUi.mapping()==1?91:9},{HidNpadButton_StickR,liveUi.mapping()==1?0:16}}) {
                            if (keycode && (c.held & button) != (lastHeld & button))
                                send({{"action", (c.held & button) ? "kbd_press" : "kbd_release"},
                                      {"key", keycode}});
                        }
                        for (auto [button, mouse] : std::initializer_list<std::pair<uint64_t, int>>{
                                 {HidNpadButton_ZR, liveUi.mapping()==1?0:1}, {HidNpadButton_ZL, liveUi.mapping()==1?0:2},
                                 {HidNpadButton_X,liveUi.mapping()==1?1:0},{HidNpadButton_Y,liveUi.mapping()==1?2:0}})
                            if (mouse && (c.held & button) != (lastHeld & button))
                                send({{"action", (c.held & button) ? "mouse_press" : "mouse_release"},
                                      {"button", mouse}});
                        unsigned arrowMask=0;
                        if(liveUi.mapping()==1 && !liveUi.gamepadMode() && !captureNetworkInput){
                            arrowMask=(axis(right.x)<-.5f?1u:0u)|(axis(right.x)>.5f?2u:0u)|(axis(right.y)>.5f?4u:0u)|(axis(right.y)<-.5f?8u:0u);
                            if((c.held&HidNpadButton_StickR) && !(lastHeld&HidNpadButton_StickR)){
                                send({{"action","kbd_press"},{"key",91}});send({{"action","kbd_press"},{"key",68}});
                                send({{"action","kbd_release"},{"key",68}});send({{"action","kbd_release"},{"key",91}});
                            }
                        }
                        const int arrowKeys[]={37,39,38,40};
                        for(unsigned i=0;i<4;++i)if((arrowMask&(1u<<i))!=(lastArrowMask&(1u<<i)))send({{"action",arrowMask&(1u<<i)?"kbd_press":"kbd_release"},{"key",arrowKeys[i]}});
                        lastArrowMask=arrowMask;
                        HidTouchScreenState touch{};
                        hidGetTouchScreenStates(&touch, 1);
                        if(liveUi.takeZoomReset())streamTouch.resetZoom();
                        if(liveUi.multitouch()){
                            phoneMouse.cancel(send);
                            streamTouch.cancel(send);streamTouch.resetZoom();
                            nativeTouch.update(touch,now,captureNetworkInput,viewport,send);
                        }else{
                            nativeTouch.cancel(send);
                            streamTouch.cancel(send);streamTouch.resetZoom();
                            phoneMouse.update(touch,now,captureNetworkInput,liveUi.pointerSpeed(),viewport,cursorX,cursorY,send);
                        }
#else
                        if (c.confirm) {
                            send({{"action", "kbd_press"}, {"key", 13}});
                            send({{"action", "kbd_release"}, {"key", 13}});
                        }
                        if (c.back) {
                            send({{"action", "kbd_press"}, {"key", 27}});
                            send({{"action", "kbd_release"}, {"key", 27}});
                        }
#endif
                    } else {
                        nativePadRequested=false;lastNativePad.clear();plusPending=startPulse=false;
                        if (wasStreaming) {
                            streamTouch.cancel(send);streamTouch.resetZoom();nativeTouch.cancel(send);phoneMouse.cancel(send);minusPending=false;
                            uu::display::resetVideo();
                            if (video) {
                                uu::display::destroyTexture(video);
                                video = nullptr;
                            }
                            serial = 0;
                            texW = texH = 0;
                            nativeImportFailures = 0;
                            nativeFallback = false;
                            overlay = false;
                            touchHeld = false;
                        }
                        if (state.authenticated) { /* live frontend owns authenticated navigation */ }
                        else if (!state.authenticated && c.logout && (!busy || state.stage == "login")) {
                            sms = SmsForm{};
                            client.command(state.loginMode == "sms" ? "login" : "sms-open");
                        } else if (c.back || c.disconnect) {
                            sms = SmsForm{};
                            client.command("cancel");
                        } else if (c.logout && state.authenticated)
                            client.command("logout");
                        else if (c.refresh && state.authenticated && !busy)
                            client.command("refresh");
                        else if (!state.authenticated && state.loginMode == "sms") {
                            if (!busy) {
                                if (c.up)
                                    sms.selected = std::max(0, sms.selected - 1);
                                if (c.down)
                                    sms.selected = std::min(4, sms.selected + 1);
                                if (c.confirm) {
                                    if (sms.selected < 3) {
                                        auto *value = sms.selected == 0   ? &sms.country
                                                      : sms.selected == 1 ? &sms.mobile
                                                                          : &sms.code;
                                        const char *title = sms.selected == 0   ? "国家或地区区号"
                                                            : sms.selected == 1 ? "手机号码"
                                                                                : "6 位短信验证码";
                                        auto input = numericInput(title, *value,
                                                                  sms.selected == 0   ? 1
                                                                  : sms.selected == 1 ? 4
                                                                                      : 6,
                                                                  sms.selected == 0   ? 4
                                                                  : sms.selected == 1 ? 15
                                                                                      : 6);
                                        if (input) {
                                            *value = *input;
                                            sms.selected = sms.selected == 1 ? 3 : sms.selected == 2 ? 4 : 1;
                                        }
#ifdef __SWITCH__
                                        padUpdate(&pad);
                                        lastHeld = padGetButtons(&pad);
#endif
                                    } else if (sms.selected == 3 && uu::monotonicMs() >= state.smsRetryAt) {
                                        client.command("sms-code", sms.credentials());
                                        sms.selected = 2;
                                    } else if (sms.selected == 4)
                                        client.command("sms-login", sms.credentials());
                                }
                            }
                        } else if (!busy && state.stage != "login") {
                            if (c.up)
                                selected = std::max(0, selected - 1);
                            if (c.down)
                                selected = std::min(std::max(0, int(state.devices.size()) - 1), selected + 1);
                            if (c.confirm) {
                                if (!state.authenticated)
                                    client.command("login");
                                else if (state.devices.empty())
                                    client.command("refresh");
                                else if (state.devices[selected].value("status", std::string{}) != "offline")
                                    client.command("connect",
                                                   {{"deviceId", state.devices[selected]["device_id"]}});
                            }
                        }
                    }
                    lastHeld = c.held;
                    if(state.streaming && !wasStreaming) streamingSince=SDL_GetTicks();
                    if(!state.streaming) streamingSince=0;
                    wasStreaming = state.streaming;
                    color(renderer, {13, 19, 25, 255});
                    uu::display::clear(renderer);
                    bool submittedNewFrame=false;
                    double frameUploadUs=0;
                    if (state.streaming) {
                        int leadMs=0;
                        if(actualVsync && stableSwaps>=6 && previousSwap && refreshPeriodMs>0) {
                            const auto ageMs=(SDL_GetPerformanceCounter()-previousSwap)*counterToUs/1000.0;
                            leadMs=int(std::ceil(std::clamp(refreshPeriodMs-ageMs,0.0,20.0)));
                        }
                        leadSumMs+=leadMs;++leadSamples;
                        auto p = media->picture(serial,leadMs);
                        if(p.serial && uu::display::active()) {
                            const auto started=SDL_GetPerformanceCounter();
                            const bool accepted=uu::display::acceptVideo(p);
                            bool consume=true;
                            if(accepted) {
                                nativeImportFailures=0;
                                submittedNewFrame=true;++uploadCount;
                            } else if(p.hardwareFrame) {
                                if(nativeFallback) {
                                    // Already latched for this session: consume the
                                    // hardware frame and let the decoder produce
                                    // CPU-readable frames from now on.
                                } else if(++nativeImportFailures>=6) {
                                    nativeFallback=true;
                                    media->setNativeOutput(false);
                                    uu::diagnostic("display-deko-import-failed-cpu-fallback");
                                } else {
                                    // Retry the same newest frame next vsync instead
                                    // of discarding it on a transient import error.
                                    consume=false;
                                }
                            }
                            frameUploadUs=(SDL_GetPerformanceCounter()-started)*counterToUs;uploadUs+=frameUploadUs;
                            if(consume) {
                                serial=p.serial;texFormat=p.pixelFormat;
                                const float scale=std::min(1280.f/p.width,720.f/p.height);
                                viewport={int((1280-p.width*scale)/2),int((720-p.height*scale)/2),
                                          int(p.width*scale),int(p.height*scale)};
                            }
                        }
                        if (p.serial && !uu::display::active()) {
                            if (!video || p.width != texW || p.height != texH || p.pixelFormat!=texFormat) {
                                if (video)
                                    uu::display::destroyTexture(video);
                                video = SDL_CreateTexture(renderer, p.pixelFormat,
                                                          SDL_TEXTUREACCESS_STREAMING, p.width, p.height);
                                texFormat=p.pixelFormat;
                                if(!video && p.pixelFormat==SDL_PIXELFORMAT_NV12) {
                                    media->setYuvOutput(false);
                                    uu::diagnostic("display-nv12-create-failed-rgba-fallback");
                                }
                                texW = p.width;
                                texH = p.height;
                            }
                            const auto uploadStarted=SDL_GetPerformanceCounter();
                            int uploadResult=-1;
                            if(video && p.pixels) {
                                if(p.pixelFormat==SDL_PIXELFORMAT_NV12) {
                                    SDL_SetYUVConversionMode(p.yuvMode);
                                    uploadResult=SDL_UpdateNVTexture(video,nullptr,p.pixels->data(),p.width,
                                        p.pixels->data()+size_t(p.width)*p.height,p.width);
                                } else uploadResult=SDL_UpdateTexture(video,nullptr,p.pixels->data(),p.width*4);
                            }
                            if(uploadResult==0) {
                                submittedNewFrame=true;++uploadCount;
                            } else if(p.pixelFormat==SDL_PIXELFORMAT_NV12) {
                                media->setYuvOutput(false);
                                uu::diagnostic("display-nv12-update-failed-rgba-fallback");
                            }
                            frameUploadUs=(SDL_GetPerformanceCounter()-uploadStarted)*counterToUs;
                            uploadUs+=frameUploadUs;
                            serial = p.serial;
                            float scale = std::min(1280.f / p.width, 720.f / p.height);
                            viewport = {int((1280 - p.width * scale) / 2), int((720 - p.height * scale) / 2),
                                        int(p.width * scale), int(p.height * scale)};
                        }
                        const auto touchView=streamTouch.view(viewport);
                        if(uu::display::active()) uu::display::drawVideo(touchView);
                        else if (video)
                            uu::display::copy(renderer, video, nullptr, &touchView);
                        else
                            text(renderer, fonts.normal, "等待电脑发送画面...", 470, 330, muted);

                        if (overlay) {
                            std::string hints =
                                "左摇杆 鼠标  ·  ZR/ZL 点击  ·  A 确认  ·  B Esc";
#ifdef __SWITCH__
                            if (appletGetOperationMode() == AppletOperationMode_Console)
                                hints += "  ·  TV 模式触屏不可用";
#endif
                            rect(renderer, 0, 0, 1280, 68, {13, 19, 25, 225});
                            text(renderer, fonts.normal, "YOUYOU远程", 28, 18);
                            text(renderer, fonts.small, hints, 200, 24, muted);
                            rect(renderer, 0, 650, 1280, 70, {13, 19, 25, 225});
                            text(renderer, fonts.small,
                                 "方向键 WASD  ·  X 空格  ·  Y Tab  ·  L Ctrl  ·  R Shift  ·  L3 画质设置",
                                 28, 665, muted);
                            text(renderer, fonts.small, media->status()+"  ·  垂直同步 "+(actualVsync?"开":"关"), 28, 694, muted);
                        }
                        if(!state.qualityNotice.empty()) {
                            rect(renderer,300,72,900,38,{13,19,25,230});
                            text(renderer,fonts.small,state.qualityNotice,314,80,accent,870);
                        }
                        if (liveUi.active() || persistentNetwork) {
                            const auto now=SDL_GetTicks();
                            if(!panelStatsAt || Uint32(now-panelStatsAt)>=250) {
                                panelStats=client.streamStats();panelStats.presentFps=presentFps;panelStatsAt=now;
                            }
                            if(persistentNetwork && !liveUi.active())compactNetworkInfo(renderer,fonts.hint,panelStats,
                                streamingSince?Uint32((now-streamingSince)/1000):0U);
                        }
                        if(!liveUi.multitouch() && !liveUi.captures())phoneMouse.draw(renderer,fonts);
                        liveUi.draw(renderer,fonts,state,panelStats);
                    } else {
                        if(state.authenticated)liveUi.draw(renderer,fonts,state,panelStats);
                        else {
                        if(!state.authenticated)tvui::loginHeader(renderer,fonts);
                        else tvui::brand(renderer,fonts);
                        text(renderer, fonts.small, state.authenticated ? state.nickname : "尚未登录", 930,
                             55, muted, 280);
                        if(state.authenticated)rect(renderer, 62, 127, 1156, 1, {44, 56, 67, 255});
                        if (state.qrUrl != qrUrl) {
                            qrUrl = state.qrUrl;
                            qr.reset();
                            qrTexture.reset();
                            if (!qrUrl.empty())
                                qr = std::make_unique<qrcodegen::QrCode>(qrcodegen::QrCode::encodeText(
                                    qrUrl.c_str(), qrcodegen::QrCode::Ecc::MEDIUM));
                        }
                        if (!state.authenticated && state.loginMode == "sms") {
                            int remaining=int(std::max<int64_t>(0,(state.smsRetryAt-uu::monotonicMs()+999)/1000));
                            tvui::smsLogin(renderer,fonts,sms,remaining,busy);
                        } else if (state.stage == "login") {
                            tvui::centered(renderer,fonts.normal,"手机扫码登录 YOUYOU远程",116);
                            tvui::centered(renderer,fonts.small,"登录与电脑端相同的 UU远程账号",162,tvui::label);
                            tvui::rounded(renderer,{506,227,268,268},tvui::panel,12);
                            tvui::rounded(renderer,{520,241,240,240},SDL_Color{255,255,255,255},6);
                            tvui::centered(renderer,fonts.small,"请在手机 UU远程 App 中扫码并确认",548,tvui::label);
                            tvui::centered(renderer,fonts.small,"X 切换短信登录   B 取消",620,tvui::label);
                            if (qr) {
                                auto layout = uu::qrGeometry(qr->getSize());
                                // Integer QR modules; no fractional texture scaling.
                                layout.scale=std::max(1,240/(qr->getSize()+8));
                                layout.size=(qr->getSize()+8)*layout.scale;
                                layout.x=(1280-layout.size)/2;layout.y=241+(240-layout.size)/2;
                                // Upload the integer-sized QR once. Thousands of tiny
                                // per-frame draw calls would exhaust native command slots.
                                if(!qrTexture) {
                                    auto *surface=SDL_CreateRGBSurfaceWithFormat(0,layout.size,layout.size,32,SDL_PIXELFORMAT_RGBA32);
                                    if(surface) {
                                        SDL_FillRect(surface,nullptr,SDL_MapRGBA(surface->format,255,255,255,255));
                                        for(int y=0;y<qr->getSize();++y) for(int x=0;x<qr->getSize();++x) {
                                            if(!qr->getModule(x,y)) continue;
                                            SDL_Rect module{(x+layout.border)*layout.scale,(y+layout.border)*layout.scale,layout.scale,layout.scale};
                                            SDL_FillRect(surface,&module,SDL_MapRGBA(surface->format,0,0,0,255));
                                        }
                                        qrTexture.reset(uu::display::textureFromSurface(renderer,surface));
                                        SDL_FreeSurface(surface);
                                    }
                                }
                                if(qrTexture) {
                                    SDL_Rect destination{layout.x,layout.y,layout.size,layout.size};
                                    uu::display::copy(renderer,qrTexture.get(),nullptr,&destination);
                                }
                            } else
                                tvui::centered(renderer,fonts.normal,"正在获取二维码",370,SDL_Color{49,53,60,255});
                        } else if (!state.authenticated) {
                            tvui::centered(renderer,fonts.normal,"在掌机上，连接你的电脑",172);
                            tvui::image(renderer,"device",{576,255,128,128});
                            tvui::centered(renderer,fonts.small,"登录与电脑端相同的 UU远程账号",432,tvui::label);
                            tvui::rounded(renderer,{440,500,400,62},tvui::blue,12);
                            text(renderer,fonts.normal,"A  扫码登录",550,515,tvui::ink);
                            tvui::centered(renderer,fonts.small,"X 使用短信验证码登录",591,tvui::label);
                        } else {
                            text(renderer, fonts.small, "我的电脑", 62, 167, muted);
                            if (!state.devices.empty()) {
                                int first = std::max(0, selected - 3);
                                float target = 223.f + (selected - first) * 88;
                                focusY += (target - focusY) * std::min(1.f, dt * 18);
                                rect(renderer, 62, int(focusY), 535, 78, {32, 50, 53, 255});
                                rect(renderer, 62, int(focusY), 4, 78, accent);
                                for (int i = first; i < std::min(first + 4, int(state.devices.size())); ++i) {
                                    auto &device = state.devices[i];
                                    int y = 232 + (i - first) * 88;
                                    bool online = device.value("status", std::string{}) != "offline";
                                    text(renderer, fonts.normal, device.value("alias", std::string("电脑")),
                                         84, y, white, 470);
                                    text(renderer, fonts.small, online ? "在线" : "离线", 84, y + 35,
                                         online ? accent : muted);
                                }
                            } else {
                                text(renderer, fonts.normal,
                                     state.authenticated ? "暂未发现电脑" : "登录你的 UU 账号", 62, 237);
                                text(renderer, fonts.small,
                                     state.authenticated ? "请在电脑上登录同一账号"
                                                         : "扫码或短信登录，同步远程电脑",
                                     62, 287, muted);
                                rect(renderer, 62, 350, 440, 64, accent);
                                text(renderer, fonts.normal,
                                     state.authenticated ? "Y  刷新设备" : "A  扫码登录", 88, 367,
                                     {13, 19, 25, 255});
                                if (!state.authenticated)
                                    text(renderer, fonts.normal, "X  短信验证码登录", 62, 450, accent);
                            }
                            rect(renderer, 718, 237, 356, 214, {28, 40, 49, 255});
                            rect(renderer, 729, 248, 334, 192, {17, 25, 33, 255});
                            rect(renderer, 886, 451, 20, 45, {44, 56, 67, 255});
                            rect(renderer, 837, 495, 118, 6, {44, 56, 67, 255});
                            text(renderer, fonts.small, "独立连接原版 UU 电脑端", 762, 535, muted);
                        }
                        if (state.authenticated && busy) {
                            float pulse = (std::sin(now / 250.f) + 1) / 2;
                            rect(renderer, 62, 591, 6, 26,
                                 {accent.r, accent.g, accent.b, uint8_t(100 + pulse * 155)});
                        }
                        if (!state.authenticated) {
                            if(!state.detail.empty())tvui::centered(renderer,fonts.small,state.detail,654,
                                state.stage=="error"?SDL_Color{255,157,141,255}:tvui::label);
                        } else
                            statusText(renderer, fonts.small, state.detail,
                                       state.stage == "error" ? SDL_Color{255, 157, 141, 255} : muted);

#ifdef __SWITCH__
                        if(state.authenticated)text(renderer, fonts.small, deviceSummary(), 62, 105, {118, 122, 130, 255}, 1050);
#endif
                    }
                    }
                    const auto presentStarted=SDL_GetPerformanceCounter();
                    uu::display::present(renderer);
                    const auto presentFinished=SDL_GetPerformanceCounter();
                    // Some handheld/native-window combinations accept swap interval
                    // 1 but do not block presentImage at 60 Hz (observed as a 120 Hz
                    // UI loop with pacing-refresh=0). Cap the loop at 60 Hz only when
                    // vsync is requested and the measured loop is clearly faster;
                    // a working 60 Hz present path is left untouched.
                    const double loopElapsedUs=(presentFinished-loopStarted)*counterToUs;
                    static double nextFrameDeadlineUs=0.0;
                    if(state.vsync && loopElapsedUs < 14000.0) {
                        static bool frameLimitLogged=false;
                        if(!frameLimitLogged){uu::diagnostic("display-vsync-framelimit-engaged");uu::diagnostic("display-vsync-precise-pacing");frameLimitLogged=true;}
                        constexpr double targetFrameUs=16666.6666667;
                        const double nowUs=presentFinished*counterToUs;
                        if(nextFrameDeadlineUs<=0.0 || nowUs>nextFrameDeadlineUs+targetFrameUs*3.0)
                            nextFrameDeadlineUs=nowUs+targetFrameUs;
                        const double remainingUs=nextFrameDeadlineUs-nowUs;
                        if(remainingUs>0.0) {
#ifdef __SWITCH__
                            // SDL_Delay rounds to whole milliseconds; on Switch that
                            // turns a 16.666 ms target into ~17 ms (57-58 Hz). Sleep
                            // with nanosecond precision and keep an absolute deadline
                            // so scheduler overshoot is corrected next frame.
                            svcSleepThread(int64_t(remainingUs*1000.0));
#else
                            SDL_Delay(Uint32(std::ceil(remainingUs/1000.0)));
#endif
                        }
                        nextFrameDeadlineUs+=targetFrameUs;
                        // The software cap makes the observed cadence 60 Hz; let
                        // the existing refresh/lead estimator use it as well.
                        actualVsync=true;
                    } else {
                        nextFrameDeadlineUs=0.0;
                    }
                    if(actualVsync && previousSwap) {
                        const auto intervalMs=(presentFinished-previousSwap)*counterToUs/1000.0;
                        // 50/60Hz only; menus, disconnects and missed swaps must
                        // not stretch the prediction or create a latency queue.
                        if(intervalMs>=14 && intervalMs<=21) {
                            refreshPeriodMs=refreshPeriodMs>0?refreshPeriodMs*.9+intervalMs*.1:intervalMs;
                            stableSwaps=std::min(stableSwaps+1,60u);irregularSwaps=0;
                        } else if(intervalMs<=0 || intervalMs>=75 || ++irregularSwaps>=3) {
                            stableSwaps=0;refreshPeriodMs=0;irregularSwaps=0;
                        } // Retain a qualified estimate across an isolated missed swap.
                    } else {stableSwaps=0;refreshPeriodMs=0;irregularSwaps=0;}
                    previousSwap=actualVsync?presentFinished:0;
                    const auto presentedAt=SDL_GetTicks();
                    // Keep target and measured rate in the local log so a
                    // preset that only acts as a cap is distinguishable from
                    // a request that never reached the sender.
                    static Uint32 lastRateLogSample=0;
                    if(state.streaming && (!lastRateLogSample || presentedAt-lastRateLogSample>=5000)){
                        const auto rate=client.streamStats();
                        char rateLine[192];
                        std::snprintf(rateLine,sizeof rateLine,
                            "stream-rate actual-mbps=%.3f target-mbps=%d loss=%.2f nack=%llu rtx=%llu feedback=%d",
                            rate.video.bitrateMbps,rate.maxBitrateMbps,rate.video.lossPercent,
                            (unsigned long long)rate.nackSent,(unsigned long long)rate.rtxRecovered,
                            rate.bitrateFeedbackSent?1:0);
                        uu::diagnostic(rateLine);
                        lastRateLogSample=presentedAt;
                    }
                    const double telemetryWorkUs=(presentStarted-loopStarted)*counterToUs;
                    const double telemetryPresentUs=(presentFinished-presentStarted)*counterToUs;
                    const double telemetryLoopUs=(presentFinished-loopStarted)*counterToUs;
                    if(telemetry.observe(presentedAt,state.streaming,submittedNewFrame,frameUploadUs,
                                         telemetryWorkUs,telemetryPresentUs,telemetryLoopUs,
                                         liveUi.active(),statsVisible||persistentNetwork))
                        telemetry.publish(client.streamStats(),actualVsync,texFormat==SDL_PIXELFORMAT_NV12,
                                          refreshPeriodMs,stableSwaps,uu::display::active(),uu::display::directFrames(),
                                          uu::display::mapHits(),uu::display::mapImports(),uu::display::mapCached());
                    // CPU diagnostics must not depend on the optional telemetry
                    // UDP sender. When telemetry is disabled, mark the UI role
                    // here and emit a local ledger every 5 s; when telemetry is
                    // enabled, publish() already drives the same collector.
                    if(!telemetry.enabled() && (!lastCpuLedgerSample || presentedAt-lastCpuLedgerSample>=5000)){
                        uu_cpu_mark(2);
                        (void)uu::cpuDiagnosticsSnapshot();
                        lastCpuLedgerSample=presentedAt;
                    }
                    if (state.streaming) {
                        const auto workUs=(presentStarted-loopStarted)*counterToUs;
                        cpuUs+=workUs;presentUs+=(presentFinished-presentStarted)*counterToUs;
                        maxCpuUs=std::max(maxCpuUs,workUs);++renderLoops;
                        if((presentFinished-loopStarted)*counterToUs>25000) ++slowLoops;
                        if (submittedNewFrame) ++presentedFrames;
                        const auto elapsed=Uint32(presentedAt-presentWindow);
                        if(elapsed>=2000) {
                            presentFps=double(presentedFrames)*1000/elapsed;
                            char timing[256];
                            std::snprintf(timing,sizeof timing,
                                "render-window-loops-%llu-uploads-%llu-panel-%d-cpu-avg-us-%.0f-cpu-max-us-%.0f-upload-avg-us-%.0f-present-avg-us-%.0f-over25ms-%llu",
                                static_cast<unsigned long long>(renderLoops),static_cast<unsigned long long>(uploadCount),int(statsVisible),
                                renderLoops?cpuUs/renderLoops:0,maxCpuUs,uploadCount?uploadUs/uploadCount:0,
                                renderLoops?presentUs/renderLoops:0,static_cast<unsigned long long>(slowLoops));
                            uu::diagnostic(timing);
                            std::snprintf(timing,sizeof timing,"pacing-window-refresh-us-%.0f-lead-avg-us-%.0f-stable-%u",
                                refreshPeriodMs*1000,leadSamples?leadSumMs*1000/leadSamples:0,stableSwaps);
                            uu::diagnostic(timing);
                            const auto mediaStats=media->stats();
                            auto delta=[](uint64_t current,uint64_t previous){
                                return current>=previous?current-previous:current;
                            };
                            const auto decoded=delta(mediaStats.decodedTotal,lastDecoded);
                            const auto published=delta(mediaStats.publishedTotal,lastPublished);
                            const auto selected=delta(mediaStats.pictureSelected,lastSelected);
                            const auto empty=delta(mediaStats.pictureEmpty,lastEmpty);
                            const auto noReady=delta(mediaStats.pictureEmptyNoReady,lastNoReady);
                            const auto future=delta(mediaStats.pictureEmptyFuture,lastFuture);
                            lastDecoded=mediaStats.decodedTotal;lastPublished=mediaStats.publishedTotal;
                            lastSelected=mediaStats.pictureSelected;lastEmpty=mediaStats.pictureEmpty;
                            lastNoReady=mediaStats.pictureEmptyNoReady;lastFuture=mediaStats.pictureEmptyFuture;
                            std::snprintf(timing,sizeof timing,
                                "video-frame decoded=%llu published=%llu selected=%llu empty=%llu no-ready=%llu future=%llu gap-max=%d loops=%llu uploads=%llu",
                                (unsigned long long)decoded,(unsigned long long)published,
                                (unsigned long long)selected,(unsigned long long)empty,
                                (unsigned long long)noReady,(unsigned long long)future,
                                mediaStats.pictureFutureGapMaxMs,
                                (unsigned long long)renderLoops,(unsigned long long)uploadCount);
                            uu::diagnostic(timing);
                            leadSumMs=0;leadSamples=0;
                            cpuUs=uploadUs=presentUs=maxCpuUs=0;renderLoops=uploadCount=slowLoops=0;
                            presentedFrames=0;presentWindow=presentedAt;
                        }
                    } else {
                        presentWindow=presentedAt;presentedFrames=0;presentFps=-1;
                        leadSumMs=0;leadSamples=0;
                        cpuUs=uploadUs=presentUs=maxCpuUs=0;renderLoops=uploadCount=slowLoops=0;
                    }
                }
                if (video)
                    uu::display::destroyTexture(video);
            }
            rtc::Cleanup().wait();
            uu::waitBackgroundTasks();
            media.reset();
            clearTextCache();
            SDL_DestroyRenderer(renderer);
            uu::display::shutdown();
            SDL_DestroyWindow(window);
        }
        curl_global_cleanup();
        TTF_Quit();
        SDL_Quit();
    } catch (const std::exception &e) {
        result = 1;
        uu::diagnosticException("main-exception", std::current_exception());
        try {
            rtc::Cleanup().wait();
            uu::waitBackgroundTasks();
        } catch (...) {
            uu::diagnosticException("cleanup-exception", std::current_exception());
        }
#ifdef __SWITCH__
        uu::display::shutdown();
        SDL_Quit();
        consoleInit(nullptr);
        printf("UU Remote NS\n%s\nPress + to exit.\n", e.what());
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
#else
        std::cerr << e.what() << '\n';
#endif
    }
#ifdef __SWITCH__
    appletSetMediaPlaybackState(false);
    if (setsysReady)
        setsysExit();
    plExit();
    romfsExit();
    sslExit();
    nifmExit();
    socketExit();
#endif
    return result;
}
