// White-box tests for the sender-report driven playout extension.
//
// All RTP/RTCP input is synthesised in memory and replayed through
// RTPSources::ProcessRawPacket, so no sockets are needed.  The session
// entry points are exercised against the same production RTPSources
// instance by exposing the session's private members (the standard headers
// are parsed first so the macro cannot affect them).

#include "task_support/check.h"

#include "rtpsources.h"
#include "rtpinternalsourcedata.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include "rtcpsrpacket.h"
#include "rtcpsdespacket.h"
#include "rtcpcompoundpacketbuilder.h"
#include "rtpstructs.h"
#include "rtperrors.h"
#include "rtptimeutilities.h"

#include <list>
#include <map>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstddef>

#define private public
#include "rtpsession.h"
#undef private

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static RTPTime msTime(long long usec)
{
	if (usec < 0)
		usec = 0;
	return RTPTime((u_int32_t)(usec/1000000),(u_int32_t)(usec%1000000));
}

static long long timeUsec(const RTPTime &t)
{
	return ((long long)t.GetSeconds())*1000000+((long long)t.GetMicroSeconds());
}

static long long floorDiv(long long value,long long divisor)
{
	long long q = value/divisor;
	long long r = value%divisor;

	if (r != 0 && ((r < 0) != (divisor < 0)))
		q--;
	return q;
}

// Independent re-implementation of the specified deadline, for whole second
// NTP differences (LSW == 0 for both the reference and the sender report).
static long long expectedDeadline(long long localUsec,long long delayUsec,
                                  long long ntpUsec,int tsdiff,u_int32_t rate)
{
	return localUsec+delayUsec+ntpUsec+floorDiv((long long)tsdiff*1000000LL,(long long)rate);
}

static RTPIPv4Address *addressFor(u_int32_t ssrc,bool rtcp)
{
	u_int32_t ip = 0x7F000000u | (ssrc & 0xFFu);
	u_int16_t port = (u_int16_t)(4000+(((ssrc>>8) & 0x3FFu)*2)+(rtcp?1:0));
	return new RTPIPv4Address(ip,port);
}

static bool feedRTP(RTPSources &sources,u_int32_t ssrc,u_int16_t seq,u_int32_t timestamp)
{
	u_int8_t *data = new u_int8_t[13];
	std::memset(data,0,13);
	data[0] = 0x80;
	data[1] = 96;
	data[2] = (u_int8_t)(seq>>8);
	data[3] = (u_int8_t)seq;
	data[4] = (u_int8_t)(timestamp>>24);
	data[5] = (u_int8_t)(timestamp>>16);
	data[6] = (u_int8_t)(timestamp>>8);
	data[7] = (u_int8_t)timestamp;
	data[8] = (u_int8_t)(ssrc>>24);
	data[9] = (u_int8_t)(ssrc>>16);
	data[10] = (u_int8_t)(ssrc>>8);
	data[11] = (u_int8_t)ssrc;
	data[12] = 0x55;

	RTPTime recvtime(10,0);
	RTPRawPacket raw(data,13,addressFor(ssrc,false),recvtime,true);
	return sources.ProcessRawPacket(&raw,0,false) >= 0;
}

static bool feedSR(RTPSources &sources,u_int32_t ssrc,u_int32_t msw,u_int32_t lsw,u_int32_t rtptimestamp)
{
	RTCPCompoundPacketBuilder builder;
	if (builder.InitBuild(1400) < 0)
		return false;
	if (builder.StartSenderReport(ssrc,RTPNTPTime(msw,lsw),rtptimestamp,0,0) < 0)
		return false;
	if (builder.AddSDESSource(ssrc) < 0)
		return false;
	const char cname[] = "t";
	if (builder.AddSDESNormalItem(RTCPSDESPacket::CNAME,cname,1) < 0)
		return false;
	if (builder.EndBuild() < 0)
		return false;

	size_t len = builder.GetCompoundPacketLength();
	u_int8_t *data = new u_int8_t[len];
	std::memcpy(data,builder.GetCompoundPacketData(),len);

	RTPTime recvtime(10,0);
	RTPRawPacket raw(data,len,addressFor(ssrc,true),recvtime,false);
	return sources.ProcessRawPacket(&raw,0,false) >= 0;
}

// The probation logic drops the first two consecutive packets of a source.
// prime() feeds those two so the following packets are queued.
static void prime(RTPSources &sources,u_int32_t ssrc,u_int16_t firstseq,u_int32_t timestamp = 0)
{
	feedRTP(sources,ssrc,firstseq,timestamp);
	feedRTP(sources,ssrc,(u_int16_t)(firstseq+1),timestamp);
}

static RTPPacket *fetch(RTPSources &sources,long long nowUsec)
{
	return sources.GetNextPlayoutPacket(msTime(nowUsec));
}

struct Fetched
{
	bool present;
	u_int16_t seq;
	u_int32_t timestamp;
	u_int32_t extseq;
};

static Fetched take(RTPSources &sources,long long nowUsec)
{
	Fetched f;
	RTPPacket *p = fetch(sources,nowUsec);
	f.present = (p != 0);
	f.seq = 0;
	f.timestamp = 0;
	f.extseq = 0;
	if (p)
	{
		f.seq = p->GetSequenceNumber();
		f.timestamp = p->GetTimestamp();
		f.extseq = p->GetExtendedSequenceNumber();
		delete p;
	}
	return f;
}

static void drainRegular(RTPSources &sources)
{
	while (sources.GotoFirstSourceWithData())
	{
		RTPPacket *p;
		while ((p = sources.GetNextPacket()) != 0)
			delete p;
	}
}

// ---------------------------------------------------------------------------
// timeline
// ---------------------------------------------------------------------------

// Two sources with different clock rates; ordering by earliest deadline,
// exact-boundary delivery and SSRC tie breaking.
static int timeline_dual_rate(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(50,0),RTPTime(0,0)) == 0);

	const u_int32_t a = 0x11;
	const u_int32_t b = 0x22;
	TASK_CHECK(sources.SetPlayoutClockRate(a,8000) == 0);
	TASK_CHECK(sources.SetPlayoutClockRate(b,90000) == 0);

	// A: anchor (1000,0) at media timestamp 1000, 8000 ticks/s
	prime(sources,a,10);
	TASK_CHECK(feedRTP(sources,a,12,1000));   // 50.000000
	TASK_CHECK(feedRTP(sources,a,13,2000));   // 50.125000
	TASK_CHECK(feedRTP(sources,a,14,9000));   // 51.000000

	// B: anchor (1000, 0.5s) at media timestamp 2000, 90000 ticks/s
	prime(sources,b,20);
	TASK_CHECK(feedRTP(sources,b,22,2000));    // 50.500000
	TASK_CHECK(feedRTP(sources,b,23,9200));    // 50.580000
	TASK_CHECK(feedRTP(sources,b,24,47000));   // 51.000000

	TASK_CHECK(feedSR(sources,a,1000,0,1000));
	TASK_CHECK(feedSR(sources,b,1000,2147483648u,2000));

	Fetched f;

	f = take(sources,50000000LL);
	TASK_CHECK(f.present && f.seq == 12 && f.timestamp == 1000);

	f = take(sources,50124999LL);              // one microsecond early
	TASK_CHECK(!f.present);

	f = take(sources,50125000LL);              // exactly on the deadline
	TASK_CHECK(f.present && f.seq == 13);

	f = take(sources,50500000LL);
	TASK_CHECK(f.present && f.seq == 22);

	f = take(sources,50579999LL);
	TASK_CHECK(!f.present);

	f = take(sources,50580000LL);
	TASK_CHECK(f.present && f.seq == 23);

	// Both are due at 51.000000; the smaller SSRC wins the tie.
	f = take(sources,51000000LL);
	TASK_CHECK(f.present && f.seq == 14 && f.extseq == 14);

	f = take(sources,51000000LL);
	TASK_CHECK(f.present && f.seq == 24 && f.extseq == 24);

	f = take(sources,60000000LL);
	TASK_CHECK(!f.present);

	return 0;
}

// 16 bit sequence numbers wrapping through zero.
static int timeline_seq_wrap(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(10,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0x33;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,65533);
	TASK_CHECK(feedRTP(sources,ssrc,65535,8000));   // 11.000000, ext 65535
	TASK_CHECK(feedRTP(sources,ssrc,0,16000));      // 12.000000, ext 65536
	TASK_CHECK(feedRTP(sources,ssrc,1,24000));      // 13.000000, ext 65537
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;

	f = take(sources,10999999LL);
	TASK_CHECK(!f.present);
	f = take(sources,11000000LL);
	TASK_CHECK(f.present && f.seq == 65535 && f.extseq == 65535);
	f = take(sources,12000000LL);
	TASK_CHECK(f.present && f.seq == 0 && f.extseq == 65536);
	f = take(sources,13000000LL);
	TASK_CHECK(f.present && f.seq == 1 && f.extseq == 65537);

	return 0;
}

// 32 bit media timestamps crossing zero, in both directions, at exact
// microsecond boundaries. Only the head of a source is considered.
static int timeline_ts_wrap(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(20,0),RTPTime(0,0)) == 0);

	const u_int32_t ahead = 0x44;   // media clock just below the wrap
	const u_int32_t behind = 0x45;  // media clock just above the wrap
	TASK_CHECK(sources.SetPlayoutClockRate(ahead,90000) == 0);
	TASK_CHECK(sources.SetPlayoutClockRate(behind,90000) == 0);

	// +8192 ticks: 8192/90000 s = 91022.22 us -> floored 91022 us
	prime(sources,ahead,5);
	TASK_CHECK(feedRTP(sources,ahead,7,0x00001000u));
	TASK_CHECK(feedSR(sources,ahead,1000,0,0xFFFFF000u));

	// Anchor 0x00001000, packet 0xFFFFE000: delta -12288 ticks. Together
	// with the SR's small NTP fraction this floors to -136534 us, giving
	// 19.863466.
	prime(sources,behind,5);
	TASK_CHECK(feedRTP(sources,behind,7,0xFFFFE000u));
	TASK_CHECK(feedSR(sources,behind,1000,0,0x00001000u));

	Fetched f;

	// Head of 'behind' is due first, at 19.863466.
	f = take(sources,19863465LL);
	TASK_CHECK(!f.present);
	f = take(sources,19863466LL);
	TASK_CHECK(f.present && f.seq == 7 && f.extseq == 7);

	// 'ahead' is due at 20.091022.
	TASK_CHECK(!fetch(sources,20091021LL));
	f = take(sources,20091022LL);
	TASK_CHECK(f.present && f.seq == 7 && f.extseq == 7);

	return 0;
}

// Packets arriving out of order are still queued by extended sequence
// number and delivered in that order, never re-ordered by timestamp.
static int timeline_out_of_order(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(35,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0x56;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	// Probation consumes two further packets, so prime with 11/12 and let 13
	// be the first queued packet. Then 13 arrives first with a later media
	// timestamp, then 12 with an earlier one, then 15 before 14.
	prime(sources,ssrc,11);
	TASK_CHECK(feedRTP(sources,ssrc,13,8000));
	TASK_CHECK(feedRTP(sources,ssrc,12,0));
	TASK_CHECK(feedRTP(sources,ssrc,15,24000));
	TASK_CHECK(feedRTP(sources,ssrc,14,16000));
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;

	f = take(sources,35000000LL);
	TASK_CHECK(f.present && f.seq == 12 && f.timestamp == 0);
	f = take(sources,36000000LL);
	TASK_CHECK(f.present && f.seq == 13 && f.timestamp == 8000);
	f = take(sources,37000000LL);
	TASK_CHECK(f.present && f.seq == 14 && f.timestamp == 16000);
	f = take(sources,38000000LL);
	TASK_CHECK(f.present && f.seq == 15 && f.timestamp == 24000);
	f = take(sources,40000000LL);
	TASK_CHECK(!f.present);

	return 0;
}

// Several packets may share one timestamp; they are still delivered in
// extended sequence order, never re-ordered by timestamp.
static int timeline_same_timestamp(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(30,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0x55;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,40);
	TASK_CHECK(feedRTP(sources,ssrc,42,8000));
	TASK_CHECK(feedRTP(sources,ssrc,43,8000));
	TASK_CHECK(feedRTP(sources,ssrc,44,8000));
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;

	f = take(sources,31000000LL);
	TASK_CHECK(f.present && f.seq == 42 && f.timestamp == 8000);
	f = take(sources,31000000LL);
	TASK_CHECK(f.present && f.seq == 43 && f.timestamp == 8000);
	f = take(sources,31000000LL);
	TASK_CHECK(f.present && f.seq == 44 && f.timestamp == 8000);
	f = take(sources,31000000LL);
	TASK_CHECK(!f.present);

	return 0;
}

// ---------------------------------------------------------------------------
// reanchor
// ---------------------------------------------------------------------------

// A newer SR pushes the not-yet-delivered packets later.
static int reanchor_later(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(50,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0x66;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,8000));    // 51.0 under SR1
	TASK_CHECK(feedRTP(sources,ssrc,13,16000));   // 52.0 under SR1
	TASK_CHECK(feedRTP(sources,ssrc,14,24000));   // 53.0 under SR1
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;

	f = take(sources,50900000LL);
	TASK_CHECK(!f.present);

	// NTP strictly advances; RTP timestamp jumps so packets move later.
	TASK_CHECK(feedSR(sources,ssrc,1001,0,4000));

	f = take(sources,51200000LL);
	TASK_CHECK(!f.present);                       // would have been due before
	f = take(sources,51500000LL);
	TASK_CHECK(f.present && f.seq == 12);
	f = take(sources,52500000LL);
	TASK_CHECK(f.present && f.seq == 13);
	f = take(sources,53500000LL);
	TASK_CHECK(f.present && f.seq == 14);

	return 0;
}

// A newer SR moves the deadlines earlier, immediately taking effect for all
// queued packets of that source.
static int reanchor_earlier(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(50,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0x67;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,8000));    // 51.0 under SR1
	TASK_CHECK(feedRTP(sources,ssrc,13,16000));   // 52.0 under SR1
	TASK_CHECK(feedRTP(sources,ssrc,14,24000));   // 53.0 under SR1
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;

	f = take(sources,49500000LL);
	TASK_CHECK(!f.present);

	// NTP strictly advances; RTP timestamp is ahead of the queued packets.
	TASK_CHECK(feedSR(sources,ssrc,1001,0,24000));

	f = take(sources,49500000LL);
	TASK_CHECK(f.present && f.seq == 12);         // now due at 49.0
	f = take(sources,50000000LL);
	TASK_CHECK(f.present && f.seq == 13);         // 50.0
	f = take(sources,51000000LL);
	TASK_CHECK(f.present && f.seq == 14);         // 51.0

	return 0;
}

// Equal or older NTP timestamps keep the playout mapping, even though the
// regular SR information is still updated.
static int reanchor_equal_and_older(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(60,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0x68;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,8000));    // 61.0
	TASK_CHECK(feedRTP(sources,ssrc,13,16000));   // 62.0
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	// Equal NTP: must not move the mapping to 56.0.
	TASK_CHECK(feedSR(sources,ssrc,1000,0,40000));
	RTPSourceData *d = sources.GetSourceInfo(ssrc);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->SR_GetRTPTimestamp() == 40000);   // regular info updated

	Fetched f;
	f = take(sources,60500000LL);
	TASK_CHECK(!f.present);

	// Older NTP: still no change to the mapping.
	TASK_CHECK(feedSR(sources,ssrc,999,0,80000));
	TASK_CHECK(sources.GetSourceInfo(ssrc)->SR_GetNTPTimestamp().GetMSW() == 999);

	f = take(sources,60500000LL);
	TASK_CHECK(!f.present);
	f = take(sources,61000000LL);
	TASK_CHECK(f.present && f.seq == 12);
	f = take(sources,62000000LL);
	TASK_CHECK(f.present && f.seq == 13);

	return 0;
}

// A source without a mapping waits while other sources keep playing.
static int reanchor_missing_mapping(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(70,0),RTPTime(0,0)) == 0);

	const u_int32_t withsr = 0x77;
	const u_int32_t nosr = 0x88;
	const u_int32_t norate = 0x99;
	TASK_CHECK(sources.SetPlayoutClockRate(withsr,8000) == 0);
	TASK_CHECK(sources.SetPlayoutClockRate(nosr,8000) == 0);

	prime(sources,withsr,10);
	TASK_CHECK(feedRTP(sources,withsr,12,0));
	TASK_CHECK(feedSR(sources,withsr,1000,0,0));

	prime(sources,nosr,10);
	TASK_CHECK(feedRTP(sources,nosr,12,0));
	// deliberately no SR for nosr

	prime(sources,norate,10);
	TASK_CHECK(feedRTP(sources,norate,12,0));
	TASK_CHECK(feedSR(sources,norate,1000,0,0));
	// deliberately no rate for norate

	Fetched f;

	f = take(sources,70000000LL);
	TASK_CHECK(f.present && f.seq == 12);
	f = take(sources,70000000LL);
	TASK_CHECK(!f.present);                       // nosr and norate wait

	TASK_CHECK(feedSR(sources,nosr,1000,0,0));
	f = take(sources,70000000LL);
	TASK_CHECK(f.present && f.seq == 12);         // now the SR-less source plays

	TASK_CHECK(sources.SetPlayoutClockRate(norate,8000) == 0);
	f = take(sources,70000000LL);
	TASK_CHECK(f.present && f.seq == 12);

	f = take(sources,70000000LL);
	TASK_CHECK(!f.present);

	return 0;
}

// Delivered extended sequence numbers form a frontier: late packets at or
// below it are never returned again, even after a gap or a new SR.
static int reanchor_late_after_gap(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(80,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0xAA;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,0));       // 80.0
	TASK_CHECK(feedRTP(sources,ssrc,13,8000));    // 81.0
	TASK_CHECK(feedRTP(sources,ssrc,15,16000));   // 82.0, gap at 14
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;

	f = take(sources,80000000LL);
	TASK_CHECK(f.present && f.seq == 12);
	f = take(sources,81000000LL);
	TASK_CHECK(f.present && f.seq == 13);

	// Packet 15 jumps over the missing 14; the frontier advances to 15.
	f = take(sources,82000000LL);
	TASK_CHECK(f.present && f.seq == 15 && f.extseq == 15);

	// Packet 14 now arrives late, after the gap was skipped; it is at or
	// below the delivered frontier and must not come out.
	TASK_CHECK(feedRTP(sources,ssrc,14,16000));
	f = take(sources,82000000LL);
	TASK_CHECK(!f.present);
	f = take(sources,200000000LL);
	TASK_CHECK(!f.present);

	// A newer SR moves deadlines around, but the delivered frontier stays.
	TASK_CHECK(feedSR(sources,ssrc,1001,0,80000));
	TASK_CHECK(feedRTP(sources,ssrc,14,16000));   // late again, still blocked
	f = take(sources,90000000LL);
	TASK_CHECK(!f.present);

	// A genuinely new sequence number above the frontier is delivered.
	TASK_CHECK(feedRTP(sources,ssrc,16,88000));
	f = take(sources,90000000LL);
	TASK_CHECK(f.present && f.seq == 16 && f.extseq == 16);

	return 0;
}

// ---------------------------------------------------------------------------
// ownership
// ---------------------------------------------------------------------------

// The old and new retrieval entry points share one real queue and one
// delivered frontier.
static int ownership_shared_queue(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(90,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0xBB;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,0));
	TASK_CHECK(feedRTP(sources,ssrc,13,8000));
	TASK_CHECK(feedRTP(sources,ssrc,14,16000));
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;

	f = take(sources,90000000LL);
	TASK_CHECK(f.present && f.seq == 12);

	// Consume the next packet through the ordinary entry point.
	TASK_CHECK(sources.GotoFirstSourceWithData());
	RTPPacket *p = sources.GetNextPacket();
	TASK_CHECK(p != 0 && p->GetSequenceNumber() == 13);
	delete p;

	// The playout entry sees the same queue and the advanced frontier.
	f = take(sources,92000000LL);
	TASK_CHECK(f.present && f.seq == 14);

	// A late packet at or below the frontier is suppressed through either
	// entry point.
	TASK_CHECK(feedRTP(sources,ssrc,13,8000));
	f = take(sources,200000000LL);
	TASK_CHECK(!f.present);
	TASK_CHECK(!sources.GotoFirstSourceWithData());

	return 0;
}

// The session entry points delegate to the same production RTPSources logic.
static int ownership_session_delegation(void)
{
	// Without a created session the entry points report that.
	{
		RTPSession fresh;
		TASK_CHECK(fresh.SetPlayoutReference(RTPNTPTime(1,0),RTPTime(0,0),RTPTime(0,0)) == ERR_RTP_SESSION_NOTCREATED);
		TASK_CHECK(fresh.SetPlayoutClockRate(0x1234,8000) == ERR_RTP_SESSION_NOTCREATED);
		TASK_CHECK(fresh.GetNextPlayoutPacket(RTPTime(0,0)) == 0);
	}

	// A unit test cannot bind sockets in this environment, so mark the
	// session as created and drive its source table directly.  The guard
	// makes sure the destructor does not try to tear down transmitters.
	struct SessionProbe
	{
		RTPSession sess;
		SessionProbe()
		{
			sess.rtptrans = 0;
			sess.created = true;
		}
		~SessionProbe()
		{
			sess.created = false;
		}
	} probe;

	RTPSession &sess = probe.sess;
	const u_int32_t ssrc = 0xCC;

	TASK_CHECK(sess.BeginDataAccess() == 0);
	TASK_CHECK(sess.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(100,0),RTPTime(0,0)) == 0);
	TASK_CHECK(sess.SetPlayoutClockRate(ssrc,8000) == 0);
	TASK_CHECK(sess.EndDataAccess() == 0);

	prime(sess.sources,ssrc,10);
	TASK_CHECK(feedRTP(sess.sources,ssrc,12,0));
	TASK_CHECK(feedRTP(sess.sources,ssrc,13,8000));
	TASK_CHECK(feedSR(sess.sources,ssrc,1000,0,0));

	TASK_CHECK(sess.GetSourceInfo(ssrc) != 0);

	TASK_CHECK(sess.BeginDataAccess() == 0);
	RTPPacket *p = sess.GetNextPlayoutPacket(msTime(100000000LL));
	TASK_CHECK(p != 0 && p->GetSequenceNumber() == 12);
	delete p;

	// The session's ordinary entry point consumes the same queue and
	// advances the same frontier. Like the original API it needs the
	// source cursor to be positioned first.
	TASK_CHECK(sess.GotoFirstSourceWithData());
	RTPPacket *q = sess.GetNextPacket();
	TASK_CHECK(q != 0 && q->GetSequenceNumber() == 13);
	delete q;

	RTPPacket *late = sess.GetNextPlayoutPacket(msTime(200000000LL));
	TASK_CHECK(late == 0);
	TASK_CHECK(sess.EndDataAccess() == 0);

	return 0;
}

// Clear removes the reference clock, the rates and all pending playout state.
static int ownership_clear(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(10,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0xDD;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,0));
	TASK_CHECK(feedRTP(sources,ssrc,13,8000));
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;
	f = take(sources,10000000LL);
	TASK_CHECK(f.present && f.seq == 12);

	sources.Clear();

	TASK_CHECK(sources.GetSourceInfo(ssrc) == 0);
	f = take(sources,10000000LL);
	TASK_CHECK(!f.present);

	// The reference clock was cleared as well, so it can be configured again.
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(2000,0),RTPTime(200,0),RTPTime(0,0)) == 0);
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(2000,0),RTPTime(200,0),RTPTime(0,0)) < 0);

	// The rate was cleared too: without configuring it again the new source
	// does not play, even with an SR.
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,0) < 0);
	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,0));
	TASK_CHECK(feedSR(sources,ssrc,2000,0,0));
	f = take(sources,201000000LL);
	TASK_CHECK(!f.present);

	// Configure the rate and the previously delivered sequence number comes
	// out again: the frontier was cleared with the source.
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);
	f = take(sources,200000000LL);
	TASK_CHECK(f.present && f.seq == 12 && f.extseq == 12);

	return 0;
}

// A timed out source is treated as brand new when the same SSRC reappears.
static int ownership_timeout_reuse(void)
{
	RTPSources sources;
	TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(10,0),RTPTime(0,0)) == 0);
	const u_int32_t ssrc = 0xEE;
	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);

	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,0));
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

	Fetched f;
	f = take(sources,10000000LL);
	TASK_CHECK(f.present && f.seq == 12);

	// Remove the source through the normal timeout path.
	sources.Timeout(RTPTime(1000,0),RTPTime(900,0));
	TASK_CHECK(sources.GetSourceInfo(ssrc) == 0);

	// The same SSRC returns as a new source: neither the SR mapping nor
	// the clock rate is inherited. The rate is configured again for the
	// new incarnation.
	prime(sources,ssrc,10);
	TASK_CHECK(feedRTP(sources,ssrc,12,0));
	f = take(sources,10000000LL);
	TASK_CHECK(!f.present);                       // no rate, no SR yet

	TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);
	f = take(sources,10000000LL);
	TASK_CHECK(!f.present);                       // still waits for a fresh SR

	// A fresh sender report (even the same values) starts playback anew and
	// the delivered frontier from the previous incarnation is gone.
	TASK_CHECK(feedSR(sources,ssrc,1000,0,0));
	f = take(sources,10000000LL);
	TASK_CHECK(f.present && f.seq == 12 && f.extseq == 12);

	return 0;
}

// Reference/rate configuration rules: one-shot reference, own SSRC allowed,
// non-empty receive queues rejected, zero rate rejected without changing the
// previous setting.
static int ownership_configuration(void)
{
	// Our own SSRC must not block the reference configuration.
	{
		RTPSources sources;
		TASK_CHECK(sources.CreateOwnSSRC(0x12345678) == 0);
		TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(500,0),RTPTime(5,0),RTPTime(0,0)) == 0);
		TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(500,0),RTPTime(5,0),RTPTime(0,0)) < 0);
	}

	// A non-empty receive queue prevents configuration; draining it allows
	// the configuration afterwards.
	{
		RTPSources sources;
		const u_int32_t ssrc = 0xFF;
		prime(sources,ssrc,10);
		TASK_CHECK(feedRTP(sources,ssrc,12,0));
		TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(500,0),RTPTime(5,0),RTPTime(0,0)) < 0);
		TASK_CHECK(sources.GetNextPlayoutPacket(RTPTime(100,0)) == 0);

		drainRegular(sources);
		TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(500,0),RTPTime(5,0),RTPTime(0,0)) == 0);
	}

	// A zero rate is rejected and leaves the previous rate in effect.
	{
		RTPSources sources;
		TASK_CHECK(sources.SetPlayoutReference(RTPNTPTime(1000,0),RTPTime(10,0),RTPTime(0,0)) == 0);
		const u_int32_t ssrc = 0x1010;
		TASK_CHECK(sources.SetPlayoutClockRate(ssrc,8000) == 0);
		TASK_CHECK(sources.SetPlayoutClockRate(ssrc,0) < 0);

		prime(sources,ssrc,10);
		TASK_CHECK(feedRTP(sources,ssrc,12,8000));   // 8000/8000 = 1.0 -> 11.0
		TASK_CHECK(feedSR(sources,ssrc,1000,0,0));

		Fetched f;
		f = take(sources,10999999LL);
		TASK_CHECK(!f.present);
		f = take(sources,11000000LL);
		TASK_CHECK(f.present && f.seq == 12);
	}

	return 0;
}

} // namespace

const struct task_case task_cases[] =
{
	{ "timeline", "dual_rate",       timeline_dual_rate },
	{ "timeline", "seq_wrap",        timeline_seq_wrap },
	{ "timeline", "ts_wrap",         timeline_ts_wrap },
	{ "timeline", "out_of_order",    timeline_out_of_order },
	{ "timeline", "same_timestamp",  timeline_same_timestamp },

	{ "reanchor", "later",            reanchor_later },
	{ "reanchor", "earlier",          reanchor_earlier },
	{ "reanchor", "equal_and_older",  reanchor_equal_and_older },
	{ "reanchor", "missing_mapping",  reanchor_missing_mapping },
	{ "reanchor", "late_after_gap",   reanchor_late_after_gap },

	{ "ownership", "shared_queue",    ownership_shared_queue },
	{ "ownership", "session_delegation", ownership_session_delegation },
	{ "ownership", "clear",           ownership_clear },
	{ "ownership", "timeout_reuse",   ownership_timeout_reuse },
	{ "ownership", "configuration",   ownership_configuration },
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
