// Phone-style pointer controls. Each physical finger keeps its role until up.
class PhoneMouse {
    enum class Role { Move,Left,Right,Middle,Wheel,Ignore };
    struct Contact {uint32_t id;Role role;int x,y,startX,startY;Uint32 began;bool moved=false;};
    std::vector<Contact> contacts;
    bool blocked=false;
    unsigned held=0;
    float wheelRemainder=0;
    Uint32 wheelAt=0;
    std::optional<SDL_FPoint> pendingPointer;
    static SDL_Rect leftBox(){return {512,568,112,80};}
    static SDL_Rect rightBox(){return {640,568,112,80};}
    static SDL_Rect middleBox(){return {1104,348,76,76};}
    static SDL_Rect wheelBox(){return {1200,286,52,200};}
    static bool inside(int x,int y,SDL_Rect box){SDL_Point p{x,y};return SDL_PointInRect(&p,&box);}
    static unsigned button(Role r){return r==Role::Left?1:r==Role::Right?2:r==Role::Middle?3:0;}
public:
    void restorePointer(std::optional<SDL_FPoint> position){pendingPointer=position;}
    void forgetPointer(){pendingPointer.reset();}
    template<class Send> void cancel(Send sendRaw,bool wait=true){
        auto send=[&](uu::Json data){data["source"]="phone_mouse";sendRaw(data);};
        for(unsigned b=1;b<=3;++b)if(held&(1u<<b))
            send(uu::Json{{"action","mouse_release"},{"button",b}});
        contacts.clear();held=0;wheelRemainder=0;blocked=wait;
    }
#ifdef __SWITCH__
    template<class Send> void update(const HidTouchScreenState &touch,Uint32 now,bool captured,
        float speed,const SDL_Rect &view,float &cursorX,float &cursorY,Send sendRaw){
        auto send=[&](uu::Json data){data["source"]="phone_mouse";sendRaw(data);};
        if(captured || view.w<=0 || view.h<=0){cancel(sendRaw);return;}
        if(blocked){if(!touch.count)blocked=false;return;}
        // Restore mouse input only after menu contacts have been released.
        // Use an actual touch position, never a default or stale mouse position.
        if(pendingPointer){
            cursorX=pendingPointer->x;cursorY=pendingPointer->y;
            send(uu::Json{{"action","mouse_move_absolute"},{"abs_x",cursorX},{"abs_y",cursorY}});
            pendingPointer.reset();
            uu::diagnostic("phone-mouse-pointer-restore-submitted");
        }
        const int count=std::clamp(int(touch.count),0,int(std::size(touch.touches)));
        const auto end=std::begin(touch.touches)+count;
        const bool existingPointer=std::any_of(contacts.begin(),contacts.end(),[&](const Contact &c){
            return c.role==Role::Move && std::any_of(std::begin(touch.touches),end,[&](const auto &p){return p.finger_id==c.id;});
        });
        std::vector<Contact> next;next.reserve(10);
        bool controlContact=false;
        for(int i=0;i<count && next.size()<10;++i){
            const auto &p=touch.touches[i];const int x=int(p.x),y=int(p.y);
            auto old=std::find_if(contacts.begin(),contacts.end(),[&](const Contact &c){return c.id==p.finger_id;});
            Contact c=old==contacts.end()?Contact{p.finger_id,Role::Move,x,y,x,y,now}:*old;
            if(old==contacts.end()){
                if(inside(x,y,leftBox()))c.role=Role::Left;
                else if(inside(x,y,rightBox()))c.role=Role::Right;
                else if(inside(x,y,middleBox()))c.role=Role::Middle;
                else if(inside(x,y,wheelBox()))c.role=Role::Wheel;
                else if(!inside(x,y,view))c.role=Role::Ignore;
                if(c.role==Role::Move && (existingPointer || std::any_of(next.begin(),next.end(),
                    [](const Contact &n){return n.role==Role::Move;})))c.role=Role::Ignore;
            }
            controlContact|=button(c.role)!=0 || c.role==Role::Wheel;
            next.push_back(c);
        }
        unsigned nextHeld=0;
        for(const auto &c:next)if(auto b=button(c.role))nextHeld|=1u<<b;
        for(unsigned b=1;b<=3;++b)if((held&(1u<<b))!=(nextHeld&(1u<<b)))
            send(uu::Json{{"action",nextHeld&(1u<<b)?"mouse_press":"mouse_release"},{"button",b}});
        bool movedPointer=false;
        for(auto &c:next){
            const auto p=std::find_if(std::begin(touch.touches),end,
                [&](const auto &p){return p.finger_id==c.id;});
            const int x=int(p->x),y=int(p->y),dx=x-c.x,dy=y-c.y;
            if(std::max(std::abs(x-c.startX),std::abs(y-c.startY))>10)c.moved=true;
            if(c.role==Role::Move){
                c.moved|=controlContact || held!=0;
                if(!movedPointer && (dx||dy)){
                    cursorX=std::clamp(cursorX+dx*speed/float(view.w),0.f,1.f);
                    cursorY=std::clamp(cursorY+dy*speed/float(view.h),0.f,1.f);
                    send(uu::Json{{"action","mouse_move_absolute"},{"abs_x",cursorX},{"abs_y",cursorY}});
                    movedPointer=true;
                }
            }else if(c.role==Role::Wheel){
                wheelRemainder-=dy*6.f;
                if(Uint32(now-wheelAt)>=16 && std::abs(wheelRemainder)>=1){
                    const int delta=int(wheelRemainder);
                    send(uu::Json{{"action","mouse_scroll"},{"delta_x",0},{"delta_y",delta}});
                    wheelRemainder-=delta;wheelAt=now;
                }
            }
            c.x=x;c.y=y;
        }
        // A short pointer tap clicks once. Button-assisted drag never adds a tap.
        for(const auto &c:contacts)if(c.role==Role::Move && !c.moved && !held && !nextHeld &&
            !controlContact && Uint32(now-c.began)<=300 &&
            std::none_of(next.begin(),next.end(),[&](const Contact &n){return n.id==c.id;})){
            send(uu::Json{{"action","mouse_press"},{"button",1}});
            send(uu::Json{{"action","mouse_release"},{"button",1}});
        }
        if(std::none_of(next.begin(),next.end(),[](const Contact &c){return c.role==Role::Wheel;}))wheelRemainder=0;
        held=nextHeld;contacts=std::move(next);
    }
#endif
    void draw(SDL_Renderer *r,Fonts &f)const {
        const SDL_Color base{24,29,37,175},line{209,218,230,225},active{58,123,252,230};
        auto mouse=[&](SDL_Rect box,unsigned b,const char *label){
            tvui::rounded(r,box,held&(1u<<b)?active:base,14);
            const int x=box.x+box.w/2-17,y=box.y+10;
            tvui::rounded(r,{x,y,34,40},line,15);
            tvui::rounded(r,{x+2,y+2,30,36},base,13);
            if(b<=2)tvui::rounded(r,{x+(b==1?4:19),y+5,11,14},line,4);
            else tvui::rounded(r,{x+14,y+5,6,13},line,3);
            tvui::aligned(r,f.hint,label,{box.x,box.y+52,box.w,22},line,true);
        };
        mouse(leftBox(),1,"左键");mouse(rightBox(),2,"右键");mouse(middleBox(),3,"中键");
        const auto box=wheelBox();tvui::rounded(r,box,base,20);
        tvui::aligned(r,f.hint,"滚轮",{box.x,box.y+8,box.w,22},line,true);
        rect(r,box.x+box.w/2-1,box.y+42,2,box.h-64,{160,173,190,155});
        int y=box.y+box.h/2;
        for(const auto &c:contacts)if(c.role==Role::Wheel)y=std::clamp(c.y,box.y+48,box.y+box.h-24);
        tvui::rounded(r,{box.x+12,y-13,box.w-24,26},line,12);
    }
};
