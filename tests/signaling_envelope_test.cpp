#include "signaling_envelope.hpp"
#include <iostream>
using namespace uu;
int main() {
    int failed = 0;
    auto check = [&](Json packet, Json session, SoacStatus expected) {
        if (inspectSoac(packet, session) != expected) ++failed;
    };
    Json answer = {{"data", {{"ice_id", 7}, {"type", "answer"}, {"sdp", "synthetic"}}}};
    check(answer, "7", SoacStatus::Accepted);
    check(answer, 8, SoacStatus::SessionMismatch);
    check(Json::array(), 7, SoacStatus::InvalidEnvelope);
    check(Json::object(), 7, SoacStatus::InvalidEnvelope);
    check({{"data", "bad"}}, 7, SoacStatus::InvalidEnvelope);
    auto bad = answer; bad["data"]["ice_id"] = Json::object();
    check(bad, 7, SoacStatus::InvalidPayload);
    bad = answer; bad["data"]["sdp"] = ""; check(bad, 7, SoacStatus::InvalidPayload);
    bad = answer; bad["data"]["type"] = 1; check(bad, 7, SoacStatus::InvalidPayload);
    bad = answer; bad["data"]["type"] = "unknown"; check(bad, 7, SoacStatus::UnknownType);
    bad = answer; bad["data"].erase("ice_id"); check(bad, 7, SoacStatus::Accepted);
    for (const auto *type : {"offer", "restart_ice"}) {
        bad = answer; bad["data"]["type"] = type; check(bad, 7, SoacStatus::Accepted);
    }
    Json candidate = {{"data", {{"type", "candidate"}, {"candidate", ""}}}};
    check(candidate, 7, SoacStatus::Accepted); // end-of-candidates remains legal
    candidate["data"]["candidate"] = {{"candidate", "synthetic"}, {"sdpMid", "0"}};
    check(candidate, 7, SoacStatus::Accepted);
    candidate["data"]["candidate"] = {{"sdpMid", "0"}};
    check(candidate, 7, SoacStatus::InvalidPayload);
    candidate["data"]["candidate"] = false;
    check(candidate, 7, SoacStatus::InvalidPayload);
    std::cout << "signaling envelope failures=" << failed << '\n';
    return failed ? 1 : 0;
}
