#include "transport_feedback_negotiation.hpp"
#include "remb_feedback.hpp"
#include <rtc/rtcpreceivingsession.hpp>
#include <algorithm>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed line " << __LINE__ << ": " #x "\n"; return 1; } } while (0)

int main() {
    using namespace uu::twcc;
    using Type = rtc::Description::Type;
    const std::string prefix = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\na=mid:0\r\na=rtcp-mux\r\n"
        "a=rtpmap:96 H265/90000\r\na=rtpmap:97 rtx/90000\r\na=fmtp:97 apt=96\r\n";
    const std::string uri1 = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
    const std::string uri2 = "http://www.webrtc.org/experiments/rtp-hdrext/transport-wide-cc-02";
    const auto localRaw = prefix + "a=recvonly\r\na=rtcp-fb:96 goog-remb\r\na=rtcp-fb:96 nack pli\r\n"
        "a=rtcp-fb:* transport-cc\r\na=extmap:5/recvonly " + uri1 + "\r\n";
    const auto remoteRaw = prefix + "a=sendonly\r\na=rtcp-fb:96 goog-remb\r\na=rtcp-fb:96 nack\r\n"
        "a=rtcp-fb:96 transport-cc\r\na=extmap:5/sendonly " + uri1 + "\r\n";
    const auto local = NegotiationCapabilities::fromSdp(localRaw, Type::Offer);
    const auto remote = NegotiationCapabilities::fromSdp(remoteRaw, Type::Answer);
    CHECK(local && remote);
    auto result = inspectMediaFeedbackCapabilities(*local, *remote);
    CHECK(result.status == NegotiationStatus::Ready && result.localIsOfferer && result.media.size() == 1);
    CHECK((result.media[0].feedback == std::vector<std::string>{"goog-remb", "transport-cc"}));
    CHECK((result.media[0].rtxPayloadTypes == std::vector<int>{97}));
    CHECK(result.media[0].transportExtensions.size() == 1);
    CHECK(result.media[0].transportExtensions[0].localId == 5 && result.media[0].transportExtensions[0].remoteId == 5);
    CHECK(inspectNegotiationCapabilities(*local, *remote).status == NegotiationStatus::Ready);
    auto id3Offer = localRaw;
    id3Offer.replace(id3Offer.find("extmap:5"), 8, "extmap:3");
    auto id3Answer = remoteRaw;
    id3Answer.replace(id3Answer.find("extmap:5"), 8, "extmap:3");
    const auto id3 = inspectNegotiationSdp(id3Offer, Type::Offer, id3Answer, Type::Answer);
    CHECK(id3.status == NegotiationStatus::Ready && id3.extensionId == 3 &&
          id3.version == TransportSequenceVersion::V1);
    CHECK(inspectNegotiationSdp(id3Offer, Type::Offer, remoteRaw, Type::Answer).status !=
          NegotiationStatus::Ready);
    CHECK(result.allowsRemb("0", 96));
    CHECK(!result.allowsRemb("1", 96) && !result.allowsRemb("0", 97));
    CHECK(!FeedbackCapabilitiesResult{}.allowsRemb("0", 96));
    const auto noLocalRemb = NegotiationCapabilities::fromSdp(prefix +
        "a=recvonly\r\na=rtcp-fb:96 transport-cc\r\n", Type::Offer);
    const auto noRemoteRemb = NegotiationCapabilities::fromSdp(prefix +
        "a=sendonly\r\na=rtcp-fb:96 transport-cc\r\n", Type::Answer);
    CHECK(noLocalRemb && noRemoteRemb);
    CHECK(!inspectMediaFeedbackCapabilities(*noLocalRemb, *remote).allowsRemb("0", 96));
    CHECK(!inspectMediaFeedbackCapabilities(*local, *noRemoteRemb).allowsRemb("0", 96));
    const auto parameterizedRemb = NegotiationCapabilities::fromSdp(prefix +
        "a=sendonly\r\na=rtcp-fb:96 goog-remb unsupported\r\n", Type::Answer);
    CHECK(parameterizedRemb && !inspectMediaFeedbackCapabilities(*local, *parameterizedRemb).allowsRemb("0", 96));
    CHECK(!inspectMediaFeedbackCapabilities(*local, *local).allowsRemb("0", 96));

    // Classic RBE timing extensions need the same plain ID usable remote-to-local.
    const std::string ast = "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";
    const std::string tof = "urn:ietf:params:rtp-hdrext:toffset";
    const auto timingLocal = NegotiationCapabilities::fromSdp(localRaw + "a=extmap:3/recvonly " + ast + "\r\na=extmap:2 " + tof + "\r\n", Type::Offer);
    const auto timingRemote = NegotiationCapabilities::fromSdp(remoteRaw + "a=extmap:3/sendonly " + ast + "\r\na=extmap:2 " + tof + "\r\n", Type::Answer);
    CHECK(timingLocal && timingRemote);
    result = inspectMediaFeedbackCapabilities(*timingLocal, *timingRemote);
    CHECK(result.media.size() == 1 && result.media[0].absSendTimeId == 3 && result.media[0].transmissionOffsetId == 2);
    CHECK(inspectMediaFeedbackCapabilities(*local, *remote).media[0].absSendTimeId == 0);
    const auto timingMoved = NegotiationCapabilities::fromSdp(remoteRaw + "a=extmap:4/sendonly " + ast + "\r\na=extmap:2/recvonly " + tof + "\r\n", Type::Answer);
    CHECK(timingMoved);
    result = inspectMediaFeedbackCapabilities(*timingLocal, *timingMoved);
    CHECK(result.media[0].absSendTimeId == 0 && result.media[0].transmissionOffsetId == 0);
    const auto timingAttributed = NegotiationCapabilities::fromSdp(remoteRaw + "a=extmap:3/sendonly " + ast + " unsupported\r\n", Type::Answer);
    CHECK(timingAttributed && inspectMediaFeedbackCapabilities(*timingLocal, *timingAttributed).media[0].absSendTimeId == 0);
    const auto timingSendOnlyLocal = NegotiationCapabilities::fromSdp(localRaw + "a=extmap:3/sendonly " + ast + "\r\n", Type::Offer);
    CHECK(timingSendOnlyLocal && inspectMediaFeedbackCapabilities(*timingSendOnlyLocal, *timingRemote).media[0].absSendTimeId == 0);

    // Only an unambiguous final common mapping authorizes a parser version.
    const auto local02 = NegotiationCapabilities::fromSdp(prefix + "a=recvonly\r\na=rtcp-fb:96 transport-cc\r\na=extmap:7 " + uri2 + "\r\n", Type::Offer);
    const auto remote02 = NegotiationCapabilities::fromSdp(prefix + "a=sendonly\r\na=rtcp-fb:96 transport-cc\r\na=extmap:7 " + uri2 + "\r\n", Type::Answer);
    CHECK(local02 && remote02);
    result = inspectMediaFeedbackCapabilities(*local02, *remote02);
    CHECK(result.media.size() == 1 && result.media[0].transportExtensions[0].version == TransportExtensionVersion::V2);
    const auto ready02 = inspectNegotiationCapabilities(*local02, *remote02);
    CHECK(ready02.status == NegotiationStatus::Ready && ready02.version == TransportSequenceVersion::V2 && ready02.extensionId == 7);
    CHECK(inspectNegotiationCapabilities(*local, *remote).version == TransportSequenceVersion::V1);
    const auto offerBoth = NegotiationCapabilities::fromSdp(localRaw + "a=extmap:7/recvonly " + uri2 + "\r\n", Type::Offer);
    CHECK(offerBoth);
    CHECK(inspectNegotiationCapabilities(*offerBoth, *remote02).version == TransportSequenceVersion::V2);
    CHECK(inspectNegotiationCapabilities(*offerBoth, *remote).status == NegotiationStatus::Ready);
    const auto answerBoth = NegotiationCapabilities::fromSdp(remoteRaw + "a=extmap:7/sendonly " + uri2 + "\r\n", Type::Answer);
    CHECK(answerBoth);
    const auto bothReady = inspectNegotiationCapabilities(*offerBoth, *answerBoth);
    CHECK(bothReady.status == NegotiationStatus::Ready && bothReady.version == TransportSequenceVersion::V2);
    CHECK(bothReady.extensionId == 7 && bothReady.legacyExtensionId == 5);
    CHECK(inspectNegotiationCapabilities(*local, *remote02).status != NegotiationStatus::Ready);
    const auto reverseOffer = NegotiationCapabilities::fromSdp(remoteRaw + "a=extmap:7/sendonly " + uri2 + "\r\n", Type::Offer);
    const auto reverseAnswer = NegotiationCapabilities::fromSdp(prefix + "a=recvonly\r\na=rtcp-fb:* transport-cc\r\na=extmap:7/recvonly " + uri2 + "\r\n", Type::Answer);
    CHECK(reverseOffer && reverseAnswer);
    CHECK(inspectNegotiationCapabilities(*reverseAnswer, *reverseOffer).version == TransportSequenceVersion::V2);
    const auto attributed02 = NegotiationCapabilities::fromSdp(prefix + "a=sendonly\r\na=rtcp-fb:96 transport-cc\r\na=extmap:7 " + uri2 + " unsupported\r\n", Type::Answer);
    CHECK(attributed02 && inspectNegotiationCapabilities(*local02, *attributed02).status == NegotiationStatus::UnsupportedExtension);
    // Equivalent stale snapshots still fail before attaching to a changed peer.
    CHECK(!local02->matchesInstalledDescription(rtc::Description(localRaw, Type::Offer)));
    const auto wrongDirection02 = NegotiationCapabilities::fromSdp(prefix + "a=sendonly\r\na=rtcp-fb:96 transport-cc\r\na=extmap:7/recvonly " + uri2 + "\r\n", Type::Answer);
    CHECK(wrongDirection02 && inspectNegotiationCapabilities(*local02, *wrongDirection02).status != NegotiationStatus::Ready);

    auto conflictingRaw = remoteRaw;
    conflictingRaw.replace(conflictingRaw.find("extmap:5"), 8, "extmap:6");
    const auto conflicting = NegotiationCapabilities::fromSdp(conflictingRaw, Type::Answer);
    CHECK(conflicting);
    result = inspectMediaFeedbackCapabilities(*local, *conflicting);
    CHECK(result.media[0].transportExtensions[0].localId == 5 && result.media[0].transportExtensions[0].remoteId == 6);
    CHECK(inspectNegotiationCapabilities(*local, *conflicting).status != NegotiationStatus::Ready);

    const auto sendOnly = NegotiationCapabilities::fromSdp(prefix + "a=sendonly\r\na=rtcp-fb:96 goog-remb\r\n", Type::Offer);
    CHECK(sendOnly && inspectMediaFeedbackCapabilities(*sendOnly, *remote).media.empty());
    CHECK(inspectMediaFeedbackCapabilities(*local, *local).status == NegotiationStatus::NotNegotiated);
    CHECK(!NegotiationCapabilities::fromSdp(localRaw + "a=extmap:5 " + uri2 + "\r\n", Type::Offer));
    auto noMux = remoteRaw;
    noMux.erase(noMux.find("a=rtcp-mux\r\n"), 12);
    const auto separateTransport = NegotiationCapabilities::fromSdp(noMux, Type::Answer);
    CHECK(separateTransport && inspectMediaFeedbackCapabilities(*local, *separateTransport).status == NegotiationStatus::UnsupportedTransport);
    CHECK(!inspectMediaFeedbackCapabilities(*local, *separateTransport).allowsRemb("0", 96));
    // SDK serialization synthesizes mux/BUNDLE. A raw unsupported transport
    // must remain unsupported even when the SDK view appears ready.
    const rtc::Description installed(noMux, Type::Answer);
    const auto rewritten = NegotiationCapabilities::fromSdp(std::string(installed), Type::Answer);
    // The SDK keeps the input group as well as synthesizing its own. This is
    // invalid as raw input, but an equivalent installed view can be compared.
    CHECK(!rewritten);
    CHECK(separateTransport->matchesInstalledDescription(installed));
    CHECK(inspectNegotiationCapabilities(*local, *separateTransport).status == NegotiationStatus::UnsupportedTransport);
    CHECK(local->matchesInstalledDescription(rtc::Description(localRaw, Type::Offer)));
    CHECK(!local->matchesInstalledDescription(rtc::Description(conflictingRaw, Type::Offer)));
    CHECK(!NegotiationCapabilities::fromSdp(localRaw + "a=group:BUNDLE 0\r\n", Type::Offer));

    const auto answerLocal = NegotiationCapabilities::fromSdp(localRaw, Type::Answer);
    const auto offerRemote = NegotiationCapabilities::fromSdp(remoteRaw, Type::Offer);
    CHECK(answerLocal && offerRemote);
    result = inspectMediaFeedbackCapabilities(*answerLocal, *offerRemote);
    CHECK(result.status == NegotiationStatus::Ready && !result.localIsOfferer);
    // Credential/candidate changes must not change the allowed capability state.
    const auto withTransport = NegotiationCapabilities::fromSdp(localRaw +
        "a=ice-ufrag:synthetic\r\na=ice-pwd:synthetic-test-only\r\na=candidate:1 1 UDP 1 127.0.0.1 9 typ host\r\n", Type::Offer);
    CHECK(withTransport && *withTransport == *local);
    const auto noExtensionLocal = NegotiationCapabilities::fromSdp(prefix + "a=recvonly\r\na=rtcp-fb:96 goog-remb\r\n", Type::Offer);
    const auto noExtensionRemote = NegotiationCapabilities::fromSdp(prefix + "a=sendonly\r\na=rtcp-fb:* goog-remb\r\n", Type::Answer);
    CHECK(noExtensionLocal && noExtensionRemote);
    result = inspectMediaFeedbackCapabilities(*noExtensionLocal, *noExtensionRemote);
    CHECK(result.media.size() == 1 && result.media[0].transportExtensions.empty());
    CHECK((result.media[0].feedback == std::vector<std::string>{"goog-remb"}));
    CHECK(result.allowsRemb("0", 96)); // Media-level wildcard applies to this video codec.
    CHECK(inspectNegotiationCapabilities(*noExtensionLocal, *noExtensionRemote).status == NegotiationStatus::NotNegotiated);
    auto differentCodec = remoteRaw;
    differentCodec.replace(differentCodec.find("H265"), 4, "H264");
    const auto incompatible = NegotiationCapabilities::fromSdp(differentCodec, Type::Answer);
    CHECK(incompatible && inspectMediaFeedbackCapabilities(*local, *incompatible).media.empty());
    CHECK(!inspectMediaFeedbackCapabilities(*local, *incompatible).allowsRemb("0", 96));
    auto wrongApt = remoteRaw;
    wrongApt.replace(wrongApt.find("apt=96"), 6, "apt=95");
    const auto unassociatedRtx = NegotiationCapabilities::fromSdp(wrongApt, Type::Answer);
    CHECK(unassociatedRtx && inspectMediaFeedbackCapabilities(*local, *unassociatedRtx).media[0].rtxPayloadTypes.empty());
    const std::string audio = "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:1\r\na=rtcp-mux\r\na=rtpmap:111 opus/48000/2\r\n";
    auto bundleLocal = localRaw, bundleRemote = remoteRaw;
    bundleLocal.replace(bundleLocal.find("BUNDLE 0"), 8, "BUNDLE 0 1");
    bundleRemote.replace(bundleRemote.find("BUNDLE 0"), 8, "BUNDLE 0 1");
    bundleLocal += audio + "a=recvonly\r\na=rtcp-fb:111 transport-cc\r\n";
    bundleRemote += audio + "a=sendonly\r\na=rtcp-fb:111 nack\r\n";
    const auto audioLocal = NegotiationCapabilities::fromSdp(bundleLocal, Type::Offer);
    const auto audioRemote = NegotiationCapabilities::fromSdp(bundleRemote, Type::Answer);
    CHECK(audioLocal && audioRemote);
    result = inspectMediaFeedbackCapabilities(*audioLocal, *audioRemote);
    CHECK(result.media.size() == 2 && result.media[1].payloadType == 111);
    CHECK(result.media[1].feedback.empty() && result.media[1].transportExtensions.empty());
    auto mixedLocal = bundleLocal + "a=extmap:5 " + uri2 + "\r\n";
    auto mixedRemote = bundleRemote + "a=extmap:5 " + uri2 + "\r\n";
    const auto mixedLocalCaps = NegotiationCapabilities::fromSdp(mixedLocal, Type::Offer);
    const auto mixedRemoteCaps = NegotiationCapabilities::fromSdp(mixedRemote, Type::Answer);
    CHECK(mixedLocalCaps && mixedRemoteCaps);
    CHECK(inspectNegotiationCapabilities(*mixedLocalCaps, *mixedRemoteCaps).status == NegotiationStatus::InconsistentId);
    const std::string sessionPrefix = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE 0\r\na=extmap:7 " + uri2 + "\r\n";
    const std::string sessionVideo = "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\na=rtcp-mux\r\n"
        "a=rtpmap:96 H265/90000\r\na=rtcp-fb:96 transport-cc\r\n";
    const auto sessionV2 = inspectNegotiationSdp(sessionPrefix + sessionVideo + "a=recvonly\r\n", Type::Offer,
        sessionPrefix + sessionVideo + "a=sendonly\r\n", Type::Answer);
    CHECK(sessionV2.status == NegotiationStatus::Ready && sessionV2.version == TransportSequenceVersion::V2);
    auto audioOnlyLocalRaw = bundleLocal, audioOnlyRemoteRaw = bundleRemote;
    audioOnlyLocalRaw.erase(audioOnlyLocalRaw.find("a=rtcp-fb:96 goog-remb\r\n"), 24);
    audioOnlyRemoteRaw.erase(audioOnlyRemoteRaw.find("a=rtcp-fb:96 goog-remb\r\n"), 24);
    audioOnlyLocalRaw += "a=rtcp-fb:111 goog-remb\r\n";
    audioOnlyRemoteRaw += "a=rtcp-fb:111 goog-remb\r\n";
    const auto audioOnlyLocal = NegotiationCapabilities::fromSdp(audioOnlyLocalRaw, Type::Offer);
    const auto audioOnlyRemote = NegotiationCapabilities::fromSdp(audioOnlyRemoteRaw, Type::Answer);
    CHECK(audioOnlyLocal && audioOnlyRemote);
    CHECK(!inspectMediaFeedbackCapabilities(*audioOnlyLocal, *audioOnlyRemote).allowsRemb("0", 96));

    // Drive the real RTCP session with the negotiated decision: changing a
    // boolean alone leaves its SR-triggered saved target active. Clear it on
    // invalidation, keep RR/SDES, then explicitly re-admit the new ceiling.
    rtc::RtcpReceivingSession session;
    session.setRtcpIdentity(123, "synthetic-admission");
    std::vector<rtc::message_ptr> emitted;
    const rtc::message_callback send = [&](rtc::message_ptr m) { emitted.push_back(std::move(m)); };
    auto rtp = rtc::make_message(16, rtc::Message::Binary);
    (*rtp)[0]=std::byte{0x80}; (*rtp)[1]=std::byte{96}; (*rtp)[3]=std::byte{1};
    (*rtp)[8]=std::byte{1}; (*rtp)[9]=std::byte{2}; (*rtp)[10]=std::byte{3}; (*rtp)[11]=std::byte{4};
    rtc::message_vector incoming{rtp}; session.incoming(incoming, send);
    uu::RembFeedbackGate gate;
    const auto allowed = inspectMediaFeedbackCapabilities(*local, *remote);
    CHECK(gate.prepare(1000,30000000,0x01020304,allowed.allowsRemb("0",96),true) == uu::RembFeedbackGate::Action::Send);
    CHECK(session.requestBitrate(30000000,send)); gate.accepted(30000000,0x01020304);
    const auto packetMask = [](const rtc::message_ptr &packet) {
        unsigned mask=0;
        for(size_t offset=0;offset<packet->size();) {
            if(packet->size()-offset<4) return 0u;
            const auto *bytes=reinterpret_cast<const uint8_t *>(packet->data())+offset;
            const size_t length=4*(1+(size_t(bytes[2])<<8|bytes[3]));
            if(length<4 || length>packet->size()-offset) return 0u;
            if(bytes[1]==201) mask|=1;
            else if(bytes[1]==202) mask|=2;
            else if(bytes[1]==206 && (bytes[0]&31)==15 && length>=20 &&
                bytes[12]=='R' && bytes[13]=='E' && bytes[14]=='M' && bytes[15]=='B') mask|=4;
            else return 0u;
            offset+=length;
        }
        return mask;
    };
    CHECK(emitted.size()==1 && packetMask(emitted.back())==7);
    emitted.clear();
    CHECK(session.requestBitrate(0,send)); gate.invalidate();
    CHECK(emitted.empty()); // Revocation sends no zero-bandwidth packet.
    const auto revoked = inspectMediaFeedbackCapabilities(*local,*noRemoteRemb);
    CHECK(gate.prepare(2000,12000000,0x01020304,revoked.allowsRemb("0",96),true)==uu::RembFeedbackGate::Action::NotNegotiated);
    auto sr=rtc::make_message(28,rtc::Message::Control);
    (*sr)[0]=std::byte{0x80}; (*sr)[1]=std::byte{200}; (*sr)[3]=std::byte{6};
    (*sr)[4]=std::byte{1}; (*sr)[5]=std::byte{2}; (*sr)[6]=std::byte{3}; (*sr)[7]=std::byte{4};
    incoming={sr}; session.incoming(incoming,send);
    CHECK(emitted.size()==1 && packetMask(emitted.back())==3);
    emitted.clear();
    CHECK(gate.prepare(3000,12000000,0x01020304,allowed.allowsRemb("0",96),true)==uu::RembFeedbackGate::Action::Send);
    CHECK(session.requestBitrate(12000000,send)); gate.accepted(12000000,0x01020304);
    CHECK(emitted.size()==1 && packetMask(emitted.back())==7 && gate.targetBps()==12000000);
    std::cout << "feedback capability intersection, RTX, directions, IDs, 01/02 and transport boundaries passed\n";
}
