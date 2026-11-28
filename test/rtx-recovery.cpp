// test/rtx-recovery.cpp — tests for receiver-side RTP retransmission (RTX, RFC 4588)
// recovery, SSRC-multiplexed into the primary stream's session.
//
// Three entry points (driven by task_support/target_main.cpp):
//   test_rtx_sdp()              - SDP parsing/validation of "rtpmap:.. rtx/..",
//                                 "fmtp:.. apt=.." and "ssrc-group:FID" declarations.
//   test_rtx_h264_recovery()    - end-to-end recovery of a lost H.264 FU fragment over
//                                 loopback UDP, incl. sequence-number wrap, RTCP SR
//                                 time mapping, and per-SSRC reception statistics.
//   test_rtx_stream_isolation() - two subsessions sharing payload types and OSNs must
//                                 not leak configuration, reordering, or statistics
//                                 into each other (one over UDP, one RTP-over-TCP).
//
// Each function returns the number of successful assertions (> 0) on success,
// or a non-positive number on failure.

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

////////// Assertion bookkeeping //////////

struct Check {
  int passed = 0;
  int failed = 0;
  void ok(bool cond, char const* what) {
    if (cond) { ++passed; printf("  ok:   %s\n", what); }
    else { ++failed; printf("  FAIL: %s\n", what); }
  }
  int result() const { return failed == 0 ? passed : -failed; }
};

////////// RTP/RTCP packet builders //////////

struct RtpSpec {
  unsigned char pt = 96;
  unsigned short seq = 0;
  unsigned timestamp = 0;
  unsigned ssrc = 0;
  bool marker = false;
  unsigned char csrcCount = 0; // emits that many dummy CSRC identifiers
  bool extension = false;      // emits a 1-word header extension
  unsigned padding = 0;        // emits that many padding bytes
  unsigned char const* payload = nullptr;
  unsigned payloadSize = 0;
};

unsigned buildRtp(unsigned char* out, RtpSpec const& s) {
  unsigned char* p = out;
  unsigned char b0 = 0x80 | (unsigned char)(s.csrcCount & 0x0F);
  if (s.extension) b0 |= 0x10;
  if (s.padding > 0) b0 |= 0x20;
  *p++ = b0;
  *p++ = (unsigned char)(s.marker ? (0x80 | s.pt) : s.pt);
  *p++ = (unsigned char)(s.seq >> 8);
  *p++ = (unsigned char)(s.seq & 0xFF);
  *p++ = (unsigned char)(s.timestamp >> 24);
  *p++ = (unsigned char)(s.timestamp >> 16);
  *p++ = (unsigned char)(s.timestamp >> 8);
  *p++ = (unsigned char)(s.timestamp);
  *p++ = (unsigned char)(s.ssrc >> 24);
  *p++ = (unsigned char)(s.ssrc >> 16);
  *p++ = (unsigned char)(s.ssrc >> 8);
  *p++ = (unsigned char)(s.ssrc);
  for (unsigned i = 0; i < s.csrcCount; ++i) {
    *p++ = 0xDE; *p++ = 0xAD; *p++ = 0xBE; *p++ = (unsigned char)i;
  }
  if (s.extension) {
    *p++ = 0xBE; *p++ = 0xDE; *p++ = 0x00; *p++ = 0x01; // profile, length = 1 word
    *p++ = 0x11; *p++ = 0x22; *p++ = 0x33; *p++ = 0x44;
  }
  if (s.payloadSize > 0) { memcpy(p, s.payload, s.payloadSize); p += s.payloadSize; }
  for (unsigned i = 0; i < s.padding; ++i) {
    *p++ = (i + 1 == s.padding) ? (unsigned char)s.padding : 0;
  }
  return (unsigned)(p - out);
}

// Builds an RTX (RFC 4588) packet: RTP header (RTX payload type/SSRC, own sequence
// number, original timestamp/marker) followed by the 2-byte OSN and the original
// codec payload.
unsigned buildRtx(unsigned char* out, RtpSpec s, unsigned short osn,
                  unsigned char const* origPayload, unsigned origSize) {
  unsigned char payload[1500];
  payload[0] = (unsigned char)(osn >> 8);
  payload[1] = (unsigned char)(osn & 0xFF);
  memcpy(payload + 2, origPayload, origSize);
  s.payload = payload;
  s.payloadSize = origSize + 2;
  return buildRtp(out, s);
}

// Builds a minimal RTCP Sender Report (28 bytes).
unsigned buildRtcpSR(unsigned char* out, unsigned ssrc,
                     unsigned ntpMsw, unsigned ntpLsw, unsigned rtpTimestamp) {
  unsigned char* p = out;
  *p++ = 0x80; // V=2, RC=0
  *p++ = 200;  // PT = SR
  *p++ = 0x00; *p++ = 6; // length = 7 words - 1
  unsigned words[6] = {ssrc, ntpMsw, ntpLsw, rtpTimestamp, 0, 0};
  for (unsigned w : words) {
    *p++ = (unsigned char)(w >> 24);
    *p++ = (unsigned char)(w >> 16);
    *p++ = (unsigned char)(w >> 8);
    *p++ = (unsigned char)(w);
  }
  return (unsigned)(p - out);
}

////////// Loopback UDP sender //////////

struct UdpSender {
  int fd = -1;
  struct sockaddr_in rtpAddr {};
  struct sockaddr_in rtcpAddr {};

  bool open(unsigned short rtpPort) {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    rtpAddr.sin_family = AF_INET;
    rtpAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rtpAddr.sin_port = htons(rtpPort);
    rtcpAddr = rtpAddr;
    rtcpAddr.sin_port = htons((unsigned short)(rtpPort | 1));
    return true;
  }
  bool sendRtp(unsigned char const* p, unsigned n) const {
    return sendto(fd, p, n, 0, (struct sockaddr*)&rtpAddr, sizeof rtpAddr) == (ssize_t)n;
  }
  bool sendRtcp(unsigned char const* p, unsigned n) const {
    return sendto(fd, p, n, 0, (struct sockaddr*)&rtcpAddr, sizeof rtcpAddr) == (ssize_t)n;
  }
};

////////// Frame-collecting reader, driven by the event loop //////////

struct FrameEntry {
  std::vector<unsigned char> data;
  struct timeval pt {};
  unsigned truncated = 0;
};

struct Reader {
  FramedSource* source = nullptr;
  TaskScheduler* scheduler = nullptr;
  unsigned char buf[4096];
  std::vector<FrameEntry> frames;
  EventLoopWatchVariable stop{0};
  unsigned stopAt = 0; // stop the event loop once this many frames have arrived
  bool armed = false;
};

void onSourceClose(void* clientData); // forward

void onFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
             struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  Reader& r = *(Reader*)clientData;
  FrameEntry e;
  e.data.assign(r.buf, r.buf + frameSize);
  e.pt = presentationTime;
  e.truncated = numTruncatedBytes;
  r.frames.push_back(e);
  if (r.stopAt != 0 && r.frames.size() >= r.stopAt) {
    r.stop = 1;
    r.armed = false;
    return;
  }
  r.source->getNextFrame(r.buf, sizeof r.buf, onFrame, &r, onSourceClose, &r);
}

void onSourceClose(void* clientData) {
  Reader& r = *(Reader*)clientData;
  r.stop = 2;
  r.armed = false;
}

void onWatchdog(void* clientData) {
  // Backstop only: fires if an expected frame never arrives, so a broken
  // implementation fails the test instead of hanging it.
  ((Reader*)clientData)->stop = 3;
}

// Runs the event loop until "targetFrames" frames have been collected (or the
// watchdog fires).  Returns True iff the target was reached.
bool runUntil(Reader& r, unsigned targetFrames) {
  if (r.frames.size() >= targetFrames) return true;
  r.stop = 0;
  r.stopAt = targetFrames;
  if (!r.armed) {
    r.armed = true;
    r.source->getNextFrame(r.buf, sizeof r.buf, onFrame, &r, onSourceClose, &r);
  }
  TaskToken watchdog = r.scheduler->scheduleDelayedTask(5000000, onWatchdog, &r);
  r.scheduler->doEventLoop(&r.stop);
  r.scheduler->unscheduleDelayedTask(watchdog);
  r.stopAt = 0;
  return r.stop == 1;
}

////////// Waiting for an incoming RTCP SR to be processed //////////

struct SrWait {
  RTPSource* source;
  unsigned ssrc;
  unsigned ntpMsw;
  TaskScheduler* scheduler;
  EventLoopWatchVariable* stop;
  TaskToken self;
};

void checkSrProcessed(void* clientData) {
  SrWait& w = *(SrWait*)clientData;
  RTPReceptionStats* stats = w.source->receptionStatsDB().lookup(w.ssrc);
  if (stats != nullptr && stats->lastReceivedSR_NTPmsw() == w.ntpMsw) {
    *w.stop = 1;
    return;
  }
  w.self = w.scheduler->scheduleDelayedTask(1000, checkSrProcessed, &w);
}

bool waitForSr(Reader& r, RTPSource* source, unsigned ssrc, unsigned ntpMsw) {
  SrWait w;
  w.source = source; w.ssrc = ssrc; w.ntpMsw = ntpMsw;
  w.scheduler = r.scheduler; w.stop = &r.stop; w.self = nullptr;
  r.stop = 0;
  w.self = r.scheduler->scheduleDelayedTask(0, checkSrProcessed, &w);
  TaskToken watchdog = r.scheduler->scheduleDelayedTask(5000000, onWatchdog, &r);
  r.scheduler->doEventLoop(&r.stop);
  r.scheduler->unscheduleDelayedTask(watchdog);
  if (r.stop != 1) r.scheduler->unscheduleDelayedTask(w.self);
  return r.stop == 1;
}

////////// Small helpers //////////

bool frameEquals(FrameEntry const& e, std::vector<unsigned char> const& expected) {
  return e.data == expected && e.truncated == 0;
}

bool ptEquals(FrameEntry const& e, long sec, long usec) {
  return e.pt.tv_sec == sec && e.pt.tv_usec == usec;
}

MediaSubsession* firstSubsession(MediaSession& session) {
  MediaSubsessionIterator iter(session);
  return iter.next();
}

} // namespace

////////// test_rtx_sdp //////////

int test_rtx_sdp() {
  Check C;
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  char const* sdpHead =
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=rtx\r\n"
    "t=0 0\r\n"
    "c=IN IP4 127.0.0.1\r\n";

  // 1. A complete, valid RTX declaration enables recovery automatically:
  {
    std::string sdp = std::string(sdpHead) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 packetization-mode=1\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    C.ok(s != nullptr, "complete RTX SDP parses");
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && sub->rtpPayloadFormat() == 96,
         "primary payload format is the first m= format");
    C.ok(sub != nullptr && strcmp(sub->codecName(), "H264") == 0, "codec is H264");
    C.ok(sub != nullptr && sub->attrVal_str("apt")[0] == '\0',
         "RTX fmtp does not pollute the primary codec's attributes");
    C.ok(sub != nullptr && sub->attrVal_int("packetization-mode") == 1,
         "primary fmtp attributes are intact");
    C.ok(sub != nullptr && sub->initiate(), "initiate() succeeds");
    RTPSource* rs = (sub != nullptr) ? sub->rtpSource() : nullptr;
    C.ok(rs != nullptr && rs->hasRTXRetransmissionConfig(),
         "RTX recovery is auto-enabled by initiate()");
    C.ok(rs != nullptr && rs->rtxPayloadFormat() == 97
         && rs->rtxPrimarySSRC() == 1111 && rs->rtxSSRC() == 2222,
         "RTX configuration values (payload type, primary SSRC, RTX SSRC)");
    Medium::close(s);
  }

  // 2. The declaration lines may appear in any order:
  {
    std::string sdp = std::string(sdpHead) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=ssrc-group:FID 1111 2222\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:96 packetization-mode=1\r\n"
      "a=rtpmap:96 H264/90000\r\n";
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && sub->initiate()
         && sub->rtpSource()->hasRTXRetransmissionConfig()
         && sub->rtpSource()->rtxSSRC() == 2222,
         "declaration order does not matter");
    Medium::close(s);
  }

  // 3. Identical duplicate declarations are allowed:
  {
    std::string sdp = std::string(sdpHead) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 1111 2222\r\n"
      "a=ssrc-group:FID 1111 2222\r\n";
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && sub->initiate()
         && sub->rtpSource()->hasRTXRetransmissionConfig(),
         "identical duplicate declarations are allowed");
    Medium::close(s);
  }

  // 4. H265 works the same way:
  {
    std::string sdp = std::string(sdpHead) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H265/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=ssrc-group:FID 42 43\r\n";
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && sub->initiate()
         && sub->rtpSource()->hasRTXRetransmissionConfig()
         && sub->rtpSource()->rtxPrimarySSRC() == 42,
         "H265 primary stream with RTX");
    Medium::close(s);
  }

  // 5. Incomplete declarations (rtx without apt, or without FID) leave recovery
  //    disabled, but the subsession still initiates normally:
  {
    std::string sdp = std::string(sdpHead) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=ssrc-group:FID 1111 2222\r\n"; // no "apt"
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && sub->initiate()
         && !sub->rtpSource()->hasRTXRetransmissionConfig(),
         "rtx without apt: no recovery, normal initiation");
    Medium::close(s);
  }
  {
    std::string sdp = std::string(sdpHead) +
      "m=video 0 RTP/AVP 96 97\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"; // no "ssrc-group:FID"
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && sub->initiate()
         && !sub->rtpSource()->hasRTXRetransmissionConfig(),
         "rtx without FID: no recovery, normal initiation");
    Medium::close(s);
  }

  // 6. No RTX declarations at all: unchanged behavior:
  {
    std::string sdp = std::string(sdpHead) +
      "m=video 0 RTP/AVP 96\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 packetization-mode=1\r\n";
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && sub->initiate()
         && !sub->rtpSource()->hasRTXRetransmissionConfig(),
         "SDP without RTX works as before");
    Medium::close(s);
  }

  // 7. Contradictory or invalid declarations make initiation fail:
  struct BadCase { char const* mediaLines; char const* what; };
  BadCase const badCases[] = {
    {"m=video 0 RTP/AVP 96 97\r\n"
     "a=rtpmap:96 H264/90000\r\n"
     "a=rtpmap:97 rtx/90000\r\n"
     "a=fmtp:97 apt=98\r\n" // apt does not name the primary payload format
     "a=ssrc-group:FID 1111 2222\r\n", "apt not naming the primary PT fails"},
    {"m=video 0 RTP/AVP 96 97\r\n"
     "a=rtpmap:96 H264/90000\r\n"
     "a=rtpmap:97 rtx/8000\r\n" // clock frequency differs from the primary stream
     "a=fmtp:97 apt=96\r\n"
     "a=ssrc-group:FID 1111 2222\r\n", "RTX clock frequency mismatch fails"},
    {"m=video 0 RTP/AVP 96 97 98\r\n"
     "a=rtpmap:96 H264/90000\r\n"
     "a=rtpmap:97 rtx/90000\r\n"
     "a=rtpmap:98 rtx/90000\r\n" // a second RTX mapping for the same primary stream
     "a=fmtp:97 apt=96\r\n"
     "a=ssrc-group:FID 1111 2222\r\n", "multiple RTX payload formats fail"},
    {"m=video 0 RTP/AVP 96 97\r\n"
     "a=rtpmap:96 H264/90000\r\n"
     "a=rtpmap:97 rtx/90000\r\n"
     "a=fmtp:97 apt=96\r\n"
     "a=ssrc-group:FID 1111 2222\r\n"
     "a=ssrc-group:FID 1111 3333\r\n", "contradictory FID groups fail"},
    {"m=video 0 RTP/AVP 96 97\r\n"
     "a=rtpmap:96 H264/90000\r\n"
     "a=rtpmap:97 rtx/90000\r\n"
     "a=fmtp:97 apt=96\r\n"
     "a=ssrc-group:FID 1111 1111\r\n", "FID with identical SSRCs fails"},
    {"m=video 0 RTP/AVP 96 97\r\n"
     "a=rtpmap:96 H264/90000\r\n"
     "a=rtpmap:97 rtx/90000\r\n"
     "a=fmtp:97 apt=abc\r\n" // non-numeric apt value
     "a=ssrc-group:FID 1111 2222\r\n", "non-numeric apt value fails"},
    {"m=video 0 RTP/AVP 96 97\r\n"
     "a=rtpmap:96 H264/90000\r\n"
     "a=rtpmap:97 rtx/90000\r\n"
     "a=fmtp:97 apt=96\r\n"
     "a=ssrc-group:FID 1111\r\n", "FID with a single SSRC fails"},
  };
  for (BadCase const& bc : badCases) {
    std::string sdp = std::string(sdpHead) + bc.mediaLines;
    MediaSession* s = MediaSession::createNew(*env, sdp.c_str());
    MediaSubsession* sub = s ? firstSubsession(*s) : nullptr;
    C.ok(sub != nullptr && !sub->initiate(), bc.what);
    Medium::close(s);
  }

  env->reclaim();
  delete scheduler;
  return C.result();
}

////////// test_rtx_h264_recovery //////////

int test_rtx_h264_recovery() {
  Check C;
  unsigned const PRIMARY_SSRC = 1111, RTX_SSRC = 2222;

  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  char const* sdp =
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=rtx-recovery\r\n"
    "t=0 0\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "m=video 0 RTP/AVP 96 97\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96\r\n"
    "a=ssrc-group:FID 1111 2222\r\n";

  MediaSession* session = MediaSession::createNew(*env, sdp);
  C.ok(session != nullptr, "session parses");
  MediaSubsession* sub = session ? firstSubsession(*session) : nullptr;
  C.ok(sub != nullptr && sub->initiate(), "subsession initiates");
  if (sub == nullptr || sub->readSource() == nullptr) {
    Medium::close(session);
    env->reclaim();
    delete scheduler;
    return C.failed > 0 ? C.result() : -1;
  }
  RTPSource* rtpSource = sub->rtpSource();
  RTPReceptionStatsDB& statsDB = rtpSource->receptionStatsDB();

  UdpSender udp;
  C.ok(udp.open(sub->clientPortNum()), "UDP sender opened");

  Reader reader;
  reader.source = sub->readSource();
  reader.scheduler = scheduler;

  unsigned char pkt[1600];
  unsigned n;
  bool sent = true;

  // --- Stage A: RTX packets are gated until a valid primary packet is seen ---
  {
    unsigned char nal1[] = {0x65, 0x01};
    n = buildRtx(pkt, RtpSpec{97, 5000, 800000, RTX_SSRC}, 10, nal1, sizeof nal1);
    sent &= udp.sendRtp(pkt, n); // before any primary packet: must be dropped
    n = buildRtx(pkt, RtpSpec{97, 5001, 800000, 9999}, 10, nal1, sizeof nal1);
    sent &= udp.sendRtp(pkt, n); // wrong RTX SSRC: must be dropped
    RtpSpec unknownPt{98, 7, 800000, PRIMARY_SSRC};
    unknownPt.payload = nal1; unknownPt.payloadSize = sizeof nal1;
    n = buildRtp(pkt, unknownPt);
    sent &= udp.sendRtp(pkt, n); // unknown payload type: must be dropped

    unsigned char sentinel[] = {0x65, 0x11};
    RtpSpec first{96, 65533, 800000, PRIMARY_SSRC, true};
    first.payload = sentinel; first.payloadSize = sizeof sentinel;
    n = buildRtp(pkt, first);
    sent &= udp.sendRtp(pkt, n); // first valid primary packet

    C.ok(sent, "stage A packets sent");
    C.ok(runUntil(reader, 1), "stage A: sentinel frame arrived");
    C.ok(reader.frames.size() == 1 && frameEquals(reader.frames[0], {0x65, 0x11}),
         "stage A: only the primary frame was delivered");
    C.ok(statsDB.lookup(RTX_SSRC) == nullptr,
         "gated RTX packets create no RTX reception record");
    C.ok(statsDB.lookup(9999) == nullptr,
         "RTX packets from a wrong SSRC create no reception record");
    RTPReceptionStats* prim = statsDB.lookup(PRIMARY_SSRC);
    C.ok(prim != nullptr && prim->totNumPacketsReceived() == 1,
         "primary stream recorded exactly its one packet");
    C.ok(statsDB.totNumPacketsReceived() == 1, "no fabricated network receptions");
  }

  // --- Stage B: FU fragment lost across the 65535/0 wrap, recovered via RTX ---
  unsigned char const fuStart[4] = {0x7C, 0x85, 'A', 'A'}; // FU-A, S=1, type 5
  unsigned char const fuMid[4]   = {0x7C, 0x05, 'B', 'B'}; // FU-A, middle
  unsigned char const fuEnd[4]   = {0x7C, 0x45, 'C', 'C'}; // FU-A, E=1
  unsigned jitterBefore = 0;
  {
    RtpSpec start{96, 65534, 900000, PRIMARY_SSRC};
    start.payload = fuStart; start.payloadSize = sizeof fuStart;
    n = buildRtp(pkt, start);
    sent &= udp.sendRtp(pkt, n);
    RtpSpec end{96, 0, 900000, PRIMARY_SSRC, true};
    end.payload = fuEnd; end.payloadSize = sizeof fuEnd;
    n = buildRtp(pkt, end);
    sent &= udp.sendRtp(pkt, n); // note: seq 65535 (the middle fragment) is lost
    n = buildRtx(pkt, RtpSpec{97, 5002, 900000, RTX_SSRC, true}, 65535, fuMid, sizeof fuMid);
    sent &= udp.sendRtp(pkt, n); // ... and retransmitted

    C.ok(sent, "stage B packets sent");
    C.ok(runUntil(reader, 2), "stage B: recovered FU frame arrived");
    C.ok(reader.frames.size() >= 2
         && frameEquals(reader.frames[1], {0x65, 'A', 'A', 'B', 'B', 'C', 'C'}),
         "complete NAL reassembled across the 65535/0 sequence wrap");

    RTPReceptionStats* prim = statsDB.lookup(PRIMARY_SSRC);
    RTPReceptionStats* rtx = statsDB.lookup(RTX_SSRC);
    C.ok(prim != nullptr && prim->totNumPacketsReceived() == 3,
         "RTX did not fabricate a primary-stream network reception");
    C.ok(prim != nullptr && prim->totNumPacketsExpected() == 4,
         "the lost primary packet is still visible as loss");
    C.ok(rtx != nullptr && rtx->totNumPacketsReceived() == 1,
         "RTX network packet recorded under the RTX SSRC");
    jitterBefore = prim != nullptr ? prim->jitter() : 0;
  }

  // --- Stage B2: late original + duplicate RTX + wild-timestamp late RTX ---
  {
    RtpSpec late{96, 65535, 900000, PRIMARY_SSRC};
    late.payload = fuMid; late.payloadSize = sizeof fuMid;
    n = buildRtp(pkt, late);
    sent &= udp.sendRtp(pkt, n); // the original packet arrives late
    n = buildRtx(pkt, RtpSpec{97, 5003, 900000, RTX_SSRC}, 65535, fuMid, sizeof fuMid);
    sent &= udp.sendRtp(pkt, n); // the same retransmission arrives twice
    n = buildRtx(pkt, RtpSpec{97, 5004, 55555, RTX_SSRC}, 65534, fuStart, sizeof fuStart);
    sent &= udp.sendRtp(pkt, n); // late RTX with a wild timestamp (jitter trap)

    unsigned char sentinel[] = {0x65, 0x22};
    RtpSpec next{96, 1, 900000, PRIMARY_SSRC, true}; // same RTP ts => jitter-neutral
    next.payload = sentinel; next.payloadSize = sizeof sentinel;
    n = buildRtp(pkt, next);
    sent &= udp.sendRtp(pkt, n);

    C.ok(sent, "stage B2 packets sent");
    C.ok(runUntil(reader, 3), "stage B2: next frame arrived");
    C.ok(reader.frames.size() == 3, "late original and duplicate RTX produced no extra output");
    C.ok(frameEquals(reader.frames[2], {0x65, 0x22}), "post-wrap NAL is intact");

    RTPReceptionStats* prim = statsDB.lookup(PRIMARY_SSRC);
    RTPReceptionStats* rtx = statsDB.lookup(RTX_SSRC);
    C.ok(prim != nullptr && prim->totNumPacketsReceived() == 5,
         "primary stats count real primary packets only");
    C.ok(rtx != nullptr && rtx->totNumPacketsReceived() == 3,
         "RTX stats count all RTX network packets");
    C.ok(prim != nullptr && prim->jitter() == jitterBefore,
         "late RTX packets did not disturb the primary stream's jitter");
    C.ok(reader.frames[1].pt.tv_sec == reader.frames[2].pt.tv_sec
         && reader.frames[1].pt.tv_usec == reader.frames[2].pt.tv_usec,
         "recovered and primary frames share the stream's time mapping");
  }

  // --- Stage C: RTCP SR establishes the mapping; recovered frames reuse it ---
  {
    unsigned const ntpMsw = 0x83AA7E80u + 2000000u; // NTP for unix time 2000000
    n = buildRtcpSR(pkt, PRIMARY_SSRC, ntpMsw, 0, 910000);
    sent &= udp.sendRtcp(pkt, n);
    C.ok(sent, "RTCP SR sent");
    C.ok(waitForSr(reader, rtpSource, PRIMARY_SSRC, ntpMsw),
         "RTCP SR was processed");

    unsigned char nal2[] = {0x65, 0x33};
    RtpSpec p2{96, 2, 914500, PRIMARY_SSRC, true};
    p2.payload = nal2; p2.payloadSize = sizeof nal2;
    n = buildRtp(pkt, p2);
    sent &= udp.sendRtp(pkt, n);

    unsigned char nal3[] = {0x65, 0x44};
    n = buildRtx(pkt, RtpSpec{97, 5005, 923500, RTX_SSRC, true}, 3, nal3, sizeof nal3);
    sent &= udp.sendRtp(pkt, n); // seq 3 arrives only via RTX

    unsigned char nal4[] = {0x65, 0x55};
    RtpSpec p4{96, 4, 932500, PRIMARY_SSRC, true};
    p4.payload = nal4; p4.payloadSize = sizeof nal4;
    n = buildRtp(pkt, p4);
    sent &= udp.sendRtp(pkt, n);

    C.ok(sent, "stage C packets sent");
    C.ok(runUntil(reader, 6), "stage C: frames arrived");
    C.ok(reader.frames.size() >= 6 && frameEquals(reader.frames[3], {0x65, 0x33})
         && ptEquals(reader.frames[3], 2000000, 50000),
         "primary frame uses the SR-established time mapping");
    C.ok(reader.frames.size() >= 6 && frameEquals(reader.frames[4], {0x65, 0x44})
         && ptEquals(reader.frames[4], 2000000, 150000),
         "recovered frame uses the SR-established time mapping");
    C.ok(reader.frames.size() >= 6 && frameEquals(reader.frames[5], {0x65, 0x55})
         && ptEquals(reader.frames[5], 2000000, 250000),
         "following primary frame stays on the same mapping");
    C.ok(rtpSource->hasBeenSynchronizedUsingRTCP(),
         "stream reports RTCP synchronization");
  }

  // --- Stage D: malformed packets are dropped without corrupting framing ---
  {
    rtpSource->setPacketReorderingThresholdTime(0); // don't wait out the seq 5/6 gap

    unsigned char shortOsn[] = {0x00}; // RTX payload too short to hold an OSN
    RtpSpec bad1{97, 5006, 941500, RTX_SSRC};
    bad1.payload = shortOsn; bad1.payloadSize = sizeof shortOsn;
    n = buildRtp(pkt, bad1);
    sent &= udp.sendRtp(pkt, n);

    // Truncated header extension (X bit set, but no room for the extension header):
    unsigned char truncExt[14] = {
      0x90, (unsigned char)(0x80 | 96), 0x00, 0x05, // seq 5
      0x00, 0x0E, 0x5A, 0x3C, // ts 941500
      0x00, 0x00, 0x04, 0x57, // SSRC 1111
      0xBE, 0xDE // only 2 of the 4 required extension-header bytes
    };
    sent &= udp.sendRtp(truncExt, sizeof truncExt);

    // Bad padding (padding count larger than the packet):
    unsigned char badPad[15] = {
      0xA0, (unsigned char)(0x80 | 96), 0x00, 0x06, // seq 6
      0x00, 0x0E, 0x5A, 0x3C,
      0x00, 0x00, 0x04, 0x57,
      0x65, 0x66, 0xC8 // last byte claims 200 padding bytes
    };
    sent &= udp.sendRtp(badPad, sizeof badPad);

    // A valid RTX packet that itself carries CSRCs, an extension, and padding:
    unsigned char nal5[] = {0x65, 0x66};
    RtpSpec fancy{97, 5007, 941500, RTX_SSRC, true, 1, true, 4};
    n = buildRtx(pkt, fancy, 7, nal5, sizeof nal5);
    sent &= udp.sendRtp(pkt, n);

    unsigned char nal6[] = {0x65, 0x77};
    RtpSpec p8{96, 8, 950500, PRIMARY_SSRC, true};
    p8.payload = nal6; p8.payloadSize = sizeof nal6;
    n = buildRtp(pkt, p8);
    sent &= udp.sendRtp(pkt, n);

    C.ok(sent, "stage D packets sent");
    C.ok(runUntil(reader, 8), "stage D: framing survived the malformed packets");
    C.ok(reader.frames.size() >= 8 && frameEquals(reader.frames[6], {0x65, 0x66})
         && ptEquals(reader.frames[6], 2000000, 350000),
         "RTX packet with CSRCs/extension/padding recovered seq 7");
    C.ok(reader.frames.size() >= 8 && frameEquals(reader.frames[7], {0x65, 0x77})
         && ptEquals(reader.frames[7], 2000000, 450000),
         "following primary NAL is intact");

    RTPReceptionStats* prim = statsDB.lookup(PRIMARY_SSRC);
    RTPReceptionStats* rtx = statsDB.lookup(RTX_SSRC);
    C.ok(prim != nullptr && prim->totNumPacketsReceived() == 8,
         "malformed primary packets were not counted as primary receptions");
    C.ok(prim != nullptr && prim->totNumPacketsExpected() == 12,
         "primary loss accounting is exact");
    C.ok(rtx != nullptr && rtx->totNumPacketsReceived() == 6,
         "all RTX network packets (incl. the dropped short one) were accounted");
    C.ok(statsDB.totNumPacketsReceived() == 14, "total reception accounting is exact");
  }

  Medium::close(session);
  close(udp.fd);
  env->reclaim();
  delete scheduler;
  return C.result();
}

////////// test_rtx_stream_isolation //////////

int test_rtx_stream_isolation() {
  Check C;

  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  // Two video subsessions that reuse the SAME payload types and the SAME RTP
  // sequence numbers/OSNs, differing only in their FID SSRCs:
  char const* sdp =
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=isolation\r\n"
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
  C.ok(session != nullptr, "two-subsession SDP parses");
  MediaSubsessionIterator iter(*session);
  MediaSubsession* subA = session ? iter.next() : nullptr;
  MediaSubsession* subB = session ? iter.next() : nullptr;
  C.ok(subA != nullptr && subB != nullptr, "two subsessions present");
  C.ok(subA != nullptr && subA->initiate(), "subsession A initiates");
  C.ok(subB != nullptr && subB->initiate(), "subsession B initiates");
  if (subA == nullptr || subB == nullptr
      || subA->readSource() == nullptr || subB->readSource() == nullptr) {
    Medium::close(session);
    env->reclaim();
    delete scheduler;
    return C.failed > 0 ? C.result() : -1;
  }
  C.ok(subA->rtpSource()->hasRTXRetransmissionConfig()
       && subA->rtpSource()->rtxSSRC() == 2222
       && subB->rtpSource()->hasRTXRetransmissionConfig()
       && subB->rtpSource()->rtxSSRC() == 4444,
       "each subsession got its own RTX configuration");

  // Subsession A receives over loopback UDP; subsession B over RTP-over-TCP
  // (interleaved), using a connected socket pair:
  UdpSender udp;
  C.ok(udp.open(subA->clientPortNum()), "UDP sender opened");
  int sv[2] = {-1, -1};
  C.ok(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "TCP socket pair created");
  subB->rtpSource()->setStreamSocket(sv[0], 0 /* RTP channel id */, nullptr);

  Reader readerA, readerB;
  readerA.source = subA->readSource(); readerA.scheduler = scheduler;
  readerB.source = subB->readSource(); readerB.scheduler = scheduler;
  readerB.armed = true;
  readerB.source->getNextFrame(readerB.buf, sizeof readerB.buf, onFrame, &readerB,
                               onSourceClose, &readerB);

  unsigned char pkt[1600];
  unsigned n;
  bool sent = true;

  // The same packet sequence on both subsessions (same PTs, seqs, OSNs):
  struct StreamDef { unsigned primarySSRC; unsigned rtxSSRC; unsigned char tag; };
  StreamDef const defs[2] = {{1111, 2222, 0xA0}, {3333, 4444, 0xB0}};
  for (unsigned i = 0; i < 2; ++i) {
    unsigned char nal1[] = {0x65, (unsigned char)(defs[i].tag | 0x01)};
    RtpSpec p10{96, 10, 5000, defs[i].primarySSRC, true};
    p10.payload = nal1; p10.payloadSize = sizeof nal1;
    n = buildRtp(pkt, p10);
    unsigned char frame[1700];
    unsigned frameLen;
    if (i == 0) {
      sent &= udp.sendRtp(pkt, n);
    } else {
      unsigned char hdr[4] = {'$', 0, (unsigned char)(n >> 8), (unsigned char)(n & 0xFF)};
      memcpy(frame, hdr, 4); memcpy(frame + 4, pkt, n); frameLen = n + 4;
      sent &= write(sv[1], frame, frameLen) == (ssize_t)frameLen;
    }

    unsigned char nal2[] = {0x65, (unsigned char)(defs[i].tag | 0x02)};
    n = buildRtx(pkt, RtpSpec{97, 600, 5000, defs[i].rtxSSRC, true}, 11, nal2, sizeof nal2);
    if (i == 0) {
      sent &= udp.sendRtp(pkt, n);
    } else {
      unsigned char hdr[4] = {'$', 0, (unsigned char)(n >> 8), (unsigned char)(n & 0xFF)};
      memcpy(frame, hdr, 4); memcpy(frame + 4, pkt, n); frameLen = n + 4;
      sent &= write(sv[1], frame, frameLen) == (ssize_t)frameLen;
    }

    unsigned char nal3[] = {0x65, (unsigned char)(defs[i].tag | 0x03)};
    RtpSpec p12{96, 12, 5000, defs[i].primarySSRC, true};
    p12.payload = nal3; p12.payloadSize = sizeof nal3;
    n = buildRtp(pkt, p12);
    if (i == 0) {
      sent &= udp.sendRtp(pkt, n);
    } else {
      unsigned char hdr[4] = {'$', 0, (unsigned char)(n >> 8), (unsigned char)(n & 0xFF)};
      memcpy(frame, hdr, 4); memcpy(frame + 4, pkt, n); frameLen = n + 4;
      sent &= write(sv[1], frame, frameLen) == (ssize_t)frameLen;
    }
  }

  // Cross-injection: each subsession must reject RTX packets carrying the OTHER
  // subsession's RTX SSRC (dropped before any statistics are recorded):
  unsigned char junkA[] = {0x65, 0xFF};
  n = buildRtx(pkt, RtpSpec{97, 700, 5000, 4444}, 13, junkA, sizeof junkA);
  sent &= udp.sendRtp(pkt, n); // B's RTX SSRC, sent to A
  unsigned char junkB[] = {0x65, 0xFE};
  n = buildRtx(pkt, RtpSpec{97, 700, 5000, 2222}, 13, junkB, sizeof junkB);
  {
    unsigned char frame[1700];
    unsigned char hdr[4] = {'$', 0, (unsigned char)(n >> 8), (unsigned char)(n & 0xFF)};
    memcpy(frame, hdr, 4); memcpy(frame + 4, pkt, n);
    sent &= write(sv[1], frame, n + 4) == (ssize_t)(n + 4); // A's RTX SSRC, sent to B
  }

  // Sentinels:
  unsigned char nalA4[] = {0x65, 0xA4};
  RtpSpec pA{96, 13, 5000, 1111, true};
  pA.payload = nalA4; pA.payloadSize = sizeof nalA4;
  n = buildRtp(pkt, pA);
  sent &= udp.sendRtp(pkt, n);
  unsigned char nalB4[] = {0x65, 0xB4};
  RtpSpec pB{96, 13, 5000, 3333, true};
  pB.payload = nalB4; pB.payloadSize = sizeof nalB4;
  n = buildRtp(pkt, pB);
  {
    unsigned char frame[1700];
    unsigned char hdr[4] = {'$', 0, (unsigned char)(n >> 8), (unsigned char)(n & 0xFF)};
    memcpy(frame, hdr, 4); memcpy(frame + 4, pkt, n);
    sent &= write(sv[1], frame, n + 4) == (ssize_t)(n + 4);
  }

  C.ok(sent, "all packets sent");
  C.ok(runUntil(readerA, 4), "subsession A delivered its frames");
  C.ok(runUntil(readerB, 4), "subsession B delivered its frames (RTP-over-TCP)");

  C.ok(readerA.frames.size() == 4
       && frameEquals(readerA.frames[0], {0x65, 0xA1})
       && frameEquals(readerA.frames[1], {0x65, 0xA2})
       && frameEquals(readerA.frames[2], {0x65, 0xA3})
       && frameEquals(readerA.frames[3], {0x65, 0xA4}),
       "subsession A recovered its own stream only");
  C.ok(readerB.frames.size() == 4
       && frameEquals(readerB.frames[0], {0x65, 0xB1})
       && frameEquals(readerB.frames[1], {0x65, 0xB2})
       && frameEquals(readerB.frames[2], {0x65, 0xB3})
       && frameEquals(readerB.frames[3], {0x65, 0xB4}),
       "subsession B recovered its own stream only");

  RTPReceptionStatsDB& dbA = subA->rtpSource()->receptionStatsDB();
  RTPReceptionStatsDB& dbB = subB->rtpSource()->receptionStatsDB();
  RTPReceptionStats* aPrim = dbA.lookup(1111);
  RTPReceptionStats* aRtx = dbA.lookup(2222);
  RTPReceptionStats* bPrim = dbB.lookup(3333);
  RTPReceptionStats* bRtx = dbB.lookup(4444);
  C.ok(aPrim != nullptr && aPrim->totNumPacketsReceived() == 3,
       "A's primary SSRC stats count A's packets only");
  C.ok(aRtx != nullptr && aRtx->totNumPacketsReceived() == 1,
       "A's RTX SSRC stats count A's RTX packets only");
  C.ok(bPrim != nullptr && bPrim->totNumPacketsReceived() == 3,
       "B's primary SSRC stats count B's packets only");
  C.ok(bRtx != nullptr && bRtx->totNumPacketsReceived() == 1,
       "B's RTX SSRC stats count B's RTX packets only");
  C.ok(dbA.lookup(3333) == nullptr && dbA.lookup(4444) == nullptr,
       "none of B's SSRCs leaked into A's statistics");
  C.ok(dbB.lookup(1111) == nullptr && dbB.lookup(2222) == nullptr,
       "none of A's SSRCs leaked into B's statistics");

  Medium::close(session);
  close(sv[0]);
  close(sv[1]);
  close(udp.fd);
  env->reclaim();
  delete scheduler;
  return C.result();
}
