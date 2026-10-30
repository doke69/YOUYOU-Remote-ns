#include "rtc_diagnostics.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

void check(bool pass) {
    if (!pass)
        throw std::runtime_error("RTC diagnostic regression");
}
int main() {
    try {
        check(uu::rtcLogEvent("doRecv@600: DTLS recv: Handshake failed [code=-30592]: private-token") ==
              "rtc-dtls-handshake-error-code--30592");
        check(
            uu::rtcLogEvent("initSctpTransport@320: Could not bind usrsctp socket, errno=22 private-token") ==
            "rtc-sctp-bind-error-code-22");
        check(uu::rtcLogEvent("SCTP connected") == "rtc-sctp-connected");
        check(uu::rtcLogEvent("信令连接已断开") == "failure-signal-read");
        check(uu::rtcLogEvent("远控权限已释放或电脑已退出房间") == "failure-control-released");
        check(uu::rtcLogEvent("Signaling write timeout private-token") == "failure-signal-write-timeout");
        check(uu::rtcLogEvent("juice: Using TURN server private-host private-token").empty());
        check(uu::rtcLogEvent("SDP private-token: SCTP connected").empty());
        check(uu::rtcLogEvent("private-token").empty());
        check(uu::rtcLogEvent("juice: UU_DIAG_ROLE_CONTROLLING") == "ice-role-controlling");
        check(uu::rtcLogEvent("juice: UU_DIAG_TURN_ALLOCATED") == "turn-allocated");
        check(uu::rtcLogEvent("juice: UU_DIAG_TURN_ERROR 403") == "turn-response-error-403");
        check(uu::rtcLogEvent("juice: UU_DIAG_TURN_ERROR 403 private-token").empty());
        check(uu::rtcLogEvent("juice: agent.c:241: UU_DIAG_ROLE_CONTROLLING") == "ice-role-controlling");
        check(uu::rtcLogEvent("juice: UU_DIAG_PAIR_CREATED 3 0 2") == "ice-pair-created-id-3-local-0-remote-2");
        check(uu::rtcLogEvent("juice: UU_DIAG_PAIR_SUCCEEDED 3") == "ice-pair-succeeded-id-3");
        check(uu::rtcLogEvent("juice: UU_DIAG_PAIR_SUCCEEDED 3 private-token").empty());
        check(uu::rtcLogEvent("UU_DIAG_DTLS_RX_DELIVERED") == "rtc-dtls-first-rx-delivered");
        check(uu::rtcLogEvent("Handshake failed [code=1234567890] private-token") ==
              "rtc-dtls-handshake-error");
        std::array<uint8_t, 32> packet{};
        check(uu::sctpHandshakeEvent(true, nullptr, 0).empty());
        check(uu::sctpHandshakeEvent(true, packet.data(), 15).empty());
        packet[12] = 1;
        check(uu::sctpHandshakeEvent(true, packet.data(), packet.size()) == "rtc-sctp-tx-init");
        packet[12] = 2;
        check(uu::sctpHandshakeEvent(false, packet.data(), packet.size()) == "rtc-sctp-rx-init-ack");
        packet[12] = 0;
        check(uu::sctpHandshakeEvent(false, packet.data(), packet.size()).empty());
        std::cout << "RTC diagnostics retain stages and numeric codes, discard payloads.\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
