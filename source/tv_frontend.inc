// Included inside main.cpp's private namespace, after Fonts/text helpers.
// Geometry and routes follow decoded UU TV layouts. This preview emits no RPCs.
namespace tvui {
constexpr SDL_Color bg{30,30,33,255}, panel{49,53,60,255}, blue{58,123,252,255},
    label{176,179,184,255}, ink{255,255,255,255};
enum class Page {Home,Devices,Toolbar,Display,Quality,Picture,Peripherals,Gamepad,
    Schemes,Guide,More,Network,Safety,Windows,Account,Login,Disconnect,Bitrate=18,Resolution,Codec,Touch,Keyboard,Stats,InputMode,Mouse,Audio,QualityPreset,Playout,Logout,About,WindowsConfirm,AppLaunch,StreamPreview,XboxLayout,DecodeProfile};
struct Frame {Page page; int focus=0;};
enum class Kind {Button,Menu,Icon,Tile,Shortcut,Tab,Toggle};
struct Button {SDL_Rect box; std::string title; int action; bool enabled=true; bool chosen=false;std::string value;Kind kind=Kind::Button;};
void rounded(SDL_Renderer *r,SDL_Rect b,SDL_Color c,int radius=8) {
    if(b.w<=0 || b.h<=0)return;
    radius=std::clamp(radius,0,std::min(b.w,b.h)/2);
    if(!radius){rect(r,b.x,b.y,b.w,b.h,c);return;}
    const auto tint=(uint32_t(c.r)<<24)|(uint32_t(c.g)<<16)|(uint32_t(c.b)<<8)|c.a;
    const std::string key="@rounded/"+std::to_string(b.w)+"/"+std::to_string(b.h)+"/"+std::to_string(radius)+"/"+std::to_string(tint);
    auto entry=std::find_if(textCache.begin(),textCache.end(),[&](const TextEntry &e){return e.renderer==r && !e.font && e.value==key;});
    if(entry==textCache.end()){
        // Analytic pixel coverage, computed once per size/color. One GPU draw thereafter.
        const size_t bytes=size_t(b.w)*b.h*4;
        std::vector<Uint8> pixels(bytes);
        for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x){
            const float dx=std::max(std::abs(x+.5f-b.w*.5f)-(b.w*.5f-radius),0.f);
            const float dy=std::max(std::abs(y+.5f-b.h*.5f)-(b.h*.5f-radius),0.f);
            const float coverage=std::clamp(radius+.5f-std::hypot(dx,dy),0.f,1.f);
            const auto i=(size_t(y)*b.w+x)*4;
            pixels[i]=c.r;pixels[i+1]=c.g;pixels[i+2]=c.b;pixels[i+3]=Uint8(std::round(c.a*coverage));
        }
        auto *surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(),b.w,b.h,32,b.w*4,SDL_PIXELFORMAT_RGBA32);
        if(!surface){rect(r,b.x,b.y,b.w,b.h,c);return;}
        auto *texture=uu::display::textureFromSurface(r,surface);SDL_FreeSurface(surface);
        if(!texture){rect(r,b.x,b.y,b.w,b.h,c);return;}
        SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
        while(!textCache.empty() && (textCache.size()>=192 || textCacheBytes+bytes>8u*1024u*1024u)){
            textCacheBytes-=textCache.back().bytes;uu::display::destroyTexture(textCache.back().texture);textCache.pop_back();
        }
        textCache.push_front({r,nullptr,key,0,texture,b.w,b.h,bytes});textCacheBytes+=bytes;
    }else textCache.splice(textCache.begin(),textCache,entry);
    uu::display::copy(r,textCache.front().texture,nullptr,&b);
}

// Original geometric fallback: public source does not redistribute APK textures.
bool symbol(SDL_Renderer *r,const std::string &name,SDL_Rect b) {
    const auto fg=ink;const int t=std::max(2,b.w/18);
    auto box=[&](int x,int y,int w,int h,SDL_Color color=ink){rect(r,b.x+x,b.y+y,w,h,color);};
    if(name=="arrow" || name=="back") {
        const int direction=name=="back"?-1:1;
        for(int i=0;i<b.h/3;++i){int x=b.w/2+direction*(b.h/6-std::abs(i-b.h/6));box(x,b.h/3+i,t,t);}
    }else if(name=="radio" || name=="radio_empty") {
        rounded(r,b,fg,b.w/2);SDL_Rect inner{b.x+t,b.y+t,b.w-2*t,b.h-2*t};rounded(r,inner,panel,inner.w/2);
        if(name=="radio")rounded(r,{b.x+b.w/3,b.y+b.h/3,b.w/3,b.h/3},fg,b.w/6);
    }else if(name=="gamepad") {
        rounded(r,{b.x,b.y+b.h/5,b.w,b.h*3/5},fg,std::max(3,b.h/5));
        rounded(r,{b.x+b.w/6,b.y+b.h/3,b.w/4,b.h/4},panel,4);
        rounded(r,{b.x+b.w*2/3,b.y+b.h/3,b.w/8,b.h/8},panel,4);
        rounded(r,{b.x+b.w*4/5,b.y+b.h/2,b.w/8,b.h/8},panel,4);
    }else if(name=="device" || name=="display") {
        box(b.w/8,b.h/6,b.w*3/4,t);box(b.w/8,b.h/6,t,b.h/2);
        box(b.w*7/8-t,b.h/6,t,b.h/2);box(b.w/8,b.h*2/3,b.w*3/4,t);
        box(b.w/2-t,b.h*2/3,t,b.h/8);box(b.w/3,b.h*4/5,b.w/3,t);
    }else if(name=="refresh") {
        box(b.w/5,b.h/4,b.w*3/5,t);box(b.w*4/5-t,b.h/4,t,b.h/4);
        box(b.w/5,b.h*11/16,b.w*3/5,t);box(b.w/5,b.h*7/16,t,b.h/4);
        box(b.w*4/5-2*t,b.h/4-t,t*2,t*2);box(b.w/5,b.h*11/16-t,t*2,t*2);
    }else if(name=="account") {
        rounded(r,{b.x+b.w*3/8,b.y+b.h/6,b.w/4,b.h/4},fg,b.w/8);
        rounded(r,{b.x+b.w/5,b.y+b.h/2,b.w*3/5,b.h/3},fg,b.w/6);
    }else if(name=="attachment") {
        rounded(r,{b.x+b.w/3,b.y+b.h/6,b.w/3,b.h*2/3},fg,b.w/6);
        rounded(r,{b.x+b.w/3+t,b.y+b.h/6+t,b.w/3-2*t,b.h*2/3-2*t},panel,b.w/8);
    }else if(name=="exit") {
        box(b.w/5,b.h/6,t,b.h*2/3);box(b.w/5,b.h/6,b.w/2,t);box(b.w/5,b.h*5/6-t,b.w/2,t);
        box(b.w/2,b.h/2-t,b.w/4,t);box(b.w*3/4-t,b.h/2-t*2,t*2,t*2);
        box(b.w*3/4-t,b.h/2,t*2,t*2);
    }else if(name=="safety") {
        for(int i=0;i<6;++i){const int inset=i*b.w/18;box(b.w/6+inset/2,b.h/6+i*b.h/12,b.w*2/3-inset,t);}
        box(b.w/3,b.h/2,b.w/12,t);box(b.w/3+b.w/12,b.h/2,b.w/12,t);box(b.w/3+b.w/6,b.h/2-b.h/8,b.w/12,t);
    }else if(name=="more") {
        for(int i=0;i<3;++i)rounded(r,{b.x+b.w/5+i*b.w/4,b.y+b.h/2-b.h/16,b.w/8,b.w/8},fg,b.w/16);
    }else if(name=="windows") {
        for(int y=0;y<2;++y)for(int x=0;x<2;++x)box(b.w/5+x*b.w/3,b.h/5+y*b.h/3,b.w/4,b.h/4);
    }else if(name=="fps") {
        box(b.w/6,b.h/4,b.w*2/3,t);box(b.w/6,b.h/2,b.w*2/3,t);box(b.w/6,b.h*3/4,b.w/2,t);
        box(b.w/2,b.h/4,t,b.h/2);box(b.w/2,b.h/4,b.w/4,t);box(b.w/3,b.h/2-t,b.w/6,t);
    }else if(name=="picture") {
        box(b.w/8,b.h/6,b.w*3/4,t);box(b.w/8,b.h/6,t,b.h*2/3);
        box(b.w*7/8-t,b.h/6,t,b.h*2/3);box(b.w/8,b.h*5/6-t,b.w*3/4,t);
        box(b.w/5,b.h*3/5,b.w/5,b.h/6);box(b.w*2/5,b.h/3,b.w/4,b.h/4);box(b.w*3/5,b.h*2/5,b.w/4,b.h/3);
    }else if(name=="guide") {
        rounded(r,{b.x+b.w/6,b.y+b.h/5,b.w/3,b.h*3/5},fg,3);
        rounded(r,{b.x+b.w/2,b.y+b.h/5,b.w/3,b.h*3/5},fg,3);
        box(b.w/6+b.w/12,b.h/3,b.w/4,t);box(b.w/2+b.w/12,b.h/3,b.w/4,t);
        box(b.w/6+b.w/12,b.h/2,b.w/4,t);box(b.w/2+b.w/12,b.h/2,b.w/4,t);
    }else if(name=="sound") {
        box(b.w/5,b.h*2/5,b.w/5,b.h/5);box(b.w/4,b.h/3,b.w/5,b.h/3);
        for(int i=0;i<3;++i){const int waveH=std::max(2,b.h/3-i*b.h/12);box(b.w*3/5+i*b.w/10,b.h/3+i*b.h/12,t,waveH);}
    }else if(name=="keyboard") {
        box(b.w/6,b.h/3,b.w*2/3,t);box(b.w/6,b.h*2/3-t,b.w*2/3,t);
        box(b.w/6,b.h/3,t,b.h/3);box(b.w*5/6-t,b.h/3,t,b.h/3);
        for(int x=0;x<3;++x)box(b.w/4+x*b.w/5,b.h/2,b.w/12,t,panel);
    }else if(name=="touch") {
        rounded(r,{b.x+b.w/3,b.y+b.h/2,b.w*2/5,b.h*2/5},fg,b.w/10);
        box(b.w/3,b.h/5,b.w/12,b.h/3);box(b.w/2-t/2,b.h/8,b.w/12,b.h/2);
        box(b.w*2/3-t,b.h/5,b.w/12,b.h/3);box(b.w/8,b.h/2,b.w/4,t);
    }else if(name=="mouse") {
        rounded(r,{b.x+b.w/3,b.y+b.h/6,b.w/3,b.h*2/3},fg,b.w/6);
        rounded(r,{b.x+b.w/3+t,b.y+b.h/6+t,b.w/3-2*t,b.h*2/3-2*t},panel,b.w/8);
        box(b.w/2-t/2,b.h/6+t,b.w/12,b.h/3);box(b.w/3,b.h/2,b.w/3,t,panel);
    }else return false;
    return true;
}
bool image(SDL_Renderer *r,const std::string &name,SDL_Rect dst,int w=64,int h=64) {
    // Keep the AA radio pair; small gamepad buttons stay geometric because the
    // restored gamepad asset is the 420x300 illustration, not a 64px icon.
    if(name=="radio" || name=="radio_empty" || (name=="gamepad" && w<=64))return symbol(r,name,dst);
    static std::vector<std::string> missing;
    if(std::find(missing.begin(),missing.end(),name)!=missing.end())return symbol(r,name,dst);
    const std::string key="@tv-asset/"+name;
    auto entry=std::find_if(textCache.begin(),textCache.end(),[&](const TextEntry &e){
        return e.renderer==r && !e.font && e.value==key;
    });
    if(entry==textCache.end()) {
#ifdef __SWITCH__
        std::ifstream file("romfs:/ui-tv/"+name+".rgba",std::ios::binary);
#else
        std::ifstream file("romfs/ui-tv/"+name+".rgba",std::ios::binary);
#endif
        std::vector<unsigned char> pixels(size_t(w)*h*4);
        if(!file.read(reinterpret_cast<char*>(pixels.data()),pixels.size())){missing.push_back(name);return symbol(r,name,dst);}
        auto *surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(),w,h,32,w*4,SDL_PIXELFORMAT_RGBA32);
        if(!surface)return false;
        auto *texture=uu::display::textureFromSurface(r,surface);SDL_FreeSurface(surface);
        if(!texture)return false;
        const size_t bytes=pixels.size();
        while(!textCache.empty() && (textCache.size()>=192 || textCacheBytes+bytes>8u*1024u*1024u)) {
            textCacheBytes-=textCache.back().bytes;
            uu::display::destroyTexture(textCache.back().texture);textCache.pop_back();
        }
        textCache.push_front({r,nullptr,key,0,texture,w,h,bytes});textCacheBytes+=bytes;
    }else textCache.splice(textCache.begin(),textCache,entry);
    return uu::display::copy(r,textCache.front().texture,nullptr,&dst)==0;
}
void centered(SDL_Renderer *r,TTF_Font *font,const std::string &message,int y,SDL_Color tint=ink) {
    int w=0,h=0;if(TTF_SizeUTF8(font,message.c_str(),&w,&h)!=0)return;
    text(r,font,message,std::max(48,(1280-w)/2),y,tint,1184);
}
void backdrop(SDL_Renderer *r) {
    // A nested page may be painted after video in an already-recording GPU frame.
    // Native clear() only begins a frame; use an explicit opaque full-screen quad.
    rect(r,0,0,1280,720,bg);
    // Restrained header illumination; keep text/controls on neutral surfaces.
    for(int i=0;i<10;++i)rect(r,0,i*12,1280,12,{Uint8(28-i/2),Uint8(33-i/2),Uint8(42-i),255});
}
std::string fit(TTF_Font *font,std::string value,int maxWidth) {
    int w=0,h=0;TTF_SizeUTF8(font,value.c_str(),&w,&h);if(w<=maxWidth)return value;
    while(!value.empty()){
        size_t last=value.size()-1;while(last>0 && (static_cast<unsigned char>(value[last])&0xc0)==0x80)--last;
        value.resize(last);const auto candidate=value+"...";TTF_SizeUTF8(font,candidate.c_str(),&w,&h);
        if(w<=maxWidth)return candidate;
    }return {};
}
void aligned(SDL_Renderer *r,TTF_Font *font,const std::string &value,SDL_Rect area,SDL_Color tint=ink,bool middle=false) {
    if(area.w<=0 || value.empty())return;
    const auto display=fit(font,value,area.w);
    const auto *entry=cachedText(r,font,display,tint);if(!entry)return;
    // Align visible glyph ink, not asymmetric ascent/descent whitespace.
    // Bounds are measured once on a cache miss; all fonts remain in memory.
    SDL_Rect src{0,0,std::min(entry->width,area.w),entry->height};
    SDL_Rect dst{area.x+(middle?(area.w-src.w)/2:0),
                 area.y+(area.h-entry->inkHeight)/2-entry->inkTop,src.w,src.h};
    uu::display::copy(r,entry->texture,&src,&dst);
}
// Endpoint strings are ASCII. Use two lines before truncating an IPv6 endpoint.
void endpointValue(SDL_Renderer *r,Fonts &f,const std::string &value,SDL_Rect area){
    int w=0,h=0;TTF_SizeUTF8(f.hint,value.c_str(),&w,&h);
    if(w<=area.w){aligned(r,f.hint,value,area,ink);return;}
    size_t split=value.size();
    while(split>1){
        --split;const auto head=value.substr(0,split);TTF_SizeUTF8(f.hint,head.c_str(),&w,&h);
        if(w<=area.w)break;
    }
    aligned(r,f.hint,value.substr(0,split),{area.x,area.y,area.w,area.h/2},ink);
    aligned(r,f.hint,value.substr(split),{area.x,area.y+area.h/2,area.w,area.h/2},ink);
}
void brand(SDL_Renderer *r,Fonts &f,int x=64,int y=48) {
    constexpr int height=48,gap=14;
    image(r,"youyou-mark",{x,y,height,height},64,64);
    const std::string name="YOUYOU远程";
    int nw=0,nh=0;TTF_SizeUTF8(f.brand,name.c_str(),&nw,&nh);
    aligned(r,f.brand,name,{x+height+gap,y,420,height},ink);
    const std::string version="v" UU_VERSION;
    aligned(r,f.hint,version,{x+height+gap+nw+14,y,100,height},label);
}
void homeHotkeys(SDL_Renderer *r,Fonts &f) {
    // Home actions stay by the device card; future stream actions have their
    // own context card rather than sharing one dense row of instructions.
    auto key=[&](int x,int y,bool plus) {
        rounded(r,{x,y,32,30},SDL_Color{64,71,82,255},6);
        rect(r,x+9,y+14,14,2,ink);
        if(plus)rect(r,x+15,y+8,2,14,ink);
    };
    key(65,586,true);
    aligned(r,f.small,"长按退出应用，返回启动器",{110,582,590,38},label);
    rounded(r,{775,410,420,208},panel,14);
    aligned(r,f.hint,"串流操作",{797,424,376,28},label);
    key(797,463,false);
    aligned(r,f.small,"长按切换：手柄 / 键鼠",{842,458,330,38},ink);
    key(797,519,true);
    aligned(r,f.small,"打开串流菜单",{842,514,330,38},ink);
    aligned(r,f.hint,"键鼠模式按一下；手柄模式长按",{797,565,376,28},label);
}
bool opensPage(int a) {
    return (a>=10 && a<=14)||(a>=20 && a<=31)||(a>=42 && a<=45)||(a>=47 && a<=58)||a==70||a==613||(a>=74 && a<=76);
}
void loginHeader(SDL_Renderer *r,Fonts &f) {
    backdrop(r);brand(r,f);

}
void smsLogin(SDL_Renderer *r,Fonts &f,const SmsForm &sms,int remaining,bool busy) {
    centered(r,f.normal,"短信验证码登录",132);
    centered(r,f.small,"登录与电脑端相同的 UU远程账号",178,label);
    const char *titles[]={"国家 / 地区","手机号码","短信验证码"};
    const std::string values[]={"+"+sms.country,sms.mobile.empty()?"输入手机号码":sms.mobile,
        sms.code.empty()?"输入验证码":std::string(sms.code.size(),'*')};
    for(int i=0;i<3;++i) {
        const int y=240+i*82;rounded(r,{355,y,570,67},sms.selected==i?blue:panel,10);
        aligned(r,f.small,titles[i],{375,y,170,67},ink);
        aligned(r,f.normal,values[i],{555,y,350,67},ink);
    }
    rounded(r,{355,508,270,58},sms.selected==3?blue:panel,10);
    rounded(r,{645,508,280,58},sms.selected==4?blue:panel,10);
    aligned(r,f.small,remaining?std::to_string(remaining)+" 秒后重发":"获取验证码",{355,508,270,58},ink,true);
    aligned(r,f.small,busy?"正在处理":"登录",{645,508,280,58},ink,true);
    centered(r,f.small,"方向键选择   A 输入或确认   X 切换扫码登录",606,label);
}
const char *buttonIcon(int action,const std::string &) {
    if(action>=1000 && action<1100)return "device";
    switch(action){case 99:return "back";case 10:return "device";case 90:return "refresh";case 11:return "account";
    case 13:case 23:case 50:case 53:case 613:return "gamepad";case 14:case 21:return "display";case 20:case 58:return "exit";
    case 22:return "safety";case 24:return "more";case 25:return "windows";
    case 30:return "fps";case 31:return "picture";case 51:return "guide";case 48:return "sound";
    case 54:return "touch";case 55:return "keyboard";case 59:return "mouse";case 70:return "device";case 57:return "account";default:return nullptr;}
}

bool preview(SDL_Renderer *r,Fonts &f,const uu::Snapshot &snapshot) {
    std::vector<Frame> stack{{Page::Login,0}};
    std::vector<Button> buttons;
    int scheme=1,draft=1,fps=snapshot.fps==30?30:60;
    int resolution=720,codec=1,quality=2,bufferMs=16,touchMode=0,inputMode=0,volume=75,mouseSpeed=2;
    bool vsync=true,sound=true,autoUnlock=false,autoLock=false,remoteMute=true,restoreSound=true,networkOverlay=false,nativeMultitouch=true;
    const char *qualityNames[]={"省流","清晰","高清","原画"};
    const int qualityMbps[]={5,12,20,30};
    std::string keyboardText,pendingShortcut;
    bool done=false,alive=true,draining=false,touchDown=false;
    std::string notice;
    int touchTarget=-1,touchStartY=0,deviceOffset=0;
    std::string device=snapshot.devices.empty()?std::string("尚未选择电脑"):
        snapshot.devices.front().value("alias",std::string("我的电脑"));
#ifdef __SWITCH__
    PadState uiPad;padInitializeDefault(&uiPad);padUpdate(&uiPad);
    bool armed=padGetButtons(&uiPad)==0;
#else
    bool armed=true;
#endif
    auto push=[&](Page p){touchTarget=-1;touchDown=false;stack.push_back({p,0});notice.clear();};
    auto back=[&](){if(stack.back().page==Page::Keyboard)SDL_StopTextInput();if(stack.size()>1)stack.pop_back();else draining=true;notice.clear();};
    while(!done && alive) {
#ifdef __SWITCH__
        if(!appletMainLoop())return false;
#endif
        auto &frame=stack.back(); const auto page=frame.page;
        buttons.clear();
        auto add=[&](int x,int y,int w,int h,std::string title,int action,bool enabled=true,bool chosen=false,std::string value="",Kind kind=Kind::Button){
            buttons.push_back({{x,y,w,h},std::move(title),action,enabled,chosen,std::move(value),kind});
        };
        auto row=[&](int n,std::string title,int action,bool enabled=true,bool chosen=false,std::string value=""){
            add(848,176+n*76,376,64,std::move(title),action,enabled,chosen,std::move(value),Kind::Menu);
        };
        auto info=[&](int n,const std::string &name,const std::string &value){
            aligned(r,f.small,name,{848,176+n*62,210,50},label);
            aligned(r,f.small,value,{1064,176+n*62,160,50},ink);
            rect(r,848,226+n*62,376,1,SDL_Color{63,67,74,255});
        };
        backdrop(r);
        const bool drawer=page!=Page::Home && page!=Page::Devices && page!=Page::Gamepad &&
            page!=Page::Schemes && page!=Page::Guide && page!=Page::Login && page!=Page::Keyboard && page!=Page::About && page!=Page::More && page!=Page::Network && page!=Page::Logout && page!=Page::StreamPreview;
        if(drawer) {
            text(r,f.large,"电脑桌面",70,275,{64,66,72,255});
            text(r,f.small,"原生菜单预览 · 串流画面在正式接入后显示",70,345,label);
            rect(r,0,0,1280,720,{0,0,0,216});rounded(r,{824,32,424,624},panel,13);
        }
        std::string title;
        switch(page) {
        case Page::Home:
            brand(r,f);
            add(736,48,176,48,"切换设备",10);add(928,48,128,48,"刷新",90);add(1072,48,144,48,"账号",11);
            rounded(r,{65,142,96,36},SDL_Color{40,62,52,255},18);
            aligned(r,f.small,snapshot.devices.empty()?"待连接":"设备",{65,142,96,36},SDL_Color{150,213,172,255},true);
            aligned(r,f.normal,device,{177,142,1038,36},ink);
            add(65,202,650,356,"进入桌面",12,true,false,"",Kind::Tile);
            // No fabricated application entries: populate only after a real application list is available.
            aligned(r,f.normal,"你的电脑，随时连接",{775,280,420,56},ink);
            aligned(r,f.small,"连接后显示可用应用",{775,348,420,48},label);
            homeHotkeys(r,f);
            break;
        case Page::StreamPreview:
            centered(r,f.normal,"串流画面预览",286);
            centered(r,f.small,"当前未连接电脑",340,label);
            add(1030,48,184,52,"工具栏",14);
            break;
        case Page::Devices:
            title="我的电脑";
            for(size_t i=size_t(deviceOffset);i<std::min<size_t>(snapshot.devices.size(),size_t(deviceOffset+5));++i) {
                const auto &d=snapshot.devices[i];const bool online=d.value("status",std::string{})!="offline";
                add(80,160+(int(i)-deviceOffset)*85,1090,72,d.value("alias",std::string("电脑"))+(online?" · 在线":" · 离线"),1000+int(i),online);
            }
            if(snapshot.devices.empty())text(r,f.normal,"暂未发现电脑，请返回原界面登录或刷新",100,210,label);
            break;
        case Page::Toolbar:
            add(848,56,112,88,"退出",20,true,false,"",Kind::Shortcut);
            add(976,56,112,88,"键盘",55,true,false,"",Kind::Shortcut);
            add(1104,56,112,88,sound?"声音":"已静音",48,true,false,"",Kind::Shortcut);
            rect(r,848,160,376,1,{73,78,85,255});
            row(0,"显示",21);row(1,"安全",22);row(2,"外设",23);row(3,"更多",24);row(4,"Windows 快捷操作",25);break;
        case Page::Display:title="显示";row(0,"画质与帧率",30,true,false,std::to_string(fps)+" FPS");row(1,"画面设置",31,true,false,std::to_string(resolution)+"p");break;
        case Page::Quality:title="画质与帧率";
            row(0,"60 帧",40,true,fps==60);row(1,"30 帧",41,true,fps==30);
            row(2,"画质档位",43,true,false,std::string(qualityNames[quality])+" · "+std::to_string(qualityMbps[quality])+" Mbps");
            row(3,"播放缓冲",47,true,false,std::to_string(bufferMs)+" ms");break;
        case Page::Picture:title="画面设置";row(0,"分辨率",44,true,false,std::to_string(resolution)+"p");row(1,"视频编码",45,true,false,codec?"H.265":"H.264");row(2,"垂直同步",46,true,false,vsync?"开启":"关闭");break;
        case Page::Bitrate:break; // Reserved page ID; independent bitrate control removed.
        case Page::Resolution:title="分辨率";row(0,"960 × 540",400+540,true,resolution==540);row(1,"1280 × 720",400+720,true,resolution==720);row(2,"1920 × 1080",400+1080,true,resolution==1080);break;
        case Page::Codec:title="视频编码";row(0,"H.264",500,true,codec==0);row(1,"H.265",501,true,codec==1);break;
        case Page::QualityPreset:title="画质";{
            for(int i=0;i<4;++i)row(i,qualityNames[i],510+i,true,quality==i,std::to_string(qualityMbps[i])+" Mbps");
            aligned(r,f.small,"高清及以上建议 CPU 超频",{848,514,376,36},label);
            aligned(r,f.small,"提升流畅度",{848,551,376,30},label);
            }break;
        case Page::Playout:title="播放缓冲";{int values[]={0,8,16,24,35};for(int i=0;i<5;++i)row(i,std::to_string(values[i])+" ms",550+values[i],true,bufferMs==values[i]);}break;
        case Page::Peripherals:title="外设";row(0,"手柄设备",13);row(1,"键盘输入",55);row(2,"触屏操作",54,true,false,nativeMultitouch?"平板多点触控":"手机鼠标指针");row(3,"鼠标设置",59);break;
        case Page::Gamepad:
            title="手柄设备";text(r,f.normal,"当前手柄",90,190,ink);
            rounded(r,{85,245,510,132},panel);image(r,"attachment",{108,272,32,32});
            text(r,f.normal,"掌机内置手柄",160,264,ink);
            text(r,f.small,inputMode?"手柄直连模式":"键鼠映射模式",160,316,label);
            add(680,230,490,72,"输入模式",53,true,false,inputMode?"手柄直连":"键鼠映射",Kind::Menu);
            add(680,322,490,72,"键鼠映射方案",50,true,false,scheme==1?"方案一":"方案二",Kind::Menu);
            add(680,414,490,72,"操作说明",51,true,false,"",Kind::Menu);break;
        case Page::Schemes: {
            title="键鼠映射方案";add(445,111,165,49,"方案一",60,true,draft==1,"",Kind::Tab);add(650,111,165,49,"方案二",61,true,draft==2,"",Kind::Tab);
            const bool second=draft==2;
            const char *head[]={"左摇杆","方向键","右摇杆","正面按键"};
            const char *one[]={"移动鼠标\n按下：显示键盘","上 Ctrl\n下 Shift\n左 Backspace\n右 Delete","方向键\n按下：显示桌面","A 回车\nB Esc\nX 鼠标左键\nY 鼠标右键"};
            const char *two[]={"上下：滚轮\n按下：显示键盘","上 / 下 / 左 / 右","移动鼠标\n按下：Shift","A 回车\nB Esc\nX 空格\nY Backspace"};
            image(r,"gamepad",{492,190,296,212},420,300);
            text(r,f.small,second?"ZL  鼠标右键":"ZL  滚轮向上",100,222,label,320);
            text(r,f.small,second?"L  Ctrl     -  Tab":"L  Alt     -  Windows",100,269,label,340);
            text(r,f.small,second?"ZR  鼠标左键":"ZR  滚轮向下",885,222,label,300);
            text(r,f.small,second?"R  Alt     +  工具栏":"R  Tab     +  工具栏",885,269,label,300);
            for(int i=0;i<4;++i) {
                text(r,f.normal,head[i],90+i*300,408,{149,191,255,255});
                std::string lines=second?two[i]:one[i];size_t start=0;int line=0;
                do{const auto end=lines.find('\n',start);text(r,f.small,lines.substr(start,end-start),90+i*300,452+line++*27,ink,275);
                    if(end==std::string::npos)break;
                    start=end+1;
                }while(start<lines.size());
            }
            add(440,584,400,48,draft==scheme?"当前方案":"使用此方案",62,draft!=scheme);
            break;}

        case Page::InputMode:title="输入模式";row(0,"键鼠映射",600,true,inputMode==0);row(1,"手柄直连",601,true,inputMode==1);break;
        case Page::Touch:title="触屏操作";
            row(0,"平板多点触控",614,true,nativeMultitouch);row(1,"手机鼠标指针",610,true,!nativeMultitouch);
            aligned(r,f.small,nativeMultitouch?"像平板一样，直接触摸电脑画面":"滑动画面移动指针，轻点执行左键",{848,365,376,45},label);
            aligned(r,f.small,nativeMultitouch?"支持多指操作，由电脑应用响应":"按住左键按钮，再用另一指拖动",{848,415,376,45},label);
            aligned(r,f.small,nativeMultitouch?"适合触屏应用与多点手势":"右侧滑动条滚动，旁边按钮为中键",{848,465,376,45},label);break;
        case Page::Mouse:title="鼠标速度";{const char *names[]={"慢速","标准","快速","很快"};for(int i=0;i<4;++i)row(i,names[i],620+i,true,mouseSpeed==i);}break;
        case Page::Audio:title="声音";row(0,"播放声音",630,true,false,sound?"开启":"关闭");{int values[]={25,50,75,100};for(int i=0;i<4;++i)row(i+1,std::to_string(values[i])+"%",640+values[i],true,volume==values[i]);}break;
        case Page::Guide:title="操作说明";
            rounded(r,{90,160,1100,420},panel,12);
            {const char *left[]={"首页退出","键鼠模式","手柄模式","触屏操作","切换模式"};
             const char *right[]={"首页长按加号键退出应用，返回启动器","用摇杆和按键操作电脑；按一下加号键打开菜单","长按加号键打开菜单，按一下为游戏菜单键","外设中可选平板多点触控，或手机鼠标指针","串流中长按减号键，在手柄和键鼠操作之间切换"};
             for(int i=0;i<5;++i){aligned(r,f.normal,left[i],{120,185+i*72,200,56},ink);aligned(r,f.small,right[i],{355,185+i*72,800,56},label);}}break;
        case Page::More:
        case Page::Network:title=page==Page::More?"更多 · 网络信息":"网络信息";{
            aligned(r,f.normal,"网络延迟  --",{90,130,700,60},ink);
            aligned(r,f.hint,"连接信息",{90,194,510,25},label);aligned(r,f.hint,"播放性能",{660,194,510,25},label);
            const char *names[]={"连接通道","被控端地址","控制端地址","客户端版本","客户端系统","解码耗时","显示帧率","视频码率","初传缺口","连接时间"};
            for(int i=0;i<10;++i){
                const int col=i/5,line=i%5,x=90+col*570,y=220+line*53;
                aligned(r,f.small,names[i],{x,y,160,42},label);
                aligned(r,f.small,"--",{x+180,y,330,42},ink);
            }
            rect(r,90,502,1100,1,{63,67,74,255});
            const char *codecNames[]={"视频编码","编码器信息","解码器"};
            for(int i=0;i<3;++i){const int x=90+i*370;aligned(r,f.hint,codecNames[i],{x,510,340,26},label);aligned(r,f.small,"--",{x,538,340,34},ink);}
            add(875,130,315,60,"常驻网络信息",754,true,networkOverlay,"",Kind::Toggle);
            add(930,579,260,48,"详细运行统计",56);
            }break;
        case Page::Stats:title="运行统计";info(0,"接收 / 解码 / 显示","-- / -- / --");info(1,"实际码率","--");info(2,"往返延迟","--");info(3,"丢帧 / 重传","-- / --");info(4,"数据来源","连接后读取");break;
        case Page::DecodeProfile:title="解码耗时分析";
            aligned(r,f.small,"离线预览不读取设备频率；运行数据请在真机串流时查看",{120,200,1000,100},label);break;
        case Page::Safety:title="安全";
            add(848,176,376,72,"自动解锁电脑（尚未开放）",750,false,autoUnlock,"",Kind::Toggle);
            add(848,260,376,72,"断开后锁定电脑",751,true,autoLock,"",Kind::Toggle);
            add(848,344,376,72,"串流时电脑静音",752,true,remoteMute,"",Kind::Toggle);
            add(848,428,376,100,"远程结束后，恢复\n被控端声音设置",753,true,restoreSound,"",Kind::Toggle);
            aligned(r,f.small,"离线演示 · 尚未向电脑发送设置",{848,552,376,55},label);break;
        case Page::Windows:title="Windows 快捷操作";
            row(0,"锁定桌面",74);row(1,"重启电脑",75);row(2,"关机",76);
            row(3,"任务管理器",72);row(4,"显示桌面",71);row(5,"切换窗口",73);break;
        case Page::WindowsConfirm:title=pendingShortcut;
            aligned(r,f.small,"预览此操作的确认流程",{848,180,376,72},label);row(2,"取消",80);row(3,"确认操作",760);break;
        case Page::AppLaunch:title=pendingShortcut;
            aligned(r,f.small,"示例应用入口",{848,180,376,64},ink);
            aligned(r,f.small,"连接电脑后可读取真实应用列表",{848,258,376,96},label);row(3,"返回",80);break;
        case Page::Account:
            title="账号";
            rounded(r,{1000,166,72,72},blue,36);image(r,"account",{1020,186,32,32});
            aligned(r,f.normal,"离线预览账号",{848,254,376,48},ink,true);
            aligned(r,f.small,"未读取真实账号资料",{848,303,376,40},label,true);
            add(848,374,376,64,"登录页面",52,true,false,"",Kind::Menu);
            add(848,450,376,64,"关于",57,true,false,"",Kind::Menu);
            add(848,526,376,64,"退出账号",58,true,false,"",Kind::Menu);break;
        case Page::Login:
            centered(r,f.normal,"手机扫码登录 YOUYOU远程",116);
            centered(r,f.small,"登录与电脑端相同的 UU远程账号",162,label);
            rounded(r,{506,227,268,268},panel,12);
            rounded(r,{520,241,240,240},{238,240,243,255},6);
            image(r,"device",{616,300,48,48});
            aligned(r,f.small,"二维码占位",{535,373,210,42},{70,76,85,255},true);
            centered(r,f.small,"二维码由实际登录流程生成",507,label);
            centered(r,f.small,"请使用手机 App 扫码并确认",548,label);break;
        case Page::Logout:
            centered(r,f.large,"是否确定退出登录？",260);
            centered(r,f.small,"此处仅演示退出流程，实际账号不受影响",332,label);
            add(330,438,290,72,"退出",770);add(660,438,290,72,"取消",80);break;
        case Page::About:title="关于";
            aligned(r,f.large,"YOUYOU远程",{120,177,550,72},ink);
            aligned(r,f.normal,"原生界面预览",{120,265,550,56},ink);
            aligned(r,f.small,"界面修订 3 · 基于 v46 共用界面",{120,333,600,48},label);
            aligned(r,f.small,"鼠标、触屏与手柄均可操作",{120,392,600,48},label);
            rounded(r,{760,180,420,320},panel,12);
            aligned(r,f.normal,"当前版本",{790,202,350,48},ink);
            aligned(r,f.small,"本机离线预览，不执行远端操作",{790,267,350,50},label);
            aligned(r,f.small,"显示组件：SDL2 / SDL_ttf",{790,329,350,50},label);
            aligned(r,f.small,"字体：Noto Sans SC · OFL 1.1",{790,391,350,50},label);
            add(120,506,300,60,"登录页面",52);break;
        case Page::Keyboard:title="键盘输入";
            aligned(r,f.small,"先在电脑选中输入框，再编辑文字",{90,132,1100,42},label);
            add(90,190,1100,92,keyboardText.empty()?"点这里输入文字":keyboardText,2304);
            add(90,304,220,58,"编辑文字",2304);add(330,304,180,58,"清空草稿",2302,!keyboardText.empty());
            add(914,304,276,58,"发送到电脑",2303,!keyboardText.empty());
            aligned(r,f.small,"常用按键",{90,402,400,40},ink);
            {const char *keys[]={"回车","Tab","Esc","退格","全选","粘贴"};for(int i=0;i<6;++i)add(90+i*186,458,170,58,keys[i],2310+i);}
            aligned(r,f.hint,"离线预览仅编辑草稿，不发送文字或按键",{90,552,1100,36},label);break;
        case Page::Disconnect:title="退出远控？";aligned(r,f.small,"退出后返回电脑首页",{848,180,376,72},label);row(2,"继续远控",80);row(3,"返回首页",81);break;
        default: break;
        }
        if(!title.empty()) {

            aligned(r,f.normal,title,{drawer?904:124,40,drawer?320:1000,64},ink);
            if(drawer)rect(r,848,136,376,1,{73,78,85,255});
        }
        if(page==Page::Toolbar)add(848,584,376,56,"返回",99);
        else if(page!=Page::Home)add(drawer?840:59,48,48,48,"",99,true,false,"",Kind::Icon);
        if(frame.focus<0 || frame.focus>=int(buttons.size()))frame.focus=0;
        for(size_t i=0;i<buttons.size();++i) {
            const auto &b=buttons[i];const bool focused=int(i)==frame.focus;
            const auto fill=!b.enabled?SDL_Color{36,36,41,255}:(touchDown && touchTarget==int(i))?SDL_Color{96,151,255,255}:focused?blue:b.chosen?SDL_Color{71,72,79,255}:panel;
            rounded(r,b.box,fill,10);const char *asset=buttonIcon(b.action,b.title);
            const auto tint=b.enabled?(focused?ink:label):SDL_Color{118,121,128,255};
            if(b.kind==Kind::Tile) {
                for(int n=0;n<12;++n)rect(r,b.box.x+5,b.box.y+5+n*24,b.box.w-10,24,{Uint8(33-n),Uint8(57-n),Uint8(89-n),255});
                image(r,"device",{b.box.x+277,b.box.y+100,96,96});
                aligned(r,f.normal,"进入桌面",{b.box.x+28,b.box.y+b.box.h-72,b.box.w-100,40},ink);
                aligned(r,f.small,"远程访问你的电脑",{b.box.x+28,b.box.y+b.box.h-32,b.box.w-100,30},label);
                image(r,"arrow",{b.box.x+b.box.w-54,b.box.y+b.box.h-55,28,28});
            }else if(b.kind==Kind::Shortcut) {
                if(asset)image(r,asset,{b.box.x+(b.box.w-30)/2,b.box.y+13,30,30});
                aligned(r,f.small,b.title,{b.box.x+8,b.box.y+51,b.box.w-16,30},tint,true);
            }else if(b.kind==Kind::Toggle) {
                const int sx=b.box.x+b.box.w-68,sy=b.box.y+(b.box.h-28)/2;
                rounded(r,{sx,sy,48,28},b.chosen?SDL_Color{41,185,184,255}:SDL_Color{99,104,113,255},14);
                rounded(r,{sx+(b.chosen?23:3),sy+3,22,22},ink,11);
                const auto split=b.title.find('\n');
                if(split==std::string::npos)aligned(r,f.small,b.title,{b.box.x+20,b.box.y,b.box.w-100,b.box.h},tint);
                else {aligned(r,f.small,b.title.substr(0,split),{b.box.x+20,b.box.y+17,b.box.w-100,32},tint);
                    aligned(r,f.small,b.title.substr(split+1),{b.box.x+20,b.box.y+51,b.box.w-100,32},tint);}
            }else if(b.kind==Kind::Icon) {
                if(asset)image(r,asset,{b.box.x+(b.box.w-24)/2,b.box.y+(b.box.h-24)/2,24,24});
            }else if(b.kind==Kind::Button || b.kind==Kind::Tab) {
                int tw=0,th=0;const auto display=fit(f.small,b.title,b.box.w-(asset?62:32));TTF_SizeUTF8(f.small,display.c_str(),&tw,&th);
                const int total=tw+(asset?36:0),x=b.box.x+(b.box.w-total)/2;
                if(asset)image(r,asset,{x,b.box.y+(b.box.h-24)/2,24,24});
                aligned(r,f.small,display,{x+(asset?36:0),b.box.y,tw,b.box.h},tint);
            }else {
                const bool arrow=opensPage(b.action);const int left=asset?64:20;
                const bool radio=page==Page::Quality?(b.action==40 || b.action==41):
                    page==Page::Bitrate || page==Page::Resolution || page==Page::Codec || page==Page::QualityPreset || page==Page::Playout || page==Page::InputMode || page==Page::Touch || page==Page::Mouse || (page==Page::Audio && b.action!=630);
                int right=(arrow||radio)?48:20,vw=0,vh=0;TTF_SizeUTF8(f.small,b.value.c_str(),&vw,&vh);vw=std::min(vw,140);
                if(asset)image(r,asset,{b.box.x+20,b.box.y+(b.box.h-26)/2,26,26});
                if(vw)aligned(r,f.small,b.value,{b.box.x+b.box.w-right-vw,b.box.y,vw,b.box.h},tint);
                aligned(r,f.small,b.title,{b.box.x+left,b.box.y,b.box.w-left-right-(vw?vw+16:0),b.box.h},tint);
                if(arrow)image(r,"arrow",{b.box.x+b.box.w-36,b.box.y+(b.box.h-22)/2,22,22});
                if(radio)image(r,b.chosen?"radio":"radio_empty",{b.box.x+b.box.w-38,b.box.y+(b.box.h-26)/2,26,26});
            }
        }

        if(networkOverlay && page==Page::StreamPreview) {
            // UU TV tv_control_hint.xml: bottom/end anchored, black alpha 40%, small text.
            rounded(r,{1012,548,264,98},SDL_Color{0,0,0,102},5);
            const SDL_Color hintColor{116,117,119,217};
            aligned(r,f.small,"延迟 -- ms",{1020,552,230,28},hintColor);
            aligned(r,f.small,"帧率 -- FPS",{1020,582,230,28},hintColor);
            aligned(r,f.small,"码率 -- Mbps",{1020,612,230,28},hintColor);
        }
        if(notice.empty())aligned(r,f.small,"离线预览 · 设置仅在本次会话生效",{65,638,710,32},label);
        else aligned(r,f.small,notice,{65,638,710,32},SDL_Color{149,191,255,255});
        rect(r,0,672,1280,48,SDL_Color{23,24,27,255});
        uu::display::present(r);
        bool confirm=false,cancel=false;int dx=0,dy=0,hit=-1;
        SDL_Event e;while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT)alive=false;
            if(page==Page::Keyboard && e.type==SDL_TEXTINPUT && keyboardText.size()+std::string(e.text.text).size()<=2048)keyboardText+=e.text.text;
            if(page==Page::Keyboard && e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_BACKSPACE && !keyboardText.empty()) {
                size_t last=keyboardText.size()-1;while(last>0 && (static_cast<unsigned char>(keyboardText[last])&0xc0)==0x80)--last;keyboardText.resize(last);
            }
            if(e.type==SDL_KEYDOWN && !e.key.repeat) {
                const auto k=e.key.keysym.sym;confirm|=k==SDLK_RETURN;cancel|=k==SDLK_ESCAPE;
                dx+=(k==SDLK_RIGHT)-(k==SDLK_LEFT);dy+=(k==SDLK_DOWN)-(k==SDLK_UP);
            }
            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
                touchDown=true;touchTarget=-1;touchStartY=e.button.y;
                SDL_Point p{e.button.x,e.button.y};
                for(size_t i=0;i<buttons.size();++i)if(buttons[i].enabled && SDL_PointInRect(&p,&buttons[i].box)){touchTarget=int(i);frame.focus=int(i);}
            }
            if(e.type==SDL_MOUSEMOTION && touchDown) {
                SDL_Point p{e.motion.x,e.motion.y};
                if(touchTarget>=0 && !SDL_PointInRect(&p,&buttons[touchTarget].box))touchTarget=-1;
                if(page==Page::Devices && std::abs(e.motion.y-touchStartY)>40) {
                    deviceOffset=std::clamp(deviceOffset+(e.motion.y<touchStartY?1:-1),0,std::max(0,int(snapshot.devices.size())-5));
                    touchTarget=-1;touchStartY=e.motion.y;
                }
            }
            if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT) {
                SDL_Point p{e.button.x,e.button.y};
                if(touchTarget>=0 && SDL_PointInRect(&p,&buttons[touchTarget].box))hit=touchTarget;
                touchTarget=-1;touchDown=false;
            }
            if(e.type==SDL_MOUSEWHEEL && page==Page::Devices)
                deviceOffset=std::clamp(deviceOffset-e.wheel.y,0,std::max(0,int(snapshot.devices.size())-5));
        }
#ifdef __SWITCH__
        padUpdate(&uiPad);const auto held=padGetButtons(&uiPad),pressed=padGetButtonsDown(&uiPad);
        if(!held)armed=true;
        if(draining){HidTouchScreenState closingTouch{};hidGetTouchScreenStates(&closingTouch,1);
            if(!held && !closingTouch.count)done=true;SDL_Delay(8);continue;}
        if(armed) {
            confirm|=pressed&HidNpadButton_A;cancel|=pressed&HidNpadButton_B;
            dx+=(bool(pressed&HidNpadButton_Right))-(bool(pressed&HidNpadButton_Left));
            dy+=(bool(pressed&HidNpadButton_Down))-(bool(pressed&HidNpadButton_Up));
            if(pressed&HidNpadButton_Plus){draining=true;continue;}
            HidTouchScreenState touch{};hidGetTouchScreenStates(&touch,1);
            if(touch.count) {
                SDL_Point p{int(touch.touches[0].x),int(touch.touches[0].y)};
                if(!touchDown){touchTarget=-1;touchStartY=p.y;
                    for(size_t i=0;i<buttons.size();++i)if(buttons[i].enabled && SDL_PointInRect(&p,&buttons[i].box)){touchTarget=int(i);frame.focus=int(i);}}
                if(touchTarget>=0 && !SDL_PointInRect(&p,&buttons[touchTarget].box))touchTarget=-1;
                if(page==Page::Devices && std::abs(p.y-touchStartY)>40) {
                    deviceOffset=std::clamp(deviceOffset+(p.y<touchStartY?1:-1),0,std::max(0,int(snapshot.devices.size())-5));
                    touchTarget=-1;touchStartY=p.y;
                }
            }else if(touchDown){hit=touchTarget;touchTarget=-1;}
            touchDown=touch.count>0;
        }
#else
        if(draining){done=true;continue;}
#endif
        if(cancel){back();continue;}
        if(!armed)continue;
        if((dx||dy) && !buttons.empty()) {
            const auto origin=buttons[frame.focus].box;double best=1e9;int next=frame.focus;
            for(size_t i=0;i<buttons.size();++i)if(buttons[i].enabled && int(i)!=frame.focus) {
                const auto b=buttons[i].box;const double x=b.x+b.w*.5-origin.x-origin.w*.5,y=b.y+b.h*.5-origin.y-origin.h*.5;
                const double forward=dx?x*dx:y*dy,side=dx?std::abs(y):std::abs(x);
                if(forward>1 && forward+side*3<best){best=forward+side*3;next=int(i);}
            }
            frame.focus=next;if(page==Page::Schemes && next<2)draft=next+1;
        }
        if(hit>=0){frame.focus=hit;confirm=true;}
        if(confirm && !buttons.empty() && buttons[frame.focus].enabled) {
            const auto action=buttons[frame.focus].action;
            if(action>=1000 && action<1100){const auto index=size_t(action-1000);if(index<snapshot.devices.size()){device=snapshot.devices[index].value("alias",std::string("电脑"));back();notice="已切换预览设备";}continue;}
            if(action>=2000 && action<2200){if(keyboardText.size()<256)keyboardText+=char(action-2000);continue;}
            if(page==Page::Resolution && action!=99){resolution=action-400;back();continue;}
            if(page==Page::Codec && action!=99){codec=action-500;back();continue;}
            if(page==Page::QualityPreset && action>=510 && action<=513){quality=action-510;back();continue;}
            if(page==Page::Playout && action!=99){bufferMs=action-550;back();continue;}
            if(page==Page::InputMode && action!=99){inputMode=action-600;back();continue;}
            if(page==Page::Touch && (action==610 || action==614)){nativeMultitouch=action==614;continue;}
            if(page==Page::Mouse && action!=99){mouseSpeed=action-620;back();continue;}
            if(page==Page::Audio && action>=665){volume=action-640;continue;}
            switch(action) {
            case 99:back();break;case 10:push(Page::Devices);break;case 11:push(Page::Account);break;
            case 12:push(Page::StreamPreview);break;case 14:push(Page::Toolbar);break;case 13:push(Page::Gamepad);break;
            case 20:push(Page::Disconnect);break;case 21:push(Page::Display);break;case 22:push(Page::Safety);break;
            case 23:push(Page::Peripherals);break;case 24:push(Page::More);break;case 25:push(Page::Windows);break;
            case 30:push(Page::Quality);break;case 31:push(Page::Picture);break;
            case 40:fps=60;break;case 41:fps=30;break;
            case 43:push(Page::QualityPreset);break;
            case 44:push(Page::Resolution);break;case 45:push(Page::Codec);break;case 46:vsync=!vsync;break;
            case 47:push(Page::Playout);break;case 48:push(Page::Audio);break;
            case 53:push(Page::InputMode);break;case 54:push(Page::Touch);break;
            case 55:SDL_StopTextInput();push(Page::Keyboard);break;case 56:push(Page::Stats);break;
            case 57:push(Page::About);break;case 58:push(Page::Logout);break;case 59:push(Page::Mouse);break;
            case 71:case 72:case 73:pendingShortcut=action==71?"显示桌面":action==72?"任务管理器":"切换窗口";push(Page::WindowsConfirm);break;
            case 74:case 75:case 76:pendingShortcut=action==74?"锁定桌面":action==75?"重启电脑":"关机";push(Page::WindowsConfirm);break;
            case 630:sound=!sound;break;
            case 750:notice="自动解锁尚未开放";break;case 751:autoLock=!autoLock;break;
            case 752:remoteMute=!remoteMute;break;case 753:restoreSound=!restoreSound;break;
            case 754:networkOverlay=!networkOverlay;notice=networkOverlay?"常驻已开启，返回串流画面查看":"常驻网络信息已关闭";break;
            case 760:back();notice="离线预览不会向电脑执行系统操作";break;
            case 770:push(Page::Login);notice="仅切换预览页面，实际账号保持登录";break;
            case 800:case 801:case 802:case 803:{const char *apps[]={"游戏库","文件管理","浏览器","终端"};pendingShortcut=apps[action-800];push(Page::AppLaunch);break;}
            case 2300:if(!keyboardText.empty()){size_t n=keyboardText.size()-1;while(n>0 && (static_cast<unsigned char>(keyboardText[n])&0xc0)==0x80)--n;keyboardText.resize(n);}break;
            case 2301:keyboardText+=' ';break;case 2302:keyboardText.clear();break;
            case 2303:notice=keyboardText.empty()?"请先输入文字":"已预览发送动作，未向远端发送";break;
            case 2304:
#ifdef __SWITCH__
                if(auto edited=textInput(keyboardText))keyboardText=*edited;
                touchDown=false;touchTarget=-1;
#else
                SDL_StartTextInput();notice="可使用电脑键盘编辑草稿";
#endif
                break;
            case 2310:case 2311:case 2312:case 2313:case 2314:case 2315:notice="离线预览不发送远程按键";break;
            case 50:draft=scheme;push(Page::Schemes);stack.back().focus=draft-1;break;
            case 51:push(Page::Guide);break;case 52:push(Page::Login);break;case 60:draft=1;break;case 61:draft=2;break;
            case 62:scheme=draft;back();notice="预览方案已应用；实际按键映射保持原配置";break;
            case 70:push(Page::Network);break;case 80:back();break;
            case 81:stack={{Page::Home,0}};break;
            case 90:notice="示例设备列表已刷新";break;
            case 91:notice="设备连接将在下一阶段接入";break;
            default:notice="离线预览不执行远端操作";break;
            }
        }
        SDL_Delay(8);
    }
    SDL_StopTextInput();return alive;
}
} // namespace tvui
