// aac-au-example — a local, self-contained example of receiving an
// MPEG4-GENERIC (AAC) RTP stream whose packets carry several Access Units
// (AUs), including an AU that is fragmented across two packets.
//
// It builds a MediaSession from an SDP description (below), feeds the
// resulting MPEG4GenericRTPSource two hand-built RTP packets over a local
// UDP socket, and prints each delivered AU's bytes and presentation time.
//
// Build (from the repository root, after "sh task_support/install.sh"):
//   c++ -std=c++20 -DNO_OPENSSL=1 \
//     -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include \
//     -IBasicUsageEnvironment/include \
//     test/aac-au-example.cpp \
//     liveMedia/libliveMedia.a groupsock/libgroupsock.a \
//     BasicUsageEnvironment/libBasicUsageEnvironment.a \
//     UsageEnvironment/libUsageEnvironment.a -o .task-build/aac-au-example
// Run:
//   .task-build/aac-au-example
//
// SDP PARAMETERS (a=fmtp, per RFC 3640; names are case-insensitive, each
// defaults to 0 when absent):
//   mode               "AAC-hbr" or "generic" are supported here
//   sizelength         bits per AU-size field (0..32)
//   indexlength        bits for the first AU's AU-index (0..32)
//   indexdeltalength   bits per later AU's AU-index-delta (0..32)
//   constantduration   AU duration in RTP TIMESTAMP UNITS (e.g. 1024 ticks
//                      at 48 kHz == 21.333 ms); spaces same-packet AUs apart
//   ctsdeltalength     bits per optional signed CTS-delta (0..32)
//
// CLOCK: the first AU of each packet uses that packet's RTP timestamp as its
// anchor (mapped to a timeval through the library's usual RTP/RTCP sync).
// Later AUs are offset by (relative index * constantDuration) ticks, or by
// their own signed CTS-delta when the AU-header's CTS-flag is set.  Tick
// arithmetic wraps at 32 bits, like the RTP timestamp itself.
//
// UNSUPPORTED FIELDS: the optional DTS-delta, RAP-flag and Stream-state
// AU-header fields are not implemented.  Streams whose SDP sets
// "dtsDeltaLength", "randomAccessIndication" or "streamStateIndication"
// to a non-zero value are rejected at setup, rather than misreading those
// header bits as AU payload.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

// A minimal bit writer, to lay out the AU-header section:
struct BitWriter {
  std::vector<unsigned char> out;
  unsigned acc = 0;
  int nbits = 0;
  void put(unsigned value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      acc = (acc << 1) | ((value >> i) & 1);
      if (++nbits == 8) { out.push_back((unsigned char)acc); acc = 0; nbits = 0; }
    }
  }
  std::vector<unsigned char> finish() {
    if (nbits > 0) { out.push_back((unsigned char)(acc << (8 - nbits))); acc = 0; nbits = 0; }
    return out;
  }
};

static char const* const SDP =
  "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=aac-au-example\r\n"
  "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
  "m=audio 0 RTP/AVP 96\r\n"
  "a=rtpmap:96 MPEG4-GENERIC/48000/2\r\n"
  "a=fmtp:96 mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
  "indexdeltalength=3;constantduration=1024\r\n";

struct State {
  FramedSource* src;
  unsigned char buf[64];
  EventLoopWatchVariable done;
  struct timeval first;
  unsigned count;
};

static void printFrame(unsigned n, unsigned char const* data, unsigned size,
		       struct timeval pt, struct timeval const& first) {
  long relUs = (long)(pt.tv_sec - first.tv_sec) * 1000000L + (long)(pt.tv_usec - first.tv_usec);
  printf("AU #%u: %u bytes [", n, size);
  for (unsigned i = 0; i < size; ++i) printf("%s%02x", i ? " " : "", data[i]);
  printf("]  presentation=%ld.%06ld  (+%ld us)\n",
	 (long)pt.tv_sec, (long)pt.tv_usec, relUs);
}

static void afterFrame(void* clientData, unsigned frameSize, unsigned /*numTruncatedBytes*/,
		       struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  State& s = *(State*)clientData;
  if (s.count == 0) s.first = presentationTime;
  printFrame(s.count, s.buf, frameSize, presentationTime, s.first);
  if (++s.count == 2) { s.done = 1; return; } // we expect 2 AUs in total
  s.src->getNextFrame(s.buf, sizeof s.buf, afterFrame, &s, NULL, NULL);
}

static void onTimeout(void* clientData) { ((State*)clientData)->done = 1; }

static void sendRtp(int fd, struct sockaddr_in const& dst, unsigned short seq,
		    unsigned ts, bool marker, std::vector<unsigned char> const& payload) {
  std::vector<unsigned char> pkt(12);
  pkt[0] = 0x80;
  pkt[1] = marker ? 0xE0 : 0x60; // M + PT 96
  pkt[2] = (unsigned char)(seq >> 8); pkt[3] = (unsigned char)(seq & 0xFF);
  pkt[4] = (unsigned char)(ts >> 24); pkt[5] = (unsigned char)(ts >> 16);
  pkt[6] = (unsigned char)(ts >> 8);  pkt[7] = (unsigned char)ts;
  pkt[11] = 0x7B; // SSRC
  pkt.insert(pkt.end(), payload.begin(), payload.end());
  sendto(fd, (char const*)pkt.data(), pkt.size(), 0,
	 (struct sockaddr const*)&dst, (socklen_t)sizeof dst);
}

int main() {
  TaskScheduler* sched = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*sched);
  MediaSession* session = MediaSession::createNew(*env, SDP);
  if (session == NULL) { fprintf(stderr, "bad SDP\n"); return 1; }
  MediaSubsessionIterator iter(*session);
  MediaSubsession* sub = iter.next();
  if (sub == NULL || !sub->initiate()) { fprintf(stderr, "initiate failed\n"); return 1; }

  State s;
  s.src = sub->readSource();
  s.done = 0;
  s.count = 0;
  s.src->getNextFrame(s.buf, sizeof s.buf, afterFrame, &s, NULL, NULL);
  TaskToken timeout = sched->scheduleDelayedTask(3000000, onTimeout, &s);

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in dst;
  memset(&dst, 0, sizeof dst);
  dst.sin_family = AF_INET;
  dst.sin_port = htons(sub->clientPortNum());
  inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

  // Packet 1 (RTP timestamp 48000): AU #0 (2 bytes, complete) followed by
  // the first 3 bytes of AU #1 (a 6-byte AU; "M"=0 because AU #1 is
  // incomplete).  AU headers: AU #0 size=2 index=0; AU #1 size=6 delta=0.
  {
    BitWriter bw;
    bw.put(2, 13); bw.put(0, 3); // AU #0: size, index
    bw.put(6, 13); bw.put(0, 3); // AU #1: size, index-delta
    std::vector<unsigned char> p;
    unsigned bits = 2 * (13 + 3);
    p.push_back((unsigned char)(bits >> 8)); p.push_back((unsigned char)bits);
    std::vector<unsigned char> hdr = bw.finish();
    p.insert(p.end(), hdr.begin(), hdr.end());
    unsigned char au0[] = {0xAA, 0xBB};
    unsigned char au1part[] = {0xC1, 0xC2, 0xC3};
    p.insert(p.end(), au0, au0 + 2);
    p.insert(p.end(), au1part, au1part + 3);
    sendRtp(fd, dst, 1, 48000, false, p);
  }
  // Packet 2 (same RTP timestamp: same AU): the remaining 3 bytes of AU #1
  // ("M"=1: the AU is now complete).  Its AU header repeats the full size 6.
  {
    BitWriter bw;
    bw.put(6, 13); bw.put(0, 3);
    std::vector<unsigned char> p;
    unsigned bits = 13 + 3;
    p.push_back((unsigned char)(bits >> 8)); p.push_back((unsigned char)bits);
    std::vector<unsigned char> hdr = bw.finish();
    p.insert(p.end(), hdr.begin(), hdr.end());
    unsigned char rest[] = {0xC4, 0xC5, 0xC6};
    p.insert(p.end(), rest, rest + 3);
    sendRtp(fd, dst, 2, 48000, true, p);
  }

  sched->doEventLoop(&s.done);
  sched->unscheduleDelayedTask(timeout);
  close(fd);

  if (s.count == 2) {
    printf("Expected: AU #1 (reassembled from both packets) is presented one\n"
	   "constantDuration (1024 ticks = 21333 us) after AU #0, keeping the\n"
	   "presentation time computed from its first fragment.\n");
  } else {
    printf("Only %u AU(s) arrived (timeout)\n", s.count);
  }
  Medium::close(session);
  env->reclaim();
  delete sched;
  return s.count == 2 ? 0 : 1;
}
