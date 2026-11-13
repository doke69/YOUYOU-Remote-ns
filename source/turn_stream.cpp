#include "turn_stream.hpp"
#include "turn_relay_allocation.hpp"
#include <mutex>
#include "diagnostics.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef __SWITCH__
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <curl/curl.h>
#endif
namespace uu {
#ifdef __SWITCH__
namespace {
using Buffer=std::vector<uint8_t>;
constexpr size_t maxQueued=256*1024;
uint16_t read16(const uint8_t *p) {return uint16_t(p[0])<<8|p[1];}
struct WireFrame {size_t payload=0,wire=0;};
WireFrame framing(const uint8_t *data,size_t length) {
    if(length<4) return {};
    if((data[0]&0xc0)==0) {
        if(length<20) return {};
        if(data[4]!=0x21 || data[5]!=0x12 || data[6]!=0xa4 || data[7]!=0x42 || (read16(data+2)&3))
            throw std::runtime_error("Invalid TURN STUN framing");
        const size_t total=20+read16(data+2);return {total,total};
    }
    if((data[0]&0xc0)==0x40) {
        const size_t total=4+read16(data+2);return {total,(total+3)&~size_t(3)};
    }
    throw std::runtime_error("Invalid TURN stream record");
}
struct CurlConnection {
    CURL *handle=curl_easy_init();
    ~CurlConnection(){if(handle) curl_easy_cleanup(handle);}
};
}
struct TurnStreamBridge::Impl {
    int udp=-1;
    uint16_t boundPort=0;
    std::string host;
    uint16_t remotePort;
    bool tls;
    std::atomic<bool> stop{false},failure{false};
    std::thread worker;
    std::mutex relayMutex;
    std::optional<TurnRelayAllocation> relay;
    Impl(std::string hostname,uint16_t port,bool secure):host(std::move(hostname)),remotePort(port),tls(secure) {
        if(host.empty() || host.find_first_of("/?#@\r\n")!=std::string::npos || !port)
            throw std::runtime_error("Invalid TURN stream endpoint");
        udp=socket(AF_INET,SOCK_DGRAM,0);
        if(udp<0) throw std::runtime_error("TURN loopback socket unavailable");
        try {
            sockaddr_in local{};local.sin_len=sizeof(local);local.sin_family=AF_INET;
            local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(bind(udp,reinterpret_cast<sockaddr *>(&local),sizeof(local))<0)
                throw std::runtime_error("TURN loopback bind failed");
            socklen_t length=sizeof(local);
            if(getsockname(udp,reinterpret_cast<sockaddr *>(&local),&length)<0)
                throw std::runtime_error("TURN loopback address failed");
            boundPort=ntohs(local.sin_port);
            const int flags=fcntl(udp,F_GETFL,0);
            if(flags<0 || fcntl(udp,F_SETFL,flags|O_NONBLOCK)<0)
                throw std::runtime_error("TURN loopback nonblocking failed");
            worker=std::thread([this]{
                try{run();}catch(...){if(!stop){failure=true;diagnosticException("turn-stream-failed",std::current_exception());}}
            });
        } catch(...) {close(udp);udp=-1;throw;}
    }
    ~Impl(){stop=true;if(worker.joinable())worker.join();if(udp>=0)close(udp);}
    static int progress(void *context,curl_off_t,curl_off_t,curl_off_t,curl_off_t) {
        return static_cast<Impl *>(context)->stop.load()?1:0;
    }
    void run() {
        // Lazy connection: only allocate the server stream when ICE actually
        // sends a TURN request, not when constructing unused fallback options.
        CurlConnection connection;
        if(!connection.handle) throw std::runtime_error("TURN connection allocation failed");
        sockaddr_in client{};bool haveClient=false,connected=false;
        Buffer incoming;std::deque<Buffer> outgoing;size_t queued=0,offset=0;
        Buffer packetBuffer(65556);auto *buffer=packetBuffer.data();
        while(!stop) {
            bool progressed=false;
            for(int budget=0;budget<16;++budget) {
                sockaddr_in source{};socklen_t sourceLength=sizeof(source);
                const auto received=recvfrom(udp,buffer,packetBuffer.size(),0,reinterpret_cast<sockaddr *>(&source),&sourceLength);
                if(received<0) {
                    if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) break;
                    throw std::runtime_error("TURN local receive failed");
                }
                if(!received || source.sin_family!=AF_INET || source.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)) continue;
                if(!haveClient) {client=source;haveClient=true;}
                else if(client.sin_port!=source.sin_port || client.sin_addr.s_addr!=source.sin_addr.s_addr) continue;
                const auto frame=framing(buffer,size_t(received));
                if(!frame.payload || size_t(received)<frame.payload || size_t(received)>frame.wire)
                    throw std::runtime_error("Incomplete TURN datagram");
                if(queued+frame.wire>maxQueued || outgoing.size()>=256)
                    throw std::runtime_error("TURN outgoing backpressure limit");
                Buffer record(buffer,buffer+frame.payload);record.resize(frame.wire,0);
                queued+=record.size();outgoing.push_back(std::move(record));progressed=true;
            }
            if(haveClient && !connected) {
                std::string authority=host.find(':')==std::string::npos?host:"["+host+"]";
                const auto url=std::string(tls?"https://":"http://")+authority+":"+std::to_string(remotePort)+"/";
                auto *curl=connection.handle;
                curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
                curl_easy_setopt(curl,CURLOPT_CONNECT_ONLY,1L);
                curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,5000L);
                curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,10000L);
                curl_easy_setopt(curl,CURLOPT_PROXY,"");
                curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
                curl_easy_setopt(curl,CURLOPT_TCP_NODELAY,1L);
                curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
                curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
                curl_easy_setopt(curl,CURLOPT_SSL_ENABLE_ALPN,0L);
                curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
                curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);
                curl_easy_setopt(curl,CURLOPT_XFERINFODATA,this);
                curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
                const auto result=curl_easy_perform(curl);
                if(result!=CURLE_OK) {
                    diagnostic(("turn-stream-connect-error-"+std::to_string(int(result))).c_str());
                    throw std::runtime_error("TURN stream connect failed");
                }
                connected=true;diagnostic(tls?"turn-stream-tls-connected":"turn-stream-tcp-connected");
            }
            if(connected) {
                for(int budget=0;budget<16 && !outgoing.empty();++budget) {
                    auto &record=outgoing.front();size_t written=0;
                    const auto result=curl_easy_send(connection.handle,record.data()+offset,record.size()-offset,&written);
                    if(result==CURLE_AGAIN) break;
                    if(result!=CURLE_OK || !written) throw std::runtime_error("TURN stream write failed");
                    offset+=written;progressed=true;
                    if(offset==record.size()){queued-=record.size();outgoing.pop_front();offset=0;}
                }
                for(int budget=0;budget<16;++budget) {
                    size_t received=0;
                    const auto result=curl_easy_recv(connection.handle,buffer,packetBuffer.size(),&received);
                    if(result==CURLE_AGAIN) break;
                    if(result!=CURLE_OK || !received) throw std::runtime_error("TURN stream closed");
                    if(incoming.size()+received>maxQueued) throw std::runtime_error("TURN incoming backpressure limit");
                    incoming.insert(incoming.end(),buffer,buffer+received);progressed=true;
                    size_t used=0;
                    while(incoming.size()-used>=4) {
                        const auto frame=framing(incoming.data()+used,incoming.size()-used);
                        if(!frame.wire || incoming.size()-used<frame.wire) break;
                        // Observe allocation metadata only. libjuice still owns
                        // TURN authentication and selected-candidate validation.
                        if(auto allocation=readTurnRelayAllocation(incoming.data()+used,frame.payload)) {
                            std::lock_guard lock(relayMutex);relay=*allocation;
                        }
                        const auto sent=sendto(udp,incoming.data()+used,frame.payload,0,reinterpret_cast<sockaddr *>(&client),sizeof(client));
                        if(sent!=static_cast<ssize_t>(frame.payload)) throw std::runtime_error("TURN local delivery failed");
                        used+=frame.wire;
                    }
                    if(used) incoming.erase(incoming.begin(),incoming.begin()+used);
                }
            }
            if(!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};
#else
struct TurnStreamBridge::Impl {};
#endif
TurnStreamBridge::TurnStreamBridge(std::string host,uint16_t port,bool tls) {
#ifdef __SWITCH__
    impl_=std::make_unique<Impl>(std::move(host),port,tls);
#else
    (void)host;(void)port;(void)tls;throw std::runtime_error("TURN stream bridge is Switch-specific");
#endif
}
TurnStreamBridge::~TurnStreamBridge()=default;
uint16_t TurnStreamBridge::localPort() const {
#ifdef __SWITCH__
    return impl_->boundPort;
#else
    return 0;
#endif
}
bool TurnStreamBridge::matchesRelay(const std::string &address,uint16_t port) const {
#ifdef __SWITCH__
    TurnRelayAllocation candidate;candidate.port=port;
    if(inet_pton(AF_INET,address.c_str(),candidate.address.data())==1)candidate.family=1;
    else if(inet_pton(AF_INET6,address.c_str(),candidate.address.data())==1)candidate.family=2;
    else return false;
    std::lock_guard lock(impl_->relayMutex);
    return impl_->relay && *impl_->relay==candidate;
#else
    (void)address;(void)port;return false;
#endif
}
bool TurnStreamBridge::failed() const {
#ifdef __SWITCH__
    return impl_->failure.load();
#else
    return true;
#endif
}
}
