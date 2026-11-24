// Tests for IPv4 unicast RTP/RTCP single-port multiplexing.

#include "task_support/check.h"

#include "rtpudpv4transmitter.h"
#include "rtpipv4address.h"
#include "rtprawpacket.h"
#include "rtppacketbuilder.h"
#include "rtppacket.h"
#include "rtpsession.h"
#include "rtpsessionparams.h"
#include "rtperrors.h"
#include "rtptimeutilities.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

const u_int32_t LOOPBACK = 0x7f000001;

struct Received {
	int fd;
	unsigned short port;
};

int make_receiver(Received *r)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return 1;

	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(LOOPBACK);
	a.sin_port = htons(0);
	if (bind(fd, (sockaddr *)&a, sizeof a) != 0) {
		close(fd);
		return 1;
	}
	socklen_t len = sizeof a;
	if (getsockname(fd, (sockaddr *)&a, &len) != 0) {
		close(fd);
		return 1;
	}
	r->fd = fd;
	r->port = ntohs(a.sin_port);
	return 0;
}

int send_to(int fd, unsigned short port, const void *data, size_t len)
{
	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(LOOPBACK);
	a.sin_port = htons(port);
	ssize_t n = sendto(fd, data, len, 0, (sockaddr *)&a, sizeof a);
	return n == (ssize_t)len ? 0 : 1;
}

// Waits (bounded, using select through WaitForIncomingData) until at least
// 'want' raw packets are available or the deadline expires.
int collect_packets(RTPUDPv4Transmitter &t, size_t want, std::vector<RTPRawPacket *> &out)
{
	RTPTime deadline = RTPTime::CurrentTime();
	deadline += RTPTime(2, 0);
	for (;;) {
		if (t.Poll() < 0)
			return 1;
		while (RTPRawPacket *p = t.GetNextPacket())
			out.push_back(p);
		if (out.size() >= want)
			return 0;
		RTPTime now = RTPTime::CurrentTime();
		if (now >= deadline)
			return out.size() >= want ? 0 : 1;
		if (t.WaitForIncomingData(RTPTime(0, 20000)) < 0)
			return 1;
	}
}

bool is_rtcp_type(u_int8_t b)
{
	return b >= 192 && b <= 223;
}

void delete_packets(std::vector<RTPRawPacket *> &v)
{
	for (size_t i = 0; i < v.size(); ++i)
		delete v[i];
	v.clear();
}

int setup_transmitter(RTPUDPv4Transmitter &t, unsigned short port, bool mux,
                      RTPUDPv4TransmissionParams &params)
{
	if (t.Init(false) < 0)
		return 1;
	params.SetBindIP(LOOPBACK);
	std::list<u_int32_t> ips;
	ips.push_back(LOOPBACK);
	params.SetLocalIPList(ips);
	params.SetPortbase(port);
	params.SetRTCPMultiplexing(mux);
	return t.Create(1500, &params);
}

// Finds an even port base for which both p and p+1 are currently free.
int find_free_port_pair(unsigned short *base)
{
	for (unsigned int p = 42000; p < 60000; p += 2) {
		int fd1 = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd1 < 0)
			return 1;
		sockaddr_in a;
		std::memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(LOOPBACK);
		a.sin_port = htons((unsigned short)p);
		bool ok = bind(fd1, (sockaddr *)&a, sizeof a) == 0;
		close(fd1);
		if (!ok)
			continue;

		int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd2 < 0)
			return 1;
		a.sin_port = htons((unsigned short)(p + 1));
		ok = bind(fd2, (sockaddr *)&a, sizeof a) == 0;
		close(fd2);
		if (ok) {
			*base = (unsigned short)p;
			return 0;
		}
	}
	return 1;
}

// Free odd port for multiplexing: p alone must be free.
int find_free_single_port(unsigned short *port)
{
	for (unsigned int p = 42001; p < 60000; p += 2) {
		int fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return 1;
		sockaddr_in a;
		std::memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(LOOPBACK);
		a.sin_port = htons((unsigned short)p);
		bool ok = bind(fd, (sockaddr *)&a, sizeof a) == 0;
		close(fd);
		if (ok) {
			*port = (unsigned short)p;
			return 0;
		}
	}
	return 1;
}

// ---------------------------------------------------------------------------
// wire: real loopback traffic, ports, source filtering and classification
// ---------------------------------------------------------------------------

int wire_loopback_interleave()
{
	RTPUDPv4Transmitter tx;
	unsigned short port = 0;
	TASK_CHECK(find_free_single_port(&port) == 0);

	RTPUDPv4TransmissionParams params;
	TASK_CHECK(setup_transmitter(tx, port, true, params) == 0);

	// A destination receiver for the outbound direction.
	Received dest;
	TASK_CHECK(make_receiver(&dest) == 0);
	TASK_CHECK(tx.AddDestination(RTPIPv4Address(LOOPBACK, dest.port)) == 0);
	sockaddr_in destname;
	socklen_t destnamelen = sizeof destname;
	TASK_CHECK(getsockname(dest.fd, (sockaddr *)&destname, &destnamelen) == 0);
	TASK_CHECK(ntohs(destname.sin_port) == dest.port);

	RTPPacketBuilder builder;
	TASK_CHECK(builder.Init(1500) == 0);
	TASK_CHECK(builder.BuildPacket("media", 5, 96, true, 3000) == 0);

	const u_int8_t rr[] = { 0x80, 201, 0, 1, 0, 0, 0, 42 };

	// Outbound: RTP and RTCP must both leave the single port and reach the
	// port given to AddDestination.
	TASK_CHECK(tx.SendRTPData(builder.GetPacket(), builder.GetPacketLength()) == 0);
	TASK_CHECK(tx.SendRTCPData(rr, sizeof rr) == 0);

	int out_packets = 0;
	bool out_rtp_ok = false, out_rtcp_ok = false;
	RTPTime deadline = RTPTime::CurrentTime();
	deadline += RTPTime(2, 0);
	while (out_packets < 2) {
		RTPTime now = RTPTime::CurrentTime();
		if (now >= deadline)
			break;
		RTPTime left = deadline;
		left -= now;
		fd_set set;
		FD_ZERO(&set);
		FD_SET(dest.fd, &set);
		timeval tv;
		tv.tv_sec = left.GetSeconds();
		tv.tv_usec = left.GetMicroSeconds();
		int ready = select(dest.fd + 1, &set, 0, 0, &tv);
		if (ready <= 0)
			break;
		u_int8_t buf[1500];
		sockaddr_in src;
		socklen_t srclen = sizeof src;
		ssize_t n = recvfrom(dest.fd, buf, sizeof buf, 0, (sockaddr *)&src, &srclen);
		if (n <= 0)
			break;
		++out_packets;
		TASK_CHECK(ntohs(src.sin_port) == port);
		TASK_CHECK(ntohl(src.sin_addr.s_addr) == LOOPBACK);
		if (n == (ssize_t)builder.GetPacketLength() && !is_rtcp_type(buf[1]))
			out_rtp_ok = true;
		if (n == (ssize_t)sizeof rr && buf[1] == 201)
			out_rtcp_ok = true;
	}
	TASK_CHECK(out_packets == 2);
	TASK_CHECK(out_rtp_ok);
	TASK_CHECK(out_rtcp_ok);
	close(dest.fd);

	// Inbound: one sender that is accepted, one that is ignored.
	Received good, ignored;
	TASK_CHECK(make_receiver(&good) == 0);
	TASK_CHECK(make_receiver(&ignored) == 0);
	TASK_CHECK(tx.SetReceiveMode(RTPTransmitter::IgnoreSome) == 0);
	TASK_CHECK(tx.AddToIgnoreList(RTPIPv4Address(LOOPBACK, ignored.port)) == 0);

	const u_int8_t media_marker[] = { 0x80, 0xE0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2,
	                                  'a', 'b', 'c', 'd' };
	const u_int8_t media_plain[] = { 0x80, 0x60, 0, 2, 0, 0, 0, 2, 0, 0, 0, 3,
	                                 'w', 'x', 'y', 'z' };

	TASK_CHECK(send_to(good.fd, port, media_marker, sizeof media_marker) == 0);
	TASK_CHECK(send_to(good.fd, port, rr, sizeof rr) == 0);
	TASK_CHECK(send_to(ignored.fd, port, media_plain, sizeof media_plain) == 0);
	TASK_CHECK(send_to(good.fd, port, media_plain, sizeof media_plain) == 0);

	std::vector<RTPRawPacket *> packets;
	TASK_CHECK(collect_packets(tx, 3, packets) == 0);
	TASK_CHECK(packets.size() == 3);

	int media = 0, control = 0;
	for (size_t i = 0; i < packets.size(); ++i) {
		RTPRawPacket *p = packets[i];
		RTPIPv4Address *src = (RTPIPv4Address *)p->GetSenderAddress();
		TASK_CHECK(src != 0);
		TASK_CHECK(src->GetIP() == LOOPBACK);
		TASK_CHECK(src->GetPort() == good.port); // ignored source never delivered
		TASK_CHECK(p->GetReceiveTime().GetSeconds() > 0);
		if (p->IsRTP()) {
			++media;
			TASK_CHECK(p->GetDataLength() == sizeof media_marker ||
			           p->GetDataLength() == sizeof media_plain);
			TASK_CHECK(!is_rtcp_type(p->GetData()[1]));
		} else {
			++control;
			TASK_CHECK(p->GetDataLength() == sizeof rr);
			TASK_CHECK(p->GetData()[1] == 201);
		}
	}
	TASK_CHECK(media == 2);
	TASK_CHECK(control == 1);

	delete_packets(packets);
	close(good.fd);
	close(ignored.fd);
	tx.Destroy();
	builder.Destroy();
	return 0;
}

int wire_short_and_bad_version_ignored()
{
	RTPUDPv4Transmitter tx;
	unsigned short port = 0;
	TASK_CHECK(find_free_single_port(&port) == 0);

	RTPUDPv4TransmissionParams params;
	TASK_CHECK(setup_transmitter(tx, port, true, params) == 0);

	Received src;
	TASK_CHECK(make_receiver(&src) == 0);

	const u_int8_t zero_len[] = { 0 };
	const u_int8_t one_byte[] = { 0x80 };
	const u_int8_t bad_version_rtcp[] = { 0x40, 201, 0, 1, 0, 0, 0, 1 };
	const u_int8_t bad_version_rtp[] = { 0x00, 0x60, 0, 1, 0, 0, 0, 1 };
	const u_int8_t damaged_rtcp[] = { 0x80, 201, 0x00, 0x00, 0x00 };
	const u_int8_t okay_rtp[] = { 0x80, 0x60, 0, 1, 0, 0, 0, 1, 1, 2, 3, 4 };

	TASK_CHECK(send_to(src.fd, port, zero_len, 0) == 0);
	TASK_CHECK(send_to(src.fd, port, one_byte, sizeof one_byte) == 0);
	TASK_CHECK(send_to(src.fd, port, bad_version_rtcp, sizeof bad_version_rtcp) == 0);
	TASK_CHECK(send_to(src.fd, port, bad_version_rtp, sizeof bad_version_rtp) == 0);
	TASK_CHECK(send_to(src.fd, port, damaged_rtcp, sizeof damaged_rtcp) == 0);
	TASK_CHECK(send_to(src.fd, port, okay_rtp, sizeof okay_rtp) == 0);

	std::vector<RTPRawPacket *> packets;
	TASK_CHECK(collect_packets(tx, 2, packets) == 0);
	// Only the damaged RTCP and the valid RTP packet may be delivered; the
	// former must stay on the RTCP path.
	TASK_CHECK(packets.size() == 2);
	bool saw_damaged = false, saw_okay = false;
	for (size_t i = 0; i < packets.size(); ++i) {
		if (packets[i]->GetDataLength() == sizeof damaged_rtcp) {
			TASK_CHECK(!packets[i]->IsRTP());
			saw_damaged = true;
		} else {
			TASK_CHECK(packets[i]->IsRTP());
			TASK_CHECK(packets[i]->GetDataLength() == sizeof okay_rtp);
			saw_okay = true;
		}
	}
	TASK_CHECK(saw_damaged);
	TASK_CHECK(saw_okay);

	delete_packets(packets);
	close(src.fd);
	tx.Destroy();
	return 0;
}

int wire_poisoned_port_fails()
{
	RTPUDPv4Transmitter tx;
	unsigned short port = 0;
	TASK_CHECK(find_free_single_port(&port) == 0);

	// Occupy the multiplexed port; creation must fail and release everything.
	int blocker = socket(AF_INET, SOCK_DGRAM, 0);
	TASK_CHECK(blocker >= 0);
	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(LOOPBACK);
	a.sin_port = htons(port);
	TASK_CHECK(bind(blocker, (sockaddr *)&a, sizeof a) == 0);

	RTPUDPv4TransmissionParams params;
	TASK_CHECK(tx.Init(false) == 0);
	params.SetBindIP(LOOPBACK);
	std::list<u_int32_t> ips;
	ips.push_back(LOOPBACK);
	params.SetLocalIPList(ips);
	params.SetPortbase(port);
	params.SetRTCPMultiplexing(true);
	TASK_CHECK(params.IsRTCPMultiplexing());
	TASK_CHECK(tx.Create(1500, &params) < 0);
	tx.Destroy();

	// Once the port is released, the same object can create successfully.
	close(blocker);
	TASK_CHECK(tx.Create(1500, &params) == 0);
	tx.Destroy();
	return 0;
}

// ---------------------------------------------------------------------------
// session: real Create + send, rejected payload types keep counters intact
// ---------------------------------------------------------------------------

static bool parse_seq_ts(const u_int8_t *p, size_t len, u_int16_t *seq, u_int32_t *ts)
{
	if (len < 12)
		return false;
	*seq = (u_int16_t)((p[2] << 8) | p[3]);
	*ts = ((u_int32_t)p[4] << 24) | ((u_int32_t)p[5] << 16) |
	      ((u_int32_t)p[6] << 8) | (u_int32_t)p[7];
	return true;
}

static int recv_packet(int fd, u_int8_t *buf, size_t buflen, unsigned short *from)
{
	RTPTime deadline = RTPTime::CurrentTime();
	deadline += RTPTime(2, 0);
	for (;;) {
		RTPTime now = RTPTime::CurrentTime();
		if (now >= deadline)
			return 1;
		RTPTime left = deadline;
		left -= now;
		fd_set set;
		FD_ZERO(&set);
		FD_SET(fd, &set);
		timeval tv;
		tv.tv_sec = left.GetSeconds();
		tv.tv_usec = left.GetMicroSeconds();
		int ready = select(fd + 1, &set, 0, 0, &tv);
		if (ready < 0)
			return 1;
		if (ready == 0)
			continue;
		sockaddr_in src;
		socklen_t srclen = sizeof src;
		ssize_t n = recvfrom(fd, buf, buflen, 0, (sockaddr *)&src, &srclen);
		if (n < 0)
			return 1;
		if (from)
			*from = ntohs(src.sin_port);
		return (int)n;
	}
}

int session_payload_type_guard()
{
	unsigned short port = 0;
	TASK_CHECK(find_free_single_port(&port) == 0);

	RTPSessionParams sessparams;
	sessparams.SetUsePollThread(false);
	sessparams.SetOwnTimestampUnit(90000.0);
	sessparams.SetResolveLocalHostname(false);

	RTPUDPv4TransmissionParams params;
	params.SetBindIP(LOOPBACK);
	std::list<u_int32_t> ips;
	ips.push_back(LOOPBACK);
	params.SetLocalIPList(ips);
	params.SetPortbase(port);
	params.SetRTCPMultiplexing(true);
	TASK_CHECK(params.IsRTCPMultiplexing());

	RTPSession session;
	TASK_CHECK(session.Create(sessparams, &params) == 0);

	Received dest;
	TASK_CHECK(make_receiver(&dest) == 0);
	TASK_CHECK(session.AddDestination(RTPIPv4Address(LOOPBACK, dest.port)) == 0);
	TASK_CHECK(session.SetDefaultPayloadType(96) == 0);
	TASK_CHECK(session.SetDefaultMark(true) == 0);
	TASK_CHECK(session.SetDefaultTimestampIncrement(3000) == 0);

	TASK_CHECK(session.SendPacket("one", 3) == 0);

	u_int8_t buf[1500];
	unsigned short from = 0;
	int n = recv_packet(dest.fd, buf, sizeof buf, &from);
	TASK_CHECK(n > 0);
	TASK_CHECK(from == port); // single-port source
	u_int16_t seq1 = 0;
	u_int32_t ts1 = 0;
	TASK_CHECK(parse_seq_ts(buf, (size_t)n, &seq1, &ts1));

	// All rejected attempts must leave sequence number, timestamp, packet
	// count and the previously built packet untouched.
	int first_len = n;
	u_int8_t first_copy[1500];
	std::memcpy(first_copy, buf, (size_t)n);

	TASK_CHECK(session.SetDefaultPayloadType(64) < 0);
	TASK_CHECK(session.SetDefaultPayloadType(70) < 0);
	TASK_CHECK(session.SetDefaultPayloadType(95) < 0);
	TASK_CHECK(session.SendPacket("bad", 3, 64, false, 3000) < 0);
	TASK_CHECK(session.SendPacket("bad", 3, 80, true, 3000) < 0);
	TASK_CHECK(session.SendPacket("bad", 3, 95, false, 3000) < 0);
	const u_int8_t ext[] = { 0, 0, 0, 0 };
	TASK_CHECK(session.SendPacketEx("bad", 3, 64, true, 3000, 1, ext, 1) < 0);
	TASK_CHECK(session.SendPacketEx("bad", 3, 90, false, 3000, 1, ext, 1) < 0);
	// Default still 96, so the next default send must use it.
	TASK_CHECK(session.SendPacket("two", 3) == 0);
	n = recv_packet(dest.fd, buf, sizeof buf, &from);
	TASK_CHECK(n > 0);
	TASK_CHECK(from == port);
	u_int16_t seq2 = 0;
	u_int32_t ts2 = 0;
	TASK_CHECK(parse_seq_ts(buf, (size_t)n, &seq2, &ts2));
	TASK_CHECK(seq2 == (u_int16_t)(seq1 + 1));
	TASK_CHECK(ts2 == ts1 + 3000);
	TASK_CHECK(buf[0] == first_copy[0] && buf[1] == first_copy[1]);

	// No stray datagram from any rejected send may have been emitted.
	fd_set set;
	FD_ZERO(&set);
	FD_SET(dest.fd, &set);
	timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 100000;
	TASK_CHECK(select(dest.fd + 1, &set, 0, 0, &tv) == 0);

	close(dest.fd);
	session.Destroy();
	TASK_CHECK(!session.IsActive());

	// A fresh dual-port session must still allow the formerly forbidden PTs.
	unsigned short dual_base = 0;
	TASK_CHECK(find_free_port_pair(&dual_base) == 0);
	RTPSessionParams dualparams;
	dualparams.SetUsePollThread(false);
	dualparams.SetOwnTimestampUnit(90000.0);
	RTPUDPv4TransmissionParams dualtrans;
	dualtrans.SetBindIP(LOOPBACK);
	dualtrans.SetLocalIPList(ips);
	dualtrans.SetPortbase(dual_base);
	RTPSession dual;
	TASK_CHECK(dual.Create(dualparams, &dualtrans) == 0);
	TASK_CHECK(dual.SetDefaultPayloadType(70) == 0);
	TASK_CHECK(dual.SetDefaultMark(false) == 0);
	TASK_CHECK(dual.SetDefaultTimestampIncrement(1) == 0);
	Received dualdest;
	TASK_CHECK(make_receiver(&dualdest) == 0);
	TASK_CHECK(dual.AddDestination(RTPIPv4Address(LOOPBACK, dualdest.port)) == 0);
	TASK_CHECK(dual.SendPacket("x", 1) == 0);
	n = recv_packet(dualdest.fd, buf, sizeof buf, &from);
	TASK_CHECK(n > 0);
	TASK_CHECK(from == dual_base);
	close(dualdest.fd);
	dual.Destroy();
	return 0;
}

int session_default_and_builder_guard()
{
	RTPPacketBuilder builder;
	TASK_CHECK(builder.Init(1500) == 0);
	TASK_CHECK(!builder.IsRTCPMultiplexing());

	// Existing default in the forbidden range must prevent enabling.
	TASK_CHECK(builder.SetDefaultPayloadType(64) == 0);
	TASK_CHECK(builder.SetRTCPMultiplexing(true) < 0);
	TASK_CHECK(!builder.IsRTCPMultiplexing());
	TASK_CHECK(builder.BuildPacket("a", 1, 64, false, 1) == 0);
	u_int16_t seq_before = builder.GetSequenceNumber();
	u_int32_t ts_before = builder.GetTimestamp();
	u_int32_t count_before = builder.GetPacketCount();
	size_t len_before = builder.GetPacketLength();
	u_int8_t copy[1500];
	std::memcpy(copy, builder.GetPacket(), len_before);

	// Enable multiplexing on a fresh builder and check the guard bounds.
	RTPPacketBuilder muxed;
	TASK_CHECK(muxed.Init(1500) == 0);
	TASK_CHECK(muxed.SetDefaultPayloadType(96) == 0);
	TASK_CHECK(muxed.SetDefaultMark(true) == 0);
	TASK_CHECK(muxed.SetDefaultTimestampIncrement(3000) == 0);
	TASK_CHECK(muxed.SetRTCPMultiplexing(true) == 0);
	TASK_CHECK(muxed.IsRTCPMultiplexing());

	TASK_CHECK(muxed.SetDefaultPayloadType(63) == 0);
	TASK_CHECK(muxed.SetDefaultPayloadType(96) == 0);
	TASK_CHECK(muxed.SetDefaultPayloadType(64) < 0);
	TASK_CHECK(muxed.SetDefaultPayloadType(80) < 0);
	TASK_CHECK(muxed.SetDefaultPayloadType(95) < 0);
	TASK_CHECK(muxed.SetDefaultPayloadType(192) == 0);

	u_int16_t mseq = muxed.GetSequenceNumber();
	u_int32_t mts = muxed.GetTimestamp();
	u_int32_t mcount = muxed.GetPacketCount();
	size_t mlen = muxed.GetPacketLength();
	u_int8_t mcopy[1500];
	std::memcpy(mcopy, muxed.GetPacket(), mlen);
	const u_int8_t ext[] = { 0, 0, 0, 0 };

	TASK_CHECK(muxed.BuildPacket("x", 1, 64, false, 10) < 0);
	TASK_CHECK(muxed.BuildPacket("x", 1, 80, true, 10) < 0);
	TASK_CHECK(muxed.BuildPacket("x", 1, 95, false, 10) < 0);
	TASK_CHECK(muxed.BuildPacketEx("x", 1, 64, true, 10, 1, ext, 1) < 0);
	TASK_CHECK(muxed.BuildPacketEx("x", 1, 95, false, 10, 1, ext, 1) < 0);
	TASK_CHECK(muxed.GetSequenceNumber() == mseq);
	TASK_CHECK(muxed.GetTimestamp() == mts);
	TASK_CHECK(muxed.GetPacketCount() == mcount);
	TASK_CHECK(muxed.GetPacketLength() == mlen);
	TASK_CHECK(std::memcmp(mcopy, muxed.GetPacket(), mlen) == 0);

	// Marker does not matter for the restriction, and valid types still build.
	TASK_CHECK(muxed.BuildPacket("y", 1, 96, true, 10) == 0);
	TASK_CHECK(muxed.SetDefaultPayloadType(96) == 0);

	// Enabling a second time is idempotent; the default-PT variant of
	// BuildPacketEx with an allowed default still works.
	TASK_CHECK(muxed.SetRTCPMultiplexing(true) == 0);
	TASK_CHECK(muxed.BuildPacketEx("q", 1, 1, ext, 1) == 0);

	// Destroy returns to the default (disabled) mode.
	muxed.Destroy();
	TASK_CHECK(!muxed.IsRTCPMultiplexing());
	TASK_CHECK(muxed.Init(1500) == 0);
	TASK_CHECK(!muxed.IsRTCPMultiplexing());
	TASK_CHECK(muxed.SetDefaultPayloadType(70) == 0);
	muxed.Destroy();

	// The untouched builder from above is unchanged by failed operations.
	TASK_CHECK(builder.GetSequenceNumber() == seq_before);
	TASK_CHECK(builder.GetTimestamp() == ts_before);
	TASK_CHECK(builder.GetPacketCount() == count_before);
	TASK_CHECK(builder.GetPacketLength() == len_before);
	TASK_CHECK(std::memcmp(copy, builder.GetPacket(), len_before) == 0);

	// A builder whose existing default is outside the forbidden range may
	// enable multiplexing and then rejects only 64..95.
	RTPPacketBuilder fresh;
	TASK_CHECK(fresh.Init(1500) == 0);
	TASK_CHECK(fresh.SetDefaultPayloadType(97) == 0);
	TASK_CHECK(fresh.SetRTCPMultiplexing(true) == 0);
	TASK_CHECK(fresh.SetDefaultPayloadType(64) < 0);
	TASK_CHECK(fresh.SetDefaultPayloadType(95) < 0);
	TASK_CHECK(fresh.BuildPacket("z", 1) == 0); // default 97 still allowed
	fresh.Destroy();
	builder.Destroy();
	return 0;
}

// ---------------------------------------------------------------------------
// lifecycle: adjacent port, waiting, resource release, dual-port rebuild
// ---------------------------------------------------------------------------

int lifecycle_single_port()
{
	RTPUDPv4Transmitter tx;
	unsigned short port = 0;
	TASK_CHECK(find_free_single_port(&port) == 0);

	RTPUDPv4TransmissionParams params;
	TASK_CHECK(setup_transmitter(tx, port, true, params) == 0);
	TASK_CHECK(tx.IsRTCPMultiplexing());

	// The adjacent port was not consumed and can be bound by another socket.
	int adjacent = socket(AF_INET, SOCK_DGRAM, 0);
	TASK_CHECK(adjacent >= 0);
	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(LOOPBACK);
	a.sin_port = htons((unsigned short)(port + 1));
	TASK_CHECK(bind(adjacent, (sockaddr *)&a, sizeof a) == 0);
	close(adjacent);

	// The configured port itself is in use.
	int same = socket(AF_INET, SOCK_DGRAM, 0);
	TASK_CHECK(same >= 0);
	a.sin_port = htons(port);
	TASK_CHECK(bind(same, (sockaddr *)&a, sizeof a) != 0);
	close(same);

	// Wait for data, then fetch it exactly once.
	Received src;
	TASK_CHECK(make_receiver(&src) == 0);
	const u_int8_t rtp[] = { 0x80, 0x60, 0, 1, 0, 0, 0, 1, 9, 9, 9, 9 };
	const u_int8_t rr[] = { 0x80, 201, 0, 1, 0, 0, 0, 7 };
	TASK_CHECK(send_to(src.fd, port, rtp, sizeof rtp) == 0);
	TASK_CHECK(send_to(src.fd, port, rr, sizeof rr) == 0);

	TASK_CHECK(tx.WaitForIncomingData(RTPTime(1, 0)) == 0);
	std::vector<RTPRawPacket *> packets;
	TASK_CHECK(collect_packets(tx, 2, packets) == 0);
	TASK_CHECK(packets.size() == 2);
	TASK_CHECK(packets[0]->IsRTP());
	TASK_CHECK(!packets[1]->IsRTP());
	TASK_CHECK(packets[0]->GetDataLength() == sizeof rtp);
	TASK_CHECK(packets[1]->GetDataLength() == sizeof rr);
	TASK_CHECK(packets[0]->GetSenderAddress() != 0);
	TASK_CHECK(((RTPIPv4Address *)packets[0]->GetSenderAddress())->GetPort() == src.port);
	delete_packets(packets);

	// Polling again must not re-read the same descriptor's old datagrams.
	TASK_CHECK(tx.Poll() == 0);
	TASK_CHECK(tx.GetNextPacket() == 0);
	close(src.fd);

	// Destroy twice must be harmless.
	tx.Destroy();
	tx.Destroy();

	// Both ports are free again.
	for (int i = 0; i < 2; ++i) {
		int fd = socket(AF_INET, SOCK_DGRAM, 0);
		TASK_CHECK(fd >= 0);
		std::memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(LOOPBACK);
		a.sin_port = htons((unsigned short)(port + i));
		TASK_CHECK(bind(fd, (sockaddr *)&a, sizeof a) == 0);
		close(fd);
	}

	// Same object can be re-created in the original dual-port mode.
	RTPUDPv4TransmissionParams dualparams;
	unsigned short pairbase = 0;
	TASK_CHECK(find_free_port_pair(&pairbase) == 0);

	dualparams.SetBindIP(LOOPBACK);
	std::list<u_int32_t> ips;
	ips.push_back(LOOPBACK);
	dualparams.SetLocalIPList(ips);
	dualparams.SetPortbase(pairbase);
	TASK_CHECK(!dualparams.IsRTCPMultiplexing());
	TASK_CHECK(tx.Create(1500, &dualparams) == 0);
	TASK_CHECK(!tx.IsRTCPMultiplexing());

	Received src2;
	TASK_CHECK(make_receiver(&src2) == 0);
	const u_int8_t rr2[] = { 0x80, 201, 0, 1, 0, 0, 0, 8 };
	TASK_CHECK(send_to(src2.fd, (unsigned short)(pairbase + 1), rr2, sizeof rr2) == 0);
	std::vector<RTPRawPacket *> dualpackets;
	TASK_CHECK(collect_packets(tx, 1, dualpackets) == 0);
	TASK_CHECK(dualpackets.size() == 1);
	TASK_CHECK(!dualpackets[0]->IsRTP());
	delete_packets(dualpackets);
	close(src2.fd);
	tx.Destroy();
	return 0;
}

int lifecycle_mux_destroy_and_rebuild()
{
	// Odd port: allowed only in multiplexed mode.
	unsigned short port = 0;
	TASK_CHECK(find_free_single_port(&port) == 0);
	TASK_CHECK(port % 2 == 1);

	RTPUDPv4Transmitter tx;
	TASK_CHECK(tx.Init(false) == 0);

	RTPUDPv4TransmissionParams no_mux;
	no_mux.SetBindIP(LOOPBACK);
	std::list<u_int32_t> ips;
	ips.push_back(LOOPBACK);
	no_mux.SetLocalIPList(ips);
	no_mux.SetPortbase(port);
	TASK_CHECK(tx.Create(1500, &no_mux) < 0); // refused: odd p/p+1
	tx.Destroy();

	RTPUDPv4TransmissionParams mux;
	mux.SetBindIP(LOOPBACK);
	mux.SetLocalIPList(ips);
	mux.SetPortbase(port);
	mux.SetRTCPMultiplexing(true);
	TASK_CHECK(tx.Create(1500, &mux) == 0);
	tx.Destroy();

	// After Destroy, a dual-port session can be created on the same object.
	unsigned short base = 0;
	TASK_CHECK(find_free_port_pair(&base) == 0);
	RTPUDPv4TransmissionParams dual;
	dual.SetBindIP(LOOPBACK);
	dual.SetLocalIPList(ips);
	dual.SetPortbase(base);
	TASK_CHECK(tx.Create(1500, &dual) == 0);
	TASK_CHECK(!tx.IsRTCPMultiplexing());

	// RTP on p, RTCP on p+1 in the rebuilt non-multiplexed mode.
	Received src;
	TASK_CHECK(make_receiver(&src) == 0);
	const u_int8_t rtp[] = { 0x80, 0x60, 0, 1, 0, 0, 0, 1, 4, 5, 6, 7 };
	const u_int8_t rr[] = { 0x80, 201, 0, 1, 0, 0, 0, 9 };
	TASK_CHECK(send_to(src.fd, base, rtp, sizeof rtp) == 0);
	TASK_CHECK(send_to(src.fd, (unsigned short)(base + 1), rr, sizeof rr) == 0);
	std::vector<RTPRawPacket *> packets;
	TASK_CHECK(collect_packets(tx, 2, packets) == 0);
	TASK_CHECK(packets.size() == 2);
	TASK_CHECK(packets[0]->IsRTP());
	TASK_CHECK(!packets[1]->IsRTP());
	delete_packets(packets);
	close(src.fd);
	tx.Destroy();
	return 0;
}

} // namespace

static const struct task_case mux_cases[] = {
	{ "wire", "loopback_interleave", wire_loopback_interleave },
	{ "wire", "short_and_bad_version_ignored", wire_short_and_bad_version_ignored },
	{ "wire", "poisoned_port_fails", wire_poisoned_port_fails },
	{ "session", "payload_type_guard", session_payload_type_guard },
	{ "session", "default_and_builder_guard", session_default_and_builder_guard },
	{ "lifecycle", "single_port", lifecycle_single_port },
	{ "lifecycle", "mux_destroy_and_rebuild", lifecycle_mux_destroy_and_rebuild },
};

const struct task_case task_cases[] = {
	mux_cases[0], mux_cases[1], mux_cases[2], mux_cases[3],
	mux_cases[4], mux_cases[5], mux_cases[6],
};

const size_t task_case_count = sizeof(mux_cases) / sizeof(mux_cases[0]);
