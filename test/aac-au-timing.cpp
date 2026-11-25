// Real receive-chain tests for multi-AU MPEG4-GENERIC/RTP.
//
// The packet writer below is deliberately independent of the production
// AU-header parser.  It writes bits exactly as a sender would, while the
// assertions observe only bytes/truncation/timevals delivered by a real
// MediaSession -> MPEG4GenericRTPSource -> MultiFramedRTPSource chain over
// loopback UDP.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_assertions = 0;

#define AAC_CHECK(cond)                                                       \
  do {                                                                        \
    ++g_assertions;                                                           \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "AAC check failed: %s (%s:%d)\n", #cond,          \
                    __FILE__, __LINE__);                                      \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

struct Widths {
  unsigned sizeLength = 13;
  unsigned indexLength = 3;
  unsigned indexDeltaLength = 3;
  unsigned ctsDeltaLength = 0;
};

struct HeaderSpec {
  uint32_t size = 0;
  uint32_t indexOrDelta = 0;
  bool ctsFlag = false;
  int32_t ctsDelta = 0;
};

struct WrittenHeaders {
  std::vector<uint8_t> bytes;
  unsigned bitLength = 0;
};

class BitWriter {
public:
  std::vector<uint8_t> bytes;
  unsigned bitCount = 0;

  BitWriter() { bytes.resize(16); }

  void put(uint32_t value, unsigned numBits) {
    if (numBits == 0) return;
    for (int bit = int(numBits) - 1; bit >= 0; --bit) putBit((value >> bit) & 1U);
  }

  void putSigned(int32_t value, unsigned numBits) {
    uint32_t raw;
    if (numBits >= 32) raw = uint32_t(value);
    else raw = uint32_t(value) & ((1U << numBits) - 1U);
    put(raw, numBits);
  }

  std::vector<uint8_t> finish() {
    bytes.resize((bitCount + 7) / 8);
    return bytes;
  }

private:
  void putBit(unsigned bit) {
    unsigned const byteIndex = bitCount / 8;
    if (byteIndex >= bytes.size()) bytes.resize(bytes.size() * 2 + 8);
    if (bit) bytes[byteIndex] |= uint8_t(0x80U >> (bitCount % 8));
    ++bitCount;
  }
};

static WrittenHeaders writeAUHeaders(Widths const& widths,
				     std::vector<HeaderSpec> const& headers) {
  BitWriter w;
  for (unsigned i = 0; i < headers.size(); ++i) {
    HeaderSpec const& h = headers[i];
    w.put(h.size, widths.sizeLength);
    w.put(h.indexOrDelta,
	  i == 0 ? widths.indexLength : widths.indexDeltaLength);
    if (widths.ctsDeltaLength > 0) {
      w.put(h.ctsFlag ? 1U : 0U, 1);
      if (h.ctsFlag) w.putSigned(h.ctsDelta, widths.ctsDeltaLength);
    }
  }
  WrittenHeaders result;
  result.bitLength = w.bitCount;
  result.bytes = w.finish();
  return result;
}

static std::vector<uint8_t> auBytes(unsigned id, unsigned size) {
  std::vector<uint8_t> bytes(size);
  for (unsigned i = 0; i < size; ++i) {
    bytes[i] = uint8_t((id * 37U + i * 11U + 0x41U) & 0xffU);
  }
  return bytes;
}

static std::vector<uint8_t>
makeRTPPacket(uint16_t seq, uint32_t timestamp, bool marker,
	      WrittenHeaders const& headers,
	      std::vector<uint8_t> const& payload,
	      uint8_t payloadType = 96) {
  std::vector<uint8_t> packet;
  packet.reserve(12 + 2 + headers.bytes.size() + payload.size());

  packet.push_back(0x80);
  packet.push_back(uint8_t((marker ? 0x80U : 0x00U) | payloadType));
  packet.push_back(uint8_t(seq >> 8));
  packet.push_back(uint8_t(seq & 0xff));
  for (int shift = 24; shift >= 0; shift -= 8) {
    packet.push_back(uint8_t(timestamp >> shift));
  }
  packet.push_back(0x00);
  packet.push_back(0x00);
  packet.push_back(0x00);
  packet.push_back(0x7b); // SSRC

  // AU-headers-length is measured in BITS.
  packet.push_back(uint8_t(headers.bitLength >> 8));
  packet.push_back(uint8_t(headers.bitLength & 0xff));
  packet.insert(packet.end(), headers.bytes.begin(), headers.bytes.end());
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

static std::vector<uint8_t>
makeAACPacket(Widths const& widths, uint16_t seq, uint32_t timestamp,
	      bool marker, std::vector<HeaderSpec> const& specs,
	      std::vector<std::vector<uint8_t>> const& auChunks) {
  WrittenHeaders headers = writeAUHeaders(widths, specs);
  std::vector<uint8_t> payload;
  for (auto const& chunk : auChunks) {
    payload.insert(payload.end(), chunk.begin(), chunk.end());
  }
  return makeRTPPacket(seq, timestamp, marker, headers, payload);
}

struct ReceivedFrame {
  std::vector<uint8_t> bytes;
  unsigned truncatedBytes;
  timeval time;
};

struct Rig {
  TaskScheduler* scheduler = nullptr;
  UsageEnvironment* environment = nullptr;
  MediaSession* session = nullptr;
  MediaSubsession* subsession = nullptr;
  FramedSource* source = nullptr;
  int fd = -1;
  sockaddr_in destination{};
  unsigned char buffer[16384]{};
  unsigned readSize = sizeof(buffer);
  unsigned expectedFrames = 0;
  std::vector<ReceivedFrame> frames;
  bool timedOut = false;
  EventLoopWatchVariable done{0};
  TaskToken timeoutToken = nullptr;

  void arm() {
    source->getNextFrame(buffer, readSize, &Rig::onFrame, this,
			 nullptr, nullptr);
  }

  void begin(unsigned timeoutMicroseconds = 2500000) {
    arm();
    timeoutToken = scheduler->scheduleDelayedTask(timeoutMicroseconds,
						  &Rig::onTimeout, this);
  }

  void run() { scheduler->doEventLoop(&done); }

  void send(std::vector<uint8_t> const& packet) {
    ssize_t sent = sendto(fd, packet.data(), packet.size(), 0,
			  (sockaddr const*)&destination,
			  sizeof(destination));
    if (sent != ssize_t(packet.size())) {
      std::fprintf(stderr, "test sendto failed for %zu bytes\n",
		   packet.size());
      std::exit(2);
    }
  }

  static void onFrame(void* clientData, unsigned frameSize,
		      unsigned numTruncatedBytes,
		      timeval presentationTime, unsigned /*durationUs*/) {
    Rig* rig = static_cast<Rig*>(clientData);
    ReceivedFrame frame;
    frame.bytes.assign(rig->buffer, rig->buffer + frameSize);
    frame.truncatedBytes = numTruncatedBytes;
    frame.time = presentationTime;
    rig->frames.push_back(frame);

    if (rig->frames.size() >= rig->expectedFrames) {
      rig->done = 1;
    } else {
      rig->arm();
    }
  }

  static void onTimeout(void* clientData) {
    Rig* rig = static_cast<Rig*>(clientData);
    rig->timedOut = true;
    rig->done = 1;
  }
};

static std::string aacSDP(unsigned clockRate, Widths const& widths,
			  std::string const& extraFmtp = "") {
  std::string fmtp =
    "mode=AAC-hbr;config=1190"
    ";sizelength=" + std::to_string(widths.sizeLength) +
    ";indexlength=" + std::to_string(widths.indexLength) +
    ";indexdeltalength=" + std::to_string(widths.indexDeltaLength);
  if (widths.ctsDeltaLength > 0) {
    fmtp += ";CTSDeltaLength=" + std::to_string(widths.ctsDeltaLength);
  }
  fmtp += extraFmtp;

  return
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=aac-au-timing\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 MPEG4-GENERIC/" + std::to_string(clockRate) + "/2\r\n"
    "a=fmtp:96 " + fmtp + "\r\n";
}

static Rig* rigStart(std::string const& sdpText, unsigned expectedFrames,
		     unsigned readSize = 16384) {
  Rig* rig = new Rig;
  rig->expectedFrames = expectedFrames;
  rig->readSize = readSize;
  rig->scheduler = BasicTaskScheduler::createNew();
  rig->environment = BasicUsageEnvironment::createNew(*rig->scheduler);
  rig->session = MediaSession::createNew(*rig->environment, sdpText.c_str());
  if (rig->session == nullptr) {
    std::fprintf(stderr, "test SDP was rejected: %s\n",
		 rig->environment->getResultMsg());
    std::exit(2);
  }

  MediaSubsessionIterator it(*rig->session);
  rig->subsession = it.next();
  if (rig->subsession == nullptr || !rig->subsession->initiate()) {
    std::fprintf(stderr,
		 "test could not bind/initiate MediaSubsession: %s\n"
		 "If local UDP binding is sandboxed, allow local binding and rerun.\n",
		 rig->environment->getResultMsg());
    std::exit(2);
  }
  rig->source = rig->subsession->readSource();
  if (rig->source == nullptr) std::exit(2);

  rig->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (rig->fd < 0) std::exit(2);
  rig->destination.sin_family = AF_INET;
  rig->destination.sin_port = htons(rig->subsession->clientPortNum());
  inet_pton(AF_INET, "127.0.0.1", &rig->destination.sin_addr);
  return rig;
}

static void rigStop(Rig* rig) {
  if (rig == nullptr) return;
  if (rig->timeoutToken != nullptr) {
    rig->scheduler->unscheduleDelayedTask(rig->timeoutToken);
  }
  if (rig->fd >= 0) close(rig->fd);
  Medium::close(rig->session);
  if (rig->environment != nullptr) rig->environment->reclaim();
  delete rig->scheduler;
  delete rig;
}

static int64_t timeDeltaMicroseconds(timeval const& first,
				     timeval const& later) {
  return int64_t(later.tv_sec - first.tv_sec) * 1000000LL
    + int64_t(later.tv_usec - first.tv_usec);
}

static int64_t ticksToMicroseconds(int64_t ticks, unsigned clockRate) {
  return ticks * 1000000LL / int64_t(clockRate);
}

static bool sdpConfigurationRejected(Widths const& widths,
				     std::string const& extraFmtp) {
  std::string sdpText = aacSDP(48000, widths, extraFmtp);
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);
  MediaSession* session = MediaSession::createNew(*env, sdpText.c_str());
  bool rejected = true;
  if (session != nullptr) {
    MediaSubsessionIterator it(*session);
    MediaSubsession* sub = it.next();
    bool initiated = sub != nullptr && sub->initiate();
    std::string message = env->getResultMsg();
    rejected = !initiated
      && (message.find("rejected") != std::string::npos
	  || message.find("unsupported") != std::string::npos);
  }
  Medium::close(session);
  env->reclaim();
  delete scheduler;
  return rejected;
}

int test_aac_au_headers() {
  int const assertionsBefore = g_assertions;

  AAC_CHECK(MPEG4GenericRTPSource::validHeaderLengths(13, 3, 3, 0));
  AAC_CHECK(MPEG4GenericRTPSource::validHeaderLengths(16, 16, 16, 32));
  AAC_CHECK(!MPEG4GenericRTPSource::validHeaderLengths(33, 3, 3, 0));
  AAC_CHECK(!MPEG4GenericRTPSource::validHeaderLengths(13, 33, 3, 0));
  AAC_CHECK(!MPEG4GenericRTPSource::validHeaderLengths(13, 3, 33, 0));
  AAC_CHECK(!MPEG4GenericRTPSource::validHeaderLengths(13, 3, 3, 33));

  Widths ctsWidths{13, 3, 3, 9};
  std::vector<uint8_t> au0 = auBytes(1, 3);
  std::vector<uint8_t> au1 = auBytes(2, 4);
  WrittenHeaders validHeaders = writeAUHeaders(ctsWidths, {
    {uint32_t(au0.size()), 0, false, 0},
    {uint32_t(au1.size()), 2, true, -37}
  });
  AAC_CHECK(validHeaders.bitLength == 43); // 17 + (13+3+1+9)
  AAC_CHECK((validHeaders.bitLength % 8) != 0);

  {
    std::string sdpText = aacSDP(48000, ctsWidths, ";constantDuration=1024");
    Rig* rig = rigStart(sdpText, 2);
    std::vector<uint8_t> payload = au0;
    payload.insert(payload.end(), au1.begin(), au1.end());
    std::vector<uint8_t> packet
      = makeRTPPacket(1, 0x40001000, true, validHeaders, payload);
    AAC_CHECK(packet[12] == 0); // AU-headers-length high byte
    AAC_CHECK(packet[13] == 43); // low byte is bits, not ceil(43/8)=6
    rig->begin();
    rig->send(packet);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 2);
    AAC_CHECK(rig->frames[0].bytes == au0);
    AAC_CHECK(rig->frames[1].bytes == au1);
    AAC_CHECK(rig->frames[0].truncatedBytes == 0);
    AAC_CHECK(rig->frames[1].truncatedBytes == 0);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[1].time)
	      == ticksToMicroseconds(-37, 48000));
    rigStop(rig);
  }

  // A declared bit length that runs past the exact final header boundary is
  // malformed.  The following normal packet must still be received.
  {
    WrittenHeaders badLengthHeaders = validHeaders;
    badLengthHeaders.bitLength = 48; // six padding bits cannot form a header
    std::vector<uint8_t> badPayload(7, 0x5d);
    std::vector<uint8_t> bad
      = makeRTPPacket(10, 0x40002000, true, badLengthHeaders, badPayload);

    HeaderSpec goodSpec{5, 0, false, 0};
    std::vector<uint8_t> goodBytes = auBytes(9, 5);
    WrittenHeaders goodHeaders = writeAUHeaders(ctsWidths, {goodSpec});
    std::vector<uint8_t> good
      = makeRTPPacket(11, 0x40002000, true, goodHeaders, goodBytes);

    std::string sdpText = aacSDP(48000, ctsWidths, ";constantDuration=1024");
    Rig* rig = rigStart(sdpText, 1);
    rig->begin();
    rig->send(bad);
    rig->send(good);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 1);
    AAC_CHECK(rig->frames[0].bytes == goodBytes);
    rigStop(rig);
  }

  // Header sizes say four payload bytes exist, but only three are present.
  {
    Widths plain{13, 3, 3, 0};
    WrittenHeaders mismatchHeaders = writeAUHeaders(plain, {
      {2, 0, false, 0}, {2, 0, false, 0}
    });
    std::vector<uint8_t> badPayload = {0xaa, 0xbb, 0xcc};
    std::vector<uint8_t> bad
      = makeRTPPacket(20, 0x50000000, true, mismatchHeaders, badPayload);

    std::vector<uint8_t> goodBytes = auBytes(4, 6);
    WrittenHeaders goodHeaders
      = writeAUHeaders(plain, {{uint32_t(goodBytes.size()), 0, false, 0}});
    std::vector<uint8_t> good
      = makeRTPPacket(21, 0x50000100, true, goodHeaders, goodBytes);

    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"), 1);
    rig->begin();
    rig->send(bad);
    rig->send(good);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 1);
    AAC_CHECK(rig->frames[0].bytes == goodBytes);
    rigStop(rig);
  }

  AAC_CHECK(!sdpConfigurationRejected({13, 3, 3, 0}, ""));
  AAC_CHECK(sdpConfigurationRejected({33, 3, 3, 0}, ""));
  AAC_CHECK(sdpConfigurationRejected({13, 3, 3, 33}, ""));
  AAC_CHECK(sdpConfigurationRejected({13, 3, 3, 9},
				     ";DTSDeltaLength=5"));
  AAC_CHECK(sdpConfigurationRejected({13, 3, 3, 0},
				     ";RandomAccessIndication=1"));
  AAC_CHECK(sdpConfigurationRejected({13, 3, 3, 0},
				     ";StreamStateIndication=4"));

  return g_assertions - assertionsBefore;
}

int test_aac_au_timing() {
  int const before = g_assertions;

  // A non-MPEG4-GENERIC MultiFramedRTPSource format still hands a plain RTP
  // payload straight through.
  {
    const char* l16SDP =
      "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=l16\r\n"
      "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
      "m=audio 0 RTP/AVP 97\r\n"
      "a=rtpmap:97 L16/48000/1\r\n";
    std::vector<uint8_t> payload = {0x12, 0x34, 0x56, 0x78};
    std::vector<uint8_t> packet = {
      0x80, 97, 0x00, 0x07, 0x00, 0x00, 0x10, 0x00,
      0x00, 0x00, 0x00, 0x21
    };
    packet.insert(packet.end(), payload.begin(), payload.end());

    Rig* rig = rigStart(l16SDP, 1);
    rig->begin();
    rig->send(packet);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 1);
    AAC_CHECK(rig->frames[0].bytes == payload);
    AAC_CHECK(rig->frames[0].truncatedBytes == 0);
    rigStop(rig);
  }

  // Old behavior: no constantDuration and no CTS leaves all enclosed AUs at
  // the packet's mapped presentation time.
  {
    Widths plain{13, 3, 3, 0};
    std::vector<std::vector<uint8_t>> aus = {
      auBytes(1, 2), auBytes(2, 3), auBytes(3, 4)
    };
    std::vector<HeaderSpec> specs = {
      {2, 0}, {3, 0}, {4, 0}
    };
    std::string sdpText = aacSDP(48000, plain); // no constantDuration
    Rig* rig = rigStart(sdpText, 3);
    std::vector<uint8_t> packet
      = makeAACPacket(plain, 1, 0x11110000, true, specs, aus);
    rig->begin();
    rig->send(packet);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 3);
    AAC_CHECK(rig->frames[0].bytes == aus[0]);
    AAC_CHECK(rig->frames[1].bytes == aus[1]);
    AAC_CHECK(rig->frames[2].bytes == aus[2]);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[1].time) == 0);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[2].time) == 0);
    rigStop(rig);
  }

  // 48 kHz, three AUs, and an index-delta jump.  Relative sequences are
  // 0, 1, and 1+2+1=4.
  {
    Widths plain{13, 3, 3, 0};
    std::vector<std::vector<uint8_t>> aus = {
      auBytes(4, 2), auBytes(5, 3), auBytes(6, 4)
    };
    std::vector<HeaderSpec> specs = {
      {2, 0}, {3, 0}, {4, 2}
    };
    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"), 3);
    std::vector<uint8_t> packet
      = makeAACPacket(plain, 2, 0x12345678, true, specs, aus);
    rig->begin();
    rig->send(packet);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 3);
    AAC_CHECK(rig->frames[1].bytes == aus[1]);
    AAC_CHECK(rig->frames[2].bytes == aus[2]);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[1].time)
	      == ticksToMicroseconds(1024, 48000));
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[2].time)
	      == ticksToMicroseconds(4 * int64_t(1024), 48000));
    rigStop(rig);
  }

  // Explicit negative CTS on a non-byte-aligned header.  It overrides
  // constantDuration for that AU.
  {
    Widths cts{13, 3, 3, 10};
    std::vector<uint8_t> au0 = auBytes(7, 4);
    std::vector<uint8_t> au1 = auBytes(8, 5);
    WrittenHeaders headers = writeAUHeaders(cts, {
      {uint32_t(au0.size()), 0, false, 0},
      {uint32_t(au1.size()), 1, true, -129}
    });
    AAC_CHECK((headers.bitLength % 8) != 0);
    std::vector<uint8_t> payload = au0;
    payload.insert(payload.end(), au1.begin(), au1.end());
    std::vector<uint8_t> packet
      = makeRTPPacket(3, 0x22220000, true, headers, payload);

    Rig* rig = rigStart(aacSDP(48000, cts, ";constantDuration=1024"), 2);
    rig->begin();
    rig->send(packet);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 2);
    AAC_CHECK(rig->frames[1].bytes == au1);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[1].time)
	      == ticksToMicroseconds(-129, 48000));
    rigStop(rig);
  }

  // Client-buffer truncation remains visible per AU and does not merge AUs.
  {
    Widths plain{13, 3, 3, 0};
    std::vector<std::vector<uint8_t>> aus = {
      auBytes(10, 4), auBytes(11, 5)
    };
    std::vector<HeaderSpec> specs = {{4, 0}, {5, 0}};
    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"),
			2, 2);
    std::vector<uint8_t> packet
      = makeAACPacket(plain, 4, 0x33330000, true, specs, aus);
    rig->begin();
    rig->send(packet);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 2);
    AAC_CHECK(rig->frames[0].bytes.size() == 2);
    AAC_CHECK(rig->frames[0].bytes[0] == aus[0][0]);
    AAC_CHECK(rig->frames[0].bytes[1] == aus[0][1]);
    AAC_CHECK(rig->frames[0].truncatedBytes == 2);
    AAC_CHECK(rig->frames[1].truncatedBytes == 3);
    rigStop(rig);
  }

  // Several successor RTP packets, with exact microsecond timestamp steps.
  {
    Widths plain{13, 3, 3, 0};
    std::vector<std::vector<uint8_t>> firstAUs = {
      auBytes(20, 2), auBytes(21, 2), auBytes(22, 2)
    };
    std::vector<HeaderSpec> firstSpecs = {{2, 0}, {2, 0}, {2, 0}};
    uint32_t const baseTimestamp = 100000;
    std::vector<uint8_t> p0
      = makeAACPacket(plain, 10, baseTimestamp, true,
		      firstSpecs, firstAUs);

    std::vector<uint8_t> au3 = auBytes(23, 3);
    std::vector<uint8_t> p1
      = makeAACPacket(plain, 11, baseTimestamp + 3 * 1024, true,
		      {{3, 0}}, {au3});
    std::vector<uint8_t> au4 = auBytes(24, 3);
    std::vector<uint8_t> p2
      = makeAACPacket(plain, 12, baseTimestamp + 5 * 1024, true,
		      {{3, 0}}, {au4});

    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"), 5);
    rig->begin();
    rig->send(p0);
    rig->send(p1);
    rig->send(p2);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 5);
    AAC_CHECK(rig->frames[3].bytes == au3);
    AAC_CHECK(rig->frames[4].bytes == au4);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[3].time)
	      == ticksToMicroseconds(3 * int64_t(1024), 48000));
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[4].time)
	      == ticksToMicroseconds(5 * int64_t(1024), 48000));
    rigStop(rig);
  }

  // 44.1 kHz and a 32-bit RTP timestamp wrap.
  {
    Widths plain{13, 3, 3, 0};
    std::vector<std::vector<uint8_t>> aus = {
      auBytes(30, 3), auBytes(31, 3), auBytes(32, 3)
    };
    std::vector<HeaderSpec> specs = {{3, 0}, {3, 0}, {3, 0}};
    uint32_t const wrappedTimestamp = 0xfffffff0U;
    Rig* rig = rigStart(aacSDP(44100, plain, ";constantDuration=1024"), 3);
    std::vector<uint8_t> packet
      = makeAACPacket(plain, 30, wrappedTimestamp, true, specs, aus);
    rig->begin();
    rig->send(packet);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 3);
    AAC_CHECK(rig->frames[0].bytes == aus[0]);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[1].time)
	      == ticksToMicroseconds(1024, 44100));
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[2].time)
	      == ticksToMicroseconds(2048, 44100));
    rigStop(rig);
  }

  return g_assertions - before;
}

int test_aac_fragment_timing() {
  int const before = g_assertions;
  Widths plain{13, 3, 3, 0};

  // Two complete AUs, followed by a trailing fragment.  The next packet
  // completes it, and a later normal packet continues the stream.
  {
    std::vector<uint8_t> au0 = auBytes(40, 4);
    std::vector<uint8_t> au1 = auBytes(41, 5);
    std::vector<uint8_t> au2Full = auBytes(42, 10);
    std::vector<uint8_t> au2First(au2Full.begin(), au2Full.begin() + 3);
    std::vector<uint8_t> au2Rest(au2Full.begin() + 3, au2Full.end());
    std::vector<uint8_t> au3 = auBytes(43, 6);

    uint32_t const ts = 0x60000000;
    std::vector<uint8_t> p0 = makeAACPacket(plain, 40, ts, false, {
      {uint32_t(au0.size()), 0},
      {uint32_t(au1.size()), 0},
      {uint32_t(au2Full.size()), 0}
    }, {au0, au1, au2First});

    std::vector<uint8_t> p1 = makeAACPacket(plain, 41, ts, true, {
      {uint32_t(au2Full.size()), 0}
    }, {au2Rest});

    std::vector<uint8_t> p2
      = makeAACPacket(plain, 42, ts + 3 * 1024, true,
		      {{uint32_t(au3.size()), 0}}, {au3});

    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"), 4);
    rig->begin();
    rig->send(p0);
    rig->send(p1);
    rig->send(p2);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 4);
    AAC_CHECK(rig->frames[0].bytes == au0);
    AAC_CHECK(rig->frames[1].bytes == au1);
    AAC_CHECK(rig->frames[2].bytes == au2Full); // delivered once, assembled
    AAC_CHECK(rig->frames[3].bytes == au3);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[1].time)
	      == ticksToMicroseconds(1024, 48000));
    // The fragmented AU is relative sequence 2 and keeps first-piece time.
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[2].time)
	      == ticksToMicroseconds(2048, 48000));
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[3].time)
	      == ticksToMicroseconds(3072, 48000));
    rigStop(rig);
  }

  // A continuous-sequence packet that carries a different AU abandons the
  // partial AU and lets the new complete AU through.
  {
    std::vector<uint8_t> partialFull = auBytes(50, 9);
    std::vector<uint8_t> chunk(partialFull.begin(), partialFull.begin() + 4);
    std::vector<uint8_t> newAU = auBytes(51, 7);
    uint32_t ts = 0x70000000;

    std::vector<uint8_t> p0
      = makeAACPacket(plain, 50, ts, false,
		      {{uint32_t(partialFull.size()), 0}}, {chunk});
    std::vector<uint8_t> p1
      = makeAACPacket(plain, 51, ts + 1024, true,
		      {{uint32_t(newAU.size()), 0}}, {newAU});

    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"), 1);
    rig->begin();
    rig->send(p0);
    rig->send(p1);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 1);
    AAC_CHECK(rig->frames[0].bytes == newAU);
    AAC_CHECK(rig->frames[0].truncatedBytes == 0);
    AAC_CHECK(!rig->source->isCurrentlyAwaitingData());
    rigStop(rig);
  }

  // A lost fragment causes recovery; after the reorder threshold the first
  // complete new-AU packet resets assembly and is received.
  {
    std::vector<uint8_t> full = auBytes(60, 12);
    std::vector<uint8_t> chunk(full.begin(), full.begin() + 5);
    std::vector<uint8_t> recoveryAU = auBytes(61, 8);
    uint32_t ts = 0x80000000;

    std::vector<uint8_t> first
      = makeAACPacket(plain, 60, ts, false,
		      {{uint32_t(full.size()), 0}}, {chunk});
    // Sequence 61 is intentionally never sent.
    std::vector<uint8_t> recovery
      = makeAACPacket(plain, 62, ts + 2048, true,
		      {{uint32_t(recoveryAU.size()), 0}}, {recoveryAU});

    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"), 1);
    rig->begin(2000000);
    rig->send(first);
    rig->send(recovery);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 1);
    AAC_CHECK(rig->frames[0].bytes == recoveryAU);
    rigStop(rig);
  }

  // An overlong continuation is rejected, and the next normal packet works.
  {
    std::vector<uint8_t> full = auBytes(70, 6);
    std::vector<uint8_t> chunk(full.begin(), full.begin() + 2);
    std::vector<uint8_t> overlongRest(8, 0x6e); // 2 + 8 > declared size 6
    std::vector<uint8_t> normalAU = auBytes(71, 5);
    uint32_t ts = 0x90000000;

    std::vector<uint8_t> first
      = makeAACPacket(plain, 70, ts, false,
		      {{uint32_t(full.size()), 0}}, {chunk});
    std::vector<uint8_t> bad
      = makeAACPacket(plain, 71, ts, true,
		      {{uint32_t(full.size()), 0}}, {overlongRest});
    std::vector<uint8_t> good
      = makeAACPacket(plain, 72, ts + 1024, true,
		      {{uint32_t(normalAU.size()), 0}}, {normalAU});

    Rig* rig = rigStart(aacSDP(48000, plain, ";constantDuration=1024"), 1);
    rig->begin();
    rig->send(first);
    rig->send(bad);
    rig->send(good);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 1);
    AAC_CHECK(rig->frames[0].bytes == normalAU);
    rigStop(rig);
  }

  // 44.1 kHz mixed packet/fragment case keeps first-piece timing exactly.
  {
    Widths smallIndex{13, 0, 3, 0};
    std::vector<uint8_t> completeAU = auBytes(80, 4);
    std::vector<uint8_t> full = auBytes(81, 9);
    std::vector<uint8_t> firstPart(full.begin(), full.begin() + 4);
    std::vector<uint8_t> rest(full.begin() + 4, full.end());
    uint32_t ts = 0xa0000000;

    std::vector<uint8_t> p0
      = makeAACPacket(smallIndex, 80, ts, false,
		      {{4, 0}, {9, 0}}, {completeAU, firstPart});
    std::vector<uint8_t> p1
      = makeAACPacket(smallIndex, 81, ts, true, {{9, 0}}, {rest});

    Rig* rig = rigStart(aacSDP(44100, smallIndex, ";constantDuration=1024"),
			2);
    rig->begin();
    rig->send(p0);
    rig->send(p1);
    rig->run();

    AAC_CHECK(!rig->timedOut);
    AAC_CHECK(rig->frames.size() == 2);
    AAC_CHECK(rig->frames[1].bytes == full);
    AAC_CHECK(timeDeltaMicroseconds(rig->frames[0].time,
				    rig->frames[1].time)
	      == ticksToMicroseconds(1024, 44100));
    rigStop(rig);
  }

  return g_assertions - before;
}
