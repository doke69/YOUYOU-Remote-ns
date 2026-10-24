#include "transport_feedback_negotiation.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace uu::twcc {
namespace {
using Media = rtc::Description::Media;
using Direction = rtc::Description::Direction;
constexpr const char *Uri = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
bool feedbackProfile(const std::string &protocol) {
    return protocol == "RTP/AVPF" || protocol == "RTP/SAVPF" ||
           protocol == "UDP/TLS/RTP/SAVPF" || protocol == "TCP/TLS/RTP/SAVPF";
}
struct ExtensionLine {
    Media::ExtMap map;
    std::string line;
};
struct CapabilitySection {
    std::string mediaLine;
    std::string mid;
    std::string direction;
    std::string attributes;
    std::vector<ExtensionLine> extensions;
    bool rtcpMux = false;
};
bool starts(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}
bool isDirection(std::string_view line) {
    return line == "a=sendrecv" || line == "a=sendonly" ||
           line == "a=recvonly" || line == "a=inactive";
}
// Small capability projection only. Unknown lines are never copied or logged.
// Deliberately rejects extended negotiation-only IDs and duplicate definitions;
// this is a final usable mapping inspector, not an offer negotiation engine.
bool projectCapabilities(std::string_view raw, std::string &projection, bool &singleTransport) {
    singleTransport = false;
    if (raw.empty() || raw.size() > 256 * 1024) return false;
    CapabilitySection session;
    std::vector<CapabilitySection> sections;
    CapabilitySection *current = &session;
    bool mediaExtensions = false;
    std::vector<std::string> bundleMids;
    bool haveBundle = false;
    while (!raw.empty()) {
        const auto end = raw.find('\n');
        auto line = raw.substr(0, end);
        raw = end == std::string_view::npos ? std::string_view{} : raw.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > 4096 || line.find('\r') != std::string_view::npos ||
            line.find('\0') != std::string_view::npos) return false;
        if (starts(line, "a=group:BUNDLE") &&
            (line.size() == 14 || line[14] == ' ' || line[14] == '\t')) {
            if (current != &session || haveBundle) return false;
            haveBundle = true;
            auto mids = line.substr(14);
            while (!mids.empty()) {
                const auto first = mids.find_first_not_of(" \t");
                if (first == std::string_view::npos) break;
                mids.remove_prefix(first);
                const auto endMid = mids.find_first_of(" \t");
                const auto mid = mids.substr(0, endMid);
                if (bundleMids.size() >= 16 ||
                    std::find(bundleMids.begin(), bundleMids.end(), mid) != bundleMids.end()) return false;
                bundleMids.emplace_back(mid);
                if (endMid == std::string_view::npos) break;
                mids.remove_prefix(endMid);
            }
            if (bundleMids.empty()) return false;
        } else if (line == "a=rtcp-mux") {
            if (current == &session) return false;
            current->rtcpMux = true;
        } else if (starts(line, "m=")) {
            if (sections.size() >= 16 || line.size() < 3) return false;
            sections.emplace_back();
            current = &sections.back();
            current->mediaLine = std::string(line) + "\r\n";
        } else if (starts(line, "a=mid:")) {
            if (current == &session || !current->mid.empty() || line.size() <= 6) return false;
            const auto mid = line.substr(6);
            if (mid.find_first_of(" \t") != std::string_view::npos) return false;
            current->mid = std::string(mid);
        } else if (isDirection(line)) {
            if (!current->direction.empty()) return false;
            current->direction = std::string(line) + "\r\n";
        } else if (starts(line, "a=extmap:")) {
            if (current->extensions.size() >= 64) return false;
            const auto value = line.substr(9);
            const auto split = value.find_first_of("/ ");
            if (split == std::string_view::npos || split == 0 || split > 3) return false;
            unsigned id = 0;
            for (const char c : value.substr(0, split)) {
                if (c < '0' || c > '9') return false;
                id = id * 10 + unsigned(c - '0');
            }
            if (!id || id > 255) return false;
            Media::ExtMap map(value);
            if (map.id != int(id) || map.uri.empty() ||
                map.uri.find_first_of(" \t") != std::string::npos) return false;
            for (const auto &existing : current->extensions) {
                if (existing.map.id == map.id || (existing.map.uri == map.uri &&
                    existing.map.attributes == map.attributes)) return false;
            }
            current->extensions.push_back({std::move(map), std::string(line) + "\r\n"});
            if (current != &session) mediaExtensions = true;
        } else if (starts(line, "a=rtpmap:") || starts(line, "a=rtcp-fb:") ||
                   line == "a=bundle-only") {
            if (current == &session) return false;
            current->attributes.append(line.data(), line.size());
            current->attributes += "\r\n";
        }
    }
    if (sections.empty() || (!session.extensions.empty() && mediaExtensions)) return false;
    projection = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n";
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto &section = sections[i];
        if (section.mid.empty()) return false;
        for (size_t j = 0; j < i; ++j) if (sections[j].mid == section.mid) return false;
        projection += section.mediaLine + "a=mid:" + section.mid + "\r\n";
        projection += section.direction.empty() ? session.direction : section.direction;
        projection += section.attributes;
        for (const auto &extension : session.extensions.empty() ? section.extensions : session.extensions)
            projection += extension.line;
    }
    // Check the ORIGINAL transport shape before libdatachannel's serializer
    // synthesizes a BUNDLE group and rtcp-mux attributes for its single transport.
    for (const auto &mid : bundleMids) {
        if (std::none_of(sections.begin(), sections.end(),
            [&mid](const CapabilitySection &section) { return section.mid == mid; })) return false;
    }
    const rtc::Description shape(projection);
    unsigned active = 0;
    bool allBundled = true, allMux = true, tagActive = false, tagMux = false;
    for (int i = 0; i < shape.mediaCount(); ++i) {
        const auto entry = shape.media(i);
        const auto *base = std::visit([](const auto *value) -> const rtc::Description::Entry * {
            return value;
        }, entry);
        if (base->isRemoved()) continue;
        ++active;
        const auto &section = sections[size_t(i)];
        if (std::find(bundleMids.begin(), bundleMids.end(), section.mid) == bundleMids.end()) allBundled = false;
        if (haveBundle && section.mid == bundleMids.front()) {
            tagActive = true; tagMux = section.rtcpMux;
        }
        if ((base->type() == "audio" || base->type() == "video") && !section.rtcpMux) allMux = false;
    }
    singleTransport = active > 0 &&
        (haveBundle ? allBundled && tagActive : active == 1) &&
        (allMux || (haveBundle && tagMux));
    return true;
}
bool receives(Direction d) {
    return d == Direction::RecvOnly || d == Direction::SendRecv || d == Direction::Unknown;
}
bool sends(Direction d) {
    return d == Direction::SendOnly || d == Direction::SendRecv || d == Direction::Unknown;
}
bool feedback(const Media &media, const Media::RtpMap *map) {
    if (map && std::find(map->rtcpFbs.begin(), map->rtcpFbs.end(), "transport-cc") != map->rtcpFbs.end()) return true;
    const auto attributes = media.attributes();
    return std::find(attributes.begin(), attributes.end(), "rtcp-fb:* transport-cc") != attributes.end();
}
bool sameFormat(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
const Media *mediaAt(const rtc::Description &d, int i) {
    const auto entry = d.media(i);
    const auto ptr = std::get_if<const Media *>(&entry);
    return ptr ? *ptr : nullptr;
}
const Media *findMid(const rtc::Description &d, const std::string &mid) {
    const Media *found = nullptr;
    for (int i = 0; i < d.mediaCount(); ++i) {
        const auto *m = mediaAt(d, i);
        if (!m || m->mid() != mid) continue;
        if (found) return nullptr;
        found = m;
    }
    return found;
}
bool ambiguous(const rtc::Description &d) {
    if (d.mediaCount() > 16) return true;
    for (int i = 0; i < d.mediaCount(); ++i) {
        const auto *a = mediaAt(d, i);
        if (!a) continue;
        if (a->mid().empty()) return true;
        for (int j = i + 1; j < d.mediaCount(); ++j) {
            const auto *b = mediaAt(d, j);
            if (b && a->mid() == b->mid()) return true;
        }
    }
    return false;
}
} // namespace

NegotiationResult inspectNegotiation(const rtc::Description &local, const rtc::Description &remote,
                                    int primaryVideoPayloadType) {
    using Type = rtc::Description::Type;
    if (!((local.type() == Type::Offer && remote.type() == Type::Answer) ||
          (local.type() == Type::Answer && remote.type() == Type::Offer))) return {};
    if (primaryVideoPayloadType < 0 || primaryVideoPayloadType > 127) return {};
    if (ambiguous(local) || ambiguous(remote)) return {NegotiationStatus::AmbiguousMedia, 0, 0};
    int commonId = 0;
    unsigned videos = 0;
    bool anyReceiveVideo = false;
    for (int i = 0; i < remote.mediaCount(); ++i) {
        const auto *r = mediaAt(remote, i);
        if (!r || r->isRemoved() || (r->type() != "video" && r->type() != "audio")) continue;
        const auto *l = findMid(local, r->mid());
        if (!l || l->isRemoved() || !receives(l->direction()) || !sends(r->direction())) continue;
        if (l->type() != r->type()) return {NegotiationStatus::AmbiguousMedia, 0, 0};
        if (!feedbackProfile(l->protocol()) || !feedbackProfile(r->protocol())) return {};
        const bool video = r->type() == "video";
        if (video) anyReceiveVideo = true;
        int id = 0;
        for (int candidate = 1; candidate <= 255; ++candidate) {
            const auto *map = r->extMap(candidate);
            if (!map) continue;
            if (map->uri == "http://www.webrtc.org/experiments/rtp-hdrext/transport-wide-cc-02")
                return {NegotiationStatus::UnsupportedExtension, 0, 0};
            if (map->uri != Uri) continue;
            if (id || !map->attributes.empty()) return {NegotiationStatus::UnsupportedExtension, 0, 0};
            id = candidate;
        }
        if (!id) {
            if (video) return {};
            continue;
        }
        const auto *rmap = r->extMap(id), *lmap = l->extMap(id);
        if (!lmap || lmap->uri != Uri || !lmap->attributes.empty() ||
            !receives(lmap->direction) || !sends(rmap->direction)) {
            // A global observer must not treat an unnegotiated audio extension
            // as part of the negotiated video transport sequence space.
            return {};
        }
        if (commonId && commonId != id) return {NegotiationStatus::InconsistentId, 0, 0};
        commonId = id;
        unsigned compatible = 0;
        for (const int pt : r->payloadTypes()) {
            if (video && pt != primaryVideoPayloadType) continue;
            const auto *rm = r->rtpMap(pt), *lm = l->rtpMap(pt);
            if (!rm || !lm || rm->format.empty() || lm->format.empty() ||
                !sameFormat(rm->format, lm->format) || rm->clockRate != lm->clockRate) continue;
            if (feedback(*r, rm) && feedback(*l, lm)) ++compatible;
        }
        if (video && !compatible) return {};
        if (video) videos += compatible;
    }
    if (!anyReceiveVideo || !commonId || !videos) return {};
    // A single observer sees all RTP on the bundled peer. An ID that means a
    // different extension on any active media cannot be read as TWCC globally.
    for (const auto *description : {&local, &remote}) {
        for (int i = 0; i < description->mediaCount(); ++i) {
            const auto *m = mediaAt(*description, i);
            if (!m || m->isRemoved()) continue;
            if (const auto *map = m->extMap(commonId); map && map->uri != Uri)
                return {NegotiationStatus::InconsistentId, 0, 0};
        }
    }
    return {NegotiationStatus::Ready, uint8_t(commonId), videos};
}

std::optional<NegotiationCapabilities> NegotiationCapabilities::fromSdp(
    std::string_view sdp, rtc::Description::Type type) {
    if (type != rtc::Description::Type::Offer && type != rtc::Description::Type::Answer)
        return std::nullopt;
    try {
        std::string projection;
        bool singleTransport = false;
        if (!projectCapabilities(sdp, projection, singleTransport)) return std::nullopt;
        // Canonicalize ONLY after duplicate checks and removal of sensitive
        // fields. The synthetic origin is fixed, no credentials are supplied.
        const rtc::Description sanitized(projection, type);
        return NegotiationCapabilities(sanitized.generateSdp(), type, singleTransport);
    } catch (const std::invalid_argument &) { return std::nullopt; }
      catch (const std::out_of_range &) { return std::nullopt; }
}
NegotiationResult inspectNegotiationCapabilities(const NegotiationCapabilities &localCaps,
                                                 const NegotiationCapabilities &remoteCaps,
                                                 int primaryVideoPayloadType) {
    try {
        if (!localCaps.singleTransport_ || !remoteCaps.singleTransport_)
            return {NegotiationStatus::UnsupportedTransport, 0, 0};
        const rtc::Description local(localCaps.projection_, localCaps.type_);
        const rtc::Description remote(remoteCaps.projection_, remoteCaps.type_);
        return inspectNegotiation(local, remote, primaryVideoPayloadType);
    } catch (const std::invalid_argument &) {
        // Parser errors can contain their input. Do not propagate/log them.
        return {NegotiationStatus::InvalidDescription, 0, 0};
    } catch (const std::out_of_range &) {
        return {NegotiationStatus::InvalidDescription, 0, 0};
    }
}
NegotiationResult inspectNegotiationSdp(std::string_view localSdp,
                                       rtc::Description::Type localType,
                                       std::string_view remoteSdp,
                                       rtc::Description::Type remoteType,
                                       int primaryVideoPayloadType) {
    const auto local = NegotiationCapabilities::fromSdp(localSdp, localType);
    const auto remote = NegotiationCapabilities::fromSdp(remoteSdp, remoteType);
    if (!local || !remote) return {NegotiationStatus::InvalidDescription, 0, 0};
    return inspectNegotiationCapabilities(*local, *remote, primaryVideoPayloadType);
}
} // namespace uu::twcc
