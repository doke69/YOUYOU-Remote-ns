#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum uu_packet_stage { UU_PACKET_UDP, UU_PACKET_SRTP, UU_PACKET_AUDIO, UU_PACKET_VIDEO, UU_PACKET_AU };
enum uu_packet_kind { UU_PACKET_OTHER, UU_PACKET_RTP, UU_PACKET_RTCP, UU_PACKET_STUN, UU_PACKET_DTLS, UU_PACKET_TURN };

typedef struct {
    uint32_t ssrc, timestamp;
    uint16_t sequence, bytes;
    uint8_t kind, type;
} uu_packet_header;

// Read only the public transport headers. No address or payload enters the trace.
static inline uu_packet_header uu_packet_read(const void *buffer, size_t size) {
    const uint8_t *data = (const uint8_t *)buffer;
    uu_packet_header header = {0, 0, 0, (uint16_t)(size > UINT16_MAX ? UINT16_MAX : size), UU_PACKET_OTHER, 0};
    if (size < 2) return header;
    if ((data[0] & 0xc0) == 0x80) {
        header.type = data[1];
        if (data[1] >= 192 && data[1] <= 223) {
            if (size < 8) return header;
            header.kind = UU_PACKET_RTCP;
            header.ssrc = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
        } else if (size >= 12) {
            header.kind = UU_PACKET_RTP;
            header.type = data[1] & 0x7f;
            header.sequence = (uint16_t)((uint16_t)data[2] << 8 | data[3]);
            header.timestamp = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
            header.ssrc = (uint32_t)data[8] << 24 | (uint32_t)data[9] << 16 | (uint32_t)data[10] << 8 | data[11];
        }
    } else if (size >= 20 && data[0] < 4 &&
               data[4] == 0x21 && data[5] == 0x12 && data[6] == 0xa4 && data[7] == 0x42) {
        header.kind = UU_PACKET_STUN;
    } else if (size >= 13 && data[0] >= 20 && data[0] <= 63) {
        header.kind = UU_PACKET_DTLS;
    } else if (size >= 4 && (data[0] & 0xc0) == 0x40) {
        header.kind = UU_PACKET_TURN;
    }
    return header;
}

void uu_packet_stage_record(enum uu_packet_stage stage, uu_packet_header header, int result);

#ifdef __cplusplus
}
namespace uu {
void notePacketStageGap(uint64_t generation, uint64_t fromUs, uint64_t toUs);
bool flushPacketStages(const char *path);
}
#endif
