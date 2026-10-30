// Native touch snapshots use the original binary CONTROL_DATA_CHANNEL protocol.
// Physical finger identity is stable across HID array reordering.
class NativeTouch {
    struct Finger {uint32_t physical;int id;float x,y;};
    std::vector<Finger> fingers;
    bool blocked=false;
    Uint32 lastSent=0;
    std::optional<SDL_FPoint> lastPointer;
    uu::Json points(const std::vector<Finger> &items){
        auto out=uu::Json::array();for(const auto &f:items)out.push_back({{"id",f.id},{"x",f.x},{"y",f.y}});return out;
    }
public:
    std::optional<SDL_FPoint> pointerPosition()const{return lastPointer;}
    void forgetPointer(){lastPointer.reset();}
    template<class Send>void cancel(Send send,bool wait=true){
        if(!fingers.empty())send(uu::Json{{"action","touch_native"},{"type",4},{"points",uu::Json::array()}});
        fingers.clear();blocked=wait;
    }
#ifdef __SWITCH__
    template<class Send>void update(const HidTouchScreenState &t,Uint32 now,bool captured,const SDL_Rect &v,Send send){
        if(captured || v.w<=0 || v.h<=0){cancel(send);return;}
        if(blocked){if(!t.count)blocked=false;return;}
        std::vector<Finger> next;bool used[10]{};
        // Reserve identities before assigning new contacts.
        // Retire lost IDs for this snapshot before reusing them on a later update.
        // A replacement finger must not silently inherit a contact that just ended.
        for(const auto &f:fingers)if(f.id>=0 && f.id<10)used[f.id]=true;
        const int count=std::clamp(int(t.count),0,int(std::size(t.touches)));
        for(int i=0;i<count && next.size()<10;++i){const auto p=t.touches[i];
            auto old=std::find_if(fingers.begin(),fingers.end(),[&](const Finger &f){return f.physical==p.finger_id;});
            const int x=int(p.x),y=int(p.y);
            if(old==fingers.end() && (x<v.x || y<v.y || x>=v.x+v.w || y>=v.y+v.h))continue;
            int id=old==fingers.end()?0:old->id;
            if(old==fingers.end()){while(id<10 && used[id])++id;if(id==10)continue;used[id]=true;}
            next.push_back({p.finger_id,id,std::clamp((x-v.x)/float(v.w),0.f,1.f),std::clamp((y-v.y)/float(v.h),0.f,1.f)});
        }
        std::sort(next.begin(),next.end(),[](const Finger&a,const Finger&b){return a.id<b.id;});
        if(!next.empty())lastPointer=SDL_FPoint{next.front().x,next.front().y};
        const auto data=points(next);const bool countChange=next.size()!=fingers.size();
        if(data==points(fingers) && (next.empty() || now-lastSent<250))return;
        if(!countChange && now-lastSent<16)return;
        const int type=next.empty()?3:fingers.empty()?1:2;
        send(uu::Json{{"action","touch_native"},{"type",type},{"points",data}});
        fingers=std::move(next);lastSent=now;
    }
#endif
};
