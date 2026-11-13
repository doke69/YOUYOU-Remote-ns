#include "receive_bandwidth_observer.hpp"
#include "system_wrappers/include/clock.h"
#include <iostream>

int main() {
    uu::rbe::InputConfig config;
    config.videoPayloads[96] = true;
    config.absSendTimeId = 3;
    config.transportSequenceIds[0] = 5;
    webrtc::SimulatedClock clock(0);
    auto observer = std::make_shared<uu::rbe::ReceiveBandwidthObserver>(config, &clock);
    auto message = rtc::make_message(1212);
    auto *bytes = reinterpret_cast<uint8_t *>(message->data());
    bytes[0] = 0x80;
    bytes[1] = 96;
    bytes[8] = 0x12; bytes[9] = 0x34; bytes[10] = 0x56; bytes[11] = 0x78;
    rtc::message_vector messages{message};
    size_t sends = 0;
    const rtc::message_callback send = [&](rtc::message_ptr) { ++sends; };
    for (int i = 0; i < 12000; ++i) {
        clock.AdvanceTimeMicroseconds(1200);
        message->receivedAtUs = clock.TimeInMicroseconds();
        const uint32_t stamp = uint32_t(0xffff0000u + uint64_t(i) * 108);
        for (int k = 0; k < 4; ++k) bytes[4 + k] = uint8_t(stamp >> (24 - 8 * k));
        observer->incoming(messages, send);
        observer->poll();
    }
    const auto active = observer->snapshot();
    if (active.input.packets != 12000 || active.estimateBps <= 0 || !active.estimates || sends ||
        messages.size() != 1 || messages.front() != message || message.use_count() != 2) return 1;
    message->receivedAtUs = -1;
    observer->incoming(messages, send);
    message->type = rtc::Message::Control;
    observer->incoming(messages, send);
    message->type = rtc::Message::Binary;
    if (observer->snapshot().input.missingArrival != 1) return 2;
    const auto before = *message;
    observer->close();
    observer->incoming(messages, send);
    observer->poll();
    const auto closed = observer->snapshot();
    if (!closed.input.closed || closed.estimateBps || closed.estimates != active.estimates ||
        closed.input.afterClose != 1 || sends || *message != before) return 3;
    // A replacement cannot inherit old output even when the old handler remains referenced.
    auto fresh = std::make_shared<uu::rbe::ReceiveBandwidthObserver>(config, &clock);
    if (fresh->snapshot().estimates || fresh->snapshot().estimateBps || fresh->snapshot().input.packets) return 4;
    std::cout << "PASS: authenticated handler preserves message ownership/content, observes only valid arrival, never sends, closes and resets\n";
}
