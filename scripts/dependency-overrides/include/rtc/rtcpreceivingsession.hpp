/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTCP_RECEIVING_SESSION_H
#define RTC_RTCP_RECEIVING_SESSION_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "mediahandler.hpp"
#include "message.hpp"
#include "rtp.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <utility>

#define RTP_SEQ_MOD (1<<16)

namespace rtc {

// Values used for the single reception report block that precedes an optional
// REMB in the compound packet. They are serialized explicitly so the exact wire
// layout can be tested without a live peer connection.
struct RembReportBlockValues {
	SSRC mediaSsrc = 0;
	uint8_t fractionLost = 0;
	int32_t packetsLost = 0; // 24-bit signed value on the wire
	uint16_t highestSeqNo = 0;
	uint16_t seqNoCycles = 0;
	uint32_t jitter = 0;
	uint32_t lastSr = 0; // middle 32 bits of the last SR NTP timestamp
	uint32_t delaySinceLastSr = 0;
};

namespace remb_detail {

inline bool validCname(const string &cname) {
	if (cname.empty() || cname.size() > 255)
		return false;
	return std::all_of(cname.begin(), cname.end(), [](unsigned char c) {
		return c >= 0x20 && c <= 0x7e;
	});
}

inline void appendU16(binary &out, uint16_t value) {
	out.push_back(byte{uint8_t(value >> 8)});
	out.push_back(byte{uint8_t(value)});
}

inline void appendU32(binary &out, uint32_t value) {
	out.push_back(byte{uint8_t(value >> 24)});
	out.push_back(byte{uint8_t(value >> 16)});
	out.push_back(byte{uint8_t(value >> 8)});
	out.push_back(byte{uint8_t(value)});
}

// Converts monotonic elapsed time since the most recent matching SR into RFC
// 3550 DLSR units (1/65536 second). A missing/negative interval is zero and the
// value is clamped to the 32-bit field, so delayed active feedback stays valid.
inline uint32_t dlsrUnits(int64_t nowMs, int64_t lastSrMs) {
	if (lastSrMs <= 0 || nowMs <= lastSrMs)
		return 0;
	const uint64_t elapsed = uint64_t(nowMs - lastSrMs);
	if (elapsed > (uint64_t(0xffffffffu) * 1000u) / 65536u)
		return 0xffffffffu;
	return uint32_t((elapsed * 65536u) / 1000u);
}

} // namespace remb_detail

// Builds RR + SDES(CNAME) without REMB:
//
//   RR    (sender = localSsrc, one report block for report.mediaSsrc)
//   SDES  (chunk = localSsrc, CNAME)
//
// The local sender SSRC and CNAME are the same stable per-session identity used
// by the REMB variant below. The report block still targets the received
// primary media SSRC. Returns an empty packet when the identity is invalid.
inline binary makeCompoundRrSdes(SSRC localSsrc, const string &cname,
                                 const RembReportBlockValues &report) {
	if (!localSsrc || !report.mediaSsrc || localSsrc == report.mediaSsrc ||
	    !remb_detail::validCname(cname))
		return {};

	// SDES chunk: SSRC, CNAME item, END item, padded to a 32-bit boundary.
	const size_t sdesChunk = 4 + 2 + cname.size() + 1;
	const size_t sdesPadded = (sdesChunk + 3) & ~size_t(3);
	const size_t rrBytes = 32;
	const size_t sdesBytes = 4 + sdesPadded;
	const size_t total = rrBytes + sdesBytes;
	if (total > 1200)
		return {};

	binary out;
	out.reserve(total);

	// RR: version 2, one reception report block. Length is 7 32-bit words.
	out.push_back(byte{0x81}); // V=2, P=0, RC=1
	out.push_back(byte{201});  // RR
	remb_detail::appendU16(out, 7);
	remb_detail::appendU32(out, localSsrc);
	remb_detail::appendU32(out, report.mediaSsrc);
	out.push_back(byte{report.fractionLost});
	const uint32_t lost24 = uint32_t(report.packetsLost) & 0x00ffffffu;
	out.push_back(byte{uint8_t(lost24 >> 16)});
	out.push_back(byte{uint8_t(lost24 >> 8)});
	out.push_back(byte{uint8_t(lost24)});
	remb_detail::appendU16(out, report.highestSeqNo);
	remb_detail::appendU16(out, report.seqNoCycles);
	remb_detail::appendU32(out, report.jitter);
	remb_detail::appendU32(out, report.lastSr);
	remb_detail::appendU32(out, report.delaySinceLastSr);

	// SDES: one chunk for the local sender SSRC carrying a CNAME item.
	out.push_back(byte{0x81}); // V=2, P=0, SC=1
	out.push_back(byte{202});  // SDES
	remb_detail::appendU16(out, uint16_t(sdesBytes / 4 - 1));
	remb_detail::appendU32(out, localSsrc);
	out.push_back(byte{1}); // CNAME
	out.push_back(byte{uint8_t(cname.size())});
	for (char c : cname)
		out.push_back(byte{uint8_t(c)});
	out.push_back(byte{0}); // END (distinct from RTCP padding)
	while (out.size() < rrBytes + sdesBytes)
		out.push_back(byte{0});

	return out;
}

// Builds one complete compound RTCP packet:
//
//   RR    (sender = localSsrc, one report block for report.mediaSsrc)
//   SDES  (chunk = localSsrc, CNAME)
//   REMB  (packet sender = localSsrc, media-source header field = 0,
//          target SSRC list = report.mediaSsrc)
//
// The REMB "SSRC of media source" field in the feedback header is always zero;
// the target is carried in the FCI SSRC list. The local sender SSRC is the
// stable per-session RTCP identity and is also used by the RR and SDES chunks,
// so all three parts describe the same sender. Returns an empty packet when the
// identity, target, CNAME or bitrate is invalid. No reduced-size RTCP and no
// bare REMB fallback is produced here.
inline binary makeCompoundRemb(SSRC localSsrc, const string &cname, unsigned int bitrate,
                               const RembReportBlockValues &report) {
	if (!bitrate)
		return {};

	binary out = makeCompoundRrSdes(localSsrc, cname, report);
	if (out.empty() || out.size() + 24 > 1200)
		return {};

	// REMB: FMT=15, PT=206. The media-source header field is zero; the target
	// SSRC is in the FCI list. Length is 5 32-bit words for one SSRC.
	out.push_back(byte{0x8f}); // V=2, P=0, FMT=15
	out.push_back(byte{206});  // PSFB
	remb_detail::appendU16(out, 5);
	remb_detail::appendU32(out, localSsrc); // packet sender SSRC
	remb_detail::appendU32(out, 0);         // media-source SSRC (always zero)
	out.push_back(byte{uint8_t('R')});
	out.push_back(byte{uint8_t('E')});
	out.push_back(byte{uint8_t('M')});
	out.push_back(byte{uint8_t('B')});
	uint32_t exponent = 0;
	uint32_t mantissa = bitrate;
	while (mantissa > 0x3ffffu) {
		mantissa >>= 1;
		++exponent;
	}
	remb_detail::appendU32(out, (1u << 24) | (exponent << 18) | (mantissa & 0x3ffffu));
	remb_detail::appendU32(out, report.mediaSsrc);

	return out;
}

// An RtcpSession can be plugged into a Track to handle the whole RTCP session
class RTC_CPP_EXPORT RtcpReceivingSession : public MediaHandler {
public:
	RtcpReceivingSession() = default;
	virtual ~RtcpReceivingSession() = default;

	void incoming(message_vector &messages, const message_callback &send) override;
	bool requestKeyframe(const message_callback &send) override;
	bool requestBitrate(unsigned int bitrate, const message_callback &send) override;

	// Stable RTCP identity for feedback sent by this session. The sender SSRC is
	// the local nonzero session SSRC; the CNAME is the matching non-secret SDES
	// name. Both must be set before REMB feedback is sent. The media target is
	// learned only from received primary RTP packets, never from SR/RR.
	void setRtcpIdentity(SSRC senderSsrc, string cname);
	bool hasRtcpIdentity() const;
	SSRC rtcpSenderSsrc() const { return mRtcpSenderSsrc; }
	const string &rtcpCname() const { return mRtcpCname; }
	SSRC receivedRtpSsrc() const { return mRtpSsrc; }

	// For backward compatibility
	[[deprecated("Use Track::requestKeyframe()")]] inline bool requestKeyframe() { return false; };
	[[deprecated("Use Track::requestBitrate()")]] inline void requestBitrate(unsigned int) {};

protected:
	static void scheduleSenderReportReply(std::function<void()> reply);
	// Sends one compound RR + SDES(CNAME), optionally followed by REMB, through
	// the authenticated SRTCP callback. `bitrate == 0` sends the RR + SDES form
	// used when REMB is not active. Returns false before the first primary RTP
	// packet or without a valid local identity; it never emits a bare REMB.
	bool pushCompoundFeedback(const message_callback &send, unsigned int bitrate);
	RembReportBlockValues makeReportBlockValues();
	void pushRR(const message_callback &send,unsigned int lastSrDelay);
	void pushPLI(const message_callback &send);

	void initSeq(uint16_t seq);
	bool updateSeq(uint16_t seq);
	
	SSRC mSsrc = 0;    // remote RTCP/report-block source (kept for RR/PLI)
	SSRC mRtpSsrc = 0; // primary media SSRC learned only from received RTP
	SSRC mRtcpSenderSsrc = 0;
	string mRtcpCname;
	uint32_t mGreatestSeqNo = 0;
    uint16_t mMaxSeq = 0;			// highest seq. number seen
    uint32_t mCycles = 0;			// shifted count of seq. number cycles
	uint32_t mBaseSeq = 0;			// base seq number
    uint32_t mBadSeq = 0;			// last 'bad' seq number + 1
	uint32_t mProbation = 0;		// sequ. packets till source is valid
    uint32_t mReceived = 0;			// packets received
    uint32_t mExpectedPrior = 0;	// packet expected at last interval
    uint32_t mReceivedPrior = 0;	// packet received at last interval
	uint32_t mTransit = 0;			// relative trans time for prev pkt
	uint32_t mJitter = 0;	
	
	uint64_t mSyncRTPTS = 0, mSyncNTPTS = 0;
	int64_t mLastSrMs = 0;
	uint64_t mLastSrNtp = 0;
	bool mHaveLastSr = false;

	std::atomic<unsigned int> mRequestedBitrate = 0;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif // RTC_RTCP_RECEIVING_SESSION_H
