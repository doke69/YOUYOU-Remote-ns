#include "turn_relay_allocation.hpp"
#include <cassert>
#include <vector>
using Bytes=std::vector<uint8_t>;
Bytes response(const uu::TurnRelayAllocation &a) {
    const size_t n=a.family==1?4:16;
    Bytes b(28+n);b[0]=1;b[1]=3;b[3]=uint8_t(b.size()-20);
    b[4]=0x21;b[5]=0x12;b[6]=0xa4;b[7]=0x42;
    for(int i=8;i<20;++i)b[i]=uint8_t(i*7);
    b[21]=0x16;b[23]=uint8_t(n+4);b[25]=a.family;
    const auto p=a.port^0x2112;b[26]=uint8_t(p>>8);b[27]=uint8_t(p);
    for(size_t i=0;i<n;++i)b[28+i]=a.address[i]^b[4+i];
    return b;
}
int main() {
    uu::TurnRelayAllocation v4;v4.family=1;v4.port=50000;v4.address[0]=192;v4.address[1]=0;v4.address[2]=2;v4.address[3]=10;
    uu::TurnRelayAllocation v6;v6.family=2;v6.port=49152;
    for(size_t i=0;i<16;++i)v6.address[i]=uint8_t(i*11);
    for(const auto &a:{v4,v6}) {
        const auto good=response(a);
        assert(uu::readTurnRelayAllocation(good.data(),good.size())==std::optional<uu::TurnRelayAllocation>(a));
        for(size_t size=0;size<good.size();++size)assert(!uu::readTurnRelayAllocation(good.data(),size));
        for(const auto index:{0,1,4,5,6,7,23,24,25}) {
            auto bad=good;bad[index]=0xff;assert(!uu::readTurnRelayAllocation(bad.data(),bad.size()));
        }
        auto duplicate=good;duplicate.insert(duplicate.end(),good.begin()+20,good.end());duplicate[3]=uint8_t(duplicate.size()-20);
        assert(!uu::readTurnRelayAllocation(duplicate.data(),duplicate.size()));
        auto tail=good;tail.insert(tail.end(),{0,1,0,8});tail[3]=uint8_t(tail.size()-20);
        assert(!uu::readTurnRelayAllocation(tail.data(),tail.size()));
        auto unknown=good;unknown.insert(unknown.begin()+20,{0x80,0x22,0,1,0x41,0,0,0});unknown[3]=uint8_t(unknown.size()-20);
        assert(uu::readTurnRelayAllocation(unknown.data(),unknown.size())==std::optional<uu::TurnRelayAllocation>(a));
    }
    auto zero=v4;zero.port=0;auto b=response(zero);assert(!uu::readTurnRelayAllocation(b.data(),b.size()));
    b=response(v4);b[0]=0x01;b[1]=0x13;assert(!uu::readTurnRelayAllocation(b.data(),b.size())); // error response
    b=response(v4);b[1]=1;assert(!uu::readTurnRelayAllocation(b.data(),b.size())); // Binding response
    b.assign(24,0);b[0]=0x40;assert(!uu::readTurnRelayAllocation(b.data(),b.size())); // ChannelData
    auto other=v4;other.port++;assert(!(other==v4));other=v4;other.address[3]++;assert(!(other==v4));
}
