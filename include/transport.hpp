#pragma once
#include "protocol.hpp"
#include <atomic>
#include <curl/curl.h>
#include <deque>
#include <exception>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace uu {
class Api {
    std::string key_, client_, device_, token_, user_, nickname_;
    bool sessionSaved_=false;
    std::atomic<bool> &cancel_;

  public:
    Api(std::string key, std::string client, std::atomic<bool> &cancel)
        : key_(std::move(key)), client_(std::move(client)), cancel_(cancel) {}
    Json request(const std::string &method, const std::string &path, const Json &data = nullptr,
                 int timeout = 20000);
    void initialize();
    void login(const Json &result);
    void logout();
    bool restoreSession();
    bool saveSession();
    bool sessionSaved()const{return sessionSaved_;}
    const std::string &nickname()const{return nickname_;}
    bool authenticated() const {
        return !token_.empty();
    }
    const std::string &device() const {
        return device_;
    }
};
class SignalSocket {
    CURL *curl_ = nullptr;
    std::atomic<bool> &cancel_;
    WsParser parser_;
    std::deque<WsMessage> received_;
    void write(const Bytes &data);
    bool receiveNow(WsMessage &message);
    void enqueue(Bytes data);
    void runIo();
    std::atomic<bool> ioStop_{false};
    std::thread ioWorker_;
    std::mutex ioMutex_;
    std::condition_variable ioReady_;
    std::deque<Bytes> outgoing_;
    std::deque<WsMessage> incoming_;
    size_t outgoingBytes_=0,incomingBytes_=0;
    std::exception_ptr ioError_;

  public:
    struct IoStats {
        bool enabled=false;
        uint64_t reads=0,readMaxUs=0,writes=0,writeMaxUs=0;
        size_t outgoing=0,incoming=0;
    };
  private:
    IoStats ioStats_;
  public:
    IoStats ioStats();
    explicit SignalSocket(std::atomic<bool> &cancel) : cancel_(cancel) {}
    ~SignalSocket();
    void open(const std::string &url, const std::string &token);
    void send(const std::string &text);
    void sendBinary(const Bytes &bytes);
    bool receive(WsMessage &message);
    void close();
};
} // namespace uu
