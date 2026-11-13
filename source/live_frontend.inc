// Live UI controller. No nested render loop, SDL polling, or presentation here.
namespace tvui {
struct LiveInput {bool confirm=false,cancel=false,up=false,down=false,left=false,right=false,toolbar=false,network=false,qualityMenu=false,keyboard=false,disconnect=false;uint64_t held=0;};
class LiveFrontend {
    uu::Client &client;std::shared_ptr<uu::Media> media;SDL_Renderer *renderer;
    std::vector<Frame> stack;std::vector<Button> buttons;
    int scheme=1,draft=1,fps=60,resolution=720,codec=0,bufferMs=0,touchMode=1,inputMode=1,xboxLayout=1,volume=100,mouseSpeed=1;
    int vibration=uu::vibration::kDefaultStrength;
    int selectedController=0,draftInputMode=1;
    bool guideRemote=false;
    std::array<int,2> controllerIllustrations{};
    std::array<uu::PadLayout,2> controllerLayouts{};
    std::array<bool,2> controllerConnected{},controllerFixedSwitch{};
    static constexpr const char *legendNames[]={"Switch","Xbox 360","PS4 DualShock 4"};
    uu::PadLegend legendFor(int index)const{
        if(controllerLayouts[index]!=uu::PadLayout::Full || controllerFixedSwitch[index])return uu::PadLegend::Switch;
        return static_cast<uu::PadLegend>(controllerIllustrations[index]);
    }
    std::string controllerTitle(int index)const{
        if(!controllerConnected[index])return "未连接";
        if(controllerLayouts[index]==uu::PadLayout::LeftJoy)return "左 Joy-Con（横握）";
        if(controllerLayouts[index]==uu::PadLayout::RightJoy)return "右 Joy-Con（横握）";
        if(controllerFixedSwitch[index])return controllerNames[index];
        return std::string("完整手柄 · ")+legendNames[controllerIllustrations[index]]+"图示";
    }
    std::string faceName(int index,int source)const{
        if(controllerLayouts[index]!=uu::PadLayout::Full){
            static constexpr const char *left[]={"→","↓","↑","←"},*right[]={"X","A","Y","B"};
            return controllerLayouts[index]==uu::PadLayout::LeftJoy?left[source]:right[source];
        }
        static constexpr const char *keys[3][4]={{"A","B","X","Y"},{"B","A","Y","X"},{"○","×","△","□"}};
        return keys[static_cast<int>(legendFor(index))][source];
    }
    std::string outputName(int source)const{
        static constexpr const char *targets[]={"不发送","Xbox A","Xbox B","Xbox X","Xbox Y","LB","RB","LT","RT","L3","R3","Start","Back","方向↑","方向↓","方向←","方向→"};
        return targets[uu::padTarget(source,remoteSwitchHabit(selectedController),controllerLayouts[selectedController])];
    }
    int selectedDevice=0,deviceOffset=0,touchTarget=-1,touchStartY=0,hit=-1,pointerX=0,pointerY=0;
    uu::DeviceSelection deviceSelection;
    uint64_t deviceListRevision=UINT64_MAX;
    std::string resolvedDeviceId;
    bool vsyncFailed=false;
    bool vsync=false,sound=true,autoUnlock=false,autoLock=false,remoteMute=true,restoreSound=true;
    bool controllerSetupRequested=false;std::array<std::string,2> controllerNames{{"未连接","未连接"}};
    bool nativeMultitouch=true;Uint32 modeNoticeUntil=0;
    std::optional<int> lastPrivacyMode;
    bool keyboardEditRequested=false,sendingText=false,customQuality=false;
    uu::Json safetyProfiles=uu::Json::object();std::string safetyDevice;bool safetyQueued=false;
    uint64_t textRevisionAtSend=0;
    uu::RuntimeProfile runtimeProfile;
    bool resetTouchZoom=false,touchDown=false,wasStreaming=false,wasAuthenticated=false,draining=false;
    Uint32 settingsSentAt=0,connectedAt=0;int requestedCap=0,sourceQuality=3,customMbps=30;
    std::string device,keyboardText,draftDevice,pendingShortcut,notice,saveError;
    static constexpr const char *prefsPath=
#ifdef __SWITCH__
        "sdmc:/switch/UURemote-ns/ui-state.json";
#else
        "ui-state.json";
#endif
    static constexpr int vibrationLevels[]={0,30,60,100};
    static constexpr const char *vibrationNames[]={"关闭","弱 30%","中 60%","强 100%"};
    std::string vibrationName()const{
        static constexpr const char *shortNames[]={"关闭","弱","中","强"};
        for(int i=0;i<4;++i)if(vibration==vibrationLevels[i])return shortNames[i];
        return std::to_string(vibration)+"%";
    }
    static std::string number(double value){if(value<0)return "--";char out[32];std::snprintf(out,sizeof out,"%.1f",value);return out;}
    enum class DeviceState {Online,Offline};
    // TV 3.5.0 MyDevice.a(): only exact raw status "CONNECTED" is online;
    // all other/missing/non-string statuses take the original DISCONNECTED fallback.
    static DeviceState deviceState(const uu::Json &device){
        if(!device.is_object()) return DeviceState::Offline;
        const auto it=device.find("status");
        if(it==device.end() || !it->is_string()) return DeviceState::Offline;
        return it->get<std::string>()=="CONNECTED"?DeviceState::Online:DeviceState::Offline;
    }
    static const char *deviceStateText(DeviceState state){
        return state==DeviceState::Online?"在线":"离线";
    }
    static bool deviceConnectable(DeviceState state){return state==DeviceState::Online;}
    static bool connectableDevice(const uu::Json &device){
        if(!deviceConnectable(deviceState(device))) return false;
        const auto id=device.find("device_id");
        return id!=device.end() && id->is_string() && !id->get<std::string>().empty();
    }
    static bool readPrefs(const std::string &path,uu::Json &p){
        try{
            std::ifstream file(path,std::ios::binary);if(!file)return false;
            std::string data((std::istreambuf_iterator<char>(file)),{});
            if(file.bad() || data.size()>4096)return false;
            auto value=uu::Json::parse(data);if(!value.is_object())return false;
            if(value.contains("schema_version")){
                const auto &version=value["schema_version"];
                if(!version.is_number_integer() || version.get<int>()<1 || version.get<int>()>1)return false;
            }else{
                // Older prefs had no schema marker; current fields are
                // backward-compatible, so migrate in memory and persist on the
                // next save.
                value["schema_version"]=1;
            }
            for(const char *key:{"network_overlay","sound","multitouch","auto_lock","remote_mute","restore_sound"})
                if(value.contains(key)&&!value[key].is_boolean())return false;
            for(const char *key:{"volume","mouse_speed","touch_mode","scheme","xbox_layout","vibration"})
                if(value.contains(key)&&!value[key].is_number_integer())return false;
            if(value.contains("controller_illustrations")){
                const auto &values=value["controller_illustrations"];
                if(!values.is_array() || values.size()!=2)return false;
                for(const auto &v:values)if(!v.is_number_integer() || v<0 || v>2)return false;
            }
            if(value.contains("last_device_id") &&
               (!value["last_device_id"].is_string() || value["last_device_id"].get_ref<const std::string&>().size()>256))return false;
            if(value.contains("safety_devices")){
                const auto &profiles=value["safety_devices"];
                if(!profiles.is_object() || profiles.size()>16)return false;
                for(auto it=profiles.begin();it!=profiles.end();++it){
                    if(it.key().empty() || it.key().size()>256 || !it.value().is_object())return false;
                    for(auto setting=it.value().begin();setting!=it.value().end();++setting){
                        if(setting.key()=="privacy_mode"){
                            if(!setting.value().is_number_integer() || setting.value().get<int>()<1 || setting.value().get<int>()>3)return false;
                        }else if((setting.key()!="auto_lock" && setting.key()!="remote_mute" && setting.key()!="restore_sound" && setting.key()!="privacy_screen") || !setting.value().is_boolean())return false;
                    }
                }
            }
            p=std::move(value);return true;
        }catch(...){return false;}
    }
    static bool commitPrefs(){
#ifdef __SWITCH__
        return R_SUCCEEDED(fsdevCommitDevice("sdmc"));
#else
        return true;
#endif
    }
    bool save(){
        const std::string path=prefsPath,tmp=path+".tmp",backup=path+".previous";
        auto fail=[&](){saveError="设置保存失败；本次仍生效，重启后可能恢复旧设置";return false;};
        try{
            auto preferences=uu::Json{{"schema_version",1},{"network_overlay",networkOverlay},{"sound",sound},{"volume",volume},{"mouse_speed",mouseSpeed},{"touch_mode",1},{"scheme",scheme},{"xbox_layout",xboxLayout},{"controller_illustrations",controllerIllustrations},{"multitouch",nativeMultitouch},{"vibration",vibration}};
            if(!safetyProfiles.empty())preferences["safety_devices"]=safetyProfiles;
            if(!deviceSelection.recent().empty())preferences["last_device_id"]=deviceSelection.recent();
            const auto data=preferences.dump(2);
            if(data.size()>4096)return fail();
            FILE *file=std::fopen(tmp.c_str(),"wb");if(!file)return fail();
            bool ok=std::fwrite(data.data(),1,data.size(),file)==data.size();
            ok=std::fflush(file)==0&&ok;ok=std::fclose(file)==0&&ok;
            if(!ok || !commitPrefs())return fail();
            struct stat st{};const bool hadOriginal=stat(path.c_str(),&st)==0;
            bool movedValidOriginal=false;
            if(hadOriginal){
                uu::Json previous;
                if(readPrefs(path,previous)){
                    if(stat(backup.c_str(),&st)==0 && std::remove(backup.c_str())!=0)return fail();
                    if(std::rename(path.c_str(),backup.c_str())!=0)return fail();
                    movedValidOriginal=true;
                }else{
                    // Keep a valid recovery copy while quarantining the corrupt primary.
                    std::string invalid;unsigned suffix=0;
                    do{invalid=path+".invalid-"+std::to_string(suffix++);}
                    while(stat(invalid.c_str(),&st)==0 && suffix<1024);
                    if(stat(invalid.c_str(),&st)==0 || std::rename(path.c_str(),invalid.c_str())!=0)return fail();
                }
            }
            if(std::rename(tmp.c_str(),path.c_str())!=0){
                if(movedValidOriginal)std::rename(backup.c_str(),path.c_str());
                commitPrefs();return fail();
            }
            if(!commitPrefs())return fail(); // Preserve recovery copy on uncertain commit.
            std::remove(backup.c_str()); // A cleanup failure does not invalidate committed data.
            saveError.clear();return true;
        }catch(...){return fail();}
    }
    void stopKeyboard(){
        SDL_StopTextInput();keyboardEditRequested=false;
        if(sendingText)client.command("text-input-cancel");
        sendingText=false;
    }
    void loadSafety(const std::string &id){
        safetyDevice=id;safetyQueued=false;
        lastPrivacyMode.reset();
        const auto profile=safetyProfiles.find(id);
        if(profile!=safetyProfiles.end() && profile->is_object()){
            const auto mode=profile->find("privacy_mode");
            if(mode!=profile->end())lastPrivacyMode=mode->get<int>();
            else {
                const auto value=profile->find("privacy_screen");
                if(value!=profile->end())lastPrivacyMode=value->get<bool>()?2:1;
            }
        }
        const auto settings=uu::safetySettingsFor(safetyProfiles,id);
        autoLock=settings.autoLock;remoteMute=settings.remoteMute;restoreSound=settings.restoreSound;
    }
    bool setSafety(const char *key,bool value,const uu::Snapshot &s){
        if(!s.streaming || !s.controlChannelReady || s.activeDeviceId.empty() || s.activeDeviceId!=safetyDevice){notice="电脑控制通道尚未就绪，请稍后重试";return false;}
        if(!safetyProfiles.contains(safetyDevice) && safetyProfiles.size()>=16){notice="已保存16台电脑的安全设置，未添加新设置";return false;}
        safetyProfiles[safetyDevice][key]=value;save();return true;
    }
    void safetyAction(unsigned action,const uu::Snapshot &s,std::optional<bool> enabled={},bool quiet=false){
        uu::Json data={{"action",action},{"deviceId",s.activeDeviceId},{"retrySafety",true}};
        if(enabled)data["enabled"]=*enabled;
        if(quiet)data["quiet"]=true;
        client.command("system-action",std::move(data));
    }
    void applyRemoteSound(const uu::Snapshot &s,bool quiet=false){
        for(auto action:uu::remoteSoundActions(remoteMute,restoreSound))
            safetyAction(action,s,std::nullopt,quiet);
    }
    bool shouldRestoreRemoteSound(const uu::Snapshot &s) const {
        return remoteMute && restoreSound && s.streaming && !s.activeDeviceId.empty();
    }
    void push(Page p){touchTarget=-1;touchDown=false;hit=-1;stack.push_back({p,0});notice.clear();}
    void back(){
        if(!stack.empty() && stack.back().page==Page::Keyboard){
            stopKeyboard();
        }
        touchTarget=-1;touchDown=false;hit=-1;if(stack.size()>1)stack.pop_back();else if(wasStreaming){stack.clear();draining=true;}
    }
    void loadSettings(const uu::Snapshot &s){
        fps=s.fps;resolution=s.height;codec=s.hevc;vsync=s.vsync;bufferMs=s.playoutDelayMs;requestedCap=s.maxBitrateMbps;sourceQuality=s.quality;
        if(s.quality==6 && s.maxBitrateMbps>0 && s.maxBitrateMbps<=100){
            customQuality=true;customMbps=s.maxBitrateMbps;
        }else{
            // Preserve saved native/unknown settings until an explicit choice.
            customQuality=false;
        }
    }
    std::string qualitySummary() const {
        if(customQuality)return "最高 "+std::to_string(customMbps)+" Mbps";
        return "当前设置";
    }
    void applyQuality(const uu::Snapshot &s){
        if(s.qualityPending){notice="正在应用上一次设置，请稍候";loadSettings(s);return;}
        // Shortcuts and the custom page share this one value and Q6 path.
        // Client applies route protection without overwriting the saved cap.
        int requestedQuality=sourceQuality;
        int requestedBitrate=requestedCap;
        if(customQuality){
            requestedQuality=6;requestedBitrate=customMbps;
        }
        client.command("quality-settings",{{"width",resolution==540?960:resolution==1080?1920:1280},{"height",resolution},{"fps",fps},
            {"quality",requestedQuality},{"vsync",vsync},{"hevc",bool(codec)},{"playout_delay_ms",bufferMs},{"max_bitrate_mbps",requestedBitrate}});
        settingsSentAt=SDL_GetTicks();notice="正在应用设置；部分设置需要重新连接";
    }
    void chord(std::initializer_list<int> keys){client.command("input",{{"action","kbd_chord"},{"keys",uu::Json(keys)}});}
    bool canOpenComputerKeyboard(const uu::Snapshot &s)const{
        return s.streaming && s.controlChannelReady && !sendingText && !s.textPending;
    }
    bool dispatchWindowsShortcut(const uu::Snapshot &s,const std::string &name){
        if(!s.streaming) return false;
        if(name=="显示桌面") client.command("system-action",{{"action",2}});
        else if(name=="任务管理器") client.command("system-action",{{"action",13}});
        else if(name=="切换窗口") chord({18,9});
        else if(name=="锁定桌面") client.command("system-action",{{"action",10}});
        else if(name=="重启电脑") client.command("system-action",{{"action",9}});
        else if(name=="关机") client.command("system-action",{{"action",12}});
        else return false;
        return true;
    }
    void action(int a,const uu::Snapshot &s){
        if(a>=1000 && a<1100){
            if(deviceSelection.choose(s.devices,a-1000)){selectedDevice=a-1000;back();}
            return;
        }
        if(a>=2000 && a<2200){if(keyboardText.size()<256)keyboardText+=char(a-2000);return;}
        const auto page=stack.back().page;
        if(a==900 || a==901){selectedController=a-900;return;}
        if(page==Page::ControllerLegend && a>=910 && a<=912){
            if(!controllerConnected[selectedController] || controllerFixedSwitch[selectedController] || controllerLayouts[selectedController]!=uu::PadLayout::Full)return;
            controllerIllustrations[selectedController]=a-910;
            client.command("release");draining=true;save();return;
        }
        if(page==Page::Resolution && a!=99){resolution=a-400;applyQuality(s);back();return;}
        if(page==Page::Codec && a!=99){codec=a-500;applyQuality(s);back();return;}
        if(page==Page::QualityPreset && a>=510 && a<=513){
            const auto choice=uu::qualityPresetRequest(a-510);
            customMbps=choice.maxBitrateMbps;requestedCap=customMbps;
            customQuality=true;applyQuality(s);back();return;
        }
        if(page==Page::Playout && a!=99){bufferMs=a-550;applyQuality(s);back();return;}
        if(page==Page::Touch && (a==610 || a==611)){touchMode=1;nativeMultitouch=false;client.command("release");resetTouchZoom=true;draining=true;save();return;}
        if(page==Page::Mouse && a>=620 && a<=623){mouseSpeed=a-620;save();back();return;}
        if(page==Page::Audio && a>=665){volume=a-640;media->setVolume(sound?volume:0);save();return;}
        if(page==Page::Vibration && a>=780 && a<=783){
            vibration=vibrationLevels[a-780];uu::vibration::setStrength(vibration);
            if(!vibration)uu::vibration::stop();
            save();return;
        }
        switch(a){
        case 99:case 80:back();break;
        case 10:push(Page::Devices);break;case 11:push(Page::Account);break;
        case 12:
            if(selectedDevice>=0 && selectedDevice<int(s.devices.size()) &&
               connectableDevice(s.devices[selectedDevice]))
                client.command("connect",{{"deviceId",s.devices[selectedDevice]["device_id"]}});
            break;
        case 13:push(Page::Gamepad);break;case 14:push(Page::Toolbar);break;
        case 20:push(Page::Disconnect);break;case 21:push(Page::Display);break;case 22:push(Page::Safety);break;case 757:push(Page::ScreenProtection);break;
        case 23:push(Page::Peripherals);break;case 24:case 70:push(Page::More);break;case 25:push(Page::Windows);break;
        case 30:loadSettings(s);push(Page::Quality);break;case 31:loadSettings(s);push(Page::Picture);break;
        case 40:fps=60;applyQuality(s);break;case 41:fps=30;applyQuality(s);break;
        case 43:push(Page::QualityPreset);break;
        case 44:push(Page::Resolution);break;case 45:push(Page::Codec);break;
        case 46:vsync=!vsync;applyQuality(s);break;case 47:push(Page::Playout);break;case 48:push(Page::Audio);break;
        case 50:draft=scheme;push(Page::Schemes);stack.back().focus=draft-1;break;
        case 51:push(Page::Guide);break;case 52:case 58:push(Page::Logout);break;case 53:draftInputMode=inputMode;push(Page::InputMode);break;
        case 54:push(Page::Touch);break;case 55:SDL_StopTextInput();push(Page::Keyboard);break;
        case 56:push(Page::Stats);break;case 57:push(Page::About);break;case 59:push(Page::Mouse);break;
        case 856:
            runtimeProfile=uu::readRuntimeProfile();
            if(page!=Page::DecodeProfile)push(Page::DecodeProfile);
            break;
        case 60:draft=1;break;case 61:draft=2;break;case 62:scheme=draft;client.command("release");save();back();break;
        case 602:case 603:xboxLayout=a-602;client.command("release");draining=true;save();break;
        case 614:nativeMultitouch=true;client.command("release");resetTouchZoom=true;draining=true;save();break;
        case 612:resetTouchZoom=true;notice="画面缩放已重置";break;
        case 613:
            if(legendFor(selectedController)==uu::PadLegend::Switch)push(Page::XboxLayout);
            else {guideRemote=true;push(Page::ControllerGuide);stack.back().focus=3;}break;
        case 617:push(Page::ControllerLegend);break;
        case 618:guideRemote=false;push(Page::ControllerGuide);stack.back().focus=selectedController;break;
        case 906:guideRemote=false;break;case 907:guideRemote=true;break;
        case 784:push(Page::Vibration);break;
        case 615:client.command("release");controllerSetupRequested=true;draining=true;break;
        case 600:case 601:draftInputMode=a-600;break;
        case 604:
            if(draftInputMode!=inputMode){
                inputMode=draftInputMode;client.command("release");draining=true;
                uu::diagnostic(inputMode?"input-mode-gamepad-confirmed":"input-mode-keyboard-confirmed");
                modeNoticeUntil=SDL_GetTicks()+1800;
            }
            back();break;
        case 605:back();break;
        case 71:case 72:case 73:{
            const char *name=a==71?"显示桌面":a==72?"任务管理器":"切换窗口";
            if(!dispatchWindowsShortcut(s,name)){notice="请先连接电脑";break;}
            stack.clear();draining=true;notice="已提交操作请求，等待电脑端结果";break;
        }
        case 74:case 75:case 76:pendingShortcut=a==74?"锁定桌面":a==75?"重启电脑":"关机";push(Page::WindowsConfirm);break;
        case 630:sound=!sound;media->setVolume(sound?volume:0);save();break;
        case 751:
            if(!setSafety("auto_lock",!autoLock,s))break;
            autoLock=!autoLock;safetyAction(19,s,autoLock);notice.clear();break;
        case 752:
            if(!setSafety("remote_mute",!remoteMute,s))break;
            remoteMute=!remoteMute;applyRemoteSound(s);notice.clear();break;
        case 753:
            if(!setSafety("restore_sound",!restoreSound,s))break;
            restoreSound=!restoreSound;
            if(remoteMute)safetyAction(uu::remoteSoundAction(remoteMute,restoreSound),s);
            notice=remoteMute?"":"已保存；开启电脑静音后生效";break;
        case 754:networkOverlay=!networkOverlay;save();break;
        case 755:case 756:
            if(s.streaming && s.textChannelReady && !s.privacyPending){
                const int current=s.privacyKnown?int(s.privacyMode):0;
                const int selected=a==755?2:3;
                client.command("privacy-screen",{{"deviceId",s.activeDeviceId},{"mode",current==selected?1:selected}});
            }
            break;
        case 759:
            if(s.streaming && s.textChannelReady && !s.cursorCapturePending){
                client.command("cursor-capture",{{"deviceId",s.activeDeviceId}});
                stack.clear();draining=true;
            }
            break;
        case 760:
            if(!dispatchWindowsShortcut(s,pendingShortcut)){notice="请先连接电脑";break;}
            stack.clear();draining=true;notice="已提交操作请求，等待电脑端结果";break;
        case 770:client.command("logout");client.command("login");stack.clear();draining=true;break;
        case 81:client.command("disconnect",{{"restoreSound",shouldRestoreRemoteSound(s)},{"deviceId",s.activeDeviceId}});stack.clear();draining=true;break;
        case 90:client.command("refresh");break;
        case 2300:if(!sendingText && !keyboardText.empty()){auto n=keyboardText.size()-1;while(n && (static_cast<unsigned char>(keyboardText[n])&0xc0)==0x80)--n;keyboardText.resize(n);}break;
        case 2301:if(!sendingText && keyboardText.size()<2048)keyboardText+=' ';break;case 2302:if(!sendingText)keyboardText.clear();break;
        case 2303:
            if(!s.streaming){notice="请先连接电脑";break;}
            if(keyboardText.empty()){notice="请先输入文字";break;}
            if(sendingText || s.textPending)break;
            textRevisionAtSend=s.textRevision;sendingText=true;client.command("text-input",{{"text",keyboardText}});notice="正在发送文字…";break;
        case 2304:
            if(!sendingText){SDL_StopTextInput();keyboardEditRequested=true;}break;
        case 2310:chord({13});break;case 2311:chord({9});break;case 2312:chord({27});break;
        case 2313:chord({8});break;case 2314:chord({17,65});break;case 2315:chord({17,86});break;
        case 2316:
            if(!canOpenComputerKeyboard(s))break;
            chord({91,17,79}); // Win + Ctrl + O: Windows On-Screen Keyboard.
            stopKeyboard();
            stack.clear();
            draining=true;
            break;
        default:notice="此功能尚未支持";break;
        }
    }
public:
    bool networkOverlay=false;
    bool restoreRemoteSoundOnDisconnect(const uu::Snapshot &s)const{return shouldRestoreRemoteSound(s);}
    explicit LiveFrontend(uu::Client &c,std::shared_ptr<uu::Media> m,SDL_Renderer *r):client(c),media(std::move(m)),renderer(r){
        uu::Json p;
        if(readPrefs(prefsPath,p) || readPrefs(std::string(prefsPath)+".previous",p)){
            networkOverlay=p.value("network_overlay",false);sound=p.value("sound",true);volume=std::clamp(p.value("volume",100),0,100);
            mouseSpeed=std::clamp(p.value("mouse_speed",1),0,3);touchMode=std::clamp(p.value("touch_mode",0),0,1);scheme=std::clamp(p.value("scheme",1),1,2);xboxLayout=std::clamp(p.value("xbox_layout",1),0,1);nativeMultitouch=p.value("multitouch",true);
            vibration=std::clamp(p.value("vibration",uu::vibration::kDefaultStrength),0,100);
            // Old input_mode and gamepad_bindings are intentionally ignored.
            if(p.contains("controller_illustrations"))controllerIllustrations=p["controller_illustrations"].get<decltype(controllerIllustrations)>();
            touchMode=1;
            safetyProfiles=p.value("safety_devices",uu::Json::object());
            deviceSelection.restore(p.value("last_device_id",std::string{}));
        }
        media->setVolume(sound?volume:0);
        uu::vibration::setStrength(vibration);
    }
    ~LiveFrontend(){SDL_StopTextInput();}
    bool active()const{return !stack.empty();}
    bool atHome()const{return !stack.empty() && stack.back().page==Page::Home;}
    bool captures()const{return active()||draining;}
    int mapping()const{return scheme;}
    bool trackpad()const{return touchMode==1;}
    bool multitouch()const{return nativeMultitouch;}
    bool takeKeyboardEdit(std::string &value){
        if(!keyboardEditRequested)return false;
        keyboardEditRequested=false;value=keyboardText;return true;
    }
    void finishKeyboardEdit(const std::optional<std::string> &value){
        SDL_StopTextInput();if(value && value->size()<=2048)keyboardText=*value;
        touchTarget=hit=-1;touchDown=false;draining=true;
    }
    bool gamepadMode()const{return inputMode==1;}
    void controllers(std::array<std::string,2> names,std::array<uu::PadLayout,2> layouts,
                     std::array<bool,2> connected,std::array<bool,2> fixedSwitch){
        controllerNames=std::move(names);controllerLayouts=layouts;controllerConnected=connected;controllerFixedSwitch=fixedSwitch;
    }
    bool takeControllerSetup(){return std::exchange(controllerSetupRequested,false);}
    bool remoteSwitchHabit(int index)const{return legendFor(index)==uu::PadLegend::Switch && xboxLayout==1;}
    bool takeZoomReset(){const bool result=resetTouchZoom;resetTouchZoom=false;return result;}
    float pointerSpeed()const{const float speeds[]={.4f,.7f,1.f,1.4f};return speeds[mouseSpeed];}
    void sync(const uu::Snapshot &s,bool displayVsyncFailed){
        vsyncFailed=displayVsyncFailed;
        // Remember only a stream that actually connected, including one ended
        // by HOME/forced close. A failed attempt never replaces the recent PC.
        if(s.streaming && deviceSelection.connected(s.activeDeviceId))save();
        if(!s.authenticated && wasAuthenticated){keyboardText.clear();draftDevice.clear();}
        if(!s.activeDeviceId.empty() && s.activeDeviceId!=draftDevice){
            if(!draftDevice.empty()){stopKeyboard();keyboardText.clear();}
            draftDevice=s.activeDeviceId;
        }
        if(s.activeDeviceId!=safetyDevice)loadSafety(s.activeDeviceId);
        if(s.streaming && s.privacyKnown && !s.activeDeviceId.empty() &&
           (!lastPrivacyMode || *lastPrivacyMode!=int(s.privacyMode))){
            lastPrivacyMode=int(s.privacyMode);
            if(safetyProfiles.contains(s.activeDeviceId) || safetyProfiles.size()<16){
                safetyProfiles[s.activeDeviceId]["privacy_mode"]=int(s.privacyMode);
                safetyProfiles[s.activeDeviceId].erase("privacy_screen");save();
            }else saveError="已保存16台电脑的安全设置，本次隐私状态无法保存";
        }
        if(s.streaming!=wasStreaming || s.authenticated!=wasAuthenticated){stack.clear();buttons.clear();touchTarget=-1;hit=-1;touchDown=false;draining=true;
            if(s.authenticated && !s.streaming)push(Page::Home);
            stopKeyboard();
            if(s.streaming){
                connectedAt=SDL_GetTicks();
            }
        }
        if(!s.streaming)safetyQueued=false;
        if(s.streaming && s.controlChannelReady && !s.activeDeviceId.empty() && !safetyQueued){
            // Apply the full effective policy, including unsaved defaults. A
            // restore-only saved profile must still select temporary mute.
            safetyAction(19,s,autoLock,true);
            applyRemoteSound(s,true);
            safetyQueued=true;
        }
        if(sendingText && s.textRevision>textRevisionAtSend && !s.textPending && s.textResult>=2){
            sendingText=false;notice=s.textNotice;
            if(s.textResult==2){keyboardText.clear();SDL_StopTextInput();stack.clear();draining=true;}
        }
        wasStreaming=s.streaming;wasAuthenticated=s.authenticated;
        if(s.authenticated && !s.streaming && stack.empty())push(Page::Home);
        if(!s.qualityPending && Uint32(SDL_GetTicks()-settingsSentAt)>1000)loadSettings(s);
        if(deviceListRevision!=s.devicesRevision || resolvedDeviceId!=deviceSelection.selected()) {
            selectedDevice=deviceSelection.resolve(s.devices);
            device=s.devices.empty()?"尚未发现电脑":s.devices[selectedDevice].value("alias",std::string("电脑"));
            deviceListRevision=s.devicesRevision;
            resolvedDeviceId=deviceSelection.selected();
        }
    }
    void pointer(bool down,int x,int y){
        if(!active() || draining)return;
        if(down){pointerX=x;pointerY=y;}SDL_Point point{pointerX,pointerY};
        if(down && !touchDown){touchTarget=-1;touchStartY=y;for(size_t i=0;i<buttons.size();++i)if(buttons[i].enabled&&SDL_PointInRect(&point,&buttons[i].box)){touchTarget=int(i);stack.back().focus=int(i);}}
        if(down && touchTarget>=0 && !SDL_PointInRect(&point,&buttons[touchTarget].box))touchTarget=-1;
        if(down && stack.back().page==Page::Devices && std::abs(y-touchStartY)>40){deviceOffset=std::max(0,deviceOffset+(y<touchStartY?1:-1));touchTarget=-1;touchStartY=y;}
        if(!down && touchDown && touchTarget>=0 && SDL_PointInRect(&point,&buttons[touchTarget].box))hit=touchTarget;
        touchDown=down;if(!down)touchTarget=-1;
    }
    void event(const SDL_Event &e){if(!active()||draining)return;
        if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT)pointer(true,e.button.x,e.button.y);
        if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT)pointer(false,e.button.x,e.button.y);
        if(e.type==SDL_MOUSEMOTION && touchDown)pointer(true,e.motion.x,e.motion.y);
        if(stack.back().page==Page::Keyboard && !sendingText && e.type==SDL_TEXTINPUT && keyboardText.size()+std::string(e.text.text).size()<=2048)keyboardText+=e.text.text;
        if(stack.back().page==Page::Keyboard && !sendingText && SDL_IsTextInputActive() &&
           e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_BACKSPACE && !keyboardText.empty()){
            size_t end=keyboardText.size()-1;
            while(end && (static_cast<unsigned char>(keyboardText[end])&0xc0)==0x80)--end;
            keyboardText.resize(end);
        }
        if(e.type==SDL_MOUSEWHEEL && stack.back().page==Page::Devices)deviceOffset=std::max(0,deviceOffset-e.wheel.y);
    }
    bool input(const LiveInput &in,const uu::Snapshot &s,bool touchHeld){
        const bool before=captures();
        if(draining){if(!in.held && !touchHeld && !in.confirm && !in.cancel && !in.toolbar && !in.network && !in.qualityMenu && !in.keyboard && !in.disconnect)draining=false;return true;}
        if(s.streaming && (in.toolbar || in.network || in.qualityMenu || in.keyboard || in.disconnect)){
            stopKeyboard();
            client.command("release");if(in.toolbar && active()){stack.clear();buttons.clear();draining=true;return true;}
            SDL_StopTextInput();stack.clear();push(in.network?Page::More:in.qualityMenu?Page::Quality:in.keyboard?Page::Keyboard:in.disconnect?Page::Disconnect:Page::Toolbar);loadSettings(s);return true;
        }
        if(!active())return before;
        if(in.cancel){back();return true;}
        auto &frame=stack.back();if(frame.focus<0 || frame.focus>=int(buttons.size()))frame.focus=0;
        const int dx=int(in.right)-int(in.left),dy=int(in.down)-int(in.up);
        if((dx||dy) && !buttons.empty()){
            const auto origin=buttons[frame.focus].box;double best=1e9;int next=frame.focus;
            for(size_t i=0;i<buttons.size();++i)if(buttons[i].enabled && int(i)!=frame.focus){auto b=buttons[i].box;double x=b.x+b.w*.5-origin.x-origin.w*.5,y=b.y+b.h*.5-origin.y-origin.h*.5;
                double forward=dx?x*dx:y*dy,side=dx?std::abs(y):std::abs(x);if(forward>1&&forward+side*3<best){best=forward+side*3;next=int(i);}}
            frame.focus=next;if(frame.page==Page::Schemes && next<2)draft=next+1;
        }
        if(hit>=0 && hit<int(buttons.size())){frame.focus=hit;hit=-1;if(buttons[frame.focus].enabled)action(buttons[frame.focus].action,s);}
        else if(in.confirm && !buttons.empty() && buttons[frame.focus].enabled)action(buttons[frame.focus].action,s);
        return true;
    }
    void draw(SDL_Renderer *r,Fonts &f,const uu::Snapshot &snapshot,const uu::StreamStatsSnapshot &stats){
        if(!active()){
            if(snapshot.streaming && int32_t(modeNoticeUntil-SDL_GetTicks())>0){
                rounded(r,{430,78,420,54},panel,12);
                aligned(r,f.small,inputMode?"手柄直连":"键鼠映射",{430,78,420,54},ink,true);
            }return;
        }
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

        const bool drawer=page!=Page::Home && page!=Page::Devices && page!=Page::Gamepad &&
            page!=Page::Schemes && page!=Page::Guide && page!=Page::Login && page!=Page::Keyboard && page!=Page::About && page!=Page::More && page!=Page::Network && page!=Page::Logout && page!=Page::StreamPreview && page!=Page::DecodeProfile && page!=Page::ControllerGuide;
        if(!snapshot.streaming || !drawer)backdrop(r);
        if(drawer) {

            rect(r,0,0,1280,720,{0,0,0,216});rounded(r,{824,32,424,624},panel,13);
        }
        std::string title;
        switch(page) {
        case Page::Home: {
            brand(r,f);
            add(736,48,176,48,"切换设备",10);add(928,48,128,48,"刷新",90);add(1072,48,144,48,"账号",11);
            const bool hasDevice=!snapshot.devices.empty() && selectedDevice>=0 &&
                                  selectedDevice<int(snapshot.devices.size());
            const auto selectedState=hasDevice?deviceState(snapshot.devices[selectedDevice]):DeviceState::Offline;
            // Only confirmed Online gets green; all non-CONNECTED states are offline/muted.
            const auto chipBg=selectedState==DeviceState::Online?SDL_Color{40,62,52,255}:panel;
            const auto chipFg=selectedState==DeviceState::Online?SDL_Color{150,213,172,255}:label;
            rounded(r,{65,142,96,36},chipBg,18);
            aligned(r,f.small,deviceStateText(selectedState),{65,142,96,36},chipFg,true);
            aligned(r,f.normal,device,{177,142,1038,36},ink);
            add(65,202,650,356,"进入桌面",12,hasDevice && snapshot.stage=="idle" &&
                connectableDevice(snapshot.devices[selectedDevice]),false,"",Kind::Tile);

            aligned(r,f.normal,"你的电脑，随时连接",{775,280,420,56},ink);
            aligned(r,f.small,"连接后显示可用应用",{775,348,420,48},label);
            homeHotkeys(r,f);
            break;
        }
        case Page::StreamPreview:
            centered(r,f.normal,"串流画面预览",286);
            centered(r,f.small,"正在连接电脑",340,label);
            add(1030,48,184,52,"工具栏",14);
            break;
        case Page::Devices:
            title="我的电脑";
            for(size_t i=size_t(deviceOffset);i<std::min<size_t>(snapshot.devices.size(),size_t(deviceOffset+5));++i) {
                const auto &d=snapshot.devices[i];const auto state=deviceState(d);
                std::string label=d.value("alias",std::string("电脑"));label+=" · ";label+=deviceStateText(state);
                add(80,160+(int(i)-deviceOffset)*85,1090,72,std::move(label),1000+int(i),connectableDevice(d));
            }
            if(snapshot.devices.empty())text(r,f.normal,"暂未发现电脑，请返回原界面登录或刷新",100,210,label);
            break;
        case Page::Toolbar:
            add(848,56,112,88,"退出",20,true,false,"",Kind::Shortcut);
            add(976,56,112,88,"电脑键盘",2316,canOpenComputerKeyboard(snapshot),false,"",Kind::Shortcut);
            add(1104,56,112,88,sound?"声音":"已静音",48,true,false,"",Kind::Shortcut);
            rect(r,848,160,376,1,{73,78,85,255});
            row(0,"显示",21);row(1,"安全",22);row(2,"外设",23);row(3,"更多",24);row(4,"Windows 快捷操作",25);break;
        case Page::Display:title="显示";row(0,"画质与帧率",30,true,false,std::to_string(fps)+" FPS");row(1,"画面设置",31,true,false,std::to_string(resolution)+"p");break;
        case Page::Quality:title="画质与帧率";
            row(0,"60 帧",40,true,fps==60);row(1,"30 帧",41,true,fps==30);
            row(2,"画质档位",43,true,false,qualitySummary());
            row(3,"播放缓冲",47,true,false,std::to_string(bufferMs)+" ms");break;
        case Page::Picture:title="画面设置";row(0,"分辨率",44,true,false,std::to_string(resolution)+"p");row(1,"视频编码",45,true,false,codec?"H.265":"H.264");row(2,"垂直同步",46,true,false,vsyncFailed?"未能应用":vsync?"开启":"关闭");break;
        case Page::Resolution:title="分辨率";row(0,"960 × 540",400+540,true,resolution==540);row(1,"1280 × 720",400+720,true,resolution==720);row(2,"1920 × 1080",400+1080,true,resolution==1080);break;
        case Page::Codec:title="视频编码";row(0,"H.264",500,true,codec==0);row(1,"H.265",501,true,codec==1);break;
        case Page::QualityPreset:title="画质";{
            const int activePreset=customQuality?uu::qualityPresetIndexForMbps(customMbps):-1;
            for(int i=0;i<4;++i)row(i,uu::qualityPresets[i].name,510+i,true,activePreset==i,
                "最高 "+std::to_string(uu::qualityPresets[i].maxBitrateMbps)+" Mbps");
            aligned(r,f.small,"高清及以上建议 CPU 超频",{848,560,376,28},label);
            aligned(r,f.small,"提升流畅度",{848,588,376,28},label);
            }break;
        case Page::Bitrate:break; // Reserved page ID while the custom entry is hidden.
        case Page::Playout:title="播放缓冲";{int values[]={0,8,16,24,35};for(int i=0;i<5;++i)row(i,std::to_string(values[i])+" ms",550+values[i],true,bufferMs==values[i]);}
            aligned(r,f.small,"缓冲越低越跟手；网络不稳时更易卡顿",{848,556,376,60},label);break;
        case Page::Peripherals:title="外设";row(0,"手柄设备",13);row(1,"Switch 文字输入",55);row(2,"触屏操作",54,true,false,nativeMultitouch?"平板多点触控":"手机鼠标指针");row(3,"鼠标设置",59);row(4,"手柄震动",784,true,false,vibrationName());break;
        case Page::Vibration:title="手柄震动";
            for(int i=0;i<4;++i)row(i,vibrationNames[i],780+i,true,vibration==vibrationLevels[i]);
            aligned(r,f.small,"仅手柄直连模式生效",{848,490,376,36},label);
            aligned(r,f.small,"由电脑游戏发出震动；菜单打开时暂停",{848,528,376,60},label);break;
        case Page::Gamepad:{
            title="手柄设备";text(r,f.normal,"已连接手柄",90,180,ink);
            for(int i=0;i<2;++i)add(85,235+i*94,510,74,"玩家 "+std::to_string(i+1)+" · "+controllerTitle(i),900+i,true,selectedController==i,"",Kind::Menu);
            add(85,438,510,66,"连接手柄／调整顺序",615,true,false,"",Kind::Menu);
            add(680,230,490,64,"输入模式",53,true,false,inputMode?"手柄直连":"键鼠映射",Kind::Menu);
            if(inputMode)add(680,318,490,64,"远程按键布局",613,controllerConnected[selectedController],false,legendFor(selectedController)==uu::PadLegend::Switch?(xboxLayout?"按字母":"按位置"):"标准位置",Kind::Menu);
            else add(680,318,490,64,"键鼠映射方案",50,true,false,scheme==1?"方案一":"方案二",Kind::Menu);
            add(680,406,490,64,"操作说明",618,controllerConnected[selectedController],false,"玩家 "+std::to_string(selectedController+1),Kind::Menu);
            const bool canChoose=controllerConnected[selectedController] && !controllerFixedSwitch[selectedController] && controllerLayouts[selectedController]==uu::PadLayout::Full;
            add(680,494,490,64,"手柄图示",617,canChoose,false,canChoose?(controllerIllustrations[selectedController]==0?"Switch":controllerIllustrations[selectedController]==1?"Xbox 360":"PS4"):controllerConnected[selectedController]?"Joy-Con 自动识别":"未连接",Kind::Menu);
            }break;
        case Page::Schemes: {
            title="键鼠映射方案";add(445,111,165,49,"方案一",60,true,draft==1,"",Kind::Tab);add(650,111,165,49,"方案二",61,true,draft==2,"",Kind::Tab);
            const bool second=draft==2;
            const char *head[]={"左摇杆","方向键","右摇杆","正面按键"};
            const char *one[]={"移动鼠标\n按下：显示键盘","上 Ctrl\n下 Shift\n左 Backspace\n右 Delete","方向键\n按下：显示桌面","B 回车\nA Esc\nY 鼠标左键\nX 鼠标右键"};
            const char *two[]={"上下：滚轮\n按下：显示键盘","上 / 下 / 左 / 右","移动鼠标\n按下：Shift","B 回车\nA Esc\nY 空格\nX Backspace"};
            switchControllerDiagram(r,f.hint);
            text(r,f.small,second?"ZL  鼠标右键":"ZL  滚轮向上",100,222,label,320);
            text(r,f.small,second?"L Ctrl   短按 − Tab":"L Alt   短按 − Windows",100,269,label,340);
            text(r,f.small,second?"ZR  鼠标左键":"ZR  滚轮向下",885,222,label,300);
            text(r,f.small,second?"R Alt   L + ＋ 菜单":"R Tab   L + ＋ 菜单",885,269,label,300);
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

        case Page::XboxLayout:title="远程按键布局";
            row(0,"按位置",602,true,xboxLayout==0);row(1,"按字母",603,true,xboxLayout==1);
            aligned(r,f.small,"只改变电脑收到的游戏按键",{848,350,376,36},ink);
            aligned(r,f.small,"按位置：下侧键 → Xbox A",{848,399,376,36},label);
            aligned(r,f.small,"按字母：逻辑 A → Xbox A",{848,439,376,36},label);
            aligned(r,f.small,"本地仍为右侧确认、下侧返回",{848,495,376,36},label);
            aligned(r,f.hint,"所有 Switch / Joy-Con 玩家共用",{848,550,376,36},label);break;
        case Page::InputMode:title="输入模式";
            row(0,"手柄直连",601,true,draftInputMode==1);row(1,"键鼠映射",600,true,draftInputMode==0);
            aligned(r,f.small,"全部玩家共用；启动默认手柄",{848,350,376,52},label);
            aligned(r,f.small,"键鼠仅由玩家 1 操作",{848,410,376,36},label);
            row(4,"确认切换",604,draftInputMode!=inputMode);row(5,"取消",605);break;
        case Page::ControllerLegend:title="完整手柄图示";
            for(int i=0;i<3;++i)row(i,legendNames[i],910+i,controllerConnected[selectedController] && !controllerFixedSwitch[selectedController] && controllerLayouts[selectedController]==uu::PadLayout::Full,controllerIllustrations[selectedController]==i);
            aligned(r,f.small,"玩家 "+std::to_string(selectedController+1)+"：按实际手柄选择",{848,430,376,40},label);
            aligned(r,f.small,"Xbox / PS 按标准位置发送",{848,480,376,36},label);
            aligned(r,f.hint,"电脑输出：Xbox 360 兼容手柄",{848,526,376,36},label);break;
        case Page::ControllerGuide:{
            title="手柄操作说明";
            for(int i=0;i<2;++i)add(85+i*255,114,235,48,"玩家 "+std::to_string(i+1)+(selectedController==i?" · 当前":""),900+i,true,selectedController==i,"",Kind::Tab);
            add(680,114,235,48,"本地菜单",906,true,!guideRemote,"",Kind::Tab);
            add(935,114,235,48,"远程游戏",907,true,guideRemote,"",Kind::Tab);
            aligned(r,f.normal,controllerTitle(selectedController),{90,177,510,42},ink);
            if(!controllerConnected[selectedController]){aligned(r,f.small,"该玩家尚未连接手柄",{90,285,510,60},label);break;}
            const auto layout=controllerLayouts[selectedController];const auto legend=legendFor(selectedController);
            controllerDiagram(r,f,layout,legend);
            aligned(r,f.small,layout==uu::PadLayout::Full?"正面按键 · 简化图":"横握：摇杆在左，面键在右",{90,585,510,36},label,true);
            if(!guideRemote){
                aligned(r,f.small,"本地菜单 · 按 Switch 位置操作",{680,205,490,44},label);
                for(int i=0;i<2;++i){
                    const SDL_Rect card{680+i*255,275,235,72};rounded(r,card,panel,10);
                    aligned(r,f.small,std::string(i==0?"右侧 ":"下侧 ")+faceName(selectedController,i)+(i==0?" → 确认":" → 返回"),card,ink,true);
                }
                aligned(r,f.small,layout==uu::PadLayout::Full?"摇杆或方向键移动选项":"横握 Joy-Con 使用摇杆移动",{680,385,490,44},label);
                aligned(r,f.small,"本地规则固定，不随远程布局改变",{680,431,490,44},label);
                const char *menu=layout==uu::PadLayout::LeftJoy?"本地菜单：触屏或另一只手柄的组合键":layout==uu::PadLayout::RightJoy?"按住 SL，再按 ＋ 打开或收起菜单":legend==uu::PadLegend::Xbox?"按住 LB，再按 Start 打开或收起菜单":legend==uu::PadLegend::PlayStation?"按住 L1，再按 OPTIONS 打开或收起菜单":"按住 L，再按 ＋ 打开或收起菜单";
                aligned(r,f.small,menu,{680,493,490,60},label);
            }else if(!gamepadMode()){
                aligned(r,f.normal,"当前为键鼠映射",{680,205,490,44},ink);
                aligned(r,f.small,"仅玩家 1 操作电脑",{680,275,490,44},label);
                aligned(r,f.small,"按键随当前键鼠方案发送",{680,335,490,44},label);
                add(680,420,490,64,"查看键鼠映射方案",50,true,false,scheme==1?"方案一":"方案二",Kind::Menu);
            }else{
                const char *layoutName=legend==uu::PadLegend::Switch?(xboxLayout?"远程游戏 · 按字母":"远程游戏 · 按位置"):"远程游戏 · 标准布局";
                aligned(r,f.small,layoutName,{680,205,490,44},label);
                static constexpr int sources[]={2,3,0,1};
                static constexpr const char *positions[]={"右侧 ","下侧 ","上侧 ","左侧 "};
                for(int i=0;i<4;++i){
                    const int source=sources[i];const SDL_Rect card{680+(i%2)*255,265+(i/2)*76,235,64};rounded(r,card,panel,10);
                    aligned(r,f.small,std::string(positions[source])+faceName(selectedController,source)+" → "+outputName(source),card,ink,true);
                }
                if(layout!=uu::PadLayout::Full){
                    aligned(r,f.small,"SL / SR → LB / RB",{680,430,490,32},label);
                    aligned(r,f.small,layout==uu::PadLayout::LeftJoy?"L / ZL → LT / RT":"R / ZR → LT / RT",{680,466,490,32},label);
                    aligned(r,f.small,layout==uu::PadLayout::LeftJoy?"− → Start":"＋ 短按 → Start",{680,502,490,32},label);
                    aligned(r,f.hint,"单摇杆；没有独立十字键和第二摇杆",{680,540,490,26},label);
                }else if(legend==uu::PadLegend::PlayStation){
                    aligned(r,f.small,"L1 / R1 → LB / RB；L2 / R2 → LT / RT",{680,430,490,36},label);
                    aligned(r,f.small,"摇杆、方向键按标准布局发送",{680,472,490,36},label);
                    aligned(r,f.small,"OPTIONS / SHARE → Start / Back",{680,514,490,36},label);
                }else{
                    aligned(r,f.small,"摇杆、方向键、肩键按标准布局发送",{680,436,490,44},label);
                    aligned(r,f.small,legend==uu::PadLegend::Xbox?"START / BACK → Start / Back":"＋ / − → Start / Back",{680,500,490,36},label);
                }
                aligned(r,f.hint,"电脑输出：Xbox 360 兼容手柄",{680,575,490,26},label);
                if(legend==uu::PadLegend::PlayStation)aligned(r,f.hint,"触摸板点击 / 滑动尚未接入",{680,601,490,30},label);
            }
            }break;
        case Page::Touch:title="触屏操作";
            row(0,"平板多点触控",614,true,nativeMultitouch);
            row(1,"手机鼠标指针",610,true,!nativeMultitouch);
            aligned(r,f.small,nativeMultitouch?"像平板一样，直接触摸电脑画面":"滑动画面移动指针，轻点执行左键",{848,365,376,45},label);
            aligned(r,f.small,nativeMultitouch?"支持多指操作，由电脑应用响应":"按住左键按钮，再用另一指拖动",{848,415,376,45},label);
            aligned(r,f.small,nativeMultitouch?"适合触屏应用与多点手势":"右侧滑动条滚动，旁边按钮为中键",{848,465,376,45},label);break;
        case Page::Mouse:title="鼠标设置";{
            const char *names[]={"慢速","标准","快速","很快"};for(int i=0;i<4;++i)row(i,names[i],620+i,true,mouseSpeed==i);
            row(4,snapshot.cursorCapturePending?"等待电脑确认…":"恢复鼠标指针",759,
                snapshot.streaming && snapshot.textChannelReady && !snapshot.cursorCapturePending);
            aligned(r,f.hint,snapshot.cursorCaptureNotice.empty()?"点击后返回画面，查看指针是否恢复":snapshot.cursorCaptureNotice,{848,580,376,64},label);
            }break;
        case Page::Audio:title="声音";row(0,"播放声音",630,true,false,sound?"开启":"关闭");{int values[]={25,50,75,100};for(int i=0;i<4;++i)row(i+1,std::to_string(values[i])+"%",640+values[i],true,volume==values[i]);}break;
        case Page::Guide:title="操作说明";
            rounded(r,{90,160,1100,420},panel,12);
            {const char *left[]={"首页退出","串流菜单","游戏菜单","触屏操作","切换模式"};
             const char *right[]={"在首页长按 ＋，退出应用并返回启动器","按住 L，再按 ＋ 打开或收起菜单；两种模式通用","手柄直连时单独按 ＋，操作电脑游戏的菜单","外设中可选平板多点触控，或手机鼠标指针","进入外设的输入模式页面，选择后确认切换"};
             for(int i=0;i<5;++i){aligned(r,f.normal,left[i],{120,185+i*72,200,56},ink);aligned(r,f.small,right[i],{355,185+i*72,800,56},label);}}break;
        case Page::More:
        case Page::Network:title=page==Page::More?"更多 · 网络信息":"网络信息";{
            aligned(r,f.normal,"网络延迟  "+(stats.rttMs>=0?std::to_string(stats.rttMs)+" ms":"--"),{90,130,700,60},ink);
            aligned(r,f.hint,"连接信息",{90,194,510,25},label);aligned(r,f.hint,"播放性能",{660,194,510,25},label);
            const char *names[]={"连接通道","被控端地址","控制端地址","客户端版本","客户端系统","解码耗时","显示帧率","视频码率","初传缺口","连接时间"};
            const std::string values[]={stats.transport,stats.remoteEndpoint,stats.localEndpoint,UU_VERSION,"Nintendo Switch",
                formatMillis(stats.decodeMs),number(stats.presentFps)+" FPS",formatMbps(stats.video.bitrateMbps),formatPercent(stats.video.initialGapPercent),
                snapshot.streaming?std::to_string((SDL_GetTicks()-connectedAt)/1000)+" 秒":"--"};
            for(int i=0;i<10;++i){
                const int col=i/5,line=i%5,x=90+col*570,y=220+line*53;
                aligned(r,f.small,names[i],{x,y,160,42},label);
                if(i==1 || i==2)endpointValue(r,f,values[i],{x+180,y-3,330,50});
                else aligned(r,f.small,values[i],{x+180,y,330,42},ink);
            }
            rect(r,90,502,1100,1,{63,67,74,255});
            const char *codecNames[]={"视频编码","编码器信息","解码器"};const std::string codecValues[]={stats.codec,stats.encoder,stats.decoder};
            for(int i=0;i<3;++i){const int x=90+i*370;aligned(r,f.hint,codecNames[i],{x,510,340,26},label);aligned(r,f.small,codecValues[i],{x,538,340,34},ink);}
            add(875,130,315,60,"常驻网络信息",754,true,networkOverlay,"",Kind::Toggle);
            add(930,579,260,48,"详细运行统计",56);
            }break;
        case Page::Stats:title="运行统计";
            info(0,"解码 / 显示 FPS",number(stats.decodeFps)+" / "+number(stats.presentFps));
            info(1,"解码耗时",formatMillis(stats.decodeMs));info(2,"初传缺口",formatPercent(stats.video.initialGapPercent));
            info(3,"NACK / RTX",std::to_string(stats.nackSent)+" / "+std::to_string(stats.rtxRecovered));
            info(4,"解码错误 / 丢帧",std::to_string(stats.decodeErrors)+" / "+std::to_string(stats.droppedFrames));
            add(848,556,376,64,"解码耗时分析",856);break;
        case Page::DecodeProfile:title="解码耗时分析";{
#ifndef UU_LEAN_STATS
            const auto stages=uu::LiveTelemetry::decodeStages();
#endif
            auto precise=[](double value){char text[48];std::snprintf(text,sizeof text,"%.3f ms",value);return std::string(text);};
            auto cell=[&](int col,int line,const std::string &name,const std::string &value){
                const int x=90+col*570,y=178+line*58;
                aligned(r,f.small,name,{x,y,200,48},label);
                aligned(r,f.small,value,{x+210,y,300,48},ink);
            };
            cell(0,0,"解码总耗时",precise(stats.decodeMs));
#ifdef UU_LEAN_STATS
            cell(0,1,"提交与码流解析","未采集");
            cell(0,2,"取帧与完成等待","未采集");
            cell(0,3,"帧整理与发布","未采集");
#else
            cell(0,1,"提交与码流解析",precise(stages.sendUs/1000.0));
            cell(0,2,"取帧与完成等待",precise(stages.receiveUs/1000.0));
            cell(0,3,"帧整理与发布",precise(stages.publishUs/1000.0));
#endif
            cell(0,4,"CPU回传 / 原生发布",std::to_string(stats.cpuTransfers)+" / "+std::to_string(stats.nativePublished));
            cell(0,5,"解码器",stats.decoder);
            const char *modules[]={"CPU 频率","GPU 频率","内存频率","NVDEC 频率"};
            for(unsigned i=0;i<4;++i){
                std::string value;
                if(!runtimeProfile.sampled)value="尚未读取";
                else if(runtimeProfile.clockResults[i]){
                    char error[40];std::snprintf(error,sizeof error,"不可读 (0x%X)",runtimeProfile.clockResults[i]);value=error;
                }else value=number(double(runtimeProfile.clockHz[i])/1000000.0)+" MHz";
                cell(1,int(i),modules[i],value);
            }
            cell(1,4,"启动方式",runtimeProfile.isNso?"完整 NSP / NSO":"NRO");
            cell(1,5,"显示输出",uu::display::active()?"原生 deko3d":"SDL 回退");
#ifdef UU_LEAN_STATS
            aligned(r,f.hint,"精简统计已启用，分段计时关闭；总耗时含软件处理与等待",{90,542,1100,32},label);
#else
            aligned(r,f.hint,"分段值为近似平均；总耗时含软件处理与等待，并非纯硬件运算时间",{90,542,1100,32},label);
#endif
            add(90,590,320,56,"刷新频率读数",856);
            aligned(r,f.hint,"频率仅在打开本页或点刷新时读取，不会调整",{440,592,750,52},label);
            }break;
        case Page::Safety:{title="安全";
            add(848,176,376,64,"自动解锁（尚未开放）",750,false,autoUnlock,"",Kind::Toggle);
            add(848,252,376,64,"断开后锁定电脑",751,snapshot.streaming,autoLock,"",Kind::Toggle);
            add(848,328,376,64,"串流时电脑静音",752,snapshot.streaming,remoteMute,"",Kind::Toggle);
            add(848,404,376,64,"结束后恢复电脑声音",753,true,restoreSound,"",Kind::Toggle);
            const std::string current=snapshot.privacyPending?"等待确认":!snapshot.privacyKnown?"未确认":
                snapshot.privacyMode==uu::PrivacyMode::ScreenOff?"显示屏关闭":
                snapshot.privacyMode==uu::PrivacyMode::Normal?"隐私保护":"关闭";
            row(4,"屏幕保护",757,true,false,current);
            break;}
        case Page::ScreenProtection:{title="屏幕保护";
            const int mode=snapshot.privacyKnown?int(snapshot.privacyMode):0;
            const bool ready=snapshot.streaming && snapshot.textChannelReady && !snapshot.privacyPending;
            const std::string status=snapshot.privacyPending?"（等待电脑）":snapshot.privacyKnown?"":"（未确认）";
            add(848,176,376,64,"隐私保护"+status,755,ready,mode==2,"",Kind::Toggle);
            aligned(r,f.hint,"遮蔽电脑画面，远程操作继续",{868,248,336,44},label);
            add(848,320,376,64,"关闭显示屏"+status,756,ready,mode==3,"",Kind::Toggle);
            aligned(r,f.hint,"使用电脑端增强隐私功能",{868,392,336,44},label);
            std::string stateText=snapshot.privacyNotice;
            if(stateText.empty())stateText=!snapshot.streaming?"连接电脑后可设置":!snapshot.privacyKnown?
                "电脑当前状态未确认，请选择要设置的模式":"状态由电脑端报告";
            // Status is internal UTF-8 text; wrap within the reserved three-line
            // area rather than truncating an important fallback/error message.
            size_t offset=0;
            for(int line=0;line<3 && offset<stateText.size();++line){
                size_t end=offset;
                while(end<stateText.size()){
                    size_t next=end+1;
                    while(next<stateText.size() && (uint8_t(stateText[next])&0xc0)==0x80)++next;
                    int width=0,height=0;
                    TTF_SizeUTF8(f.hint,stateText.substr(offset,next-offset).c_str(),&width,&height);
                    if(width>376)break;
                    end=next;
                }
                aligned(r,f.hint,stateText.substr(offset,end-offset),{848,448+line*26,376,26},label);
                offset=end;
            }
            break;}
        case Page::Windows:title="Windows 快捷操作";
            row(0,"锁定桌面",74);row(1,"重启电脑",75);row(2,"关机",76);
            row(3,"任务管理器",72);row(4,"显示桌面",71);row(5,"切换窗口",73);break;
        case Page::WindowsConfirm:title=pendingShortcut;
            aligned(r,f.small,pendingShortcut=="重启电脑"?"确认重启当前连接的电脑？":pendingShortcut=="关机"?"确认关闭当前连接的电脑？":"确认锁定当前连接的电脑？",{848,180,376,72},label);row(2,"取消",80);row(3,"确认操作",760);break;
        case Page::AppLaunch:title=pendingShortcut;
            aligned(r,f.small,"示例应用入口",{848,180,376,64},ink);
            aligned(r,f.small,"连接电脑后可读取真实应用列表",{848,258,376,96},label);row(3,"返回",80);break;
        case Page::Account:
            title="账号";
            rounded(r,{1000,166,72,72},blue,36);image(r,"account",{1020,186,32,32});
            aligned(r,f.normal,snapshot.nickname.empty()?"已登录账号":snapshot.nickname,{848,254,376,48},ink,true);
            aligned(r,f.small,"与电脑端使用同一账号",{848,303,376,40},label,true);
            add(848,374,376,64,"重新登录",52,true,false,"",Kind::Menu);
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
            centered(r,f.small,"退出后需要重新扫码登录",332,label);
            add(330,438,290,72,"退出",770);add(660,438,290,72,"取消",80);break;
        case Page::About:title="关于";
            aboutInformation(r,f);
            add(980,574,180,48,"重新登录",52);break;
        case Page::Keyboard:title="键盘输入";
            aligned(r,f.small,"先在电脑选中输入框，再编辑文字",{90,132,1100,42},label);
            add(90,190,1100,92,keyboardText.empty()?"点这里输入文字":keyboardText,2304,!sendingText);
            add(90,304,220,58,"编辑文字",2304,!sendingText);
            add(330,304,180,58,"清空草稿",2302,!sendingText && !keyboardText.empty());
            add(530,304,364,58,"电脑屏幕键盘",2316,canOpenComputerKeyboard(snapshot));
            add(914,304,276,58,sendingText?"发送中…":"发送到电脑",2303,!sendingText && !keyboardText.empty());
            aligned(r,f.small,"常用按键",{90,402,400,40},ink);
            {const char *keys[]={"回车","Tab","Esc","退格","全选","粘贴"};
             for(int i=0;i<6;++i)add(90+i*186,458,170,58,keys[i],2310+i,snapshot.streaming && !sendingText);}
            aligned(r,f.hint,"文字会同步到电脑剪贴板，确认后自动粘贴；失败时保留草稿",{90,552,1100,36},label);
            aligned(r,f.small,sendingText?"正在等待电脑确认…":snapshot.textNotice,{90,593,1100,36},label);break;
        case Page::Disconnect:title="退出远控？";aligned(r,f.small,"退出后返回电脑首页",{848,180,376,72},label);row(2,"继续远控",80);row(3,"返回首页",81);break;
        }
        if(!title.empty()) {

            aligned(r,f.normal,title,{drawer?904:124,40,drawer?320:1000,64},ink);
            if(drawer)rect(r,848,136,376,1,{73,78,85,255});
        }
        if(page==Page::Toolbar)add(848,584,376,56,"返回",99);
        else if(page!=Page::Home)add(drawer?840:59,48,48,48,"",99,true,false,"",Kind::Icon);
        // Toolbar shortcuts occupy this band; only titled drawers have room for the hint.
        if(snapshot.streaming && drawer && page!=Page::Toolbar)
            aligned(r,f.hint,"本地菜单 · 游戏操作已暂停",{848,110,376,22},label);
        if(snapshot.streaming && !drawer && page!=Page::Home)
            aligned(r,f.hint,"本地菜单 · 摇杆移动 · 右侧键确认 / 下侧键返回 · 远程按键暂停",{85,666,1110,30},label);
        for(auto &button:buttons) {
            const int a=button.action;
            const bool pendingSetting=snapshot.qualityPending && (a==40||a==41||a==46||(a>=510&&a<=515)||(page==Page::Bitrate&&a!=99)||(page==Page::Resolution&&a!=99)||(page==Page::Codec&&a!=99)||(page==Page::Playout&&a!=99));
            const bool unsupported=a==750;
            const bool needsStream=(a>=71&&a<=76)||a==2303||(a>=751&&a<=753);
            if(pendingSetting || unsupported || (needsStream&&!snapshot.streaming) ||
               (a>=751 && a<=753 && !snapshot.controlChannelReady))button.enabled=false;
            if(unsupported && button.kind==Kind::Menu)button.value="暂未支持";
        }
        deviceOffset=std::clamp(deviceOffset,0,std::max(0,int(snapshot.devices.size())-5));
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
                else {aligned(r,f.small,b.title.substr(0,split),{b.box.x+20,b.box.y+b.box.h/2-27,b.box.w-100,27},tint);
                    aligned(r,f.small,b.title.substr(split+1),{b.box.x+20,b.box.y+b.box.h/2,b.box.w-100,27},tint);}
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
                    page==Page::Bitrate || page==Page::Resolution || page==Page::Codec || (page==Page::QualityPreset && b.action!=514) || page==Page::Playout || (page==Page::InputMode && (b.action==600 || b.action==601)) || page==Page::ControllerLegend || (page==Page::XboxLayout && (b.action==602 || b.action==603)) || (page==Page::Touch && b.action!=612) || (page==Page::Mouse && b.action>=620 && b.action<=623) || page==Page::Vibration || (page==Page::Audio && b.action!=630);
                int right=(arrow||radio)?48:20,vw=0,vh=0;TTF_SizeUTF8(f.small,b.value.c_str(),&vw,&vh);vw=std::min(vw,140);
                if(asset)image(r,asset,{b.box.x+20,b.box.y+(b.box.h-26)/2,26,26});
                if(vw)aligned(r,f.small,b.value,{b.box.x+b.box.w-right-vw,b.box.y,vw,b.box.h},tint);
                aligned(r,f.small,b.title,{b.box.x+left,b.box.y,b.box.w-left-right-(vw?vw+16:0),b.box.h},tint);
                if(arrow)image(r,"arrow",{b.box.x+b.box.w-36,b.box.y+(b.box.h-22)/2,22,22});
                if(radio)image(r,b.chosen?"radio":"radio_empty",{b.box.x+b.box.w-38,b.box.y+(b.box.h-26)/2,26,26});
            }
        }

        if(!saveError.empty())aligned(r,f.small,saveError,{65,638,710,32},SDL_Color{255,170,140,255});
        else if(!snapshot.qualitySaveError.empty())aligned(r,f.small,snapshot.qualitySaveError,{65,638,710,32},SDL_Color{255,170,140,255});
        else if(snapshot.qualitySavePending)aligned(r,f.small,"正在保存画质设置…",{65,638,710,32},label);
        else if((page==Page::Quality || page==Page::Bitrate) && !snapshot.routeBitrateNotice.empty())aligned(r,f.small,snapshot.routeBitrateNotice,{65,638,710,32},label);
        else if(page==Page::Safety && notice.empty() && !snapshot.safetyNotice.empty())aligned(r,f.small,snapshot.safetyNotice,{65,638,710,32},label);
        else if(!snapshot.videoScaleNotice.empty())aligned(r,f.small,snapshot.videoScaleNotice,{65,638,710,32},label);
        else if(notice.empty())aligned(r,f.small,snapshot.qualityNotice.empty()?snapshot.detail:snapshot.qualityNotice,{65,638,710,32},label);
        else aligned(r,f.small,notice,{65,638,710,32},SDL_Color{149,191,255,255});


    }
};
} // namespace tvui
