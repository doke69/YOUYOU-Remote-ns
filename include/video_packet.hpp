#pragma once
extern "C" {
#include <libavcodec/defs.h>
#include <libavcodec/packet.h>
#include <libavutil/buffer.h>
}
#include <algorithm>
#include <memory>
#include <vector>

namespace uu {
struct VideoPacketFree {
    void operator()(AVPacket *packet) const noexcept { av_packet_free(&packet); }
};
using VideoPacket = std::unique_ptr<AVPacket,VideoPacketFree>;

// Own the completed compressed AU across asynchronous decode. The receiver
// reserves 64 extra bytes; keep that contract checked against the actual SDK.
inline VideoPacket videoPacket(std::vector<uint8_t> data) {
    static_assert(AV_INPUT_BUFFER_PADDING_SIZE==64);
    const auto size=data.size();
    if(!size || size>4u*1024u*1024u) return {};
    VideoPacket packet(av_packet_alloc());
    if(!packet) return {};
    const size_t padded=size+AV_INPUT_BUFFER_PADDING_SIZE;
    // Do not grow/copy the AU just to append padding, or retain a vector whose
    // geometric growth wastes substantial queue memory. At the unchanged
    // 8 MiB / 32 AU queue limit, extra capacity is at most 1 MiB + padding.
    if(data.capacity()>=padded && data.capacity()-padded<=size/8) {
        data.resize(padded,0);
        auto owner=std::make_unique<std::vector<uint8_t>>(std::move(data));
        packet->buf=av_buffer_create(owner->data(),owner->size(),
            [](void *opaque,uint8_t *) { delete static_cast<std::vector<uint8_t> *>(opaque); },owner.get(),0);
        if(!packet->buf) return {};
        packet->data=owner->data();packet->size=int(size);
        owner.release();
    } else {
        if(av_new_packet(packet.get(),int(size))<0) return {};
        std::copy(data.begin(),data.end(),packet->data);
    }
    return packet;
}
} // namespace uu
