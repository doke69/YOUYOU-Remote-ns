/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtcpreceivingsession.hpp"
#include "track.hpp"

#include "impl/logcounter.hpp"
#include "impl/threadpool.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace rtc {

namespace {

int64_t steadyNowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

} // namespace

static impl::LogCounter COUNTER_BAD_RTP_HEADER(plog::warning, "Number of malformed RTP headers");
static impl::LogCounter COUNTER_UNKNOWN_PPID(plog::warning, "Number of Unknown PPID messages");
static impl::LogCounter COUNTER_BAD_NOTIF_LEN(plog::warning,
                                              "Number of Bad-Lengthed notifications");
static impl::LogCounter COUNTER_BAD_SCTP_STATUS(plog::warning,
                                                "Number of unknown SCTP_STATUS errors");

void RtcpReceivingSession::setRtcpIdentity(SSRC senderSsrc, string cname) {
	mRtcpSenderSsrc = senderSsrc;
	mRtcpCname = std::move(cname);
	if (!hasRtcpIdentity()) {
		mRtcpSenderSsrc = 0;
		mRtcpCname.clear();
	}
}

bool RtcpReceivingSession::hasRtcpIdentity() const {
	return mRtcpSenderSsrc != 0 && remb_detail::validCname(mRtcpCname);
}

void RtcpReceivingSession::scheduleSenderReportReply(std::function<void()> reply) {
    impl::ThreadPool::Instance().enqueueDetached(std::move(reply));
}

void RtcpReceivingSession::incoming(message_vector &messages, const message_callback &send) {
	size_t kept=0;
	for (auto message : messages) {
		switch (message->type) {
		case Message::Binary: {
			if (message->size() < sizeof(RtpHeader)) {
				COUNTER_BAD_RTP_HEADER++;
				PLOG_VERBOSE << "RTP packet is too small, size=" << message->size();
				continue;
			}

			auto rtp = reinterpret_cast<const RtpHeader *>(message->data());

			// https://www.rfc-editor.org/rfc/rfc3550.html#appendix-A.1
			if (rtp->version() != 2) {
				COUNTER_BAD_RTP_HEADER++;
				PLOG_VERBOSE << "RTP packet is not version 2";
				continue;
			}

			if (rtp->payloadType() == 201 || rtp->payloadType() == 200) {
				COUNTER_BAD_RTP_HEADER++;
				PLOG_VERBOSE << "RTP packet has a payload type indicating RR/SR";
				continue;
			}

			// Only received primary RTP establishes the REMB target. SR/RR
			// sender SSRCs below remain valid RR/PLI identities but must not be
			// used to invent feedback targets.
			mRtpSsrc = rtp->ssrc();
			mSsrc = mRtpSsrc;

			updateSeq(rtp->seqNumber());

			// Output never overtakes input; keep the original storage and avoid
			// allocating a second vector for the usual single RTP packet.
			messages[kept++]=std::move(message);
			break;
		}

		case Message::Control: {
			if (message->size() < sizeof(RtcpHeader))
				break;
			auto *controlHeader = reinterpret_cast<const RtcpHeader *>(message->data());
			const size_t controlSize = controlHeader->lengthInBytes();
			if (controlHeader->version() != 2 || controlSize < sizeof(RtcpHeader) ||
			    (controlSize & 3) || controlSize > message->size())
				break;
			if (controlHeader->payloadType() == 201 &&
			    controlSize < RtcpRr::SizeWithReportBlocks(controlHeader->reportCount()))
				break;
			if (controlHeader->payloadType() == 200 &&
			    controlSize < RtcpSr::Size(controlHeader->reportCount()))
				break;
			auto rr = reinterpret_cast<const RtcpRr *>(message->data());
			if (rr->header.payloadType() == 201) { // RR
				mSsrc = rr->senderSSRC();
				rr->log();
			} else if (rr->header.payloadType() == 200) { // SR
				const SSRC srSsrc = rr->senderSSRC();
				mSsrc = srSsrc;
				auto sr = reinterpret_cast<const RtcpSr *>(message->data());
				mSyncRTPTS = sr->rtpTimestamp();
				mSyncNTPTS = sr->ntpTimestamp();
				if (mRtpSsrc != 0 && srSsrc == mRtpSsrc) {
					// DLSR uses the monotonic receive time of the most recent SR
					// that matches the primary media source. Keeping it separate
					// from mSyncNTPTS preserves the existing RR fallback.
					mLastSrNtp = sr->ntpTimestamp();
					mLastSrMs = steadyNowMs();
					mHaveLastSr = true;
				}
				sr->log();

				// A session with a local identity and a known primary media SSRC
				// always reports as RR + SDES(CNAME); REMB is appended only when a
				// target has been requested. Without an identity or target the
				// original bare RR path is retained (for example audio).
				const unsigned int bitrate = mRequestedBitrate.load();
				if (!pushCompoundFeedback(send, bitrate))
					pushRR(send, 0);
			}
			break;
		}

		default:
			break;
		}
	}

	messages.resize(kept);
}

bool RtcpReceivingSession::requestBitrate(unsigned int bitrate, const message_callback &send) {
	// Do not store the target before the first primary RTP packet or without a
	// stable local identity: a later SR must not resurrect unvalidated feedback.
	if (!hasRtcpIdentity() || mRtpSsrc == 0)
		return false;
	if (bitrate == 0) {
		mRequestedBitrate.store(0);
		return true;
	}
	if (!pushCompoundFeedback(send, bitrate))
		return false;
	mRequestedBitrate.store(bitrate);
	return true;
}

RembReportBlockValues RtcpReceivingSession::makeReportBlockValues() {
	RembReportBlockValues report;
	report.mediaSsrc = mRtpSsrc;

	const uint32_t extendedMax = mCycles + uint32_t(mMaxSeq);
	const uint32_t expected = extendedMax - mBaseSeq + 1;
	const int64_t totalLost = int64_t(expected) - int64_t(mReceived);
	report.packetsLost = int32_t(std::clamp<int64_t>(totalLost, -8388608, 8388607));

	const uint32_t expectedInterval = expected - mExpectedPrior;
	mExpectedPrior = expected;
	const uint32_t receivedInterval = mReceived - mReceivedPrior;
	mReceivedPrior = mReceived;
	const int64_t lostInterval = int64_t(expectedInterval) - int64_t(receivedInterval);
	report.fractionLost = uint8_t(expectedInterval && lostInterval > 0
	                                  ? std::min<int64_t>(255, (lostInterval * 256) / expectedInterval)
	                                  : 0);

	report.highestSeqNo = uint16_t(mMaxSeq);
	report.seqNoCycles = uint16_t(mCycles >> 16);
	report.jitter = mJitter;
	if (mHaveLastSr) {
		report.lastSr = uint32_t(mLastSrNtp >> 16);
		report.delaySinceLastSr = remb_detail::dlsrUnits(steadyNowMs(), mLastSrMs);
	}
	return report;
}

bool RtcpReceivingSession::pushCompoundFeedback(const message_callback &send,
                                                unsigned int bitrate) {
	if (!hasRtcpIdentity() || mRtpSsrc == 0)
		return false;

	const RembReportBlockValues report = makeReportBlockValues();
	auto packet = bitrate == 0
	                  ? makeCompoundRrSdes(mRtcpSenderSsrc, mRtcpCname, report)
	                  : makeCompoundRemb(mRtcpSenderSsrc, mRtcpCname, bitrate, report);
	if (packet.empty())
		return false;
	send(make_message(packet.begin(), packet.end(), Message::Control));
	return true;
}

void RtcpReceivingSession::pushRR(const message_callback &send, unsigned int lastSrDelay) {
	auto message = make_message(RtcpRr::SizeWithReportBlocks(1), Message::Control);
	auto rr = reinterpret_cast<RtcpRr *>(message->data());
	rr->preparePacket(mSsrc, 1);

	// calculate packets lost, packet expected, fraction
	auto extended_max = mCycles + mMaxSeq;
    auto expected = extended_max - mBaseSeq + 1;
	auto lost = 0;
	if (mReceived > 0) {
		lost = expected - mReceived;
	} 

	auto expected_interval = expected - mExpectedPrior;
    mExpectedPrior = expected;
    auto received_interval = mReceived - mReceivedPrior;
    mReceivedPrior = mReceived;
    auto lost_interval = expected_interval - received_interval;

	uint8_t fraction;

	if (expected_interval == 0 || lost_interval <= 0) {  
		fraction = 0;
	}	
	else { 
		fraction = (lost_interval << 8) / expected_interval;
	}

	rr->getReportBlock(0)->preparePacket(mSsrc, fraction, lost, uint16_t(mGreatestSeqNo), mMaxSeq, 0, mSyncNTPTS,
	                                     lastSrDelay);
	rr->log();
	send(message);
}

bool RtcpReceivingSession::requestKeyframe(const message_callback &send) {
	pushPLI(send);
	return true;
}

void RtcpReceivingSession::pushPLI(const message_callback &send) {
	auto message = make_message(RtcpPli::Size(), Message::Control);
	auto *pli = reinterpret_cast<RtcpPli *>(message->data());
	pli->preparePacket(mSsrc);
	send(message);
}

void RtcpReceivingSession::initSeq(uint16_t seq) {
	mBaseSeq = seq;
	mMaxSeq = seq;
	mBadSeq = RTP_SEQ_MOD + 1;   /* so seq == bad_seq is false */
	mCycles = 0;
	mReceived = 0;
	mReceivedPrior = 0;
	mExpectedPrior = 0;
}

bool RtcpReceivingSession::updateSeq(uint16_t seq) {
	uint16_t udelta = seq - mMaxSeq;
	const int MAX_DROPOUT = 3000;
	const int MAX_MISORDER = 100;
	const int MIN_SEQUENTIAL = 2;

	/*
	* Source is not valid until MIN_SEQUENTIAL packets with
	* sequential sequence numbers have been received.
	*/
	if (mProbation) {
		/* packet is in sequence */
		if (seq == mMaxSeq + 1) {
			mProbation--;
			mMaxSeq = seq;
			if (mProbation == 0) {
				initSeq(seq);
				mReceived++;
				return true;
			}
		} else {
			mProbation = MIN_SEQUENTIAL - 1;
			mMaxSeq = seq;
		}
		return false;
	} else if (udelta < MAX_DROPOUT) {
		/* in order, with permissible gap */
		if (seq < mMaxSeq) {
			/*
			* Sequence number wrapped - count another 64K cycle.
			*/
			mCycles += RTP_SEQ_MOD;
		}
		mMaxSeq = seq;
	} else if (udelta <= RTP_SEQ_MOD - MAX_MISORDER) {
		/* the sequence number made a very large jump */
		if (seq == mBadSeq) {
			/*
			* Two sequential packets -- assume that the other side
			* restarted without telling us so just re-sync
			* (i.e., pretend this was the first packet).
			*/
			initSeq(seq);
		}
		else {
			mBadSeq = (seq + 1) & (RTP_SEQ_MOD-1);
			return false;
		}
	}
	mReceived++;
	return true;
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
