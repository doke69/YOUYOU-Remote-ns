#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uu {
struct TurnRelayAllocation {
    uint8_t family=0; // STUN address family: 1=IPv4, 2=IPv6.
    uint16_t port=0;
    std::array<uint8_t,16> address{};
    bool operator==(const TurnRelayAllocation &other) const {
        return family==other.family && port==other.port && address==other.address;
    }
};

// A complete, framed Allocate success response from this bridge's server.
// This is an observation, not authentication; libjuice remains authoritative.
inline std::optional<TurnRelayAllocation> readTurnRelayAllocation(const uint8_t *data,size_t size) {
    if(size<20 || data[0]!=0x01 || data[1]!=0x03 ||
       data[4]!=0x21 || data[5]!=0x12 || data[6]!=0xa4 || data[7]!=0x42)
        return {};
    const size_t body=(size_t(data[2])<<8)|data[3];
    if((body&3) || body!=size-20)return {};
    std::optional<TurnRelayAllocation> result;
    for(size_t offset=20;offset<size;) {
        if(size-offset<4)return {};
        const unsigned type=(unsigned(data[offset])<<8)|data[offset+1];
        const size_t length=(size_t(data[offset+2])<<8)|data[offset+3];
        offset+=4;
        const size_t padded=(length+3)&~size_t(3);
        if(padded>size-offset)return {};
        if(type==0x0016) { // XOR-RELAYED-ADDRESS
            if(result || length<4 || data[offset]!=0)return {};
            TurnRelayAllocation value;value.family=data[offset+1];
            const size_t bytes=value.family==1?4:value.family==2?16:0;
            if(!bytes || length!=bytes+4)return {};
            value.port=uint16_t(((unsigned(data[offset+2])<<8)|data[offset+3])^0x2112u);
            if(!value.port)return {};
            for(size_t i=0;i<bytes;++i)value.address[i]=data[offset+4+i]^data[4+i];
            result=value;
        }
        offset+=padded;
    }
    return result;
}
} // namespace uu
