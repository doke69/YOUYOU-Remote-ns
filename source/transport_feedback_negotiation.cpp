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
constexpr const char *Uri2 = "http://www.webrtc.org/experiments/rtp-hdrext/transport-wide-cc-02";
constexpr const char *AbsSendTimeUri = "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";
constexpr const char *TransmissionOffsetUri = "urn:ietf:params:rtp-hdrext:toffset";
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
bool projectCapabilities(std::string_view raw, std::string &projection, bool &singleTransport,
                         bool installedSdkView = false) {
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
            if (current != &session) return false;
            std::vector<std::string> parsedMids;
            auto mids = line.substr(14);
            while (!mids.empty()) {
                const auto first = mids.find_first_not_of(" \t");
                if (first == std::string_view::npos) break;
                mids.remove_prefix(first);
                const auto endMid = mids.find_first_of(" \t");
                const auto mid = mids.substr(0, endMid);
                if (parsedMids.size() >= 16 ||
                    std::find(parsedMids.begin(), parsedMids.end(), mid) != parsedMids.end()) return false;
                parsedMids.emplace_back(mid);
                if (endMid == std::string_view::npos) break;
                mids.remove_prefix(endMid);
            }
            if (parsedMids.empty()) return false;
            if (haveBundle) {
                if (!installedSdkView || parsedMids != bundleMids) return false;
            } else {
                bundleMids = std::move(parsedMids);
                haveBundle = true;
            }
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
        } else if (starts(line, "a=rtpmap:") || starts(line, "a=fmtp:") || starts(line, "a=rtcp-fb:") ||
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
std::vector<int> extensionIds(const rtc::Description::Entry &media) {
    // This SDK exposes extIds only as non-const, while extMap throws for an
    // absent ID. Copy the small sanitized entry instead of probing 1..255 or
    // casting away const. This runs only at description changes, never on RTP.
    auto view = media;
    return view.extIds();
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
    int commonLegacyId = 0;
    TransportSequenceVersion commonVersion = TransportSequenceVersion::V1;
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
        int id1 = 0, id2 = 0;
        const auto localIds = extensionIds(*l);
        for (const int candidate : extensionIds(*r)) {
            const auto *map = r->extMap(candidate);
            if (!map || (map->uri != Uri && map->uri != Uri2)) continue;
            if (std::find(localIds.begin(), localIds.end(), candidate) == localIds.end()) continue;
            const auto *localMap = l->extMap(candidate);
            if (localMap->uri != map->uri) continue;
            auto &selected = map->uri == Uri2 ? id2 : id1;
            if (selected || !map->attributes.empty() || !localMap->attributes.empty())
                return {NegotiationStatus::UnsupportedExtension, 0, 0};
            if (!receives(localMap->direction) || !sends(map->direction)) return {};
            selected = candidate;
        }
        // TV Answer selection prefers 02; its packet parser tries 02 before
        // 01. Retain a genuinely negotiated 01 mapping as a packet fallback.
        const int id = id2 ? id2 : id1;
        const int legacyId = id2 ? id1 : 0;
        const auto version = id2 ? TransportSequenceVersion::V2 : TransportSequenceVersion::V1;
        if (!id) {
            if (video) return {};
            continue;
        }
        if (commonId && (commonId != id || commonVersion != version || commonLegacyId != legacyId))
            return {NegotiationStatus::InconsistentId, 0, 0};
        commonId = id;
        commonLegacyId = legacyId;
        commonVersion = version;
        unsigned compatible = 0;
        for (const int pt : r->payloadTypes()) {
            if (video && pt != primaryVideoPayloadType) continue;
            if (!l->hasPayloadType(pt)) continue;
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
            const auto ids = extensionIds(*m);
            if (std::find(ids.begin(), ids.end(), commonId) != ids.end() &&
                m->extMap(commonId)->uri != (commonVersion == TransportSequenceVersion::V2 ? Uri2 : Uri))
                return {NegotiationStatus::InconsistentId, 0, 0};
            if (commonLegacyId && std::find(ids.begin(), ids.end(), commonLegacyId) != ids.end() &&
                m->extMap(commonLegacyId)->uri != Uri)
                return {NegotiationStatus::InconsistentId, 0, 0};
        }
    }
    return {NegotiationStatus::Ready, uint8_t(commonId), videos, commonVersion, uint8_t(commonLegacyId)};
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
bool NegotiationCapabilities::matchesInstalledDescription(const rtc::Description &installed) const {
    if (installed.type() != type_) return false;
    std::string projection;
    bool sdkTransportShape = false;
    if (!projectCapabilities(std::string(installed), projection, sdkTransportShape, true)) return false;
    // Preserve singleTransport_ from the original wire snapshot. Generated
    // rtcp-mux/BUNDLE lines must never create transport consent retroactively.
    const rtc::Description sanitized(projection, type_);
    return projection_ == sanitized.generateSdp();
}
bool FeedbackCapabilitiesResult::allowsRemb(std::string_view videoMid, int primaryVideoPayloadType) const {
    if (status != NegotiationStatus::Ready) return false;
    return std::any_of(media.begin(), media.end(), [&](const MediaFeedbackCapabilities &entry) {
        return entry.mid == videoMid && entry.payloadType == primaryVideoPayloadType &&
            std::find(entry.feedback.begin(), entry.feedback.end(), "goog-remb") != entry.feedback.end();
    });
}
FeedbackCapabilitiesResult inspectMediaFeedbackCapabilities(const NegotiationCapabilities &localCaps,
                                                             const NegotiationCapabilities &remoteCaps,
                                                             int primaryVideoPayloadType) {
    using Type = rtc::Description::Type;
    FeedbackCapabilitiesResult result;
    if (!((localCaps.type_ == Type::Offer && remoteCaps.type_ == Type::Answer) ||
          (localCaps.type_ == Type::Answer && remoteCaps.type_ == Type::Offer))) return result;
    if (primaryVideoPayloadType < 0 || primaryVideoPayloadType > 127) return result;
    result.localIsOfferer = localCaps.type_ == Type::Offer;
    if (!localCaps.singleTransport_ || !remoteCaps.singleTransport_) {
        result.status = NegotiationStatus::UnsupportedTransport;
        return result;
    }
    // Both inputs were validated and canonicalized by fromSdp. Do not parse
    // raw or credential-bearing descriptions here, or relax observer admission.
    const rtc::Description local(localCaps.projection_, localCaps.type_);
    const rtc::Description remote(remoteCaps.projection_, remoteCaps.type_);
    if (ambiguous(local) || ambiguous(remote)) {
        result.status = NegotiationStatus::AmbiguousMedia;
        return result;
    }
    for (int i = 0; i < remote.mediaCount(); ++i) {
        const auto *r = mediaAt(remote, i);
        if (!r || r->isRemoved() || (r->type() != "video" && r->type() != "audio")) continue;
        const auto *l = findMid(local, r->mid());
        if (!l || l->isRemoved() || !receives(l->direction()) || !sends(r->direction())) continue;
        if (l->type() != r->type()) {
            result.status = NegotiationStatus::AmbiguousMedia;
            result.media.clear();
            return result;
        }
        if (!feedbackProfile(l->protocol()) || !feedbackProfile(r->protocol())) continue;
        for (const int pt : r->payloadTypes()) {
            if (r->type() == "video" && pt != primaryVideoPayloadType) continue;
            if (!l->hasPayloadType(pt)) continue;
            const auto *rm = r->rtpMap(pt), *lm = l->rtpMap(pt);
            if (!rm || !lm || rm->format.empty() || !sameFormat(rm->format, lm->format) ||
                rm->clockRate != lm->clockRate || sameFormat(rm->format, "rtx")) continue;
            MediaFeedbackCapabilities media;
            media.mid = r->mid(); media.codec = rm->format;
            media.payloadType = pt; media.clockRate = rm->clockRate;
            auto localFeedback = lm->rtcpFbs, remoteFeedback = rm->rtcpFbs;
            // Wildcard feedback applies to each codec in this media section.
            // Keep the complete name/parameter pair: nack != nack pli.
            for (const auto &attribute : l->attributes())
                if (starts(attribute, "rtcp-fb:* ")) localFeedback.push_back(attribute.substr(10));
            for (const auto &attribute : r->attributes())
                if (starts(attribute, "rtcp-fb:* ")) remoteFeedback.push_back(attribute.substr(10));
            for (const auto &entry : localFeedback)
                if (std::find(remoteFeedback.begin(), remoteFeedback.end(), entry) != remoteFeedback.end())
                    media.feedback.push_back(entry);
            std::sort(media.feedback.begin(), media.feedback.end());
            media.feedback.erase(std::unique(media.feedback.begin(), media.feedback.end()), media.feedback.end());
            for (const int rtxPt : r->payloadTypes()) {
                if (!l->hasPayloadType(rtxPt)) continue;
                const auto *rr = r->rtpMap(rtxPt), *lr = l->rtpMap(rtxPt);
                if (!rr || !lr || !sameFormat(rr->format, "rtx") || !sameFormat(lr->format, "rtx") ||
                    rr->clockRate != rm->clockRate || lr->clockRate != rm->clockRate) continue;
                const auto apt = "apt=" + std::to_string(pt);
                if (std::find(rr->fmtps.begin(), rr->fmtps.end(), apt) != rr->fmtps.end() &&
                    std::find(lr->fmtps.begin(), lr->fmtps.end(), apt) != lr->fmtps.end())
                    media.rtxPayloadTypes.push_back(rtxPt);
            }
            const auto remoteIds = extensionIds(*r);
            for (const int localId : extensionIds(*l)) {
                const auto *le = l->extMap(localId);
                if (!le || !receives(le->direction)) continue;
                const bool v1 = le->uri == Uri;
                const bool v2 = le->uri == "http://www.webrtc.org/experiments/rtp-hdrext/transport-wide-cc-02";
                if (!v1 && !v2) continue;
                for (const int remoteId : remoteIds) {
                    const auto *re = r->extMap(remoteId);
                    if (!re || re->uri != le->uri || !sends(re->direction)) continue;
                    media.transportExtensions.push_back({v1 ? TransportExtensionVersion::V1 : TransportExtensionVersion::V2,
                        uint8_t(localId), uint8_t(remoteId), le->attributes.empty() && re->attributes.empty()});
                }
            }
            for (const int localId : extensionIds(*l)) {
                const auto *le = l->extMap(localId);
                const bool ast = le && le->uri == AbsSendTimeUri, tof = le && le->uri == TransmissionOffsetUri;
                if ((!ast && !tof) || !receives(le->direction) || !le->attributes.empty() ||
                    std::find(remoteIds.begin(), remoteIds.end(), localId) == remoteIds.end()) continue;
                const auto *re = r->extMap(localId);
                if (!re || re->uri != le->uri || !sends(re->direction) || !re->attributes.empty()) continue;
                (ast ? media.absSendTimeId : media.transmissionOffsetId) = uint8_t(localId);
            }
            result.media.push_back(std::move(media));
        }
    }
    if (!result.media.empty()) result.status = NegotiationStatus::Ready;
    return result;
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
