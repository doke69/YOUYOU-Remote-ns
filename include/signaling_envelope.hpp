#pragma once
#include "protocol.hpp"

namespace uu {
// The network boundary accepts only the established UU envelope. Never guess
// alternate nesting or accept a different session to make a connection succeed.
enum class SoacStatus { Accepted, InvalidEnvelope, SessionMismatch, UnknownType, InvalidPayload };
inline SoacStatus inspectSoac(const Json &packet, const Json &session) {
    if (!packet.is_object()) return SoacStatus::InvalidEnvelope;
    const auto payload = packet.find("data");
    if (payload == packet.end() || !payload->is_object()) return SoacStatus::InvalidEnvelope;
    const auto id = payload->find("ice_id");
    if (id != payload->end()) {
        if (!id->is_string() && !id->is_number_integer()) return SoacStatus::InvalidPayload;
        const auto text = id->is_string() ? id->get<std::string>() : id->dump();
        const auto expected = session.is_string() ? session.get<std::string>() : session.dump();
        if (text != expected) return SoacStatus::SessionMismatch;
    }
    const auto type = payload->find("type");
    if (type == payload->end() || !type->is_string()) return SoacStatus::InvalidPayload;
    const auto &name = type->get_ref<const std::string &>();
    if (name == "answer" || name == "offer" || name == "restart_ice") {
        const auto sdp = payload->find("sdp");
        return sdp != payload->end() && sdp->is_string() && !sdp->get_ref<const std::string &>().empty()
            ? SoacStatus::Accepted : SoacStatus::InvalidPayload;
    }
    if (name == "candidate") {
        const auto candidate = payload->find("candidate");
        if (candidate == payload->end()) return SoacStatus::InvalidPayload;
        if (candidate->is_string()) return SoacStatus::Accepted;
        if (candidate->is_object()) {
            const auto text = candidate->find("candidate");
            if (text != candidate->end() && text->is_string()) return SoacStatus::Accepted;
        }
        return SoacStatus::InvalidPayload;
    }
    return SoacStatus::UnknownType;
}
} // namespace uu
