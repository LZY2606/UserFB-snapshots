// multissrc.cpp — regression tests for H264VideoRTPSource multi-SSRC reception.
//
// Every case drives the REAL receiver over a local UDP socket: a Groupsock
// bound to a loopback ephemeral port feeds H264VideoRTPSource through the
// normal BasicTaskScheduler event loop, and a plain UDP sender injects RTP
// packets carrying several interleaved SSRCs (all payload type 96).  The
// assertions inspect what the getNextFrame() callback actually delivered:
// SSRC, sequence number, RTP timestamp, marker bit, RTCP sync flag and NAL
// bytes.
//
// Groups (consumed by task_support/driver.c):
//   isolation — two interleaved sources keep independent reorder/FU-A state,
//               STAP-A ordering, duplicates, 65535->0 wrap
//   loss      — one source's incomplete/lost fragments never block the other,
//               and broken FU-A NALs are never emitted while recovery works
//   clock     — an RTCP Sender Report for one SSRC synchronises only that
//               source's presentation clock
//   lifecycle — stopGettingFrames()/re-read and close/recreate release all
//               per-SSRC state without rebinding the socket

#include "../task_support/check.h"
#include "H264VideoRTPSource.hh"
#include "RTCP.hh"
#include "BasicUsageEnvironment.hh"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

unsigned const H264_PAYLOAD_TYPE = 96;
u_int32_t const SSRC_A = 0xAAAAAAAAu;
u_int32_t const SSRC_B = 0xBBBBBBBBu;

// Everything the getNextFrame() callback observed for one delivered NAL.
struct DeliveredNAL {
  u_int32_t ssrc;
  u_int16_t seqNum;
  u_int32_t rtpTimestamp;
  Boolean marker;
  Boolean rtcpSynced;
  struct timeval presentationTime;
  std::vector<unsigned char> bytes;
};

// One bound receiver + UDP sender, torn down at the end of each case.
struct Harness {
  TaskScheduler* scheduler;
  BasicUsageEnvironment* env;
  Groupsock* gs;
  H264VideoRTPSource* source;
  int senderFd;
  sockaddr_in destination;

  EventLoopWatchVariable done;
  unsigned wantedDeliveries;
  std::vector<DeliveredNAL> delivered;
  unsigned char frameBuffer[8192];
  TaskToken timeoutTask;

  static void onTimeout(void* clientData) {
    Harness* h = static_cast<Harness*>(clientData);
    h->done = 1;
  }

  static void afterGetting(void* clientData, unsigned frameSize,
			   unsigned /*numTruncatedBytes*/,
			   struct timeval presentationTime,
			   unsigned /*durationInMicroseconds*/) {
    Harness* h = static_cast<Harness*>(clientData);
    DeliveredNAL nal;
    nal.ssrc = h->source->curPacketSSRC();
    nal.seqNum = h->source->curPacketRTPSeqNum();
    nal.rtpTimestamp = h->source->curPacketRTPTimestamp();
    nal.marker = h->source->curPacketMarkerBit();
    nal.rtcpSynced = h->source->hasBeenSynchronizedUsingRTCP();
    nal.presentationTime = presentationTime;
    nal.bytes.assign(h->frameBuffer, h->frameBuffer + frameSize);
    h->delivered.push_back(nal);

    if (h->delivered.size() >= h->wantedDeliveries) {
      h->done = 1;
      return;
    }
    h->source->getNextFrame(h->frameBuffer, sizeof h->frameBuffer,
			    afterGetting, h, nullptr, nullptr);
  }

  Harness()
    : scheduler(BasicTaskScheduler::createNew()),
      env(BasicUsageEnvironment::createNew(*scheduler)), gs(nullptr),
      source(nullptr), senderFd(-1), done(0), wantedDeliveries(0),
      timeoutTask(nullptr) {
    sockaddr_storage address;
    memset(&address, 0, sizeof address);
    sockaddr_in& ipv4 = reinterpret_cast<sockaddr_in&>(address);
    ipv4.sin_family = AF_INET;
    ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    gs = new Groupsock(*env, address, Port(0), 1);
    if (::bind(gs->socketNum(), reinterpret_cast<sockaddr*>(&ipv4),
	       sizeof ipv4) != 0) {
      // Mirrors baseline.cpp: the sandbox/grader must permit loopback binds.
      std::fprintf(stderr, "harness: bind failed: %s\n", std::strerror(errno));
    }
    sockaddr_in bound;
    socklen_t length = sizeof bound;
    memset(&bound, 0, sizeof bound);
    if (getsockname(gs->socketNum(), reinterpret_cast<sockaddr*>(&bound),
		    &length) != 0) {
      std::fprintf(stderr, "harness: getsockname failed: %s\n",
		   std::strerror(errno));
    }
    bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination = bound;

    senderFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    source = H264VideoRTPSource::createNew(*env, gs, H264_PAYLOAD_TYPE);
    source->enableMultiSSRCReception(True);
  }

  ~Harness() {
    if (timeoutTask != nullptr) scheduler->unscheduleDelayedTask(timeoutTask);
    Medium::close(source);
    if (senderFd >= 0) ::close(senderFd);
    delete gs;
    env->reclaim();
    delete scheduler;
  }

  void arm(unsigned wanted) {
    wantedDeliveries = wanted;
    done = 0;
    delivered.clear();
    source->getNextFrame(frameBuffer, sizeof frameBuffer, afterGetting, this,
			 nullptr, nullptr);
  }

  // Runs the event loop until "wantedDeliveries" arrived or the timeout fires.
  void pump(unsigned timeoutUSeconds = 1000000) {
    timeoutTask = scheduler->scheduleDelayedTask(timeoutUSeconds, onTimeout,
						  this);
    scheduler->doEventLoop(&done);
    scheduler->unscheduleDelayedTask(timeoutTask);
    timeoutTask = nullptr;
  }

  void sendRtp(u_int32_t ssrc, u_int16_t seq, u_int32_t rtpTimestamp,
	       Boolean marker, std::vector<unsigned char> const& payload) {
    std::vector<unsigned char> packet(12 + payload.size());
    packet[0] = 0x80; // RTP version 2
    packet[1] = static_cast<unsigned char>(H264_PAYLOAD_TYPE
					  | (marker ? 0x80 : 0x00));
    packet[2] = static_cast<unsigned char>(seq >> 8);
    packet[3] = static_cast<unsigned char>(seq & 0xFF);
    packet[4] = static_cast<unsigned char>(rtpTimestamp >> 24);
    packet[5] = static_cast<unsigned char>(rtpTimestamp >> 16);
    packet[6] = static_cast<unsigned char>(rtpTimestamp >> 8);
    packet[7] = static_cast<unsigned char>(rtpTimestamp & 0xFF);
    packet[8] = static_cast<unsigned char>(ssrc >> 24);
    packet[9] = static_cast<unsigned char>(ssrc >> 16);
    packet[10] = static_cast<unsigned char>(ssrc >> 8);
    packet[11] = static_cast<unsigned char>(ssrc & 0xFF);
    memmove(&packet[12], payload.data(), payload.size());
    ssize_t sent = ::sendto(senderFd, packet.data(), packet.size(), 0,
			    reinterpret_cast<sockaddr const*>(&destination),
			    sizeof destination);
    if (sent != static_cast<ssize_t>(packet.size())) {
      std::fprintf(stderr, "harness: sendto failed: %s\n",
		   std::strerror(errno));
    }
  }

  // FU-A payload fragments for an original NAL unit of "nalType".
  static std::vector<unsigned char> fua(unsigned char nalType,
					unsigned char fuBits,
					std::vector<unsigned char> frag) {
    std::vector<unsigned char> p;
    p.push_back(0x7C); // FU indicator: type 28 (FU-A), NRI 3
    p.push_back(static_cast<unsigned char>((fuBits & 0xE0)
					   | (nalType & 0x1F)));
    p.insert(p.end(), frag.begin(), frag.end());
    return p;
  }
};

bool bytesEqual(std::vector<unsigned char> const& got,
		std::vector<unsigned char> const& expected) {
  return got.size() == expected.size()
    && memcmp(got.data(), expected.data(), got.size()) == 0;
}

////////// isolation group //////////

// Two SSRCs interleaved on the one channel: each NAL is attributed to the
// SSRC/sequence/timestamp of the packet that carried it.
int test_interleaved_single_nal() {
  Harness h;
  h.arm(4);
  h.sendRtp(SSRC_A, 1, 1000, True, {0x65, 0xA1});
  h.sendRtp(SSRC_B, 1, 2000, True, {0x67, 0xB1});
  h.sendRtp(SSRC_A, 2, 1100, True, {0x65, 0xA2});
  h.sendRtp(SSRC_B, 2, 2100, True, {0x67, 0xB2});
  h.pump();

  TASK_CHECK(h.delivered.size() == 4);
  TASK_CHECK(h.delivered[0].ssrc == SSRC_A && h.delivered[0].seqNum == 1
	     && h.delivered[0].rtpTimestamp == 1000);
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x65, 0xA1}));
  TASK_CHECK(h.delivered[1].ssrc == SSRC_B && h.delivered[1].seqNum == 1
	     && h.delivered[1].rtpTimestamp == 2000);
  TASK_CHECK(h.delivered[2].ssrc == SSRC_A && h.delivered[2].seqNum == 2);
  TASK_CHECK(h.delivered[3].ssrc == SSRC_B && h.delivered[3].seqNum == 2);
  return 0;
}

// A's FU-A start is held open while B's complete NAL delivers first; when A's
// fragments complete, A delivers with the start fragment's timestamp and the
// end fragment's sequence number/marker.
int test_fua_interleaved_order_and_metadata() {
  Harness h;
  h.arm(2);
  h.sendRtp(SSRC_A, 10, 5000, False,
	    h.fua(5, 0x80, {0x11, 0x22}));            // FU-A start
  h.sendRtp(SSRC_B, 3, 9000, True, {0x67, 0xDE});   // B ready immediately
  h.sendRtp(SSRC_A, 11, 5000, False,
	    h.fua(5, 0x00, {0x33}));                // middle
  h.sendRtp(SSRC_A, 12, 5000, True,
	    h.fua(5, 0x40, {0x44}));                // end
  h.pump();

  TASK_CHECK(h.delivered.size() == 2);
  // B must not wait for A's fragment to finish:
  TASK_CHECK(h.delivered[0].ssrc == SSRC_B);
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x67, 0xDE}));

  DeliveredNAL const& a = h.delivered[1];
  TASK_CHECK(a.ssrc == SSRC_A);
  TASK_CHECK(a.seqNum == 12);                 // sequence from END fragment
  TASK_CHECK(a.rtpTimestamp == 5000);         // timestamp from START fragment
  TASK_CHECK(a.marker == True);               // marker from END fragment
  TASK_CHECK(bytesEqual(a.bytes, {0x65, 0x11, 0x22, 0x33, 0x44}));
  return 0;
}

// NAL units inside a STAP-A packet keep their in-packet order and inherit the
// carrying packet's SSRC/sequence/timestamp.
int test_stap_a_keeps_packet_order() {
  Harness h;
  h.arm(2);
  // STAP-A header (0x18), then (size 2, NAL 0x61 0xAA), (size 1, NAL 0x41):
  h.sendRtp(SSRC_B, 4, 9100, True,
	    {0x18, 0x00, 0x02, 0x61, 0xAA, 0x00, 0x01, 0x41});
  h.pump();

  TASK_CHECK(h.delivered.size() == 2);
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x61, 0xAA}));
  TASK_CHECK(bytesEqual(h.delivered[1].bytes, {0x41}));
  TASK_CHECK(h.delivered[0].ssrc == SSRC_B && h.delivered[1].ssrc == SSRC_B);
  TASK_CHECK(h.delivered[0].seqNum == 4 && h.delivered[1].seqNum == 4);
  TASK_CHECK(h.delivered[0].rtpTimestamp == 9100
	     && h.delivered[1].rtpTimestamp == 9100);
  return 0;
}

// Reordering and duplicate suppression are maintained per SSRC.
int test_per_source_reorder_and_duplicates() {
  Harness h;
  h.arm(5);
  h.sendRtp(SSRC_A, 1, 100, True, {0x65, 0x01}); // establish expectation
  h.sendRtp(SSRC_A, 2, 200, True, {0x65, 0x02});
  h.sendRtp(SSRC_A, 4, 400, True, {0x65, 0x04}); // gap: A now waits for seq 3
  h.sendRtp(SSRC_B, 7, 700, True, {0x67, 0xB7}); // B unaffected by A's gap
  h.sendRtp(SSRC_A, 3, 300, True, {0x65, 0x03}); // fills A's gap out of order
  h.sendRtp(SSRC_A, 3, 300, True, {0x65, 0x03}); // duplicate: must be ignored
  h.pump();

  TASK_CHECK(h.delivered.size() == 5); // 4 A NALs + 1 B NAL, duplicate dropped
  // B was ready while A waited, so it comes out before A's seq 3/4:
  TASK_CHECK(h.delivered[0].ssrc == SSRC_A && h.delivered[0].seqNum == 1);
  TASK_CHECK(h.delivered[1].ssrc == SSRC_A && h.delivered[1].seqNum == 2);
  TASK_CHECK(h.delivered[2].ssrc == SSRC_B && h.delivered[2].seqNum == 7);
  TASK_CHECK(h.delivered[3].ssrc == SSRC_A && h.delivered[3].seqNum == 3);
  TASK_CHECK(h.delivered[4].ssrc == SSRC_A && h.delivered[4].seqNum == 4);
  TASK_CHECK(bytesEqual(h.delivered[4].bytes, {0x65, 0x04}));
  return 0;
}

// Sequence-number wrap from 65535 to 0 is handled with each source's own
// 16-bit sequence space.
int test_sequence_wrap() {
  Harness h;
  h.arm(4);
  h.sendRtp(SSRC_A, 65534, 600, True, {0x65, 0xFE});
  h.sendRtp(SSRC_A, 65535, 700, True, {0x65, 0xFF});
  h.sendRtp(SSRC_A, 0, 800, True, {0x65, 0x00});
  h.sendRtp(SSRC_A, 1, 900, True, {0x65, 0x01});
  h.pump();

  TASK_CHECK(h.delivered.size() == 4);
  TASK_CHECK(h.delivered[0].seqNum == 65534);
  TASK_CHECK(h.delivered[1].seqNum == 65535);
  TASK_CHECK(h.delivered[2].seqNum == 0);
  TASK_CHECK(h.delivered[3].seqNum == 1);
  TASK_CHECK(bytesEqual(h.delivered[2].bytes, {0x65, 0x00}));
  return 0;
}

////////// loss group //////////

// While A holds an incomplete FU-A NAL, B's complete NALs keep flowing; A's
// wait (or loss) can never stall B.
int test_waiting_source_does_not_block_other() {
  Harness h;
  h.arm(3);
  h.sendRtp(SSRC_A, 1, 5000, False, h.fua(5, 0x80, {0x11})); // A start only
  h.sendRtp(SSRC_B, 1, 9000, True, {0x67, 0xB1});
  h.sendRtp(SSRC_B, 2, 9100, True, {0x67, 0xB2});
  h.sendRtp(SSRC_B, 3, 9200, True, {0x67, 0xB3});
  h.pump();

  TASK_CHECK(h.delivered.size() == 3);
  for (unsigned i = 0; i < 3; ++i) TASK_CHECK(h.delivered[i].ssrc == SSRC_B);

  // Now finish A: it delivers without disturbing B's counters.
  h.arm(1);
  h.sendRtp(SSRC_A, 2, 5000, False, h.fua(5, 0x00, {0x22}));
  h.sendRtp(SSRC_A, 3, 5000, True, h.fua(5, 0x40, {0x33}));
  h.pump();
  TASK_CHECK(h.delivered.size() == 1);
  TASK_CHECK(h.delivered[0].ssrc == SSRC_A);
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x65, 0x11, 0x22, 0x33}));
  return 0;
}

// A FU-A NAL whose middle fragment is lost must never be emitted; the next
// complete NAL from that source must still be delivered (recovery).
int test_missing_fua_middle_no_nal_then_recovers() {
  Harness h;
  // Don't depend on wall-clock timing: a sequence gap is given up on at once.
  h.source->setPacketReorderingThresholdTime(0);
  h.arm(1);
  h.sendRtp(SSRC_A, 2, 7000, False, h.fua(5, 0x80, {0xAA})); // start
  // seq 3 (middle) deliberately not sent
  h.sendRtp(SSRC_A, 4, 7000, True, h.fua(5, 0x40, {0xBB}));  // end after gap
  h.sendRtp(SSRC_A, 5, 7100, True, {0x65, 0xCC});            // complete NAL
  h.pump(500000);

  TASK_CHECK(h.delivered.size() == 1);
  TASK_CHECK(h.delivered[0].ssrc == SSRC_A);
  TASK_CHECK(h.delivered[0].seqNum == 5); // broken FU-A suppressed, NAL recovered
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x65, 0xCC}));
  return 0;
}

// FU-A continuation/end fragments arriving with no start fragment cannot build
// a valid NAL; a following complete NAL still delivers.
int test_missing_fua_start_recovers() {
  Harness h;
  h.arm(1);
  h.sendRtp(SSRC_A, 2, 7000, False, h.fua(5, 0x00, {0xAA})); // no start first
  h.sendRtp(SSRC_A, 3, 7000, True, h.fua(5, 0x40, {0xBB}));
  h.sendRtp(SSRC_A, 4, 7100, True, {0x65, 0xCC});
  h.pump();

  TASK_CHECK(h.delivered.size() == 1);
  TASK_CHECK(h.delivered[0].seqNum == 4);
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x65, 0xCC}));
  return 0;
}

////////// clock group //////////

// An RTCP Sender Report addressed to SSRC A synchronises A's presentation
// clock only; SSRC B's clock must remain untouched.
int test_rtcp_sr_clock_isolation() {
  Harness h;

  RTCPInstance* rtcp = RTCPInstance::createNew(
    *h.env, h.gs, 1024, reinterpret_cast<unsigned char const*>("multissrc-test"),
    nullptr /* sink */, h.source /* source */);
  TASK_CHECK(rtcp != nullptr);

  h.arm(2);
  h.sendRtp(SSRC_A, 1, 30000, True, {0x65, 0xA1});
  h.sendRtp(SSRC_B, 1, 80000, True, {0x67, 0xB1});
  h.pump();
  TASK_CHECK(h.delivered.size() == 2);
  TASK_CHECK(h.delivered[0].rtcpSynced == False);
  TASK_CHECK(h.delivered[1].rtcpSynced == False);

  // Inject an SR for SSRC A: NTP epoch second 1700000000, mapping RTP ts 30000.
  u_int32_t const unixSecond = 1700000000u;
  u_int32_t const ntpMsw = unixSecond + 2208988800u; // 1900->1970 offset
  unsigned char sr[28];
  memset(sr, 0, sizeof sr);
  sr[0] = 0x80;          // V=2, RC=0
  sr[1] = 200;           // RTCP SR
  sr[2] = 0x00; sr[3] = 0x06; // length: 7 words - 1
  sr[4] = (SSRC_A >> 24) & 0xFF; sr[5] = (SSRC_A >> 16) & 0xFF;
  sr[6] = (SSRC_A >> 8) & 0xFF;  sr[7] = SSRC_A & 0xFF;
  sr[8] = (ntpMsw >> 24) & 0xFF; sr[9] = (ntpMsw >> 16) & 0xFF;
  sr[10] = (ntpMsw >> 8) & 0xFF; sr[11] = ntpMsw & 0xFF;
  // NTP LSW = 0; RTP timestamp = 30000:
  sr[16] = 0x00; sr[17] = 0x00; sr[18] = 0x75; sr[19] = 0x30;
  sockaddr_storage from;
  memset(&from, 0, sizeof from);
  rtcp->injectReport(sr, sizeof sr, from);

  h.arm(2);
  h.sendRtp(SSRC_A, 2, 30000, True, {0x65, 0xA2});
  h.sendRtp(SSRC_B, 2, 80100, True, {0x67, 0xB2});
  h.pump();
  TASK_CHECK(h.delivered.size() == 2);

  DeliveredNAL const& a = h.delivered[0];
  DeliveredNAL const& b = h.delivered[1];
  TASK_CHECK(a.ssrc == SSRC_A && b.ssrc == SSRC_B);
  TASK_CHECK(a.rtcpSynced == True);   // A's SR synchronised A
  TASK_CHECK(b.rtcpSynced == False);  // ... and must not have touched B
  TASK_CHECK(a.presentationTime.tv_sec == static_cast<time_t>(unixSecond));
  TASK_CHECK(a.rtpTimestamp == 30000);
  TASK_CHECK(b.rtpTimestamp == 80100);

  Medium::close(rtcp);
  return 0;
}

////////// lifecycle group //////////

// stopGettingFrames() drops undelivered fragments, reordering packets and
// ready NALs; a re-read starts from empty state.
int test_stop_and_reread_clears_state() {
  Harness h;
  h.arm(1);
  h.sendRtp(SSRC_A, 1, 5000, False, h.fua(5, 0x80, {0x11})); // open fragment
  h.sendRtp(SSRC_B, 1, 9000, True, {0x67, 0xB1});            // delivered
  h.pump();
  TASK_CHECK(h.delivered.size() == 1);
  TASK_CHECK(h.delivered[0].ssrc == SSRC_B);

  h.source->stopGettingFrames(); // must discard A's open FU-A and all queues

  // A stale FU-A END for the abandoned fragment must NOT resurrect a NAL,
  // because the fragment state was cleared:
  h.arm(1);
  h.sendRtp(SSRC_A, 2, 5000, True, h.fua(5, 0x40, {0x22}));
  h.sendRtp(SSRC_A, 3, 5100, True, {0x65, 0xCC}); // fresh complete NAL
  h.pump(500000);

  TASK_CHECK(h.delivered.size() == 1);
  TASK_CHECK(h.delivered[0].ssrc == SSRC_A);
  TASK_CHECK(h.delivered[0].seqNum == 3);
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x65, 0xCC}));
  return 0;
}

// Closing the receiver releases every per-SSRC structure; a new receiver on
// the same Groupsock works without rebinding or opening another socket.
int test_close_and_recreate_keeps_socket() {
  Harness h;
  int const fd = h.gs->socketNum();

  h.arm(1);
  h.sendRtp(SSRC_A, 1, 1000, False, h.fua(5, 0x80, {0x11})); // incomplete FU-A
  h.sendRtp(SSRC_A, 1, 1000, False, h.fua(5, 0x80, {0x11})); // duplicate
  h.pump(300000);
  TASK_CHECK(h.delivered.empty()); // nothing complete was deliverable

  Medium::close(h.source);
  h.source = H264VideoRTPSource::createNew(*h.env, h.gs, H264_PAYLOAD_TYPE);
  h.source->enableMultiSSRCReception(True);
  TASK_CHECK(h.gs->socketNum() == fd); // same channel, no rebind/new socket

  h.arm(1);
  h.sendRtp(SSRC_B, 50, 4000, True, {0x67, 0xDD});
  h.pump();
  TASK_CHECK(h.delivered.size() == 1);
  TASK_CHECK(h.delivered[0].ssrc == SSRC_B);
  TASK_CHECK(h.delivered[0].seqNum == 50);
  TASK_CHECK(bytesEqual(h.delivered[0].bytes, {0x67, 0xDD}));
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
  {"isolation", "interleaved_single_nal", test_interleaved_single_nal},
  {"isolation", "fua_interleaved_order_and_metadata",
   test_fua_interleaved_order_and_metadata},
  {"isolation", "stap_a_keeps_packet_order", test_stap_a_keeps_packet_order},
  {"isolation", "per_source_reorder_and_duplicates",
   test_per_source_reorder_and_duplicates},
  {"isolation", "sequence_wrap", test_sequence_wrap},
  {"loss", "waiting_source_does_not_block_other",
   test_waiting_source_does_not_block_other},
  {"loss", "missing_fua_middle_no_nal_then_recovers",
   test_missing_fua_middle_no_nal_then_recovers},
  {"loss", "missing_fua_start_recovers", test_missing_fua_start_recovers},
  {"clock", "rtcp_sr_clock_isolation", test_rtcp_sr_clock_isolation},
  {"lifecycle", "stop_and_reread_clears_state",
   test_stop_and_reread_clears_state},
  {"lifecycle", "close_and_recreate_keeps_socket",
   test_close_and_recreate_keeps_socket},
};

const size_t task_case_count
  = sizeof(task_cases) / sizeof(task_cases[0]);
