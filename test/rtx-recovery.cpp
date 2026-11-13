// test/rtx-recovery.cpp - same-session SSRC-multiplexed RTP retransmission
// (RFC 4588) regression tests.
//
// Three entry points, each returning the number of successful assertions
// (a positive number) or a non-positive value on failure:
//   test_rtx_sdp                 - SDP parsing/validation, no sockets
//   test_rtx_h264_recovery        - end-to-end H.264 recovery over real
//                                   loopback UDP and TCP-interleaved sockets
//   test_rtx_stream_isolation     - two subsessions sharing payload types and
//                                   OSNs must keep config/reorder/stats apart
//
// Reception is driven by the live555 task scheduler's real I/O events: a frame
// callback sets the stop variable. A delayed task is used only as a watchdog
// against hangs; no sleep() is ever used to sequence packets.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- assertion bookkeeping -------------------------------------------------

struct Checker {
  int checks = 0;
  int failures = 0;
  void ok(bool cond, char const* what) {
    ++checks;
    if (cond) {
      printf("  ok:   %s\n", what);
    } else {
      ++failures;
      printf("  FAIL: %s\n", what);
    }
  }
  int result() const { return failures == 0 ? checks : -checks; }
};

// ---- hand-written RTP/RTCP packet builders --------------------------------

struct RtpOpts {
  unsigned char csrcCount = 0;
  bool extension = false;
  bool padding = false;
  unsigned char paddingBytes = 0;
};

void put16(std::vector<unsigned char>& v, unsigned x) {
  v.push_back((unsigned char)(x >> 8));
  v.push_back((unsigned char)(x & 0xFF));
}
void put32(std::vector<unsigned char>& v, unsigned x) {
  v.push_back((unsigned char)(x >> 24));
  v.push_back((unsigned char)(x >> 16));
  v.push_back((unsigned char)(x >> 8));
  v.push_back((unsigned char)(x & 0xFF));
}

// Build a regular RTP packet (primary codec payload in "payload").
std::vector<unsigned char>
buildRtp(unsigned char pt, unsigned seq, unsigned rtpTs, unsigned ssrc,
         bool marker, std::vector<unsigned char> const& payload,
         RtpOpts opts = RtpOpts()) {
  std::vector<unsigned char> p;
  unsigned char vpxcc = 0x80 | (opts.csrcCount & 0x0F);
  if (opts.extension) vpxcc |= 0x10;
  if (opts.padding) vpxcc |= 0x20;
  p.push_back(vpxcc);
  p.push_back((unsigned char)((marker ? 0x80 : 0x00) | (pt & 0x7F)));
  put16(p, seq);
  put32(p, rtpTs);
  put32(p, ssrc);
  for (unsigned i = 0; i < opts.csrcCount; ++i) put32(p, 0xC5C00000u + i);
  if (opts.extension) {
    put16(p, 0xBEDE);      // extension id
    put16(p, 1);           // one 32-bit extension word
    put32(p, 0x12345678);
  }
  p.insert(p.end(), payload.begin(), payload.end());
  if (opts.padding) {
    for (unsigned i = 1; i < opts.paddingBytes; ++i) p.push_back(0);
    p.push_back(opts.paddingBytes);
  }
  return p;
}

// Build an RFC 4588 retransmission packet: outer header + 2-byte OSN + the
// original codec payload.
std::vector<unsigned char>
buildRtx(unsigned char rtxPt, unsigned outerSeq, unsigned rtpTs,
         unsigned rtxSsrc, bool marker, unsigned osn,
         std::vector<unsigned char> const& origPayload,
         RtpOpts opts = RtpOpts()) {
  std::vector<unsigned char> rtxPayload;
  put16(rtxPayload, osn);
  rtxPayload.insert(rtxPayload.end(), origPayload.begin(), origPayload.end());
  return buildRtp(rtxPt, outerSeq, rtpTs, rtxSsrc, marker, rtxPayload, opts);
}

// H.264 FU-A fragment payload.  The FU indicator carries ref_idc=3, type=28.
std::vector<unsigned char>
fuPayload(unsigned char fuHeader, std::vector<unsigned char> const& frag) {
  std::vector<unsigned char> v;
  v.push_back(0x7C); // FU indicator: ref_idc=3, type=28 (FU-A)
  v.push_back(fuHeader);
  v.insert(v.end(), frag.begin(), frag.end());
  return v;
}

// A minimal RTCP Sender Report for "senderSsrc" mapping "rtpTimestamp".
std::vector<unsigned char>
buildSr(unsigned senderSsrc, unsigned rtpTimestamp,
        unsigned ntpMsw = 0xE0000000u, unsigned ntpLsw = 0) {
  std::vector<unsigned char> p;
  p.push_back(0x80);          // V=2, RC=0
  p.push_back(200);           // PT=SR
  put16(p, 6);                // length in 32-bit words minus one
  put32(p, senderSsrc);
  put32(p, ntpMsw);
  put32(p, ntpLsw);
  put32(p, rtpTimestamp);
  put32(p, 0);                // sender's packet count
  put32(p, 0);                // sender's octet count
  return p;
}

bool sendAll(int fd, void const* data, size_t len) {
  auto const* p = static_cast<unsigned char const*>(data);
  while (len > 0) {
    ssize_t n = ::send(fd, p, len, 0);
    if (n <= 0) return false;
    p += n;
    len -= (size_t)n;
  }
  return true;
}

bool sendToPort(unsigned short port, std::vector<unsigned char> const& pkt) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  dst.sin_port = htons(port);
  bool ok = ::sendto(fd, pkt.data(), pkt.size(), 0,
                     (sockaddr*)&dst, sizeof(dst)) == (ssize_t)pkt.size();
  ::close(fd);
  return ok;
}

// Prefix an RTP packet with the RFC 2326 interleaved framing header.
std::vector<unsigned char>
interleave(unsigned char channel, std::vector<unsigned char> const& pkt) {
  std::vector<unsigned char> frame;
  frame.push_back('$');
  frame.push_back(channel);
  put16(frame, (unsigned)pkt.size());
  frame.insert(frame.end(), pkt.begin(), pkt.end());
  return frame;
}

// ---- reception harness -----------------------------------------------------

struct ReceivedFrame {
  std::vector<unsigned char> bytes;
  struct timeval presentationTime;
  bool syncedUsingRTCP;
};

struct Runner {
  TaskScheduler* scheduler = nullptr;
  FramedSource* source = nullptr;
  RTPSource* rtpSource = nullptr;
  unsigned char out[65536];
  std::vector<ReceivedFrame> frames;
  EventLoopWatchVariable stop = 0;
  bool timedOut = false;
  unsigned wantedFrames = 1;
  TaskToken watchdog = nullptr;

  static void frameCb(void* clientData, unsigned frameSize,
                      unsigned numTruncated, struct timeval presentationTime,
                      unsigned /*duration*/) {
    auto* self = static_cast<Runner*>(clientData);
    ReceivedFrame f;
    f.bytes.assign(self->out, self->out + frameSize);
    f.presentationTime = presentationTime;
    f.syncedUsingRTCP = self->rtpSource != nullptr
                        && self->rtpSource->hasBeenSynchronizedUsingRTCP();
    self->frames.push_back(std::move(f));
    if (numTruncated != 0) {
      self->stop = 2; // truncation is a test failure
      return;
    }
    if (self->frames.size() >= self->wantedFrames) {
      self->stop = 1;
    } else {
      self->source->getNextFrame(self->out, sizeof(self->out),
                                 frameCb, self, nullptr, nullptr);
    }
  }

  static void watchdogCb(void* clientData) {
    auto* self = static_cast<Runner*>(clientData);
    self->timedOut = true;
    self->stop = 3;
  }

  // Arm the read and pump the event loop until the wanted number of frames
  // arrives (or the watchdog fires).  Every byte moved goes through real
  // scheduler I/O events; the watchdog exists only to prevent a hang.
  bool run(unsigned timeoutUSeconds = 3000000) {
    source->getNextFrame(out, sizeof(out), frameCb, this, nullptr, nullptr);
    watchdog = scheduler->scheduleDelayedTask(timeoutUSeconds,
                                              watchdogCb, this);
    scheduler->doEventLoop(&stop);
    scheduler->unscheduleDelayedTask(watchdog);
    return !timedOut && stop == 1;
  }
};

RTPReceptionStats const* primaryStats(MediaSubsession& sub, unsigned ssrc) {
  return sub.rtpSource()->receptionStatsDB().lookup(ssrc);
}

MultiFramedRTPSource* multiFramed(MediaSubsession& sub) {
  return static_cast<MultiFramedRTPSource*>(sub.rtpSource());
}

char const* const kSdpHeader =
  "v=0\r\n"
  "o=- 1 1 IN IP4 127.0.0.1\r\n"
  "s=local\r\n"
  "t=0 0\r\n"
  "c=IN IP4 127.0.0.1\r\n";

// The canonical SDP used by the recovery tests.
char const* const kH264RtxSdp =
  "v=0\r\n"
  "o=- 1 1 IN IP4 127.0.0.1\r\n"
  "s=local\r\n"
  "t=0 0\r\n"
  "c=IN IP4 127.0.0.1\r\n"
  "m=video 0 RTP/AVP 96 97\r\n"
  "a=rtpmap:96 H264/90000\r\n"
  "a=fmtp:96 packetization-mode=1\r\n"
  "a=rtpmap:97 rtx/90000\r\n"
  "a=fmtp:97 apt=96\r\n"
  "a=ssrc-group:FID 1111 2222\r\n";

unsigned constexpr kPrimarySSRC = 1111;
unsigned constexpr kRtxSSRC = 2222;
unsigned char constexpr kPrimaryPT = 96;
unsigned char constexpr kRtxPT = 97;

MediaSubsession* firstSubsession(MediaSession& session) {
  MediaSubsessionIterator it(session);
  return it.next();
}

// Build and initiate a real session from hand-written SDP.  Each scenario
// gets its own, so its reordering state starts at that scenario's first
// sequence number (mirroring a single monotonic real stream).
struct SessionCtx {
  MediaSession* session = nullptr;
  MediaSubsession* sub = nullptr;
  bool started = false;
};

SessionCtx startSession(UsageEnvironment* env,
                        char const* sdp = kH264RtxSdp) {
  SessionCtx ctx;
  ctx.session = MediaSession::createNew(*env, sdp);
  if (ctx.session == nullptr) return ctx;
  ctx.sub = firstSubsession(*ctx.session);
  ctx.started = ctx.sub->initiate();
  if (ctx.started) {
    ctx.sub->rtpSource()->setPacketReorderingThresholdTime(0);
  }
  return ctx;
}

void stopSession(SessionCtx& ctx) {
  Medium::close(ctx.session);
  ctx.session = nullptr;
  ctx.sub = nullptr;
  ctx.started = false;
}

} // namespace

// ===========================================================================
// test_rtx_sdp: SDP parsing and declaration validation (no sockets)
// ===========================================================================
int test_rtx_sdp() {
  printf("== test_rtx_sdp ==\n");
  Checker c;
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  auto parse = [&](char const* sdp) -> MediaSession* {
    return MediaSession::createNew(*env, sdp);
  };

  // 1. The canonical, complete declaration enables recovery.
  {
    MediaSession* s = parse(kH264RtxSdp);
    c.ok(s != nullptr, "canonical SDP parses");
    if (s != nullptr) {
      MediaSubsession* sub = firstSubsession(*s);
      c.ok(sub->rtxConfigured(), "complete apt/FID declaration enables RTX");
      c.ok(sub->rtxConfig().primaryPayloadType == kPrimaryPT,
           "config keeps the primary payload type");
      c.ok(sub->rtxConfig().rtxPayloadType == kRtxPT,
           "config keeps the RTX payload type");
      c.ok(sub->rtxConfig().primarySSRC == kPrimarySSRC
           && sub->rtxConfig().rtxSSRC == kRtxSSRC,
           "config keeps both FID SSRCs");
      c.ok(std::string(sub->attrVal_str("packetization-mode")) == "1",
           "primary codec fmtp parameters are retained");
      c.ok(std::string(sub->attrVal_str("apt")) == "",
           "RTX fmtp (apt) does not pollute the primary codec attributes");
      Medium::close(s);
    }
  }

  // 2. Line order must not matter (FID first, then fmtp, then rtpmap).
  {
    char const* sdp =
      "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=l\r\nt=0 0\r\n"
      "c=IN IP4 127.0.0.1\r\n"
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=ssrc-group:FID 1111 2222\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:96 packetization-mode=1\r\n"
      "a=rtpmap:96 H264/90000\r\n";
    MediaSession* s = parse(sdp);
    c.ok(s != nullptr && firstSubsession(*s)->rtxConfigured(),
         "rtpmap/fmtp/FID in reversed order still enables RTX");
    Medium::close(s);
  }

  // 3. No RTX declaration at all: ordinary session, no error.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    c.ok(sub != nullptr && !sub->rtxConfigured()
         && !sub->rtxDeclarationHasError(),
         "SDP without RTX works unchanged, with no error");
    Medium::close(s);
  }

  // 4. rtx present but apt missing: no recovery, no hard failure.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(!sub->rtxConfigured() && !sub->rtxDeclarationHasError(),
         "rtx without apt leaves recovery off but video readable");
    Medium::close(s);
  }

  // 5. rtx present but FID missing: no recovery, no hard failure.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(!sub->rtxConfigured() && !sub->rtxDeclarationHasError(),
         "rtx without FID leaves recovery off but video readable");
    Medium::close(s);
  }

  // 6. apt points at something other than the primary PT: explicit error.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=95\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(sub->rtxDeclarationHasError() && !sub->rtxConfigured(),
         "apt not pointing at the primary PT is a declaration error");
    Medium::close(s);
  }

  // 7. Clock-rate mismatch between primary and rtx: explicit error.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/48000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(sub->rtxDeclarationHasError(),
         "mismatched rtx clock rate is a declaration error");
    Medium::close(s);
  }

  // 8. Two different rtx mappings on the same primary stream: error.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97 98\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=rtpmap:98 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=fmtp:98 apt=96\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(sub->rtxDeclarationHasError(),
         "two RTX mappings for one primary stream is an error");
    Medium::close(s);
  }

  // 9. Two contradictory FID pairs: error.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 1111 2222\r\n"
      "a=ssrc-group:FID 1111 3333\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(sub->rtxDeclarationHasError(),
         "contradictory FID pairs are a declaration error");
    Medium::close(s);
  }

  // 10. Fully duplicated declaration: allowed.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 1111 2222\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(sub->rtxConfigured() && !sub->rtxDeclarationHasError(),
         "identical repeated rtpmap/fmtp/FID lines are allowed");
    Medium::close(s);
  }

  // 11. Malformed FID groups: too few / too many / equal SSRCs.
  {
    char const* variants[3] = {
      "a=ssrc-group:FID 1111\r\n",
      "a=ssrc-group:FID 1111 2222 3333\r\n",
      "a=ssrc-group:FID 1111 1111\r\n"
    };
    for (char const* fid : variants) {
      std::string sdp = std::string(kSdpHeader) +
        "m=video 0 RTP/AVP 96 97\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=rtpmap:97 rtx/90000\r\n"
        "a=fmtp:97 apt=96\r\n" + std::string(fid);
      MediaSession* s = parse(sdp.c_str());
      MediaSubsession* sub = firstSubsession(*s);
      c.ok(sub->rtxDeclarationHasError(),
           "malformed FID group (count/equal SSRC) is an error");
      Medium::close(s);
    }
  }

  // 12. H.265 is within the supported codec scope.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H265/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(sub->rtxConfigured(), "H.265 with apt/FID enables RTX");
    Medium::close(s);
  }

  // 13. An erroring declaration makes initiate() fail explicitly.  Socket
  //     creation is part of initiate(); where binding is permitted this also
  //     exercises the explicit initiate() failure path.
  {
    std::string sdp = std::string(kSdpHeader) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=95\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = parse(sdp.c_str());
    MediaSubsession* sub = firstSubsession(*s);
    c.ok(!sub->initiate(), "initiate() fails on a contradictory declaration");
    Medium::close(s);
  }

  env->reclaim();
  delete scheduler;
  return c.result();
}

// ===========================================================================
// test_rtx_h264_recovery: end-to-end H.264 recovery over UDP and TCP
// ===========================================================================
int test_rtx_h264_recovery() {
  printf("== test_rtx_h264_recovery ==\n");
  Checker c;
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  // ---- Scenario A: FU middle fragment lost, recovered by RTX; a duplicate
  //      RTX must not duplicate the output; the two SSRCs' stats are split. -
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    c.ok(ctx.started, "A: session initiates with RTX configured");
    c.ok(ctx.sub->rtxConfigured() && multiFramed(*ctx.sub)->retransmissionEnabled(),
         "A: the RTP source has recovery enabled");
    MediaSubsession* sub = ctx.sub;
    unsigned short const rtpPort = sub->clientPortNum();

    std::vector<unsigned char> fragA(3, 'A'), fragB(3, 'B'), fragC(3, 'C');
    std::vector<unsigned char> nalExpect;
    nalExpect.push_back(0x65);
    nalExpect.insert(nalExpect.end(), fragA.begin(), fragA.end());
    nalExpect.insert(nalExpect.end(), fragB.begin(), fragB.end());
    nalExpect.insert(nalExpect.end(), fragC.begin(), fragC.end());

    auto fuStart = fuPayload(0x85, fragA); // S=1, NAL type 5
    auto fuMid = fuPayload(0x05, fragB);   // continuation
    auto fuEnd = fuPayload(0x45, fragC);   // E=1

    // Primary: seq 100 (start), 102 (end). The middle (101) is "lost".
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 100, 9000, kPrimarySSRC, false, fuStart)),
         "A: send FU start");
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 5000, 9000, kRtxSSRC, false, 101, fuMid)),
         "A: send RTX for the lost FU middle");
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 5001, 9000, kRtxSSRC, false, 101, fuMid)),
         "A: send duplicate RTX for the same OSN");
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 102, 9000, kPrimarySSRC, true, fuEnd)),
         "A: send FU end");

    Runner runner;
    runner.scheduler = scheduler;
    runner.source = sub->readSource();
    runner.rtpSource = sub->rtpSource();
    runner.wantedFrames = 1;
    c.ok(runner.run(), "A: recovered NAL delivered without a hang");
    c.ok(runner.frames.size() == 1,
         "A: exactly one NAL output despite duplicate RTX");
    if (runner.frames.size() == 1) {
      c.ok(runner.frames[0].bytes == nalExpect,
           "A: recovered NAL bytes equal the original NAL unit");
    }

    RTPReceptionStats const* ps = primaryStats(*sub, kPrimarySSRC);
    c.ok(ps != nullptr, "A: primary SSRC has its own reception stats record");
    if (ps != nullptr) {
      c.ok(ps->totNumPacketsReceived() == 2,
           "A: primary stats count only the two primary packets");
      unsigned const expected = ps->totNumPacketsExpected();
      int const lost = (int)expected - (int)ps->totNumPacketsReceived();
      c.ok(expected == 3 && lost == 1,
           "A: primary reception reports still show the one lost packet");
    }
    RTPRetransmissionStats* rs = multiFramed(*sub)->retransmissionStats();
    c.ok(rs != nullptr && rs->SSRC() == kRtxSSRC,
         "A: RTX stats exist and are keyed to the RTX SSRC");
    if (rs != nullptr) {
      c.ok(rs->totNumPacketsReceived() == 2,
           "A: both outer RTX packets are counted under outer seq numbers");
      c.ok(rs->numPacketsRecovered() == 1, "A: one RTX packet filled the gap");
      c.ok(rs->numPacketsRejected() == 1,
           "A: the duplicate-OSN RTX packet is rejected");
      c.ok((rs->highestExtSeqNumReceived() & 0xFFFF) == 5001,
           "A: RTX stats track the RTX outer sequence space, not the OSN");
    }
    stopSession(ctx);
  }

  // ---- Scenario B: RTX fills the gap, then the original primary packet
  //      arrives late.  The NAL is still delivered exactly once. -------------
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    MediaSubsession* sub = ctx.sub;
    unsigned short const rtpPort = sub->clientPortNum();
    std::vector<unsigned char> fragD(2, 'D'), fragE(2, 'E');
    auto start = fuPayload(0x85, fragD);
    auto mid = fuPayload(0x45, fragE);
    std::vector<unsigned char> expect2;
    expect2.push_back(0x65);
    expect2.insert(expect2.end(), fragD.begin(), fragD.end());
    expect2.insert(expect2.end(), fragE.begin(), fragE.end());

    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 200, 18000, kPrimarySSRC, false, start)),
         "B: send FU start");
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 6000, 18000, kRtxSSRC, true, 201, mid)),
         "B: send RTX middle (carries marker)");
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 201, 18000, kPrimarySSRC, true, mid)),
         "B: original middle arrives late");

    Runner runner;
    runner.scheduler = scheduler;
    runner.source = sub->readSource();
    runner.rtpSource = sub->rtpSource();
    runner.wantedFrames = 1;
    c.ok(runner.run(), "B: frame delivered");
    c.ok(runner.frames.size() == 1 && runner.frames[0].bytes == expect2,
         "B: late original does not duplicate the NAL bytes");
    stopSession(ctx);
  }

  // ---- Scenario C: sequence-number wrap 65535 -> 0 across a recovered FU
  //      gap, followed by a complete NAL that must still come out. ----------
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    MediaSubsession* sub = ctx.sub;
    unsigned short const rtpPort = sub->clientPortNum();
    std::vector<unsigned char> fragF(2, 'F'), fragG(2, 'G'), fragH(2, 'H');
    auto start = fuPayload(0x85, fragF);
    auto mid = fuPayload(0x05, fragG);
    auto end = fuPayload(0x45, fragH);
    std::vector<unsigned char> wrapNal;
    wrapNal.push_back(0x65);
    wrapNal.insert(wrapNal.end(), fragF.begin(), fragF.end());
    wrapNal.insert(wrapNal.end(), fragG.begin(), fragG.end());
    wrapNal.insert(wrapNal.end(), fragH.begin(), fragH.end());

    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 65534, 27000, kPrimarySSRC, false, start)),
         "C: FU start at seq 65534");
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 7000, 27000, kRtxSSRC, false, 65535, mid)),
         "C: RTX carries OSN 65535");
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 0, 27000, kPrimarySSRC, false, end)),
         "C: FU end at seq 0 (wrap)");
    std::vector<unsigned char> singleNal = {0x67, 0x42, 0x00, 0x0A};
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 1, 36000, kPrimarySSRC, true, singleNal)),
         "C: complete NAL after the wrap");

    Runner runner;
    runner.scheduler = scheduler;
    runner.source = sub->readSource();
    runner.rtpSource = sub->rtpSource();
    runner.wantedFrames = 2;
    c.ok(runner.run(), "C: both frames delivered across the wrap");
    c.ok(runner.frames.size() == 2, "C: exactly two NALs output");
    if (runner.frames.size() == 2) {
      c.ok(runner.frames[0].bytes == wrapNal,
           "C: wrapped FU NAL assembled via RTX");
      c.ok(runner.frames[1].bytes == singleNal,
           "C: complete NAL after the wrap is output normally");
    }
    stopSession(ctx);
  }

  // ---- Scenario D: an RTCP Sender Report establishes the RTP->wall-clock
  //      mapping; recovered video inherits it without extra primary counts. -
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    MediaSubsession* sub = ctx.sub;
    unsigned short const rtpPort = sub->clientPortNum();
    unsigned short const rtcpPort = (unsigned short)(rtpPort + 1);

    c.ok(sendToPort(rtcpPort, buildSr(kPrimarySSRC, 45000)),
         "D: send RTCP SR for the primary SSRC");
    std::vector<unsigned char> nal = {0x65, 0x11, 0x22};
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 10, 45000, kPrimarySSRC, true, nal)),
         "D: send primary packet at the SR timestamp");

    Runner runner;
    runner.scheduler = scheduler;
    runner.source = sub->readSource();
    runner.rtpSource = sub->rtpSource();
    runner.wantedFrames = 1;
    c.ok(runner.run(), "D: frame delivered after SR");
    c.ok(runner.frames.size() == 1 && runner.frames[0].syncedUsingRTCP,
         "D: presentation time is marked RTCP-synchronised");
    unsigned primaryBefore =
      primaryStats(*sub, kPrimarySSRC)->totNumPacketsReceived();

    // A recovered packet reuses the mapping but must not re-count the primary.
    std::vector<unsigned char> fx = {'X'}, fy = {'Y'};
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 12, 54000, kPrimarySSRC, false,
                             fuPayload(0x85, fx))),
         "D: send FU start");
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 8000, 54000, kRtxSSRC, true, 13,
                             fuPayload(0x45, fy))),
         "D: RTX completes the frame");
    Runner runner2;
    runner2.scheduler = scheduler;
    runner2.source = sub->readSource();
    runner2.rtpSource = sub->rtpSource();
    runner2.wantedFrames = 1;
    c.ok(runner2.run(), "D: recovered frame delivered");
    c.ok(runner2.frames.size() == 1 && runner2.frames[0].syncedUsingRTCP,
         "D: recovered video keeps the RTCP-synchronised timing");
    c.ok(primaryStats(*sub, kPrimarySSRC)->totNumPacketsReceived()
         == primaryBefore + 1,
         "D: recovery added no forged packet count to the primary SSRC");
    stopSession(ctx);
  }

  // ---- Scenario E: malformed / mismatched RTX packets are dropped without
  //      harming normal framing. --------------------------------------------
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    MediaSubsession* sub = ctx.sub;
    unsigned short const rtpPort = sub->clientPortNum();
    std::vector<unsigned char> goodNal = {0x65, 0x77, 0x88};

    // E1: RTX PT but unknown SSRC.
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 9000, 63000, 9999, true, 20, goodNal)),
         "E: RTX PT with a foreign SSRC sent");
    // E2: RTX SSRC but primary PT.
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 9001, 63000, kRtxSSRC, true, goodNal)),
         "E: primary PT with the RTX SSRC sent");
    // E3: short OSN (only the two OSN bytes, no recovered payload).
    {
      std::vector<unsigned char> shortPkt;
      put16(shortPkt, 21);
      c.ok(sendToPort(rtpPort,
                      buildRtp(kRtxPT, 9002, 63000, kRtxSSRC, true, shortPkt)),
           "E: short RTX payload sent");
    }
    // E4: bad padding (padding bit set, length byte exceeds the packet).
    {
      std::vector<unsigned char> p =
        buildRtx(kRtxPT, 9003, 63000, kRtxSSRC, true, 22, goodNal);
      p[0] |= 0x20;
      p[p.size() - 1] = 0xFF;
      c.ok(sendToPort(rtpPort, p), "E: bad-padding RTX sent");
    }
    // E5: truncated header extension (length word claims data that is absent).
    {
      std::vector<unsigned char> p =
        buildRtx(kRtxPT, 9004, 63000, kRtxSSRC, true, 23, goodNal);
      p[0] |= 0x10;
      // No CSRC: extension length word is at offset 14.
      p[14] = 0x7F;
      p[15] = 0xFF;
      c.ok(sendToPort(rtpPort, p), "E: truncated-extension RTX sent");
    }
    // A perfectly normal primary NAL must still be delivered cleanly.
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 30, 72000, kPrimarySSRC, true, goodNal)),
         "E: send a normal primary NAL after the malformed RTX burst");
    Runner runner;
    runner.scheduler = scheduler;
    runner.source = sub->readSource();
    runner.rtpSource = sub->rtpSource();
    runner.wantedFrames = 1;
    c.ok(runner.run(), "E: normal NAL still delivered");
    c.ok(runner.frames.size() == 1 && runner.frames[0].bytes == goodNal,
         "E: malformed RTX packets did not corrupt framing");
    stopSession(ctx);
  }

  // ---- Scenario F: the recovery gate.  An RTX packet seen before any valid
  //      primary packet is discarded; the primary stream still reads. --------
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    MediaSubsession* sub = ctx.sub;
    unsigned short const rtpPort = sub->clientPortNum();
    std::vector<unsigned char> nal = {0x65, 0xAA, 0xBB};
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 1, 81000, kRtxSSRC, true, 0, nal)),
         "F: RTX arrives before any primary packet");
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 0, 81000, kPrimarySSRC, true, nal)),
         "F: primary NAL then arrives");
    Runner runner;
    runner.scheduler = scheduler;
    runner.source = sub->readSource();
    runner.rtpSource = sub->rtpSource();
    runner.wantedFrames = 1;
    c.ok(runner.run(), "F: primary NAL delivered");
    c.ok(runner.frames.size() == 1 && runner.frames[0].bytes == nal,
         "F: ordinary video remains readable without recovery");
    RTPRetransmissionStats* rs = multiFramed(*sub)->retransmissionStats();
    c.ok(rs != nullptr && rs->totNumPacketsReceived() == 1
         && rs->numPacketsRecovered() == 0,
         "F: pre-gate RTX is counted but not used for recovery");
    stopSession(ctx);
  }

  // ---- Scenario H: an INVALID primary-SSRC packet (bad padding) must not
  //      open the recovery gate; an RTX in that window is counted but used,
  //      and a later valid primary still reads normally. --------------------
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    MediaSubsession* sub = ctx.sub;
    unsigned short const rtpPort = sub->clientPortNum();
    std::vector<unsigned char> goodNal = {0x65, 0x99};

    // A primary-SSRC packet whose padding length is impossible.
    std::vector<unsigned char> badPrimary =
      buildRtp(kPrimaryPT, 0, 82000, kPrimarySSRC, true, goodNal);
    badPrimary[0] |= 0x20;
    badPrimary[badPrimary.size() - 1] = 0xFF;
    c.ok(sendToPort(rtpPort, badPrimary),
         "H: invalid primary-SSRC packet (bad padding) sent");
    // RTX while the gate is still shut.
    c.ok(sendToPort(rtpPort,
                    buildRtx(kRtxPT, 2, 82000, kRtxSSRC, true, 0, goodNal)),
         "H: RTX sent while the gate is shut");
    // A genuinely valid primary packet opens the gate and must be delivered.
    c.ok(sendToPort(rtpPort,
                    buildRtp(kPrimaryPT, 1, 82000, kPrimarySSRC, true, goodNal)),
         "H: valid primary packet then arrives");
    Runner runner;
    runner.scheduler = scheduler;
    runner.source = sub->readSource();
    runner.rtpSource = sub->rtpSource();
    runner.wantedFrames = 1;
    c.ok(runner.run(), "H: valid primary NAL delivered");
    c.ok(runner.frames.size() == 1 && runner.frames[0].bytes == goodNal,
         "H: the invalid packet did not corrupt normal framing");
    RTPRetransmissionStats* rs = multiFramed(*sub)->retransmissionStats();
    c.ok(rs != nullptr && rs->totNumPacketsReceived() == 1
         && rs->numPacketsRecovered() == 0,
         "H: an RTX seen while the gate was shut is counted but not recovered");
    stopSession(ctx);
  }

  // ---- Scenario G: RTP-over-TCP interleaved delivery of the same recovery
  //      path, driven through the real interleaved framing state machine. ---
  {
    SessionCtx ctx = startSession(env);
    if (!ctx.started) {
      c.ok(false, "session initiation (socket setup) succeeded");
      env->reclaim(); delete scheduler;
      return c.result();
    }
    MediaSubsession* sub = ctx.sub;
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bool tcpOk = ::bind(listener, (sockaddr*)&addr, sizeof(addr)) == 0
                 && ::listen(listener, 1) == 0;
    c.ok(tcpOk, "G: loopback TCP listener created");
    if (tcpOk) {
      socklen_t alen = sizeof(addr);
      ::getsockname(listener, (sockaddr*)&addr, &alen);
      int client = ::socket(AF_INET, SOCK_STREAM, 0);
      c.ok(::connect(client, (sockaddr*)&addr, sizeof(addr)) == 0,
           "G: TCP peer connects");
      int server = ::accept(listener, nullptr, nullptr);
      c.ok(server >= 0, "G: connection accepted");

      // The RTP source now reads interleaved frames on channel 0.
      sub->rtpSource()->setStreamSocket(server, 0, nullptr);

      std::vector<unsigned char> fragI(2, 'I'), fragJ(2, 'J');
      auto start = fuPayload(0x85, fragI);
      auto mid = fuPayload(0x45, fragJ);
      std::vector<unsigned char> expect;
      expect.push_back(0x65);
      expect.insert(expect.end(), fragI.begin(), fragI.end());
      expect.insert(expect.end(), fragJ.begin(), fragJ.end());

      auto f1 = interleave(0,
                 buildRtp(kPrimaryPT, 40, 90000, kPrimarySSRC, false, start));
      auto f2 = interleave(0,
                 buildRtx(kRtxPT, 9100, 90000, kRtxSSRC, true, 41, mid));
      c.ok(sendAll(client, f1.data(), f1.size())
           && sendAll(client, f2.data(), f2.size()),
           "G: interleaved primary + RTX frames written");

      Runner runner;
      runner.scheduler = scheduler;
      runner.source = sub->readSource();
      runner.rtpSource = sub->rtpSource();
      runner.wantedFrames = 1;
      c.ok(runner.run(), "G: NAL delivered over TCP interleaved framing");
      c.ok(runner.frames.size() == 1 && runner.frames[0].bytes == expect,
           "G: RTX recovery works on the TCP-interleaved path");
      ::close(client);
      ::close(server);
    }
    ::close(listener);
    stopSession(ctx);
  }

  env->reclaim();
  delete scheduler;
  return c.result();
}

// ===========================================================================
// test_rtx_stream_isolation: two subsessions must never share state
// ===========================================================================
int test_rtx_stream_isolation() {
  printf("== test_rtx_stream_isolation ==\n");
  Checker c;
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  // Two video subsessions that deliberately reuse the same payload types
  // (96/97) and will reuse the same OSN values; only the SSRC pairs differ.
  char const* sdp =
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=local\r\n"
    "t=0 0\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "m=video 0 RTP/AVP 96 97\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96\r\n"
    "a=ssrc-group:FID 1111 2222\r\n"
    "m=video 0 RTP/AVP 96 97\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96\r\n"
    "a=ssrc-group:FID 3333 4444\r\n";

  MediaSession* session = MediaSession::createNew(*env, sdp);
  c.ok(session != nullptr, "two-subsession SDP parses");
  MediaSubsessionIterator it(*session);
  MediaSubsession* a = it.next();
  MediaSubsession* b = it.next();
  c.ok(a != nullptr && b != nullptr, "two video subsessions present");
  c.ok(a->initiate() && b->initiate(), "both subsessions initiate");
  if (a->readSource() == NULL || b->readSource() == NULL) {
    c.ok(false, "both subsessions have a read source");
    Medium::close(session); env->reclaim(); delete scheduler;
    return c.result();
  }
  a->rtpSource()->setPacketReorderingThresholdTime(0);
  b->rtpSource()->setPacketReorderingThresholdTime(0);

  c.ok(a->rtxConfig().primarySSRC == 1111
       && b->rtxConfig().primarySSRC == 3333,
       "parsed RTX configs are kept per subsession");
  c.ok(multiFramed(*a)->retransmissionStats()
       != multiFramed(*b)->retransmissionStats(),
       "RTX stats objects are distinct per subsession");

  unsigned short portA = a->clientPortNum();

  // Each subsession gets a single-packet NAL with the SAME RTP sequence
  // number (7) but on its own SSRC pair and port.
  std::vector<unsigned char> nalA = {0x65, 'A', '1'};
  std::vector<unsigned char> nalB = {0x65, 'B', '2'};
  c.ok(sendToPort(portA,
                  buildRtp(kPrimaryPT, 7, 1000, 1111, true, nalA)),
       "send NAL to subsession A");
  c.ok(sendToPort(b->clientPortNum(),
                  buildRtp(kPrimaryPT, 7, 1000, 3333, true, nalB)),
       "send same-seq NAL to subsession B");

  Runner ra;
  ra.scheduler = scheduler;
  ra.source = a->readSource();
  ra.rtpSource = a->rtpSource();
  ra.wantedFrames = 1;
  Runner rb;
  rb.scheduler = scheduler;
  rb.source = b->readSource();
  rb.rtpSource = b->rtpSource();
  rb.wantedFrames = 1;
  c.ok(ra.run() && rb.run(), "both subsessions deliver a frame");
  c.ok(ra.frames.size() == 1 && ra.frames[0].bytes == nalA,
       "subsession A outputs only its own NAL");
  c.ok(rb.frames.size() == 1 && rb.frames[0].bytes == nalB,
       "subsession B outputs only its own NAL");

  // An RTX packet for B's SSRC pair sent to A's port must be ignored by A.
  std::vector<unsigned char> foreignRtx = {0x65, 'X', 'X'};
  c.ok(sendToPort(portA,
                  buildRtx(kRtxPT, 50, 2000, 4444, true, 7, foreignRtx)),
       "send B's RTX SSRC packet at A's socket");
  // An RTX with A's RTX SSRC but an OSN A already delivered is a no-op.
  c.ok(sendToPort(portA,
                  buildRtx(kRtxPT, 51, 2000, 2222, true, 7, foreignRtx)),
       "send RTX at A with an OSN A already delivered");
  c.ok(multiFramed(*a)->retransmissionStats()->numPacketsRecovered() == 0,
       "A recovers nothing from B's packets or already-delivered OSNs");

  // Independent reordering: recover a gap in B only; A is untouched.
  std::vector<unsigned char> f1 = {'P'}, f2 = {'Q'};
  c.ok(sendToPort(b->clientPortNum(),
                  buildRtp(kPrimaryPT, 100, 3000, 3333, false,
                           fuPayload(0x85, f1))),
       "B: FU start");
  c.ok(sendToPort(b->clientPortNum(),
                  buildRtx(kRtxPT, 700, 3000, 4444, true, 101,
                           fuPayload(0x45, f2))),
       "B: RTX completes the NAL");
  std::vector<unsigned char> expectB;
  expectB.push_back(0x65);
  expectB.push_back('P');
  expectB.push_back('Q');
  Runner rb2;
  rb2.scheduler = scheduler;
  rb2.source = b->readSource();
  rb2.rtpSource = b->rtpSource();
  rb2.wantedFrames = 1;
  c.ok(rb2.run(), "B recovers its NAL");
  c.ok(rb2.frames.size() == 1 && rb2.frames[0].bytes == expectB,
       "B's recovered NAL is correct and independent of A");
  c.ok(multiFramed(*a)->retransmissionStats()->totNumPacketsReceived()
       == multiFramed(*a)->retransmissionStats()->numPacketsRejected(),
       "A's RTX stats are unaffected by B's recovery");

  Medium::close(session);
  env->reclaim();
  delete scheduler;
  return c.result();
}
