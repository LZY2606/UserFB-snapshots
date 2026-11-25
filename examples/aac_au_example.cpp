// Local end-to-end example for MPEG4-GENERIC AU timing.
//
// It creates a real MediaSession receiver on loopback UDP, sends two RTP
// packets containing AAC-like AUs through the normal live555 receive chain,
// and prints each delivered frame's time relative to the first frame.
//
// Build/run with:
//   sh task_support/run_example.sh
//
// The bytes are deliberately tiny patterns, not playable AAC frames.  The
// example demonstrates depacketization and timing without opening a sound
// device or adding a network service.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct BitWriter {
  std::vector<uint8_t> bytes{std::vector<uint8_t>(16, 0)};
  unsigned bits = 0;

  void put(uint32_t value, unsigned n) {
    for (int b = int(n) - 1; b >= 0; --b) {
      unsigned const i = bits++;
      if (i / 8 >= bytes.size()) bytes.resize(bytes.size() * 2);
      if ((value >> b) & 1U) bytes[i / 8] |= 0x80U >> (i % 8);
    }
  }

  std::vector<uint8_t> finish() {
    bytes.resize((bits + 7) / 8);
    return bytes;
  }
};

struct AU {
  std::vector<uint8_t> data;
  uint32_t indexOrDelta;
};

static std::vector<uint8_t> auHeaderBits(std::vector<AU> const& aus,
					 unsigned& bitLength) {
  BitWriter w;
  for (unsigned i = 0; i < aus.size(); ++i) {
    w.put(uint32_t(aus[i].data.size()), 13); // SizeLength
    w.put(aus[i].indexOrDelta, i == 0 ? 3U : 3U); // Index[Delta]Length
    // This example leaves CTSDeltaLength unset, so there is no CTS flag.
  }
  bitLength = w.bits;
  return w.finish();
}

static std::vector<uint8_t> rtpPacket(uint16_t seq, uint32_t ts, bool marker,
				      std::vector<AU> const& aus) {
  unsigned headerBits = 0;
  std::vector<uint8_t> headerBytes = auHeaderBits(aus, headerBits);

  std::vector<uint8_t> p;
  p.push_back(0x80);
  p.push_back(uint8_t((marker ? 0x80 : 0x00) | 96));
  p.push_back(seq >> 8);
  p.push_back(seq & 255);
  for (int s = 24; s >= 0; s -= 8) p.push_back(uint8_t(ts >> s));
  p.insert(p.end(), {0, 0, 0, 0x42});

  // AU-headers-length is in bits.
  p.push_back(headerBits >> 8);
  p.push_back(headerBits & 255);
  p.insert(p.end(), headerBytes.begin(), headerBytes.end());
  for (AU const& au : aus) p.insert(p.end(), au.data.begin(), au.data.end());
  return p;
}

struct Receiver {
  TaskScheduler* scheduler;
  UsageEnvironment* env;
  MediaSession* session;
  FramedSource* source = nullptr;
  EventLoopWatchVariable done{0};
  TaskToken timer = nullptr;
  unsigned char buffer[4096];
  unsigned received = 0;
  timeval firstTime{};
};

static void onTimeout(void* p) {
  (void)p;
  std::fprintf(stderr, "example timed out waiting for frames\n");
  std::exit(1);
}

static void onFrame(void* clientData, unsigned frameSize, unsigned trunc,
		    timeval t, unsigned /*duration*/) {
  Receiver* r = static_cast<Receiver*>(clientData);
  if (r->received == 0) r->firstTime = t;

  long long deltaUs = 0;
  if (r->received != 0) {
    deltaUs = (long long)(t.tv_sec - r->firstTime.tv_sec) * 1000000LL
      + (t.tv_usec - r->firstTime.tv_usec);
  }

  std::printf("frame %u: size=%u trunc=%u time_delta_us=%lld bytes=",
	      r->received + 1, frameSize, trunc, deltaUs);
  for (unsigned i = 0; i < frameSize && i < 12; ++i) {
    std::printf("%02x%s", r->buffer[i], i + 1 == frameSize ? "" : " ");
  }
  if (frameSize > 12) std::printf(" ...");
  std::printf("\n");

  ++r->received;
  if (r->received == 3) {
    r->done = 1;
  } else {
    r->source->getNextFrame(r->buffer, sizeof r->buffer, onFrame, r,
			    nullptr, nullptr);
  }
}

int main() {
  std::printf("MPEG4-GENERIC AU timing example\n");
  std::printf("SDP: a=rtpmap:96 MPEG4-GENERIC/48000/2\n");
  std::printf("SDP: a=fmtp:96 mode=AAC-hbr;config=1190;"
	      "sizelength=13;indexlength=3;indexdeltalength=3;"
	      "constantDuration=1024\n");
  std::printf("Clock: 48000 RTP ticks/second; 1024 ticks = 21333 us "
	      "(integer conversion)\n");
  std::printf("CTSDeltaLength: optional; when nonzero each AU header has a "
	      "CTS-flag and flag=1 adds signed two's-complement CTS-delta.\n");
  std::printf("Not extended: DTSDeltaLength, RandomAccessIndication/RAP, and "
	      "StreamStateIndication; nonzero SDP values are rejected.\n\n");

  const char* sdp =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=aac-au-example\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 MPEG4-GENERIC/48000/2\r\n"
    "a=fmtp:96 mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
    "indexdeltalength=3;constantDuration=1024\r\n";

  Receiver r;
  r.scheduler = BasicTaskScheduler::createNew();
  r.env = BasicUsageEnvironment::createNew(*r.scheduler);
  r.session = MediaSession::createNew(*r.env, sdp);
  if (!r.session) return 2;

  MediaSubsessionIterator it(*r.session);
  MediaSubsession* sub = it.next();
  if (!sub || !sub->initiate()) {
    std::fprintf(stderr, "initiate failed: %s\n"
		  "If local UDP binding is blocked, enable local binding and "
		  "rerun task_support/run_example.sh\n",
		  r.env->getResultMsg());
    return 2;
  }
  r.source = sub->readSource();

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(sub->clientPortNum());
  inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

  // Packet 1: two complete AUs.  Packet 2: one complete AU whose RTP timestamp
  // continues after the two 1024-tick AUs.
  std::vector<AU> two = {
    {{0xaa, 0xbb}, 0},
    {{0xcc, 0xdd, 0xee}, 0} // delta 0 => relative sequence 1
  };
  std::vector<AU> one = {
    {{0x11, 0x22, 0x33, 0x44}, 0}
  };
  auto p1 = rtpPacket(1, 0x90000000, true, two);
  auto p2 = rtpPacket(2, 0x90000000U + 2048, true, one);

  r.source->getNextFrame(r.buffer, sizeof r.buffer, onFrame, &r,
			 nullptr, nullptr);
  r.timer = r.scheduler->scheduleDelayedTask(2000000, onTimeout, &r);

  sendto(fd, p1.data(), p1.size(), 0, (sockaddr*)&dst, sizeof dst);
  sendto(fd, p2.data(), p2.size(), 0, (sockaddr*)&dst, sizeof dst);
  r.scheduler->doEventLoop(&r.done);

  r.scheduler->unscheduleDelayedTask(r.timer);
  close(fd);
  Medium::close(r.session);
  r.env->reclaim();
  delete r.scheduler;

  std::printf("\nExpected deltas: frame1=0 us, frame2=21333 us, "
	      "frame3=42666 us (direct tick conversion, no accumulated "
	      "rounding).\n");
  return r.received == 3 ? 0 : 1;
}
