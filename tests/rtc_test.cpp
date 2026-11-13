#include "background.hpp"
#include "rtc_signaling.hpp"
#include "dtls_rx_diagnostics.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <rtc/rtc.hpp>
#include <thread>
#ifdef UU_TEST_NO_DETACH
#include <atomic>
#include <cerrno>
#include <pthread.h>
std::atomic<int> detachCalls{0};
extern "C" int __wrap_pthread_detach(pthread_t) {
    ++detachCalls;
    return ENOSYS;
}
#endif

int loopback() {
    std::mutex mutex;
    std::condition_variable cv;
    bool input = false, picture = false, audio = false;
    unsigned pictures=0,inputs=0;uint32_t lastTimestamp=0;bool ordered=true,payloadIntact=true;
    rtc::binary bulkFrame(1204,std::byte{0x5a});
    bulkFrame[0]=bulkFrame[1]=bulkFrame[2]=std::byte{0};bulkFrame[3]=std::byte{1};bulkFrame[4]=std::byte{0x65};
    std::atomic<bool> slowConsumer{false};
    int result = 1;
    {
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        auto controller = std::make_shared<rtc::PeerConnection>(config);
        auto host = std::make_shared<rtc::PeerConnection>(config);
        std::weak_ptr<rtc::PeerConnection> weakController = controller, weakHost = host;
        std::deque<rtc::Candidate> earlyCandidates, earlyAnswerCandidates;
        controller->onLocalDescription([weakHost, &earlyCandidates](rtc::Description sdp) {
            if (auto pc = weakHost.lock()) {
                // Deliberately deliver ICE before SDP, as the real signaling service may do.
                const auto queued = uu::applyRemoteSignal(
                    *pc,
                    {{"type", "candidate"},
                     {"candidate", "candidate:resolver-test 1 UDP 2122260223 localhost 9 typ host"},
                     {"sdpMid", "0"}},
                    earlyCandidates);
                if (queued != uu::RemoteSignal::CandidateQueued || pc->remoteDescription())
                    throw std::runtime_error("Candidate must wait for remote SDP");
                uu::applyRemoteSignal(*pc, {{"type", "offer"}, {"sdp", std::string(sdp)}}, earlyCandidates);
                if (!earlyCandidates.empty())
                    throw std::runtime_error("Candidates must drain after SDP");
            }
        });
        host->onLocalDescription([weakController, &earlyAnswerCandidates](rtc::Description sdp) {
            if (auto pc = weakController.lock()) {
                // Exercise the Switch's answering path and nested candidate format too.
                const auto queued = uu::applyRemoteSignal(
                    *pc,
                    {{"type", "candidate"},
                     {"candidate",
                      {{"candidate", "candidate:early-answer 1 UDP 2122260223 localhost 9 typ host"},
                       {"sdpMid", "0"}}}},
                    earlyAnswerCandidates);
                if (queued != uu::RemoteSignal::CandidateQueued || pc->remoteDescription())
                    throw std::runtime_error("Answer candidates must wait for remote SDP");
                uu::applyRemoteSignal(*pc, {{"type", "answer"}, {"sdp", std::string(sdp)}},
                                      earlyAnswerCandidates);
                if (!earlyAnswerCandidates.empty())
                    throw std::runtime_error("Answer candidates must drain after SDP");
            }
        });
        controller->onLocalCandidate([weakHost](rtc::Candidate candidate) {
            if (auto pc = weakHost.lock())
                pc->addRemoteCandidate(candidate);
        });
        host->onLocalCandidate([weakController](rtc::Candidate candidate) {
            if (auto pc = weakController.lock())
                pc->addRemoteCandidate(candidate);
        });
        rtc::Description::Video recv("0", rtc::Description::Direction::RecvOnly);
        recv.addH264Codec(96);
        auto receiver = controller->addTrack(recv);
        auto depacketizer =
            std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        receiver->setMediaHandler(depacketizer);
        receiver->onFrame([&](rtc::binary data, rtc::FrameInfo info) {
            // Deliberately slow the synchronous consumer to exercise a queued
            // authenticated-media burst without relying on machine speed.
            if(slowConsumer.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::lock_guard lock(mutex);
            ordered=ordered && info.timestamp>lastTimestamp;lastTimestamp=info.timestamp;++pictures;
            picture = data == rtc::binary{std::byte{0},    std::byte{0},    std::byte{0},   std::byte{1},
                                          std::byte{0x65}, std::byte{0x42}, std::byte{0x17}};
            payloadIntact=payloadIntact && (picture || data==bulkFrame);
            cv.notify_all();
        });
        rtc::Description::Video send("0", rtc::Description::Direction::SendOnly);
        send.addH264Codec(96);
        send.addSSRC(1234, "test");
        auto sender = host->addTrack(send);
        rtc::Description::Audio recvAudio("1", rtc::Description::Direction::RecvOnly);
        recvAudio.addOpusCodec(111);
        auto audioReceiver = controller->addTrack(recvAudio);
        audioReceiver->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
        audioReceiver->onFrame([&](rtc::binary data, rtc::FrameInfo) {
            std::lock_guard lock(mutex);
            audio = data == rtc::binary{std::byte{0xf8}, std::byte{0xff}, std::byte{0xfe}};
            cv.notify_all();
        });
        rtc::Description::Audio sendAudio("1", rtc::Description::Direction::SendOnly);
        sendAudio.addOpusCodec(111);
        sendAudio.addSSRC(1235, "audio-test");
        auto audioSender = host->addTrack(sendAudio);
        std::vector<std::shared_ptr<rtc::DataChannel>> remoteChannels;
        host->onDataChannel([&](std::shared_ptr<rtc::DataChannel> channel) {
            channel->onMessage(
                [&](rtc::binary data) {
                    std::string value(reinterpret_cast<const char *>(data.data()), data.size());
                    std::lock_guard lock(mutex);
                    input = value == "{\"action\":\"kbd_press\",\"key\":87}";
                    if(input)++inputs;
                    cv.notify_all();
                },
                nullptr);
            std::lock_guard lock(mutex);
            remoteChannels.push_back(std::move(channel));
        });
        auto control = controller->createDataChannel("CONTROL_DATA_CHANNEL");
        std::vector<std::shared_ptr<rtc::DataChannel>> channels{control};
        for (const char *label :
             {"TEXT_DATA_CHANNEL", "STREAMER_DATA_CHANNEL", "FILE_DATA_CHANNEL", "BINARY_DATA_CHANNEL"})
            channels.push_back(controller->createDataChannel(label));
        auto channelsOpen = [&] {
            return std::all_of(channels.begin(), channels.end(),
                               [](const auto &channel) { return channel->isOpen(); });
        };
        controller->setLocalDescription();
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < until &&
               (!channelsOpen() || !sender->isOpen() || !audioSender->isOpen()))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (channelsOpen() && sender->isOpen() && audioSender->isOpen()) {
            const std::string message = "{\"action\":\"kbd_press\",\"key\":87}";
            control->send(reinterpret_cast<const std::byte *>(message.data()), message.size());
            const uint8_t packet[] = {0x80, 0xe0, 0, 1, 0, 0, 0, 1, 0, 0, 4, 0xd2, 0x65, 0x42, 0x17};
            sender->send(reinterpret_cast<const std::byte *>(packet), sizeof(packet));
            const uint8_t opusPacket[] = {0x80, 0xef, 0, 1, 0, 0, 0, 1, 0, 0, 4, 0xd3, 0xf8, 0xff, 0xfe};
            audioSender->send(reinterpret_cast<const std::byte *>(opusPacket), sizeof(opusPacket));
            std::unique_lock lock(mutex);
            cv.wait_until(lock, until, [&] { return input && picture && audio; });
            if (input && picture && audio)
                result = 0;
            lock.unlock();
            if(result==0) {
                constexpr unsigned count=128;
                const auto before=uu::dtlsRxDiagnostics();
                slowConsumer=true;
                std::thread burst([&] {
                    for(unsigned i=2;i<count+2;++i) {
                        std::array<uint8_t,1212> rtp;rtp.fill(0x5a);
                        const uint8_t header[]={0x80,0xe0,uint8_t(i>>8),uint8_t(i),
                            uint8_t(i>>24),uint8_t(i>>16),uint8_t(i>>8),uint8_t(i),0,0,4,0xd2,0x65};
                        std::copy(std::begin(header),std::end(header),rtp.begin());
                        sender->send(reinterpret_cast<const std::byte *>(rtp.data()),rtp.size());
                    }
                });
                {
                    std::unique_lock ready(mutex);
                    cv.wait_until(ready,until,[&]{return pictures>=9;});
                }
                const auto sentAt=std::chrono::steady_clock::now();
                control->send(reinterpret_cast<const std::byte *>(message.data()),message.size());
                const auto controlUs=std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now()-sentAt).count();
                burst.join();
                {
                    std::unique_lock ready(mutex);
                    cv.wait_until(ready,until,[&]{return pictures>=count+1 && inputs>=2;});
                    if(pictures!=count+1 || inputs<2 || !ordered || !payloadIntact)result=1;
                }
                slowConsumer=false;
                const auto after=uu::dtlsRxDiagnostics();
#ifdef UU_RECEIVE_TIMESTAMPS
                if(after.timingEnabled && after.inputAgeSamples<before.inputAgeSamples+count)result=1;
#endif
#ifndef UU_MEDIA_DISPATCH
                if(after.timingEnabled && after.bounded && after.yields<=before.yields)result=1;
#endif
                if(!after.timingEnabled && (after.reads || after.sends || after.inputAgeSamples))result=1;
                std::cout<<"synthetic-burst frames="<<pictures<<" control-send-us="<<controlUs
                         <<" timing="<<after.timingEnabled<<" bounded="<<after.bounded
                         <<" yields="<<(after.yields-before.yields)<<'\n';
                // Fast-consumer large-packet burst: no artificial sleep and no
                // timing assertion. Check throughput can progress without loss.
                for(unsigned i=count+2;i<count+514;++i) {
                    std::array<uint8_t,1212> rtp;rtp.fill(0x5a);
                    const uint8_t header[]={0x80,0xe0,uint8_t(i>>8),uint8_t(i),
                        uint8_t(i>>24),uint8_t(i>>16),uint8_t(i>>8),uint8_t(i),0,0,4,0xd2,0x65};
                    std::copy(std::begin(header),std::end(header),rtp.begin());
                    sender->send(reinterpret_cast<const std::byte *>(rtp.data()),rtp.size());
                }
                {
                    std::unique_lock ready(mutex);
                    cv.wait_until(ready,until,[&]{return pictures>=count+513;});
                    if(pictures!=count+513 || !ordered || !payloadIntact)result=1;
                    std::cout<<"fast-burst frames="<<pictures<<" ordered="<<ordered
                             <<" payload-intact="<<payloadIntact<<'\n';
                }
            }
        }
        controller->resetCallbacks();
        host->resetCallbacks();
        receiver->resetCallbacks();
        audioReceiver->resetCallbacks();
        controller->close();
        host->close();
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (controller->state() != rtc::PeerConnection::State::Closed &&
               std::chrono::steady_clock::now() < closeDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (controller->state() != rtc::PeerConnection::State::Closed || !controller->remoteDescription())
            result = 1;
        // A closed peer retains SDP but releases ICE. Late signaling must neither
        // call into that missing transport nor keep stale queued candidates.
        earlyAnswerCandidates.emplace_back("candidate:late 1 UDP 2122260223 127.0.0.1 9 typ host", "0");
        try {
            for (const auto &signal : std::vector<uu::Json>{
                     {{"type", "candidate"},
                      {"candidate", "candidate:late 1 UDP 2122260223 127.0.0.1 9 typ host"}},
                     {{"type", "answer"}, {"sdp", "late SDP must not be parsed"}}}) {
                if (uu::applyRemoteSignal(*controller, signal, earlyAnswerCandidates) !=
                        uu::RemoteSignal::Closed ||
                    !earlyAnswerCandidates.empty())
                    result = 1;
            }
        } catch (...) {
            result = 1;
        }
    }
    rtc::Cleanup().wait();
    uu::waitBackgroundTasks();
#ifdef UU_TEST_NO_DETACH
    if (detachCalls != 0)
        result = 1;
#endif
    std::cout
        << (result
                ? "WebRTC loopback failed"
                : "WebRTC loopback passed: ICE, DTLS, five SCTP channels, binary input, SRTP H.264 and Opus")
        << '\n';
    return result;
}

int main() {
#ifdef UU_TEST_NO_DETACH
    // Prove the runtime models Horizon before exercising the library paths.
    std::thread probe([] {});
    bool unsupported = false;
    try {
        probe.detach();
    } catch (const std::system_error &error) {
        unsupported = error.code().value() == ENOSYS;
    }
    if (probe.joinable())
        probe.join();
    if (!unsupported)
        return 1;
    detachCalls = 0;
#endif
    for (int cycle = 0; cycle < 3; ++cycle)
        if (loopback())
            return 1;
    return 0;
}
