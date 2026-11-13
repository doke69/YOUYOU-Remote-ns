#include "transport_feedback_attachment.hpp"
#include <rtc/rtcpreceivingsession.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed line " << __LINE__ << ": " #x "\n"; return 1; } } while (0)

int main() {
    using namespace uu::twcc;
    // TV 0x6f7a58 wire contract: 2 or 4 extension bytes; the high bit
    // requests timestamps and the remaining 15 bits count sequences.
    uint8_t v2[24] = {0x90,96,0,1,0,0,0,0,0,0,0,96,0xbe,0xde,0,2,
                      0x53,0xff,0xff,0x80,3,0,0,0};
    auto parsed = readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2);
    CHECK(parsed.status == ExtensionStatus::Found && parsed.sequence == 65535);
    CHECK(parsed.request && parsed.request->includeTimestamps && parsed.request->sequenceCount == 3);
    CHECK(readTransportSequence(v2, sizeof(v2), 5).status == ExtensionStatus::Malformed);
    v2[19] = 0x7f; v2[20] = 0xff;
    parsed = readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2);
    CHECK(parsed.request && !parsed.request->includeTimestamps && parsed.request->sequenceCount == 32767);
    v2[19] = 0x80; v2[20] = 0;
    parsed = readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2);
    CHECK(parsed.status == ExtensionStatus::Found && !parsed.request);
    v2[19] = 0; v2[20] = 0;
    CHECK(!readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2).request);
    v2[16] = 0x51; // No request bytes: valid in either negotiated version.
    CHECK(readTransportSequence(v2, sizeof(v2), 5).status == ExtensionStatus::Found);
    CHECK(!readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2).request);
    v2[16] = 0x52; // Three bytes is invalid, not a truncated request.
    CHECK(readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2).status == ExtensionStatus::Malformed);
    v2[16] = 0x53; v2[21] = 0x51; v2[22] = 0; v2[23] = 1;
    CHECK(readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2).status == ExtensionStatus::Malformed);
    v2[12] = 0x10; v2[13] = 0; v2[16] = 5; v2[17] = 4;
    v2[18] = 0x12; v2[19] = 0x34; v2[20] = 0x80; v2[21] = 2; v2[22] = v2[23] = 0;
    parsed = readTransportSequence(v2, sizeof(v2), 5, TransportSequenceVersion::V2);
    CHECK(parsed.sequence == 0x1234 && parsed.request && parsed.request->sequenceCount == 2);
    // TV T=0 uses small-received for every received position, preserves the
    // reference, and omits delta bytes. Parse independently of the encoder.
    const std::vector<std::optional<int64_t>> times{1000000, std::nullopt, 1500000};
    const auto timed = encodeFeedback(123, 96, 65535, 7, times);
    const auto untimed = encodeFeedback(123, 96, 65535, 7, times, 1200, std::nullopt, false);
    CHECK(timed.status == EncodeStatus::Ok && untimed.status == EncodeStatus::Ok);
    CHECK(timed.packet.size() == 28 && untimed.packet.size() == 24);
    CHECK((untimed.packet[0] & 0x20) && untimed.packet.back() == 2);
    CHECK((size_t(untimed.packet[2]) * 256 + untimed.packet[3] + 1) * 4 == untimed.packet.size());
    CHECK(untimed.packet[14] == 0 && untimed.packet[15] == 3 && untimed.packet[19] == 7);
    CHECK(untimed.packet[20] == 0xd1 && untimed.packet[21] == 0); // small, missing, small
    CHECK(std::equal(timed.packet.begin() + 4, timed.packet.begin() + 20, untimed.packet.begin() + 4));
    const std::vector<std::optional<int64_t>> missing(7);
    const auto unknown = encodeFeedback(123, 96, 1, 0, missing, 1200, 1000000, false);
    CHECK(unknown.status == EncodeStatus::Ok && unknown.packet.size() == 24);
    CHECK(unknown.packet[20] == 0xc0 && unknown.packet[21] == 0);
    for (size_t count : {size_t(1), size_t(7), size_t(8), size_t(511), size_t(512)}) {
        std::vector<std::optional<int64_t>> arrivals(count);
        for (size_t i = 0; i < count; ++i)
            if (i % 3 != 1) arrivals[i] = 1000000 + int64_t(i) * 250;
        const auto encoded = encodeFeedback(123, 96, 65530, 255, arrivals, 1200, 1000000, false);
        CHECK(encoded.status == EncodeStatus::Ok);
        const auto &bytes = encoded.packet;
        const size_t padding = (bytes[0] & 0x20) ? bytes.back() : 0;
        CHECK(padding <= 3 && bytes.size() >= 20 + padding);
        const size_t end = bytes.size() - padding;
        CHECK(end == 20 + 2 * ((count + 6) / 7)); // No room for hidden receive deltas.
        CHECK((size_t(bytes[14]) << 8 | bytes[15]) == count);
        CHECK(bytes[12] == 0xff && bytes[13] == 0xfa && bytes[19] == 255);
        size_t index = 0;
        for (size_t offset = 20; offset < end; offset += 2) {
            const uint16_t chunk = uint16_t(uint16_t(bytes[offset]) << 8 | bytes[offset + 1]);
            CHECK((chunk >> 14) == 3);
            for (size_t j = 0; j < 7; ++j, ++index) {
                const auto symbol = (chunk >> (12 - 2 * j)) & 3;
                CHECK(symbol == (index < count && arrivals[index] ? 1 : 0));
            }
        }
    }
    CHECK(encodeFeedback(123, 96, 0, 0, {}, 1200, 1000000, false).status == EncodeStatus::InvalidInput);
    CHECK(encodeFeedback(123, 96, 0, 0, {1000000, 20000000}, 1200, std::nullopt, false).status == EncodeStatus::Ok);
    CHECK(encodeFeedback(123, 96, 0, 0, {1000000, 20000000}, 1200, std::nullopt, true).status == EncodeStatus::SplitRequired);
    const NegotiationResult ready{NegotiationStatus::Ready, 5, 1};
    const FeedbackTiming timing{100000, 0, 2048};
    auto requested = std::make_unique<FeedbackCoordinator>(timing, FeedbackMode::Requested);
    RecordedArrival last;
    for (int i = 0; i < 600; ++i) {
        last = requested->record(uint16_t(65530 + i), 1000000 + i * 250);
        CHECK(last.status == ArrivalStatus::Accepted);
    }
    CHECK(last.extendedSequence == 66129);
    CHECK(!requested->prepare(1200000, 123, 96)); // No unsolicited periodic report.
    CHECK(requested->request(last.extendedSequence, {true, 0}) == RequestStatus::NoRequest);
    CHECK(requested->request(last.extendedSequence + 1, {true, 1}) == RequestStatus::HistoryUnavailable);
    CHECK(requested->request(last.extendedSequence, {true, 32768}) == RequestStatus::InvalidRange);
    CHECK(requested->request(last.extendedSequence, {true, 600}) == RequestStatus::Queued);
    auto firstPart = *requested->prepare(1200000, 123, 96);
    CHECK(firstPart.firstSequence == 65530 && firstPart.lastSequence == 66041);
    CHECK(firstPart.packet[12] == 0xff && firstPart.packet[13] == 0xfa);
    CHECK(!requested->commit(firstPart.ticket + 1, 1200000));
    CHECK(requested->prepare(1200001, 123, 96)->packet == firstPart.packet);
    CHECK(requested->commit(firstPart.ticket, 1200001));
    auto secondPart = *requested->prepare(1200001, 123, 96);
    CHECK(secondPart.firstSequence == 66042 && secondPart.lastSequence == 66129);
    CHECK(secondPart.feedbackCount == 1 && secondPart.ticket != firstPart.ticket);
    CHECK(requested->commit(secondPart.ticket, 1200001));
    CHECK(!requested->prepare(1400000, 123, 96));
    CHECK(requested->request(last.extendedSequence, {false, 5}) == RequestStatus::Queued);
    auto repeat = *requested->prepare(1400000, 123, 96);
    CHECK(repeat.firstSequence == 66125 && repeat.lastSequence == 66129 && repeat.packet.size() == 24);
    CHECK((repeat.packet[20] & 0x30) == 0x10); // Previously reported arrivals still present.
    requested->reset();
    CHECK(!requested->commit(repeat.ticket, 1400000));
    CHECK(requested->record(1, 2000000).status == ArrivalStatus::Accepted);
    CHECK(requested->request(1, {true, 1}) == RequestStatus::Queued);
    CHECK(requested->prepare(2000000, 123, 96)->ticket != repeat.ticket);
    requested.reset();

    auto pinned = std::make_unique<FeedbackCoordinator>(timing, FeedbackMode::Requested);
    for (unsigned i = 0; i < ArrivalWindow::Capacity; ++i)
        CHECK(pinned->record(uint16_t(i), 1000000 + i).status == ArrivalStatus::Accepted);
    CHECK(pinned->request(7, {false, 8}) == RequestStatus::Queued);
    CHECK(pinned->record(4096, 1100000).status == ArrivalStatus::WindowFull);
    auto protectedReport = *pinned->prepare(1200000, 123, 96);
    CHECK(protectedReport.firstSequence == 0 && protectedReport.lastSequence == 7);
    CHECK(pinned->commit(protectedReport.ticket, 1200000));
    for (unsigned i = 4096; i <= 12000; ++i)
        CHECK(pinned->record(uint16_t(i), 1200000 + i).status == ArrivalStatus::Accepted);
    CHECK(pinned->request(0, {true, 1}) == RequestStatus::HistoryUnavailable);
    CHECK(pinned->request(12000, {false, 32767}) == RequestStatus::QueuedWithUnretainedPrefix);
    int64_t prefixNext = 12000 - 32767 + 1;
    unsigned prefixParts = 0, receivedPositions = 0;
    while (const auto *report = pinned->prepare(1300000, 123, 96)) {
        CHECK(report->firstSequence == prefixNext);
        const auto &wire = report->packet;
        const size_t count = size_t(wire[14]) * 256 + wire[15];
        CHECK(count <= 512 && count == size_t(report->lastSequence - report->firstSequence + 1));
        for (size_t i = 0; i < count; ++i) {
            const size_t offset = 20 + 2 * (i / 7);
            const uint16_t chunk = uint16_t(uint16_t(wire[offset]) << 8 | wire[offset + 1]);
            const unsigned symbol = (chunk >> (12 - 2 * (i % 7))) & 3;
            CHECK(symbol == unsigned(report->firstSequence + int64_t(i) >= 7905));
            receivedPositions += symbol != 0;
        }
        CHECK(++prefixParts <= 64);
        prefixNext = report->lastSequence + 1;
        CHECK(pinned->commit(report->ticket, 1300000));
    }
    CHECK(prefixNext == 12001 && prefixParts == 64 && receivedPositions == ArrivalWindow::Capacity);
    CHECK(!pinned->prepare(1300000, 123, 96));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(pinned->request(12000, {false, 1}) == RequestStatus::Queued);
    CHECK(pinned->request(12000, {false, 1}) == RequestStatus::QueueFull);
    for (unsigned i = 0; i < 8; ++i) {
        const auto report = *pinned->prepare(1300000, 123, 96);
        CHECK(report.firstSequence == 12000 && report.lastSequence == 12000);
        CHECK(pinned->commit(report.ticket, 1300000));
    }
    CHECK(!pinned->prepare(1500000, 123, 96));
    pinned.reset();
    auto limited = std::make_unique<FeedbackCoordinator>(timing, FeedbackMode::Requested);
    for (unsigned i = 0; i < 30; ++i) CHECK(limited->record(uint16_t(i), 1000000 + i * 250).status == ArrivalStatus::Accepted);
    CHECK(limited->request(29, {true, 30}) == RequestStatus::Queued);
    int64_t rangeCursor = 0;
    unsigned parts = 0;
    while (const auto *report = limited->prepare(2000000, 123, 96, 24)) {
        CHECK(report->firstSequence == rangeCursor && report->packet.size() <= 24);
        rangeCursor = report->lastSequence + 1;
        CHECK(++parts <= 30);
        CHECK(limited->commit(report->ticket, 2000000));
    }
    CHECK(rangeCursor == 30 && parts > 1);
    limited->reset();
    CHECK(limited->record(10, 1000000).status == ArrivalStatus::Accepted);
    CHECK(limited->record(11, 20000000).status == ArrivalStatus::Accepted);
    CHECK(limited->request(11, {true, 2}) == RequestStatus::Queued);
    auto split = *limited->prepare(21000000, 123, 96);
    CHECK(split.firstSequence == 10 && split.lastSequence == 10);
    CHECK(limited->commit(split.ticket, 21000000));
    split = *limited->prepare(21000000, 123, 96);
    CHECK(split.firstSequence == 11 && split.lastSequence == 11);
    CHECK(limited->commit(split.ticket, 21000000));
    limited.reset();
    FeedbackCoordinator periodic(timing);
    CHECK(periodic.request(1, {true, 1}) == RequestStatus::WrongMode);
    const auto packet = [](uint16_t sequence, int64_t time, unsigned pt, bool twoByte = false) {
        uint8_t bytes[20] = {0x90, uint8_t(pt), 0, 1, 0, 0, 0, 0, 0, 0, 0, uint8_t(pt),
            uint8_t(twoByte ? 0x10 : 0xbe), uint8_t(twoByte ? 0 : 0xde), 0, 1,
            uint8_t(twoByte ? 5 : 0x51), 0, 0, 0};
        if (twoByte) { bytes[17] = 2; bytes[18] = uint8_t(sequence >> 8); bytes[19] = uint8_t(sequence); }
        else { bytes[17] = uint8_t(sequence >> 8); bytes[18] = uint8_t(sequence); }
        auto result = rtc::make_message(sizeof(bytes), rtc::Message::Binary);
        std::memcpy(result->data(), bytes, sizeof(bytes));
        result->receivedAtUs = time;
        return result;
    };
    unsigned sends = 0;
    const rtc::message_callback noSend = [&sends](rtc::message_ptr) { ++sends; };
    const NegotiationResult requestedMode{NegotiationStatus::Ready, 5, 1, TransportSequenceVersion::V2};
    auto v2Observer = std::make_shared<TransportFeedbackObserver>(requestedMode, timing);
    v2[18] = 0; v2[19] = 101; v2[20] = 0; v2[21] = 2;
    auto withRequest = rtc::make_message(sizeof(v2), rtc::Message::Binary);
    std::memcpy(withRequest->data(), v2, sizeof(v2)); withRequest->receivedAtUs = 1001000;
    rtc::message_vector requestedMessages{packet(100, 1000000, 96), withRequest};
    const auto requestedOriginal = requestedMessages;
    v2Observer->incoming(requestedMessages, noSend);
    CHECK(requestedMessages == requestedOriginal && sends == 0);
    CHECK(v2Observer->counters().requestsQueued == 1 && v2Observer->counters().accepted == 2);
    v2Observer->incoming(requestedMessages, noSend);
    CHECK(v2Observer->counters().requestsQueued == 1 && v2Observer->counters().duplicates == 2);
    const auto requestedReport = v2Observer->prepare(1100000, 123, 96);
    CHECK(requestedReport && requestedReport->firstSequence == 100 && requestedReport->lastSequence == 101);
    CHECK(requestedReport->packet.size() == 24);
    CHECK(v2Observer->commit(requestedReport->ticket, 1100000));
    CHECK(!v2Observer->prepare(2000000, 123, 96));
    v2[19] = 102; v2[20] = 0x7f; v2[21] = 0xff;
    auto unavailable = rtc::make_message(sizeof(v2), rtc::Message::Binary);
    std::memcpy(unavailable->data(), v2, sizeof(v2)); unavailable->receivedAtUs = 2000000;
    requestedMessages = {unavailable};
    v2Observer->incoming(requestedMessages, noSend);
    CHECK(v2Observer->counters().requestsWithUnretainedPrefix == 1);
    CHECK(requestedMessages[0] == unavailable && sends == 0 && v2Observer->prepare(2100000, 123, 96));
    v2Observer->stopObserving();
    CHECK(!v2Observer->prepare(2200000, 123, 96));
    v2Observer.reset();
    const NegotiationResult bothMappings{NegotiationStatus::Ready, 5, 1, TransportSequenceVersion::V2, 7};
    auto bothObserver = std::make_shared<TransportFeedbackObserver>(bothMappings, timing);
    uint8_t bothBytes[24] = {0x90,96,0,1,0,0,0,0,0,0,0,96,0xbe,0xde,0,2,
        0x71,0,200,0x53,0,201,0x80,1};
    auto bothPacket = rtc::make_message(sizeof(bothBytes), rtc::Message::Binary);
    std::memcpy(bothPacket->data(), bothBytes, sizeof(bothBytes)); bothPacket->receivedAtUs = 1000000;
    rtc::message_vector bothInput{bothPacket};
    bothObserver->incoming(bothInput, noSend);
    const auto preferredReport = bothObserver->prepare(1000000, 123, 96);
    CHECK(preferredReport && preferredReport->firstSequence == 201 && preferredReport->lastSequence == 201);
    CHECK(bothObserver->commit(preferredReport->ticket, 1000000));
    // Fallback to a negotiated 01 extension does not restore the periodic mode.
    bothBytes[16] = 0x71; bothBytes[18] = 199;
    std::fill(bothBytes + 19, bothBytes + 24, 0);
    auto legacyOnlyPacket = rtc::make_message(sizeof(bothBytes), rtc::Message::Binary);
    std::memcpy(legacyOnlyPacket->data(), bothBytes, sizeof(bothBytes)); legacyOnlyPacket->receivedAtUs = 1001000;
    bothInput = {legacyOnlyPacket}; bothObserver->incoming(bothInput, noSend);
    CHECK(bothObserver->counters().accepted == 2 && !bothObserver->prepare(2000000, 123, 96));
    CHECK(bothInput[0] == legacyOnlyPacket && sends == 0);
    auto v2OnlyObserver = std::make_shared<TransportFeedbackObserver>(requestedMode, timing);
    v2OnlyObserver->incoming(bothInput, noSend);
    CHECK(v2OnlyObserver->counters().accepted == 0 && v2OnlyObserver->counters().missingExtension == 1);
    auto observer = std::make_shared<TransportFeedbackObserver>(ready, timing);
    rtc::message_vector messages{packet(65534, 1000000, 96), packet(65535, 1001000, 111), packet(0, 1002000, 97, true)};
    const auto original = messages;
    std::vector<rtc::binary> payloads;
    for (const auto &m : messages) payloads.emplace_back(m->begin(), m->end());
    observer->incoming(messages, noSend);
    CHECK(messages == original && sends == 0);
    for (size_t i = 0; i < messages.size(); ++i) CHECK(rtc::binary(messages[i]->begin(), messages[i]->end()) == payloads[i]);
    CHECK(observer->counters().accepted == 3);
    observer->incoming(messages, noSend);
    CHECK(observer->counters().duplicates == 3 && sends == 0);
    auto pending = observer->prepare(1200000, 123, 0);
    CHECK(pending && pending->firstSequence == 65534 && pending->lastSequence == 65536);
    CHECK(pending->packet[1] == 205 && (pending->packet[0] & 31) == 15);
    const auto retry = observer->prepare(1210000, 123, 0);
    CHECK(retry && retry->ticket == pending->ticket && retry->packet == pending->packet);
    CHECK(!observer->commit(pending->ticket + 1, 1210000));
    CHECK(observer->commit(pending->ticket, 1210000));
    auto missingTime = packet(1, -1, 96);
    auto malformed = rtc::make_message(3, rtc::Message::Binary);
    auto control = rtc::make_message(8, rtc::Message::Control);
    rtc::message_vector invalid{missingTime, malformed, control};
    observer->incoming(invalid, noSend);
    CHECK(observer->counters().missingTimestamp == 1 && observer->counters().malformed == 1 && sends == 0);
    observer->stopObserving();
    const auto accepted = observer->counters().accepted;
    observer->incoming(messages, noSend);
    CHECK(observer->counters().accepted == accepted && !observer->prepare(1300000, 123, 0));

    auto attachment = std::make_shared<FeedbackAttachment>(ready, timing);
    auto budget = std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0);
    const FeedbackSendPolicy policy{1000, 8000, 1000000, 3, 64, "synthetic-test"};
#if defined(UU_TWCC_SEND_EXPERIMENT) && UU_TWCC_SEND_EXPERIMENT
    // End-to-end request waiting is normal even well beyond the legacy two
    // second startup deadline. A later request uses the same authenticated
    // endpoint and emits a compound packet, with no unsolicited periodic send.
    const std::string v2Sdp = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE 0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\na=rtcp-mux\r\n"
        "a=rtpmap:96 H265/90000\r\na=rtcp-fb:96 transport-cc\r\n"
        "a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/transport-wide-cc-02\r\n";
    const auto negotiatedV2 = inspectNegotiationSdp(v2Sdp + "a=recvonly\r\n", rtc::Description::Type::Offer,
        v2Sdp + "a=sendonly\r\n", rtc::Description::Type::Answer);
    CHECK(negotiatedV2.status == NegotiationStatus::Ready && negotiatedV2.version == TransportSequenceVersion::V2);
    // Device regression: the negotiated ID is absent in every media packet.
    // Client cannot create the sender until accepted > 0, so admission must
    // expire without a sender, and must not mistake this for V2 request wait.
    for (auto mode : {ready, negotiatedV2}) {
        mode.extensionId = 6;
        mode.legacyExtensionId = 0;
        auto absent = std::make_shared<FeedbackAttachment>(mode, timing);
        CHECK(absent->poll(0) == FeedbackPollResult::NoReport);
        CHECK(absent->poll(10000000) == FeedbackPollResult::NoReport); // No media, no deadline.
        rtc::message_vector wrongId{packet(1, 10000000, 96)}; // Carries ID5, not negotiated ID6.
        const auto unchanged = wrongId;
        absent->incoming(wrongId, noSend);
        CHECK(wrongId == unchanged && sends == 0);
        CHECK(absent->observationCounters().accepted == 0 &&
              absent->observationCounters().missingExtension == 1);
        CHECK(absent->poll(10000000) == FeedbackPollResult::RetryLater);
        CHECK(absent->state() == FeedbackAttachmentState::WaitingForExtension);
        CHECK(absent->poll(11999999) == FeedbackPollResult::RetryLater);
        CHECK(absent->poll(12000000) == FeedbackPollResult::Stopped);
        CHECK(absent->state() == FeedbackAttachmentState::Faulted);
        CHECK(absent->sendCounters().stopped == FeedbackStopReason::MissingExtensionTimeout);
        CHECK(absent->sendCounters().submitted == 0 && absent->sendCounters().attemptedBytes == 0);
        CHECK(!absent->startSending(budget, policy, 123, 96));
        absent->incoming(wrongId, noSend);
        CHECK(absent->observationCounters().missingExtension == 1 && sends == 0);
        CHECK(absent->poll(20000000) == FeedbackPollResult::Stopped);
        absent->stop();
    }
    // A valid extension arriving during admission cancels the missing-ID
    // deadline. V2 without a request must still wait normally, not fall back.
    auto delayed = std::make_shared<FeedbackAttachment>(negotiatedV2, timing);
    auto wrongId = packet(1, 0, 96);
    (*wrongId)[16] = std::byte{0x61};
    rtc::message_vector delayedInput{wrongId};
    delayed->incoming(delayedInput, noSend);
    CHECK(delayed->poll(0) == FeedbackPollResult::RetryLater);
    delayedInput = {packet(2, 1999999, 96)};
    delayed->incoming(delayedInput, noSend);
    CHECK(delayed->poll(1999999) == FeedbackPollResult::NoReport);
    CHECK(delayed->state() == FeedbackAttachmentState::Observing);
    CHECK(delayed->startSending(budget, policy, 123, 96));
    CHECK(delayed->poll(2000000) == FeedbackPollResult::RetryLater);
    delayedInput = {packet(3, 2000001, 96)};
    delayed->incoming(delayedInput, noSend);
    CHECK(delayed->poll(2000001) == FeedbackPollResult::NoReport);
    CHECK(delayed->state() == FeedbackAttachmentState::WaitingForRequest);
    CHECK(delayed->poll(20000000) == FeedbackPollResult::NoReport && sends == 0);
    delayed->stop();
    auto requestedAttachment = std::make_shared<FeedbackAttachment>(negotiatedV2, timing);
    auto requestBudget = std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0);
    CHECK(requestedAttachment->startSending(requestBudget, policy, 123, 96));
    CHECK(requestedAttachment->poll(0) == FeedbackPollResult::RetryLater);
    unsigned requestSends = 0;
    bool rejectRequest = false;
    std::vector<uint8_t> requestWire;
    const rtc::message_callback requestEndpoint = [&](rtc::message_ptr m) {
        ++requestSends;
        if (rejectRequest) throw std::runtime_error("synthetic rejection");
        requestWire.assign(reinterpret_cast<const uint8_t *>(m->data()),
                           reinterpret_cast<const uint8_t *>(m->data()) + m->size());
    };
    rtc::message_vector requestInput{packet(100, 1000000, 96)};
    requestedAttachment->incoming(requestInput, requestEndpoint);
    CHECK(requestedAttachment->poll(1000000) == FeedbackPollResult::NoReport);
    CHECK(requestedAttachment->state() == FeedbackAttachmentState::WaitingForRequest);
    CHECK(requestedAttachment->poll(20000000) == FeedbackPollResult::NoReport && requestSends == 0);
    requestInput = {withRequest};
    requestedAttachment->incoming(requestInput, requestEndpoint);
    CHECK(requestedAttachment->poll(20001000) == FeedbackPollResult::Submitted && requestSends == 1);
    CHECK(requestWire[1] == 201);
    size_t requestOffset = 0;
    while (requestOffset + 4 <= requestWire.size() && requestWire[requestOffset + 1] != 205)
        requestOffset += 4 * (1 + (size_t(requestWire[requestOffset + 2]) << 8 | requestWire[requestOffset + 3]));
    CHECK(requestOffset + 24 == requestWire.size()); // T=0, two received positions, padded RTCP.
    CHECK(requestWire[requestOffset + 12] == 0 && requestWire[requestOffset + 13] == 100);
    CHECK(requestWire[requestOffset + 15] == 2);
    CHECK(requestedAttachment->poll(30000000) == FeedbackPollResult::NoReport && requestSends == 1);
    requestInput = {withRequest}; // Duplicate must not schedule another report.
    requestedAttachment->incoming(requestInput, requestEndpoint);
    CHECK(requestedAttachment->poll(30001000) == FeedbackPollResult::NoReport);
    auto secondRequest = rtc::make_message(withRequest->size(), rtc::Message::Binary);
    std::memcpy(secondRequest->data(), withRequest->data(), withRequest->size());
    (*secondRequest)[19] = std::byte{102};
    secondRequest->receivedAtUs = 30002000;
    requestInput = {secondRequest};
    requestedAttachment->incoming(requestInput, requestEndpoint);
    rejectRequest = true;
    CHECK(requestedAttachment->poll(30003000) == FeedbackPollResult::SendFailed);
    CHECK(requestedAttachment->poll(30004000) == FeedbackPollResult::SendFailed);
    CHECK(requestedAttachment->poll(30006000) == FeedbackPollResult::Stopped);
    CHECK(requestedAttachment->sendCounters().stopped == FeedbackStopReason::SendFailureLimit);
    CHECK(requestedAttachment->poll(30007000) == FeedbackPollResult::Stopped);
    // Missing the post-start callback is a real startup failure for either
    // version, not a reason to leave legacy feedback suppressed indefinitely.
    for (const auto mode : {ready, negotiatedV2}) {
        auto missingEndpoint = std::make_shared<FeedbackAttachment>(mode, timing);
        CHECK(missingEndpoint->startSending(std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0), policy, 123, 96));
        CHECK(missingEndpoint->poll(0) == FeedbackPollResult::RetryLater);
        CHECK(missingEndpoint->poll(1999999) == FeedbackPollResult::RetryLater);
        CHECK(missingEndpoint->poll(2000000) == FeedbackPollResult::Stopped);
        CHECK(missingEndpoint->sendCounters().stopped == FeedbackStopReason::StartupTimeout);
    }
    auto staleClock = std::make_shared<FeedbackAttachment>(negotiatedV2, timing);
    CHECK(staleClock->startSending(std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0), policy, 123, 96));
    CHECK(staleClock->poll(100) == FeedbackPollResult::RetryLater);
    CHECK(staleClock->poll(99) == FeedbackPollResult::Stopped);
    CHECK(staleClock->sendCounters().stopped == FeedbackStopReason::ClockInvalid);
    auto emptyV1 = std::make_shared<FeedbackAttachment>(ready, timing);
    CHECK(emptyV1->startSending(std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0), policy, 123, 96));
    auto controlOnly = rtc::make_message(8, rtc::Message::Control);
    rtc::message_vector controlInput{controlOnly};
    emptyV1->incoming(controlInput, requestEndpoint);
    CHECK(emptyV1->poll(0) == FeedbackPollResult::NoReport);
    CHECK(emptyV1->poll(2000000) == FeedbackPollResult::Stopped);
    CHECK(emptyV1->sendCounters().stopped == FeedbackStopReason::StartupTimeout);
    // An explicitly stopped V2 attachment cannot resume or submit after a
    // later packet; a replacement generation starts with no inherited request.
    auto stoppedRequest = std::make_shared<FeedbackAttachment>(negotiatedV2, timing);
    CHECK(stoppedRequest->startSending(std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0), policy, 123, 96));
    stoppedRequest->stop();
    const auto sendsBeforeStop = requestSends;
    stoppedRequest->incoming(requestInput, requestEndpoint);
    CHECK(stoppedRequest->poll(40000000) == FeedbackPollResult::Stopped && requestSends == sendsBeforeStop);
    CHECK(!stoppedRequest->startSending(requestBudget, policy, 123, 96));
    // A request with an unretained prefix can still be served, without
    // inventing arrival timestamps or treating it as idle waiting.
    auto unavailableRequest = std::make_shared<FeedbackAttachment>(negotiatedV2, timing);
    CHECK(unavailableRequest->startSending(std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0), policy, 123, 96));
    requestInput = {packet(100, 1000000, 96)};
    unavailableRequest->incoming(requestInput, requestEndpoint);
    CHECK(unavailableRequest->poll(1000000) == FeedbackPollResult::NoReport);
    requestInput = {unavailable};
    unavailableRequest->incoming(requestInput, requestEndpoint);
    rejectRequest = false;
    CHECK(unavailableRequest->poll(2000000) == FeedbackPollResult::Submitted);
    CHECK(unavailableRequest->observationCounters().requestsWithUnretainedPrefix == 1);
    unavailableRequest->stop();
    // Use the real legacy RTCP session beside the new sender: clearing its
    // remembered target must stop SR-triggered REMB without suppressing RR.
    rtc::RtcpReceivingSession legacy;
    legacy.setRtcpIdentity(123, policy.rtcpCname);
    std::vector<rtc::message_ptr> legacyPackets;
    const rtc::message_callback legacySend = [&](rtc::message_ptr m) { legacyPackets.push_back(std::move(m)); };
    rtc::message_vector primary{packet(19, 1000000, 96)};
    legacy.incoming(primary, legacySend);
    CHECK(legacy.requestBitrate(30000000, legacySend) && legacyPackets.size() == 1);
    legacyPackets.clear();
    CHECK(legacy.requestBitrate(0, legacySend) && legacyPackets.empty());
    CHECK(attachment->startSending(budget, policy, 123, 96));
    auto sr = rtc::make_message(28, rtc::Message::Control);
    std::memset(sr->data(), 0, sr->size());
    (*sr)[0] = std::byte{0x80}; (*sr)[1] = std::byte{200};
    (*sr)[3] = std::byte{6}; (*sr)[7] = std::byte{96};
    rtc::message_vector reports{sr};
    legacy.incoming(reports, legacySend);
    CHECK(legacyPackets.size() == 1);
    size_t reportOffset = 0; unsigned reportBlocks = 0;
    while (reportOffset < legacyPackets[0]->size()) {
        const auto *data = reinterpret_cast<const uint8_t *>(legacyPackets[0]->data());
        CHECK(legacyPackets[0]->size() - reportOffset >= 4 && reportBlocks < 2);
        CHECK(data[reportOffset + 1] == (reportBlocks == 0 ? 201 : 202));
        const size_t bytes = 4 * (1 + (size_t(data[reportOffset + 2]) << 8 | data[reportOffset + 3]));
        CHECK(bytes <= legacyPackets[0]->size() - reportOffset);
        reportOffset += bytes; ++reportBlocks;
    }
    CHECK(reportBlocks == 2);
    CHECK(attachment->poll(1000000) == FeedbackPollResult::RetryLater);
    bool reject = false;
    std::vector<uint8_t> submitted;
    const rtc::message_callback endpoint = [&](rtc::message_ptr m) {
        if (reject) throw std::runtime_error("synthetic send failure");
        ++sends; submitted.assign(reinterpret_cast<const uint8_t *>(m->data()), reinterpret_cast<const uint8_t *>(m->data()) + m->size());
    };
    messages = {packet(20, 1100000, 96)};
    attachment->incoming(messages, endpoint);
    CHECK(sends == 0);
    CHECK(attachment->poll(1200000) == FeedbackPollResult::Submitted && sends == 1);
    CHECK(attachment->sendCounters().submitted == 1 && submitted.size() <= 1200);
    CHECK(attachment->sendCounters().submittedBytes == submitted.size());
    CHECK(attachment->sendCounters().attemptedBytes == submitted.size());
    size_t offset = 0; unsigned blocks = 0;
    const unsigned types[] = {201, 202, 205};
    while (offset < submitted.size()) {
        CHECK(blocks < 3 && submitted.size() - offset >= 4 && submitted[offset + 1] == types[blocks]);
        const size_t bytes = 4 * (1 + (size_t(submitted[offset + 2]) << 8 | submitted[offset + 3]));
        CHECK(bytes >= 4 && bytes <= submitted.size() - offset);
        if (submitted[offset + 1] == 205) {
            CHECK(bytes >= 20);
            const uint32_t mediaSsrc = uint32_t(submitted[offset + 8]) << 24 |
                uint32_t(submitted[offset + 9]) << 16 | uint32_t(submitted[offset + 10]) << 8 |
                submitted[offset + 11];
            // Independent PC dispatch contract: a nonmatching media SSRC
            // never reaches the transport-feedback callback for this stream.
            CHECK(mediaSsrc == 96 && mediaSsrc != 0);
        }
        offset += bytes; ++blocks;
    }
    CHECK(blocks == 3);
    reject = true;
    messages = {packet(21, 1300000, 96)};
    attachment->incoming(messages, endpoint);
    CHECK(attachment->poll(1400000) == FeedbackPollResult::SendFailed);
    CHECK(attachment->sendCounters().submitted == 1 && attachment->sendCounters().sendFailures == 1);
    CHECK(attachment->sendCounters().attemptedBytes > attachment->sendCounters().submittedBytes);
    CHECK(attachment->poll(1400001) == FeedbackPollResult::RetryLater);
    reject = false;
    CHECK(attachment->poll(1401000) == FeedbackPollResult::Submitted && sends == 2);
    // A terminal send failure must stop the new producer before legacy REMB
    // is restored; the old session still has a valid identity and RTP source.
    reject = true;
    messages = {packet(22, 1500000, 96)};
    attachment->incoming(messages, endpoint);
    CHECK(attachment->poll(1600000) == FeedbackPollResult::SendFailed);
    CHECK(attachment->poll(1601000) == FeedbackPollResult::SendFailed);
    CHECK(attachment->poll(1603000) == FeedbackPollResult::Stopped);
    CHECK(attachment->state() == FeedbackAttachmentState::Faulted);
    CHECK(attachment->sendCounters().stopped == FeedbackStopReason::SendFailureLimit);
    legacyPackets.clear();
    CHECK(legacy.requestBitrate(12000000, legacySend) && legacyPackets.size() == 1);
    const auto &restored = legacyPackets[0];
    CHECK(restored->size() >= 24 && std::to_integer<uint8_t>((*restored)[restored->size() - 23]) == 206);

    // A slow feedback send must not hold up authenticated media admission.
    // The owning attachment must still serialize stop with that in-flight send.
    auto concurrent = std::make_shared<FeedbackAttachment>(ready, timing);
    auto concurrentBudget = std::make_shared<SharedRtcpBudget>(100000, 6000, 6000, 600, 0);
    CHECK(concurrent->startSending(concurrentBudget, policy, 123, 96));
    std::promise<void> sendEntered, releaseSend, stopStarted;
    auto entered = sendEntered.get_future();
    auto release = releaseSend.get_future().share();
    unsigned concurrentSends = 0, replacementSends = 0;
    const rtc::message_callback slowSend = [&](rtc::message_ptr) {
        ++concurrentSends;
        sendEntered.set_value();
        release.wait();
    };
    const rtc::message_callback replacementSend = [&](rtc::message_ptr) { ++replacementSends; };
    rtc::message_vector initialInput{packet(30, 1000000, 96)};
    concurrent->incoming(initialInput, slowSend);
    auto sending = std::async(std::launch::async, [&] { return concurrent->poll(1200000); });
    const bool didEnter = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!didEnter) {
        releaseSend.set_value();
        sending.get();
        CHECK(didEnter);
    }
    rtc::message_vector nextInput{packet(31, 1201000, 96)};
    const auto nextPacket = nextInput.front();
    auto receiving = std::async(std::launch::async, [&] { concurrent->incoming(nextInput, replacementSend); });
    const bool receivedWhileSending = receiving.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    auto stopping = std::async(std::launch::async, [&] {
        stopStarted.set_value();
        concurrent->stop();
    });
    stopStarted.get_future().wait();
    const bool stopWaited = stopping.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    releaseSend.set_value();
    const auto sendResult = sending.get();
    receiving.get();
    stopping.get();
    CHECK(receivedWhileSending && stopWaited);
    CHECK(sendResult == FeedbackPollResult::Submitted && concurrentSends == 1 && replacementSends == 0);
    CHECK(nextInput.size() == 1 && nextInput.front() == nextPacket);
    CHECK(concurrent->observationCounters().accepted == 2);
    concurrent->incoming(nextInput, replacementSend);
    CHECK(concurrent->poll(1400000) == FeedbackPollResult::Stopped && replacementSends == 0);
    CHECK(concurrent->observationCounters().accepted == 2);

    auto reconnected = std::make_shared<FeedbackAttachment>(ready, timing);
    CHECK(reconnected->startSending(concurrentBudget, policy, 124, 96));
    reconnected->incoming(nextInput, replacementSend);
    CHECK(reconnected->poll(1400000) == FeedbackPollResult::Submitted);
    CHECK(replacementSends == 1 && concurrentSends == 1);
    reconnected->stop();
    std::cout << "blocked feedback send: media continued, stop waited, reconnect used a fresh endpoint\n";
#else
    CHECK(!attachment->startSending(budget, policy, 123, 0));
    attachment->incoming(messages, noSend);
    CHECK(attachment->observationCounters().accepted == 3 && sends == 0);
    CHECK(attachment->poll(1200000) == FeedbackPollResult::NoReport);
#endif
    attachment->stop();
    attachment->stop();
    const auto beforeStop = sends;
    attachment->incoming(messages, noSend);
    CHECK(attachment->state() == FeedbackAttachmentState::Stopped && sends == beforeStop);
    CHECK(attachment->poll(1500000) == FeedbackPollResult::Stopped);
    CHECK(!attachment->startSending(budget, policy, 123, 0));
    auto next = std::make_shared<FeedbackAttachment>(ready, timing);
    CHECK(next->observationCounters().accepted == 0);
    next->incoming(messages, noSend);
    CHECK(next->observationCounters().accepted == messages.size() && sends == beforeStop);
    std::cout << "observation ownership, common sequence space, stop and feedback lifecycle passed\n";
}
