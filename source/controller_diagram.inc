// Shared by the native UI and its offline renderer; no image assets or GPU changes.
namespace tvui {
void controllerDiagram(SDL_Renderer *r,Fonts &f,uu::PadLayout layout,uu::PadLegend legend){
    const SDL_Color control{39,46,57,255},ring{146,157,173,255};
    const bool half=layout!=uu::PadLayout::Full;
    const bool ps=!half && legend==uu::PadLegend::PlayStation;
    const bool xbox=!half && legend==uu::PadLegend::Xbox;
    // The approved 280px prototype at 1.5x: retain its proportions and spacing.
    const SDL_Rect body=half?SDL_Rect{125,340,420,162}:SDL_Rect{125,280,420,276};
    if(half){
        rounded(r,body,{50,141,163,255},53);
    }else if(legend==uu::PadLegend::Switch){
        rounded(r,{129,286,420,276},{0,0,0,45},53);
        rounded(r,body,{105,119,132,255},53);
        rounded(r,{128,283,202,270},{50,141,163,255},50);
        rounded(r,{340,283,202,270},{190,88,95,255},50);
        rect(r,229,283,212,270,{48,54,64,255});
    }else{
        const SDL_Color shell=ps?SDL_Color{108,119,136,255}:SDL_Color{97,109,125,255};
        rounded(r,body,{Uint8(shell.r+8),Uint8(shell.g+8),Uint8(shell.b+8),255},72);
        rounded(r,{125,282,420,274},shell,72);
    }
    auto stick=[&](int x,int y){rounded(r,{x-24,y-24,48,48},ring,24);rounded(r,{x-18,y-18,36,36},control,18);};
    auto dpad=[&](int x,int y){rounded(r,{x-24,y-24,48,48},control,12);rect(r,x-2,y-14,4,28,ink);rect(r,x-14,y-2,28,4,ink);};
    auto line=[&](int x1,int y1,int x2,int y2){
        const int count=std::max(std::abs(x2-x1),std::abs(y2-y1));
        for(int n=0;n<=count;++n)rect(r,x1+(x2-x1)*n/count-1,y1+(y2-y1)*n/count-1,3,3,ink);
    };
    if(half){
        stick(215,421);
        rounded(r,{316,365,38,28},control,8);
        aligned(r,f.small,layout==uu::PadLayout::LeftJoy?"−":"＋",{316,365,38,28},ink,true);
        rounded(r,{148,320,93,24},control,8);aligned(r,f.hint,"SL",{148,320,93,24},ink,true);
        rounded(r,{430,320,93,24},control,8);aligned(r,f.hint,"SR",{430,320,93,24},ink,true);
        aligned(r,f.small,layout==uu::PadLayout::LeftJoy?"左端 / 背面：L / ZL":"右端 / 背面：R / ZR",{125,530,420,36},label,true);
    }else{
        stick(ps?260:208,ps?460:367);stick(ps?410:xbox?386:374,ps?460:459);
        dpad(ps?205:287,ps?372:448);
        const char *left=ps?"L1 / L2":xbox?"LB / LT":"L / ZL";
        const char *right=ps?"R1 / R2":xbox?"RB / RT":"R / ZR";
        aligned(r,f.small,left,{125,232,180,34},label,true);aligned(r,f.small,right,{365,232,180,34},label,true);
        if(ps){rounded(r,{275,318,120,63},control,14);aligned(r,f.small,"触摸板",{275,318,120,63},ink,true);}
        else if(xbox){rounded(r,{310,344,50,50},control,25);rounded(r,{323,357,24,24},ring,12);rounded(r,{329,363,12,12},control,6);}
        else aligned(r,f.small,"−   ＋",{290,335,90,34},label,true);
    }
    const SDL_Point fullPoints[]={{494,372},{457,409},{457,334},{419,372}};
    const SDL_Point halfPoints[]={{484,418},{445,457},{445,379},{406,418}};
    const char *switchKeys[]={"A","B","X","Y"},*xboxKeys[]={"B","A","Y","X"};
    const char *leftKeys[]={"→","↓","↑","←"},*rightKeys[]={"X","A","Y","B"};
    for(int i=0;i<4;++i){
        const auto p=half?halfPoints[i]:fullPoints[i];rounded(r,{p.x-18,p.y-18,36,36},control,18);
        if(ps){
            if(i==0){rounded(r,{p.x-10,p.y-10,20,20},ink,10);rounded(r,{p.x-7,p.y-7,14,14},control,7);}
            else if(i==1){line(p.x-7,p.y-7,p.x+7,p.y+7);line(p.x+7,p.y-7,p.x-7,p.y+7);}
            else if(i==2){line(p.x,p.y-10,p.x-10,p.y+8);line(p.x-10,p.y+8,p.x+10,p.y+8);line(p.x+10,p.y+8,p.x,p.y-10);}
            else {rect(r,p.x-9,p.y-9,18,3,ink);rect(r,p.x-9,p.y+6,18,3,ink);rect(r,p.x-9,p.y-9,3,18,ink);rect(r,p.x+6,p.y-9,3,18,ink);}
        }else{
            const char *key=half?(layout==uu::PadLayout::LeftJoy?leftKeys[i]:rightKeys[i]):xbox?xboxKeys[i]:switchKeys[i];
            aligned(r,f.small,key,{p.x-18,p.y-18,36,36},ink,true);
        }
    }
}
} // namespace tvui
