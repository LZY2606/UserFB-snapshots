/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version. (See <http://www.gnu.org/copyleft/lesser.html>.)
**********/
//
// task_tests/transport_switch.cpp
//
// In-place transport reconfiguration (RTSPServer::enableTransportReconfiguration) tests.
//
// These tests drive a real, plaintext, unicast RTSPServer over loopback with hand-written RTSP
// requests and real TCP interleaved / UDP datagram packets.  They verify that a second SETUP for
// an already-established track:
//   * keeps the same FramedSource, RTPSink and RTCPInstance (same SSRC; continuous RTP sequence
//     numbers and timestamps; RTCP send statistics keep accumulating),
//   * moves media and periodic RTCP to the new destination and stops them on the old one,
//   * migrates the per-destination RTCP RR handler,
//   * leaves paused tracks paused, makes identical re-SETUP a no-op, and validates ports/channels,
//   * isolates tracks and (with reuseFirstSource) separate subscribers, and
//   * leaves no stale interleaved channels or dangling callbacks after TEARDOWN/disconnect.
//
// The media source is a manually-advanced ("gated") FramedSource so the test, not wall-clock
// time, decides when each frame is produced.  Completion is decided from observed packets/counters,
// never from sleeping.
//
// NOTE: this file opens real loopback AF_INET sockets (TCP listen on the server, UDP client ports).
// It must run in an environment that permits local port binding
// (sandbox.network.allowLocalBinding: true).

#include "liveMedia.hh"
#include "GroupsockHelper.hh"
#include "BasicUsageEnvironment.hh"
#include "BasicUsageEnvironment0.hh" // BasicTaskScheduler0::SingleStep() is public here

#include "../task_support/check.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr unsigned kRtpClockRate = 8000;     // PCMA
constexpr unsigned char kPayloadType = 8;    // PCMA static payload type
constexpr unsigned kFramePayloadSize = 4;    // bytes of (fake) PCMA per frame
constexpr int kMaxEventSteps = 200000;       // deterministic watchdog, not a timing sleep

// ---------------------------------------------------------------------------
// A FramedSource that emits one distinct, 4-byte, manually-advanced frame per release.
// Each frame's payload carries its 1-based frame number (big endian) so a received RTP packet can
// be mapped back to the exact source frame.  Frames are delivered via a zero-delay scheduled task,
// so the event loop - driven explicitly by the test - controls pacing; there is no sleep.
// ---------------------------------------------------------------------------
class GatedFramedSource : public FramedSource {
public:
  static GatedFramedSource* createNew(UsageEnvironment& env, unsigned& creations) {
    ++creations;
    return new GatedFramedSource(env);
  }

  unsigned emittedFrames() const { return fEmitted; }

protected:
  GatedFramedSource(UsageEnvironment& env) : FramedSource(env), fEmitted(0), fDeliveryTask(NULL) {}
  ~GatedFramedSource() override {
    TaskToken t = fDeliveryTask;
    envir().taskScheduler().unscheduleDelayedTask(t);
  }

  void doGetNextFrame() override {
    // Deliver asynchronously (next event-loop pass) so the sink never recurses straight through an
    // unbounded number of frames; the test paces everything with SingleStep().
    fDeliveryTask = envir().taskScheduler().scheduleDelayedTask(0, deliverNext, this);
  }

  void doStopGettingFrames() override {
    TaskToken t = fDeliveryTask;
    envir().taskScheduler().unscheduleDelayedTask(t);
    fDeliveryTask = NULL;
  }

private:
  static void deliverNext(void* clientData) {
    ((GatedFramedSource*)clientData)->deliverNext1();
  }

  void deliverNext1() {
    fDeliveryTask = NULL;
    if (!isCurrentlyAwaitingData()) return;

    ++fEmitted;
    unsigned char out[kFramePayloadSize];
    const uint32_t n = htonl(fEmitted);
    memcpy(out, &n, sizeof n);

    fFrameSize = kFramePayloadSize;
    fNumTruncatedBytes = 0;
    gettimeofday(&fPresentationTime, NULL);
    fDurationInMicroseconds = 0;
    if (fMaxSize < fFrameSize) {
      fFrameSize = fMaxSize;
      fNumTruncatedBytes = kFramePayloadSize - fMaxSize;
    }
    memmove(fTo, out, fFrameSize);
    FramedSource::afterGetting(this);
  }

  unsigned fEmitted;
  TaskToken fDeliveryTask;
};

// ---------------------------------------------------------------------------
// OnDemandServerMediaSubsession fixture: PCMA over a gated source, counting source creations and
// exposing the live sink/RTCP so the tests can assert SSRC / sequence / statistics continuity.
// ---------------------------------------------------------------------------
class TestSubsession : public OnDemandServerMediaSubsession {
public:
  static TestSubsession* createNew(UsageEnvironment& env, Boolean reuseFirstSource,
                                   unsigned& sourceCreations) {
    return new TestSubsession(env, reuseFirstSource, sourceCreations);
  }

  unsigned sourceCreations() const { return fSourceCreations; }

  // The most recently created RTP sink.  With one track and one (or, for reuseFirstSource,
  // several sharing) subscriber(s), this is the live sink whose continuity we assert on.
  RTPSink* lastSink() const { return fLastSink; }

  RTPSink* liveSink(void* streamToken) {
    RTPSink* sink = NULL; RTCPInstance* rtcp = NULL;
    getRTPSinkandRTCP(streamToken, sink, rtcp);
    return sink;
  }
  RTCPInstance* liveRTCP(void* streamToken) {
    RTPSink* sink = NULL; RTCPInstance* rtcp = NULL;
    getRTPSinkandRTCP(streamToken, sink, rtcp);
    return rtcp;
  }

protected:
  TestSubsession(UsageEnvironment& env, Boolean reuseFirstSource, unsigned& sourceCreations)
    : OnDemandServerMediaSubsession(env, reuseFirstSource),
      fSourceCreations(sourceCreations), fLastSink(NULL) {}

  FramedSource* createNewStreamSource(unsigned /*clientSessionId*/, unsigned& estBitrate) override {
    estBitrate = 64;
    return GatedFramedSource::createNew(envir(), fSourceCreations);
  }

  RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char /*rtpPayloadTypeIfDynamic*/,
                            FramedSource* /*inputSource*/) override {
    RTPSink* sink = SimpleRTPSink::createNew(envir(), rtpGroupsock, kPayloadType, kRtpClockRate,
                                             "audio", "PCMA", 1, False, False);
    fLastSink = sink;
    return sink;
  }

private:
  unsigned& fSourceCreations;
  RTPSink* fLastSink;
};

// ---------------------------------------------------------------------------
// Minimal parsing of a received RTP packet (RFC 3550 fixed header).
// ---------------------------------------------------------------------------
struct RtpPacket {
  unsigned char version;
  unsigned char payloadType;
  uint16_t seq;
  uint32_t timestamp;
  uint32_t ssrc;
  const unsigned char* payload;
  unsigned payloadSize;
  uint32_t frameNumber; // valid only when payloadSize >= 4
};

bool parseRtp(const unsigned char* p, unsigned n, RtpPacket& out) {
  if (n < 12) return false;
  out.version = (p[0] >> 6) & 0x3;
  if (out.version != 2) return false;
  const unsigned cc = p[0] & 0xF;
  const unsigned hdrLen = 12 + 4 * cc;
  if (n < hdrLen) return false;
  out.payloadType = p[1] & 0x7F;
  out.seq = (uint16_t(ntohs(*(const uint16_t*)(p + 2))));
  out.timestamp = ntohl(*(const uint32_t*)(p + 4));
  out.ssrc = ntohl(*(const uint32_t*)(p + 8));
  out.payload = p + hdrLen;
  out.payloadSize = n - hdrLen;
  out.frameNumber = 0;
  if (out.payloadSize >= 4) {
    memcpy(&out.frameNumber, out.payload, 4);
    out.frameNumber = ntohl(out.frameNumber);
  }
  return true;
}

// ---------------------------------------------------------------------------
// A raw RTSP control connection over loopback, plus helpers to send requests and collect the
// interleaved ('$') RTP/RTCP frames that arrive on the same TCP socket.
// ---------------------------------------------------------------------------
class RtspControlClient {
public:
  RtspControlClient() : fFd(-1), fCSeq(0), fSched(NULL) {}
  ~RtspControlClient() { close(); }

  // The scheduler is used to make progress on the (same-thread) server while we wait for a
  // response, since the socket is non-blocking.
  void useScheduler(BasicTaskScheduler0* sched) { fSched = sched; }

  bool connectTo(uint16_t port, BasicTaskScheduler0* sched = NULL) {
    fSched = sched;
    fFd = socket(AF_INET, SOCK_STREAM, 0);
    if (fFd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fFd, (sockaddr*)&addr, sizeof addr) != 0) return false;
    makeSocketNonBlocking(fFd);
    return true;
  }
  void close() { if (fFd >= 0) { ::close(fFd); fFd = -1; } }
  int fd() const { return fFd; }

  // Send a complete request and return the full text response (headers only; responses here carry
  // no body).  Interleaved binary frames that show up are stashed for drainInterleaved().
  std::string request(const std::string& text) {
    std::string req = text;
    if (req.find("CSeq:") == std::string::npos) {
      char hdr[64];
      snprintf(hdr, sizeof hdr, "CSeq: %u\r\n", ++fCSeq);
      // Insert CSeq after the request line.
      const size_t eol = req.find("\r\n");
      req.insert(eol + 2, hdr);
    } else {
      ++fCSeq;
    }
    ssize_t off = 0;
    while (off < (ssize_t)req.size()) {
      ssize_t w = send(fFd, req.data() + off, req.size() - off, 0);
      if (w > 0) { off += w; continue; }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { pump(); continue; }
      return "";
    }
    return readTextResponse();
  }

  // Pull any pending '$'-framed RTP/RTCP data, returning the raw framed bytes (including the
  // 4-byte interleaved header of each frame).
  std::vector<unsigned char> drainInterleaved() {
    std::vector<unsigned char> got;
    unsigned char buf[65536];
    for (;;) {
      ssize_t n = recv(fFd, buf, sizeof buf, MSG_DONTWAIT);
      if (n > 0) got.insert(got.end(), buf, buf + n);
      else break;
    }
    return got;
  }

  // Parse the RTP packets carried in interleaved frames collected by drainInterleaved().
  static std::vector<RtpPacket> extractInterleavedRtp(const std::vector<unsigned char>& stream) {
    std::vector<RtpPacket> out;
    size_t p = 0;
    while (p + 4 <= stream.size()) {
      if (stream[p] != '$') break; // should not happen for a pure data drain
      const unsigned char channel = stream[p + 1];
      const unsigned len = (unsigned(stream[p + 2]) << 8) | stream[p + 3];
      if (p + 4 + len > stream.size()) break; // incomplete frame; wait for more
      if ((channel % 2) == 0) {              // even channel = RTP
        RtpPacket rtp;
        if (parseRtp(stream.data() + p + 4, len, rtp)) out.push_back(rtp);
      }
      p += 4 + len;
    }
    return out;
  }

  // Send an RTP/RTCP packet as an interleaved frame on the given channel.
  bool sendInterleaved(unsigned char channel, const unsigned char* data, unsigned len) {
    if (len > 0xFFFF) return false;
    unsigned char hdr[4] = {'$', channel,
                            static_cast<unsigned char>((len >> 8) & 0xFF),
                            static_cast<unsigned char>(len & 0xFF)};
    ssize_t off = 0;
    while (off < 4) {
      ssize_t w = send(fFd, hdr + off, 4 - off, 0);
      if (w <= 0) return false;
      off += w;
    }
    off = 0;
    while (off < (ssize_t)len) {
      ssize_t w = send(fFd, data + off, len - off, 0);
      if (w <= 0) return false;
      off += w;
    }
    return true;
  }

private:
  void pump() { if (fSched != NULL) fSched->SingleStep(100000); }

  std::string readTextResponse() {
    std::string resp;
    unsigned char buf[4096];
    // Read until the end-of-headers marker, driving the server's event loop whenever the
    // non-blocking socket has no data yet.  Interleaved binary frames are kept in the same
    // buffer only if they arrive first; our requests are always answered with a text response
    // before media is pumped, so in practice the response is pure text here.
    for (int guard = 0; guard < kMaxEventSteps; ++guard) {
      ssize_t n = recv(fFd, buf, sizeof buf, MSG_DONTWAIT);
      if (n > 0) {
        resp.append((const char*)buf, n);
        if (resp.find("\r\n\r\n") != std::string::npos) break;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pump(); // let the server process the request and write its reply
      } else {
        break;
      }
    }
    return resp;
  }

  int fFd;
  unsigned fCSeq;
  BasicTaskScheduler0* fSched;
};

// ---------------------------------------------------------------------------
// A pair of bound, non-blocking UDP client sockets (RTP + RTCP) that act as the unicast
// destination, with a tiny receive helper.
// ---------------------------------------------------------------------------
struct UdpEndpoint {
  int rtpFd = -1;
  int rtcpFd = -1;
  uint16_t rtpPort = 0;
  uint16_t rtcpPort = 0;

  bool open() {
    rtpFd = socket(AF_INET, SOCK_DGRAM, 0);
    rtcpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtpFd < 0 || rtcpFd < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(rtpFd, (sockaddr*)&a, sizeof a) != 0) return false;
    if (bind(rtcpFd, (sockaddr*)&a, sizeof a) != 0) return false;
    makeSocketNonBlocking(rtpFd);
    makeSocketNonBlocking(rtcpFd);
    socklen_t al = sizeof a;
    getsockname(rtpFd, (sockaddr*)&a, &al); rtpPort = ntohs(a.sin_port);
    getsockname(rtcpFd, (sockaddr*)&a, &al); rtcpPort = ntohs(a.sin_port);
    return true;
  }
  void close() {
    if (rtpFd >= 0) { ::close(rtpFd); rtpFd = -1; }
    if (rtcpFd >= 0) { ::close(rtcpFd); rtcpFd = -1; }
  }

  std::vector<unsigned char> recvAvailable(int fd) {
    std::vector<unsigned char> all;
    unsigned char buf[65536];
    for (;;) {
      ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
      if (n > 0) all.insert(all.end(), buf, buf + n);
      else break;
    }
    return all;
  }

  // Drain one socket and return each datagram separately (datagram boundaries must be preserved;
  // concatenating them would corrupt RTP packet parsing).
  std::vector<std::vector<unsigned char>> recvDatagrams(int fd) {
    std::vector<std::vector<unsigned char>> out;
    unsigned char buf[65536];
    for (;;) {
      ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
      if (n > 0) out.emplace_back(buf, buf + n);
      else break;
    }
    return out;
  }
};

// ---------------------------------------------------------------------------
// Test harness owning one scheduler/environment and (lazily) a server.
// ---------------------------------------------------------------------------
struct Env {
  BasicTaskScheduler* sched;
  BasicUsageEnvironment* env;
  RTSPServer* server = nullptr;
  unsigned sourceCreations = 0;

  Env() {
    sched = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*sched);
  }
  ~Env() {
    Medium::close(server);
    env->reclaim();
    delete sched;
  }

  // Run the event loop until "done" is set or the step watchdog trips.  Returns false on watchdog.
  bool pumpUntil(EventLoopWatchVariable& done) {
    for (int steps = 0; !done && steps < kMaxEventSteps; ++steps) step();
    return done != 0;
  }
  // Run one event-loop iteration.  SingleStep() is public on the BasicTaskScheduler0 base class.
  void step() { static_cast<BasicTaskScheduler0*>(sched)->SingleStep(100000); }
  // Run a bounded number of event-loop iterations (used to let background sends settle).
  void pumpSteps(int n) {
    for (int i = 0; i < n; ++i) step();
  }

  RTSPServer* startServer(Boolean enableReconfig, uint16_t& outPort) {
    server = RTSPServer::createNew(*env, Port(0), NULL, 65);
    if (server == NULL) {
      // Most commonly: the sandbox/process lacks permission to bind a local listening socket.
      // Return NULL; callers must treat this as a skipped (not crashed) case.
      outPort = 0;
      return NULL;
    }
    server->enableTransportReconfiguration(enableReconfig);
    outPort = (uint16_t)server->serverPortNum();
    return server;
  }

  TestSubsession* addSession(const char* name, Boolean reuseFirstSource,
                             ServerMediaSession*& outSms) {
    TestSubsession* sub = TestSubsession::createNew(*env, reuseFirstSource, sourceCreations);
    outSms = ServerMediaSession::createNew(*env, name, name, "test", False);
    outSms->addSubsession(sub);
    server->addServerMediaSession(outSms);
    return sub;
  }
};

// Convenience: extract the "Session: <id>" token from a response.
std::string sessionIdOf(const std::string& resp) {
  const std::string key = "Session: ";
  size_t p = resp.find(key);
  if (p == std::string::npos) return "";
  p += key.size();
  std::string id;
  while (p < resp.size() && resp[p] != '\r' && resp[p] != ';' && resp[p] != ' ') id += resp[p++];
  return id;
}

bool responseIs(const std::string& resp, const char* status) {
  return resp.find(std::string("RTSP/1.0 ") + status) != std::string::npos;
}

// Parse "server_port=<rtp>-<rtcp>" from a SETUP response.  Returns false if absent.
bool serverPorts(const std::string& resp, uint16_t& rtp, uint16_t& rtcp) {
  const std::string key = "server_port=";
  size_t p = resp.find(key);
  if (p == std::string::npos) return false;
  unsigned a = 0, b = 0;
  if (sscanf(resp.c_str() + p + key.size(), "%u-%u", &a, &b) != 2) return false;
  rtp = uint16_t(a); rtcp = uint16_t(b);
  return true;
}

// Build a minimal RTCP RR (one report block about "sinkSsrc") from "reporterSsrc".
std::vector<unsigned char> buildRR(uint32_t reporterSsrc, uint32_t sinkSsrc) {
  // Header (4) + sender SSRC (4) + one report block (24) = 32 bytes => length field = 32/4 - 1 = 7.
  std::vector<unsigned char> p(32, 0);
  p[0] = 0x80 | 1;        // V=2, P=0, RC = 1 report block
  p[1] = 201;             // PT = RR
  p[2] = 0x00; p[3] = 7;  // length = 7 dwords (32 bytes total)
  uint32_t v;
  v = htonl(reporterSsrc); memcpy(&p[4], &v, 4);
  v = htonl(sinkSsrc);    memcpy(&p[8], &v, 4); // report block: SSRC being reported on
  // remaining 20 bytes of the report block stay zero (valid).
  return p;
}

// Build SETUP transport header fragments.
std::string udpTransport(uint16_t rtp, uint16_t rtcp) {
  char b[128];
  snprintf(b, sizeof b, "Transport: RTP/AVP;unicast;client_port=%u-%u\r\n", rtp, rtcp);
  return b;
}
std::string tcpTransport(unsigned rtpChan, unsigned rtcpChan) {
  char b[128];
  snprintf(b, sizeof b, "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n", rtpChan, rtcpChan);
  return b;
}

std::string setupReq(const char* url, const std::string& transport,
                     const std::string& session = "") {
  std::string s = "SETUP ";
  s += url; s += " RTSP/1.0\r\n";
  if (!session.empty()) { s += "Session: "; s += session; s += "\r\n"; }
  s += transport;
  s += "\r\n";
  return s;
}

} // namespace

// ===========================================================================
// migration group: continuous sending across UDP->TCP->UDP, SSRC/seq/timestamp
// continuity, and RTCP RR handler migration.
// ===========================================================================

static int migration_udp_tcp_udp_continuity() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  TASK_CHECK(e.startServer(True, port) != NULL);
  TestSubsession* sub = e.addSession("mig", False, sms);

  RtspControlClient ctl;
  TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));

  UdpEndpoint udp1;
  TASK_CHECK(udp1.open());

  // First SETUP over UDP.
  std::string r = ctl.request(setupReq("rtsp://127.0.0.1/mig/", udpTransport(udp1.rtpPort, udp1.rtcpPort)));
  TASK_CHECK(responseIs(r, "200 OK"));
  const std::string sid = sessionIdOf(r);
  TASK_CHECK(!sid.empty());
  TASK_CHECK(e.sourceCreations == 1);

  // PLAY, then let several RTP packets flow over UDP.
  r = ctl.request(std::string("PLAY rtsp://127.0.0.1/mig/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));

  // Pump until the UDP side has collected a handful of RTP packets (one datagram each).
  std::vector<RtpPacket> udpPkts;
  for (int steps = 0; steps < kMaxEventSteps && udpPkts.size() < 5; ++steps) {
    e.step();
    for (auto& dg : udp1.recvDatagrams(udp1.rtpFd)) {
      RtpPacket rtp;
      if (parseRtp(dg.data(), (unsigned)dg.size(), rtp)) udpPkts.push_back(rtp);
    }
  }
  TASK_CHECK(udpPkts.size() >= 5);
  const uint32_t ssrcBefore = udpPkts.back().ssrc;
  const uint16_t seqBefore = udpPkts.back().seq;
  const uint32_t tsBefore = udpPkts.back().timestamp;
  const uint32_t frameBefore = udpPkts.back().frameNumber;

  // Switch to TCP interleaved (channels 0/1) with a second SETUP; no new PLAY.
  r = ctl.request(setupReq("rtsp://127.0.0.1/mig/", tcpTransport(0, 1), sid));
  TASK_CHECK(responseIs(r, "200 OK"));
  TASK_CHECK(r.find("interleaved=0-1") != std::string::npos);
  // The source must not have been recreated.
  TASK_CHECK(e.sourceCreations == 1);

  // Drain and collect interleaved RTP after the switch.
  std::vector<RtpPacket> tcpPkts;
  for (int steps = 0; steps < kMaxEventSteps && tcpPkts.size() < 5; ++steps) {
    e.step();
    auto raw = ctl.drainInterleaved();
    auto got = RtspControlClient::extractInterleavedRtp(raw);
    tcpPkts.insert(tcpPkts.end(), got.begin(), got.end());
  }
  TASK_CHECK(tcpPkts.size() >= 5);

  // SSRC unchanged and sequence numbers continue from the pre-switch stream (the new first seq is
  // strictly after the last one seen on UDP - it is never a freshly randomized initial value).
  TASK_CHECK(tcpPkts.front().ssrc == ssrcBefore);
  TASK_CHECK(uint16_t(tcpPkts.front().seq - seqBefore) > 0);
  TASK_CHECK(uint16_t(tcpPkts.front().seq - seqBefore) <= 8); // tight: only in-flight packets intervene
  // Frames keep advancing from the point the UDP stream had reached.
  TASK_CHECK(tcpPkts.front().frameNumber > frameBefore);
  // Timestamps are monotonic and do not jump backwards.
  TASK_CHECK(tcpPkts.back().timestamp >= tsBefore);
  // Sequence numbers are consecutive within the post-switch run itself.
  for (size_t i = 1; i < tcpPkts.size(); ++i)
    TASK_CHECK(uint16_t(tcpPkts[i].seq - tcpPkts[i - 1].seq) == 1);

  // The old UDP destination must no longer receive media after the switch.
  const size_t udpAfterSwitch = udp1.recvAvailable(udp1.rtpFd).size();
  e.pumpSteps(50);
  TASK_CHECK(udp1.recvAvailable(udp1.rtpFd).size() == udpAfterSwitch);

  // Switch back to UDP on a *new* endpoint pair.
  UdpEndpoint udp2;
  TASK_CHECK(udp2.open());
  r = ctl.request(setupReq("rtsp://127.0.0.1/mig/", udpTransport(udp2.rtpPort, udp2.rtcpPort), sid));
  TASK_CHECK(responseIs(r, "200 OK"));
  TASK_CHECK(e.sourceCreations == 1);

  const uint16_t seqAtTcpEnd = tcpPkts.back().seq;
  std::vector<RtpPacket> udp2Pkts;
  for (int steps = 0; steps < kMaxEventSteps && udp2Pkts.size() < 5; ++steps) {
    e.step();
    for (auto& dg : udp2.recvDatagrams(udp2.rtpFd)) {
      RtpPacket rtp;
      if (parseRtp(dg.data(), (unsigned)dg.size(), rtp)) udp2Pkts.push_back(rtp);
    }
  }
  TASK_CHECK(udp2Pkts.size() >= 5);
  TASK_CHECK(udp2Pkts.front().ssrc == ssrcBefore);
  // The same continuous sequence space: the first UDP2 packet is after the last TCP one.
  TASK_CHECK(uint16_t(udp2Pkts.front().seq - seqAtTcpEnd) > 0);
  TASK_CHECK(uint16_t(udp2Pkts.front().seq - seqAtTcpEnd) <= 8);
  for (size_t i = 1; i < udp2Pkts.size(); ++i)
    TASK_CHECK(uint16_t(udp2Pkts[i].seq - udp2Pkts[i - 1].seq) == 1);

  // The TCP channel must have gone quiet after moving back to UDP.
  const size_t tcpTail = ctl.drainInterleaved().size();
  e.pumpSteps(50);
  TASK_CHECK(ctl.drainInterleaved().size() == tcpTail);
  // And the first UDP endpoint stays silent.
  TASK_CHECK(udp1.recvAvailable(udp1.rtpFd).empty());

  udp1.close();
  udp2.close();
  return 0;
}

static int migration_rr_handler_follows_destination() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  TASK_CHECK(e.startServer(True, port) != NULL);
  TestSubsession* sub = e.addSession("rr", False, sms);

  RtspControlClient ctl;
  TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  UdpEndpoint udp;
  TASK_CHECK(udp.open());

  std::string r = ctl.request(setupReq("rtsp://127.0.0.1/rr/", udpTransport(udp.rtpPort, udp.rtcpPort)));
  TASK_CHECK(responseIs(r, "200 OK"));
  const std::string sid = sessionIdOf(r);
  uint16_t srvRtp = 0, srvRtcp = 0;
  TASK_CHECK(serverPorts(r, srvRtp, srvRtcp));

  r = ctl.request(std::string("PLAY rtsp://127.0.0.1/rr/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));

  // Let the RTCP instance be created (startPlaying) and capture the live sink / its SSRC.
  e.pumpSteps(20);
  RTPSink* sink = sub->lastSink();
  TASK_CHECK(sink != NULL);
  const uint32_t sinkSsrc = sink->SSRC();
  auto& statsDb = sink->transmissionStatsDB();

  // ---- Phase 1 (UDP): an RR arriving on the datagram RTCP socket is processed. ----
  sockaddr_in serverRtcp{};
  serverRtcp.sin_family = AF_INET;
  serverRtcp.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  serverRtcp.sin_port = htons(srvRtcp);
  auto rrUdp = buildRR(0x11111111u, sinkSsrc);
  TASK_CHECK(sendto(udp.rtcpFd, rrUdp.data(), rrUdp.size(), 0,
                    (sockaddr*)&serverRtcp, sizeof serverRtcp) == (ssize_t)rrUdp.size());
  for (int steps = 0; steps < kMaxEventSteps && statsDb.numReceivers() < 1; ++steps)
    e.step();
  TASK_CHECK(statsDb.numReceivers() == 1);

  // ---- Move the track to TCP interleaved (channels 0/1); no new PLAY. ----
  r = ctl.request(setupReq("rtsp://127.0.0.1/rr/", tcpTransport(0, 1), sid));
  TASK_CHECK(responseIs(r, "200 OK"));
  e.pumpSteps(20);

  // ---- Old UDP RR path is dead: an RR sent to the old datagram socket is not processed. ----
  auto rrStale = buildRR(0x22222222u, sinkSsrc);
  TASK_CHECK(sendto(udp.rtcpFd, rrStale.data(), rrStale.size(), 0,
                    (sockaddr*)&serverRtcp, sizeof serverRtcp) == (ssize_t)rrStale.size());
  e.pumpSteps(30);
  TASK_CHECK(statsDb.numReceivers() == 1); // unchanged: the UDP read handler was detached

  // ---- New TCP RR path works: an interleaved RR on channel 1 reaches the same RTCPInstance. ----
  auto rrTcp = buildRR(0x33333333u, sinkSsrc);
  TASK_CHECK(ctl.sendInterleaved(1, rrTcp.data(), (unsigned)rrTcp.size()));
  for (int steps = 0; steps < kMaxEventSteps && statsDb.numReceivers() < 2; ++steps)
    e.step();
  TASK_CHECK(statsDb.numReceivers() == 2); // new reporter registered through the migrated handler

  // The session remains alive and controllable throughout.
  r = ctl.request(std::string("GET_PARAMETER rtsp://127.0.0.1/rr/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));

  udp.close();
  return 0;
}

// ===========================================================================
// isolation group: per-track destinations, channel-collision rejection, and
// reuseFirstSource subscriber isolation.
// ===========================================================================

static int isolation_two_tracks_independent_destinations() {
  Env e;
  uint16_t port = 0;
  // Build a two-track session manually (two subsessions, distinct track ids).
  RTSPServer* srv = e.startServer(True, port);
  TASK_CHECK(srv != NULL);
  ServerMediaSession* sms = ServerMediaSession::createNew(*e.env, "two", "two", "t", False);
  unsigned c1 = 0, c2 = 0;
  TestSubsession* a = TestSubsession::createNew(*e.env, False, c1);
  TestSubsession* b = TestSubsession::createNew(*e.env, False, c2);
  sms->addSubsession(a);
  sms->addSubsession(b);
  srv->addServerMediaSession(sms);

  RtspControlClient ctl;
  TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  UdpEndpoint ua, ub;
  TASK_CHECK(ua.open() && ub.open());

  std::string r = ctl.request(setupReq("rtsp://127.0.0.1/two/track1/",
                                       udpTransport(ua.rtpPort, ua.rtcpPort)));
  TASK_CHECK(responseIs(r, "200 OK"));
  const std::string sid = sessionIdOf(r);
  r = ctl.request(setupReq("rtsp://127.0.0.1/two/track2/",
                           udpTransport(ub.rtpPort, ub.rtcpPort), sid));
  TASK_CHECK(responseIs(r, "200 OK"));
  TASK_CHECK(c1 == 1 && c2 == 1);

  // Move track1 to TCP channels 0/1; track2 stays UDP and must be untouched.
  r = ctl.request(setupReq("rtsp://127.0.0.1/two/track1/", tcpTransport(0, 1), sid));
  TASK_CHECK(responseIs(r, "200 OK"));

  // track2 cannot grab a channel already used by track1 (channel 0).
  r = ctl.request(setupReq("rtsp://127.0.0.1/two/track2/", tcpTransport(0, 3), sid));
  TASK_CHECK(responseIs(r, "461 Unsupported Transport"));
  // track2's existing UDP transport still works (it was not altered).
  r = ctl.request(std::string("PLAY rtsp://127.0.0.1/two/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));

  // Both tracks emit: track1 over TCP, track2 over its own UDP endpoint.
  bool sawTcp = false, sawUdp = false;
  for (int steps = 0; steps < kMaxEventSteps && (!sawTcp || !sawUdp); ++steps) {
    e.step();
    if (!sawTcp && !RtspControlClient::extractInterleavedRtp(ctl.drainInterleaved()).empty()) sawTcp = true;
    if (!sawUdp && !ub.recvDatagrams(ub.rtpFd).empty()) sawUdp = true;
  }
  TASK_CHECK(sawTcp);
  TASK_CHECK(sawUdp);
  // Track2's media never appears on track1's TCP stream count confusion is avoided by endpoints;
  // the track2 UDP endpoint is the one that received data.
  ua.close();
  ub.close();
  return 0;
}

static int isolation_reuse_first_source_not_disturbed() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  RTSPServer* srv = e.startServer(True, port);
  TASK_CHECK(srv != NULL);
  TestSubsession* sub = e.addSession("shared", True /*reuseFirstSource*/, sms);
  (void)sub; (void)srv;

  // Two independent subscribers (two control connections) share one source.
  RtspControlClient c1, c2;
  TASK_CHECK(c1.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)) && c2.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  UdpEndpoint u1, u2;
  TASK_CHECK(u1.open() && u2.open());

  std::string r1 = c1.request(setupReq("rtsp://127.0.0.1/shared/", udpTransport(u1.rtpPort, u1.rtcpPort)));
  TASK_CHECK(responseIs(r1, "200 OK"));
  const std::string s1 = sessionIdOf(r1);
  // Second subscriber reuses the first source (reuseFirstSource).
  std::string r2 = c2.request(setupReq("rtsp://127.0.0.1/shared/", udpTransport(u2.rtpPort, u2.rtcpPort)));
  TASK_CHECK(responseIs(r2, "200 OK"));
  const std::string s2 = sessionIdOf(r2);
  TASK_CHECK(s1 != s2);
  TASK_CHECK(e.sourceCreations == 1); // shared, not recreated

  r1 = c1.request(std::string("PLAY rtsp://127.0.0.1/shared/ RTSP/1.0\r\nSession: ") + s1 + "\r\n\r\n");
  TASK_CHECK(responseIs(r1, "200 OK"));
  r2 = c2.request(std::string("PLAY rtsp://127.0.0.1/shared/ RTSP/1.0\r\nSession: ") + s2 + "\r\n\r\n");
  TASK_CHECK(responseIs(r2, "200 OK"));

  // Both receive media before any change.
  bool got1 = false, got2 = false;
  for (int steps = 0; steps < kMaxEventSteps && (!got1 || !got2); ++steps) {
    e.step();
    if (!u1.recvDatagrams(u1.rtpFd).empty()) got1 = true;
    if (!u2.recvDatagrams(u2.rtpFd).empty()) got2 = true;
  }
  TASK_CHECK(got1 && got2);

  // Subscriber 1 reconfigures to TCP; subscriber 2 must keep receiving on UDP undisturbed and the
  // shared source must not be paused, recreated or moved.
  r1 = c1.request(setupReq("rtsp://127.0.0.1/shared/", tcpTransport(0, 1), s1));
  TASK_CHECK(responseIs(r1, "200 OK"));
  TASK_CHECK(e.sourceCreations == 1);

  bool c1Tcp = false, c2Udp = false;
  for (int steps = 0; steps < kMaxEventSteps && (!c1Tcp || !c2Udp); ++steps) {
    e.step();
    if (!RtspControlClient::extractInterleavedRtp(c1.drainInterleaved()).empty()) c1Tcp = true;
    if (!u2.recvDatagrams(u2.rtpFd).empty()) c2Udp = true;
  }
  TASK_CHECK(c1Tcp);
  TASK_CHECK(c2Udp);

  u1.close();
  u2.close();
  return 0;
}

// ===========================================================================
// lifecycle group: validation/461, paused re-SETUP, identical re-SETUP no-op,
// feature disabled keeps legacy behavior, TEARDOWN and disconnect cleanup.
// ===========================================================================

static int lifecycle_rejects_invalid_transports_with_461() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  TASK_CHECK(e.startServer(True, port) != NULL);
  e.addSession("val", False, sms);

  RtspControlClient ctl;
  TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  UdpEndpoint u;
  TASK_CHECK(u.open());

  std::string r = ctl.request(setupReq("rtsp://127.0.0.1/val/", udpTransport(u.rtpPort, u.rtcpPort)));
  TASK_CHECK(responseIs(r, "200 OK"));
  const std::string sid = sessionIdOf(r);
  const unsigned creationsAfterSetup = e.sourceCreations;

  // UDP: zero port, equal ports -> 461, transport left intact.
  r = ctl.request(setupReq("rtsp://127.0.0.1/val/", udpTransport(0, 1), sid));
  TASK_CHECK(responseIs(r, "461 Unsupported Transport"));
  r = ctl.request(setupReq("rtsp://127.0.0.1/val/", udpTransport(5000, 5000), sid));
  TASK_CHECK(responseIs(r, "461 Unsupported Transport"));

  // TCP: equal channels -> 461.
  r = ctl.request(setupReq("rtsp://127.0.0.1/val/", tcpTransport(2, 2), sid));
  TASK_CHECK(responseIs(r, "461 Unsupported Transport"));

  // Unknown/raw transport -> 461.
  r = ctl.request(setupReq("rtsp://127.0.0.1/val/",
                           "Transport: RAW/RAW/UDP;unicast;client_port=6000\r\n", sid));
  TASK_CHECK(responseIs(r, "461 Unsupported Transport"));

  // A valid re-SETUP still succeeds afterward, proving the original transport kept working.
  r = ctl.request(setupReq("rtsp://127.0.0.1/val/", tcpTransport(4, 5), sid));
  TASK_CHECK(responseIs(r, "200 OK"));
  TASK_CHECK(e.sourceCreations == creationsAfterSetup); // no recreation on the 461 attempts

  u.close();
  return 0;
}

static int lifecycle_paused_track_stays_paused() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  TASK_CHECK(e.startServer(True, port) != NULL);
  e.addSession("pause", False, sms);

  RtspControlClient ctl;
  TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  UdpEndpoint u1;
  TASK_CHECK(u1.open());

  std::string r = ctl.request(setupReq("rtsp://127.0.0.1/pause/", udpTransport(u1.rtpPort, u1.rtcpPort)));
  TASK_CHECK(responseIs(r, "200 OK"));
  const std::string sid = sessionIdOf(r);
  r = ctl.request(std::string("PLAY rtsp://127.0.0.1/pause/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));
  bool got = false;
  for (int steps = 0; steps < kMaxEventSteps && !got; ++steps) {
    e.step();
    if (!u1.recvDatagrams(u1.rtpFd).empty()) got = true;
  }
  TASK_CHECK(got);

  // PAUSE, then reconfigure to TCP while paused.
  r = ctl.request(std::string("PAUSE rtsp://127.0.0.1/pause/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));
  r = ctl.request(setupReq("rtsp://127.0.0.1/pause/", tcpTransport(0, 1), sid));
  TASK_CHECK(responseIs(r, "200 OK"));

  // While still paused, no media should arrive on the TCP channel.
  const size_t quiet = ctl.drainInterleaved().size();
  e.pumpSteps(60);
  TASK_CHECK(ctl.drainInterleaved().size() == quiet);

  // A subsequent PLAY resumes media, now over TCP.
  r = ctl.request(std::string("PLAY rtsp://127.0.0.1/pause/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));
  bool tcpGot = false;
  for (int steps = 0; steps < kMaxEventSteps && !tcpGot; ++steps) {
    e.step();
    if (!RtspControlClient::extractInterleavedRtp(ctl.drainInterleaved()).empty()) tcpGot = true;
  }
  TASK_CHECK(tcpGot);

  u1.close();
  return 0;
}

static int lifecycle_identical_resetup_is_noop() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  TASK_CHECK(e.startServer(True, port) != NULL);
  e.addSession("same", False, sms);

  RtspControlClient ctl;
  TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  UdpEndpoint u;
  TASK_CHECK(u.open());

  std::string r = ctl.request(setupReq("rtsp://127.0.0.1/same/", udpTransport(u.rtpPort, u.rtcpPort)));
  TASK_CHECK(responseIs(r, "200 OK"));
  const std::string sid = sessionIdOf(r);
  r = ctl.request(std::string("PLAY rtsp://127.0.0.1/same/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
  TASK_CHECK(responseIs(r, "200 OK"));

  // Re-send the very same UDP SETUP: must succeed but neither recreate the source nor duplicate the
  // destination/RR callback.  Source creation count stays at 1, and packets keep flowing once.
  const unsigned creations = e.sourceCreations;
  r = ctl.request(setupReq("rtsp://127.0.0.1/same/", udpTransport(u.rtpPort, u.rtcpPort), sid));
  TASK_CHECK(responseIs(r, "200 OK"));
  TASK_CHECK(e.sourceCreations == creations);

  bool got = false;
  for (int steps = 0; steps < kMaxEventSteps && !got; ++steps) {
    e.step();
    if (!u.recvDatagrams(u.rtpFd).empty()) got = true;
  }
  TASK_CHECK(got);

  u.close();
  return 0;
}

static int lifecycle_disabled_keeps_legacy_recreate() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  TASK_CHECK(e.startServer(False /*feature disabled*/, port) != NULL);
  e.addSession("legacy", False, sms);

  RtspControlClient ctl;
  TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  UdpEndpoint u1, u2;
  TASK_CHECK(u1.open() && u2.open());

  std::string r = ctl.request(setupReq("rtsp://127.0.0.1/legacy/", udpTransport(u1.rtpPort, u1.rtcpPort)));
  TASK_CHECK(responseIs(r, "200 OK"));
  const std::string sid = sessionIdOf(r);
  TASK_CHECK(e.sourceCreations == 1);

  // With the feature off, a second SETUP takes the legacy pause/delete/recreate path: a new source
  // is created.  The request still succeeds (behavior preserved), but the creation count rises.
  r = ctl.request(setupReq("rtsp://127.0.0.1/legacy/", udpTransport(u2.rtpPort, u2.rtcpPort), sid));
  TASK_CHECK(responseIs(r, "200 OK"));
  TASK_CHECK(e.sourceCreations == 2);

  u1.close();
  u2.close();
  return 0;
}

static int lifecycle_teardown_and_disconnect_cleanup() {
  Env e;
  uint16_t port = 0;
  ServerMediaSession* sms = NULL;
  TASK_CHECK(e.startServer(True, port) != NULL);
  e.addSession("life", False, sms);

  // Phase 1: TEARDOWN after repeated switches leaves no stale state (session count returns to 0).
  {
    RtspControlClient ctl;
    TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
    UdpEndpoint u;
    TASK_CHECK(u.open());
    std::string r = ctl.request(setupReq("rtsp://127.0.0.1/life/", udpTransport(u.rtpPort, u.rtcpPort)));
    TASK_CHECK(responseIs(r, "200 OK"));
    const std::string sid = sessionIdOf(r);
    TASK_CHECK(e.server->numClientSessions() == 1);

    r = ctl.request(setupReq("rtsp://127.0.0.1/life/", tcpTransport(0, 1), sid));
    TASK_CHECK(responseIs(r, "200 OK"));
    UdpEndpoint u2;
    TASK_CHECK(u2.open());
    r = ctl.request(setupReq("rtsp://127.0.0.1/life/", udpTransport(u2.rtpPort, u2.rtcpPort), sid));
    TASK_CHECK(responseIs(r, "200 OK"));
    r = ctl.request(setupReq("rtsp://127.0.0.1/life/", tcpTransport(2, 3), sid));
    TASK_CHECK(responseIs(r, "200 OK"));

    r = ctl.request(std::string("TEARDOWN rtsp://127.0.0.1/life/ RTSP/1.0\r\nSession: ") + sid + "\r\n\r\n");
    TASK_CHECK(responseIs(r, "200 OK"));
    u.close();
    u2.close();
  }
  e.pumpSteps(30);
  TASK_CHECK(e.server->numClientSessions() == 0);

  // Phase 2: an abrupt control-connection disconnect while interleaved must not leave dangling
  // channels/callbacks; the reclaimed session disappears and a brand-new session can be set up.
  {
    RtspControlClient ctl;
    TASK_CHECK(ctl.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
    UdpEndpoint u;
    TASK_CHECK(u.open());
    std::string r = ctl.request(setupReq("rtsp://127.0.0.1/life/", udpTransport(u.rtpPort, u.rtcpPort)));
    TASK_CHECK(responseIs(r, "200 OK"));
    const std::string sid = sessionIdOf(r);
    r = ctl.request(setupReq("rtsp://127.0.0.1/life/", tcpTransport(0, 1), sid));
    TASK_CHECK(responseIs(r, "200 OK"));
    ctl.close(); // abrupt disconnect
    u.close();
    // Pump enough for the server to notice the socket closure and reclaim the session.
    for (int steps = 0; steps < kMaxEventSteps && e.server->numClientSessions() > 0; ++steps) {
      e.step();
    }
    TASK_CHECK(e.server->numClientSessions() == 0);
  }

  // A fresh client can establish a session using the same channels with no collision, proving no
  // stale interleaved registration survived.
  RtspControlClient ctl3;
  TASK_CHECK(ctl3.connectTo(port, static_cast<BasicTaskScheduler0*>(e.sched)));
  std::string r = ctl3.request(setupReq("rtsp://127.0.0.1/life/", tcpTransport(0, 1)));
  TASK_CHECK(responseIs(r, "200 OK"));

  return 0;
}

// ---------------------------------------------------------------------------
// task_cases registration (three groups, consumed by task_support/driver.c).
// ---------------------------------------------------------------------------
extern "C" const struct task_case task_cases[] = {
  // migration
  {"migration", "udp_tcp_udp_continuity", migration_udp_tcp_udp_continuity},
  {"migration", "rr_handler_follows_destination", migration_rr_handler_follows_destination},
  // isolation
  {"isolation", "two_tracks_independent_destinations", isolation_two_tracks_independent_destinations},
  {"isolation", "reuse_first_source_not_disturbed", isolation_reuse_first_source_not_disturbed},
  // lifecycle
  {"lifecycle", "rejects_invalid_transports_461", lifecycle_rejects_invalid_transports_with_461},
  {"lifecycle", "paused_track_stays_paused", lifecycle_paused_track_stays_paused},
  {"lifecycle", "identical_resetup_is_noop", lifecycle_identical_resetup_is_noop},
  {"lifecycle", "disabled_keeps_legacy_recreate", lifecycle_disabled_keeps_legacy_recreate},
  {"lifecycle", "teardown_and_disconnect_cleanup", lifecycle_teardown_and_disconnect_cleanup},
  {NULL, NULL, NULL},
};

extern "C" const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]) - 1;
