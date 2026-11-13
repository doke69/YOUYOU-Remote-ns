#include "video_packet.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>

#define CHECK(x) do {if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";std::exit(1);}}while(0)
static std::atomic<void *> watched{nullptr};
static std::atomic<unsigned> frees{0};
void *operator new(size_t n){if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void *p) noexcept {if(p && p==watched.load())++frees;std::free(p);}
void operator delete(void *p,size_t) noexcept {::operator delete(p);}
#ifdef UU_PACKET_FAULT_TEST
static int fault=0;
extern "C" {
AVPacket *__real_av_packet_alloc();
AVBufferRef *__real_av_buffer_create(uint8_t *,size_t,void (*)(void *,uint8_t *),void *,int);
int __real_av_new_packet(AVPacket *,int);
AVPacket *__wrap_av_packet_alloc(){return fault==1?nullptr:__real_av_packet_alloc();}
AVBufferRef *__wrap_av_buffer_create(uint8_t *p,size_t n,void (*f)(void *,uint8_t *),void *o,int flags){return fault==2?nullptr:__real_av_buffer_create(p,n,f,o,flags);}
int __wrap_av_new_packet(AVPacket *p,int n){return fault==3?-12:__real_av_new_packet(p,n);}
}
#endif
int main() {
    unsigned tests=0;
    for(size_t size:{size_t(1),size_t(3),size_t(4096),size_t(32337),size_t(90470),size_t(115907),size_t(350000),size_t(4194304)}) {
        std::vector<uint8_t> data;data.reserve(size+64);data.resize(size);
        for(size_t i=0;i<size;++i)data[i]=uint8_t(i*17+3);
        const auto expected=data;watched=data.data();frees=0;
        auto owned=uu::videoPacket(std::move(data));CHECK(owned && owned->data==watched && size_t(owned->size)==size);
        CHECK(std::memcmp(owned->data,expected.data(),size)==0);
        CHECK(std::all_of(owned->data+size,owned->data+size+64,[](uint8_t b){return b==0;}));
        auto *decoder=av_packet_alloc(),*retained=av_packet_alloc();CHECK(decoder && retained);
        av_packet_move_ref(decoder,owned.get());CHECK(!owned->buf && !owned->data);owned.reset();CHECK(frees==0);
        CHECK(av_packet_ref(retained,decoder)==0 && retained->data==watched);
        av_packet_free(&decoder);CHECK(frees==0);
        std::thread consume([&]{CHECK(std::memcmp(retained->data,expected.data(),size)==0);av_packet_free(&retained);});
        consume.join();CHECK(frees==1);watched=nullptr;++tests;
    }
    // No headroom, excessive capacity, and exact capacity-ratio boundary.
    for(size_t capacity:{size_t(4096),size_t(8192),size_t(4672),size_t(4673)}) {
        std::vector<uint8_t> data;data.reserve(capacity);data.resize(4096,42);
        watched=data.data();frees=0;auto packet=uu::videoPacket(std::move(data));CHECK(packet && packet->size==4096);
        const bool direct=capacity==4672;
        CHECK((packet->data==watched)==direct && frees==(direct?0u:1u));
        CHECK(std::all_of(packet->data,packet->data+4096,[](uint8_t b){return b==42;}));
        CHECK(std::all_of(packet->data+4096,packet->data+4160,[](uint8_t b){return b==0;}));
        packet.reset();CHECK(frees==1);watched=nullptr;++tests;
    }
    {
        std::vector<uu::VideoPacket> queue;
        for(int i=0;i<32;++i){std::vector<uint8_t> d;d.reserve(1024+64);d.resize(1024,uint8_t(i));queue.push_back(uu::videoPacket(std::move(d)));CHECK(queue.back());}
        auto *inflight=av_packet_alloc();CHECK(inflight && av_packet_ref(inflight,queue.front().get())==0);
        watched=queue.front()->data;frees=0;queue.clear();CHECK(frees==0 && inflight->data[100]==0);
        av_packet_free(&inflight);CHECK(frees==1);watched=nullptr;++tests;
    }
    CHECK(!uu::videoPacket({}));++tests;
    {std::vector<uint8_t> d(4194305);watched=d.data();frees=0;CHECK(!uu::videoPacket(std::move(d)));CHECK(frees==1);watched=nullptr;++tests;}
#ifdef UU_PACKET_FAULT_TEST
    for(int mode:{1,2,3}) {
        std::vector<uint8_t> d;d.reserve(mode==3?8192:4160);d.resize(4096,7);
        watched=d.data();frees=0;fault=mode;auto packet=uu::videoPacket(std::move(d));fault=0;
        CHECK(!packet && frees==1);watched=nullptr;++tests;
    }
#endif
    std::cout<<"PASS video packet ownership: "<<tests<<" cases, payload/padding/ref/move/async/clear/failure/bounds\n";
}
