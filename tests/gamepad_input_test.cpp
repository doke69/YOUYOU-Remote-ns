#include "gamepad_input.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

int main(){
    using namespace uu;
    const PadSample full{0xffff,120,-230,340,-450};
    const auto unchanged=normalizePad(full,PadLayout::Full);
    assert(unchanged.held==full.held && unchanged.rx==340 && unchanged.ly==-230);
    // Horizontal halves must both expose a left stick; no right-stick leakage.
    const auto left=normalizePad({(1ULL<<13)|(1ULL<<24)|(1ULL<<11),123,456,0,0},PadLayout::LeftJoy);
    assert(left.lx==-456 && left.ly==123 && left.rx==0 && left.ry==0);
    assert(left.held==((1ULL<<3)|(1ULL<<6)|(1ULL<<11)));
    const auto right=normalizePad({(1ULL<<0)|(1ULL<<27)|(1ULL<<10),0,0,123,456},PadLayout::RightJoy);
    assert(right.lx==456 && right.ly==-123 && right.rx==0 && right.ry==0);
    assert(right.held==((1ULL<<1)|(1ULL<<7)|(1ULL<<10)));
    for(auto layout:{PadLayout::LeftJoy,PadLayout::RightJoy}){
        const auto neutral=normalizePad({},layout);assert(neutral.held==0 && neutral.lx==0);
    }
    // Both full and half layouts use the same final remote preset.
    assert(xboxButtons(1,true).buttons==4096);
    assert(xboxButtons(1,false).buttons==8192);
    const auto leftFace=normalizePad({1ULL<<15},PadLayout::LeftJoy);
    assert(xboxButtons(leftFace.held,true).buttons==4096);
    assert(xboxButtons(leftFace.held,false).buttons==8192);
    assert(xboxButtons(0,true).buttons==0);
    assert(xboxButtons(1ULL<<10,true).buttons==16);
    assert(xboxButtons(1ULL<<15,true).buttons==2);
    // Minus is not folded into Plus: local menu detection must never see it.
    const auto minusLeft=normalizePad({1ULL<<11},PadLayout::LeftJoy);
    assert(minusLeft.held==(1ULL<<11));
    for(int heldFrame=0;heldFrame<300;++heldFrame){
        assert(xboxButtons(1ULL<<11,false).buttons==32); // full pad Back continuously
        assert(xboxButtons(minusLeft.held,false,PadLayout::LeftJoy).buttons==16); // half pad Start continuously
    }
    const auto leftShoulders=normalizePad({(1ULL<<6)|(1ULL<<8)|(1ULL<<24)|(1ULL<<25)},PadLayout::LeftJoy);
    const auto shoulderOutput=xboxButtons(leftShoulders.held,false,PadLayout::LeftJoy);
    assert(shoulderOutput.buttons==(256|512));
    assert(shoulderOutput.leftTrigger==255 && shoulderOutput.rightTrigger==255);
    PadNavigation navigation;
    assert(navigation.update({0,32767,0},true)==(1ULL<<14));
    assert(navigation.update({0,32767,0},true)==0);
    assert(navigation.update({},true)==0);
    assert(navigation.update({0,0,32767},true)==(1ULL<<13));
    assert(navigation.update({0,32767,0},false)==0); // no navigation in gameplay
    constexpr uint64_t l=1ULL<<6, plus=1ULL<<10, chord=l|plus;
    PadMenuChord player1,player2;
    assert(player1.update(l,l,true)==0);
    assert(player2.update(plus,plus,true)==0); // different players cannot form a chord
    assert(player1.update(chord,plus,true)==2);
    for(int i=0;i<1000;++i)assert(player1.update(chord,0,true)==0);
    assert(player1.update(plus,0,true)==0);
    assert(player1.update(0,0,true)==0); // chord release never sends remote Start
    assert(player2.update(0,0,true)==1); // ordinary Plus still sends Start on release
    assert(player1.update(plus,plus,true)==0);
    for(int i=0;i<1000;++i)assert(player1.update(plus,0,true)==0); // no long-Plus menu
    assert(player1.update(0,0,true)==1);
    assert(player1.update(plus,plus,true)==0);
    assert(player1.update(chord,l,true)==2); // Plus-first works without remote Start leakage
    assert(player1.update(l,0,true)==0);
    assert(player1.update(chord,plus,true)==0); // both buttons must be released before rearming
    assert(player1.update(0,0,true)==0);
    assert(player1.update(chord,chord,true)==2); // same-frame press, also closes active menu
    assert(player1.update(chord,0,false)==0); // menu close drains held input
    assert(player1.update(chord,0,true)==0);
    assert(player1.update(0,0,true)==0);
    assert(player1.update(plus,plus,false)==0); // a press outside streaming cannot leak in
    assert(player1.update(plus,0,true)==0);
    assert(player1.update(0,0,true)==0);
    const auto rightMenu=normalizePad({(1ULL<<26)|plus},PadLayout::RightJoy);
    assert(player1.update(rightMenu.held,rightMenu.held,true)==2); // horizontal SL + Plus
    assert(player2.update((1ULL<<11)|l,(1ULL<<11)|l,true)==0); // Minus remains remote-only
    std::cout<<"PASS: Joy-Con geometry, presets, Minus, triggers, navigation, two-player L+Plus menu lifecycle\n";
}
