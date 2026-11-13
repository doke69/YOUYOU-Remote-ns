#pragma once
#include "video_repair_trace.hpp"
#include "video_supply_trace.hpp"
#include <rtc/mediahandler.hpp>

namespace uu {
// Decorate the existing chain; retain its packet pointers, ordering and sends.
class VideoRepairObserver final : public rtc::MediaHandler {
public:
    VideoRepairObserver(std::shared_ptr<rtc::MediaHandler> handler,std::shared_ptr<VideoRepairTrace> trace,uint64_t generation)
        :handler_(std::move(handler)),trace_(std::move(trace)),generation_(generation) {}
    void media(const rtc::Description::Media &description) override {handler_->mediaChain(description);}
    void incoming(rtc::message_vector &messages,const rtc::message_callback &send) override {
        if(!observedSend_) {
            observedSend_=[trace=trace_,generation=generation_,send](rtc::message_ptr message) {
                const auto token=trace->feedbackBegin(generation,reinterpret_cast<const uint8_t*>(message->data()),message->size(),VideoSupplyTrace::nowUs());
                const auto started=token?VideoSupplyTrace::nowUs():0;
                send(std::move(message));
                if(token)trace->feedbackEnd(generation,token,started,VideoSupplyTrace::nowUs());
            };
        }
        handler_->incomingChain(messages,observedSend_);
    }
    void outgoing(rtc::message_vector &messages,const rtc::message_callback &send) override {handler_->outgoingChain(messages,send);}
    bool requestKeyframe(const rtc::message_callback &send) override {return handler_->requestKeyframe(send);}
    bool requestBitrate(unsigned int bitrate,const rtc::message_callback &send) override {return handler_->requestBitrate(bitrate,send);}
private:
    std::shared_ptr<rtc::MediaHandler> handler_;
    std::shared_ptr<VideoRepairTrace> trace_;
    uint64_t generation_;
    rtc::message_callback observedSend_;
};
}
