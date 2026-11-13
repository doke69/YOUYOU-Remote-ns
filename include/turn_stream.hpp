#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace uu {
// In-process TURN datagram/stream framing adapter. Authentication and TURN
// allocation remain owned by libjuice. No external gateway is involved.
class TurnStreamBridge {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    TurnStreamBridge(std::string hostname,uint16_t port,bool tls);
    ~TurnStreamBridge();
    TurnStreamBridge(const TurnStreamBridge &)=delete;
    uint16_t localPort() const;
    bool failed() const;
    bool matchesRelay(const std::string &address,uint16_t port) const;
};
}
