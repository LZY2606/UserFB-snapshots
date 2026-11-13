// test/aac-au-timing.cpp — tests for MPEG4-GENERIC (AAC) RTP reception:
// AU-header parsing, per-AU presentation times, and AU fragmentation.
//
// Everything is exercised through the real receive chain: a MediaSession
// created from an SDP description, an MPEG4GenericRTPSource, and
// MultiFramedRTPSource, fed with hand-built RTP packets over a local UDP
// socket, with frames collected via the normal FramedSource callback.
// AU headers and AAC payload bytes are generated with an independent bit
// writer (below); the production parser is never used to build expectations.
// Expected presentation-time differences are computed independently in
// integer RTP ticks.  Each scenario ends via the event loop (a watch
// variable) with a bounded timeout task — never via sleep-based guesses
// about how many frames arrived.
//
// The three entry points each return the number of assertions that passed;
// any failed assertion exits non-zero immediately.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

////////// assertion counting //////////

static int gChecks = 0;

static void check(bool cond, char const* what) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
  }
  ++gChecks;
}

////////// independent bit writer (for building AU headers) //////////

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

////////// AU packet construction (independent of the production parser) //////////

struct AuCfg {
  unsigned sizeLen, idxLen, idxDeltaLen, ctsLen;
};

struct AuSpec {
  unsigned size;  // AU-size: the COMPLETE AU's size, even for a fragment
  unsigned index; // AU-index (first AU in a packet) or index-delta (later AUs)
  bool ctsOn;     // CTS-flag (meaningful only when cfg.ctsLen > 0)
  int cts;        // signed CTS-delta, in RTP ticks
  std::vector<unsigned char> data; // this AU's bytes *in this packet*
  AuSpec(unsigned sz, unsigned idx, std::vector<unsigned char> d)
    : size(sz), index(idx), ctsOn(false), cts(0), data(d) {}
  AuSpec& withCts(int c) { ctsOn = true; cts = c; return *this; }
};

static unsigned auHeaderBits(AuCfg const& cfg, std::vector<AuSpec> const& aus) {
  unsigned bits = 0;
  for (size_t i = 0; i < aus.size(); ++i) {
    bits += cfg.sizeLen + (i == 0 ? cfg.idxLen : cfg.idxDeltaLen);
    if (cfg.ctsLen > 0) {
      bits += 1; // CTS-flag
      if (aus[i].ctsOn) bits += cfg.ctsLen;
    }
  }
  return bits;
}

// Builds an RTP payload: 2-byte AU-headers-length (in BITS), the AU-header
// section, then the concatenated AU data bytes.
static std::vector<unsigned char> buildAuPayload(AuCfg const& cfg, std::vector<AuSpec> const& aus) {
  std::vector<unsigned char> out;
  unsigned bits = auHeaderBits(cfg, aus);
  out.push_back((unsigned char)(bits >> 8));
  out.push_back((unsigned char)(bits & 0xFF));
  BitWriter bw;
  for (size_t i = 0; i < aus.size(); ++i) {
    AuSpec const& a = aus[i];
    bw.put(a.size, (int)cfg.sizeLen);
    bw.put(a.index, (int)(i == 0 ? cfg.idxLen : cfg.idxDeltaLen));
    if (cfg.ctsLen > 0) {
      bw.put(a.ctsOn ? 1 : 0, 1);
      if (a.ctsOn) {
	unsigned mask = cfg.ctsLen >= 32 ? 0xFFFFFFFFu : ((1u << cfg.ctsLen) - 1);
	bw.put((unsigned)a.cts & mask, (int)cfg.ctsLen);
      }
    }
  }
  std::vector<unsigned char> hdr = bw.finish();
  out.insert(out.end(), hdr.begin(), hdr.end());
  for (size_t i = 0; i < aus.size(); ++i)
    out.insert(out.end(), aus[i].data.begin(), aus[i].data.end());
  return out;
}

////////// expected presentation-time differences, in integer RTP ticks //////////

static long long ticksToUs(long long ticks, unsigned freq) {
  long long num = ticks * 1000000LL;
  long long den = (long long)freq;
  return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

////////// the receiving harness //////////

struct Frame {
  std::vector<unsigned char> bytes;
  struct timeval pt;
  unsigned truncated;
};

static long long ptDiffUs(Frame const& later, Frame const& earlier) {
  return (long long)(later.pt.tv_sec - earlier.pt.tv_sec) * 1000000LL
       + (long long)(later.pt.tv_usec - earlier.pt.tv_usec);
}

static void checkDiff(Frame const& later, Frame const& earlier, long long wantUs, char const* what) {
  long long got = ptDiffUs(later, earlier);
  if (got != wantUs) {
    fprintf(stderr, "FAIL: %s (got %lld us, want %lld us)\n", what, got, wantUs);
    exit(1);
  }
  ++gChecks;
}

struct OutPacket {
  unsigned short seq;
  unsigned rtpTs;
  bool marker;
  std::vector<unsigned char> payload;
  unsigned char payloadType = 96;
};

struct Receiver {
  TaskScheduler* sched = nullptr;
  UsageEnvironment* env = nullptr;
  MediaSession* session = nullptr;
  MediaSubsession* sub = nullptr;
  FramedSource* src = nullptr;
  unsigned char buf[256];
  unsigned bufSize = 64;
  std::vector<Frame> frames;
  unsigned expect = 0;
  EventLoopWatchVariable done;
  bool timedOut = false;
  int sendFd = -1;
  struct sockaddr_in dst;
};

static void afterFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
		       struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  Receiver& r = *(Receiver*)clientData;
  Frame f;
  f.bytes.assign(r.buf, r.buf + frameSize);
  f.pt = presentationTime;
  f.truncated = numTruncatedBytes;
  r.frames.push_back(f);
  if (r.frames.size() >= r.expect) {
    r.done = 1;
  } else {
    r.src->getNextFrame(r.buf, r.bufSize, afterFrame, &r, NULL, NULL);
  }
}

static void onTimeout(void* clientData) {
  Receiver& r = *(Receiver*)clientData;
  r.timedOut = true;
  r.done = 1;
}

static void sendPacket(Receiver& r, OutPacket const& p) {
  std::vector<unsigned char> pkt(12);
  pkt[0] = 0x80; // RTP version 2
  pkt[1] = (unsigned char)((p.marker ? 0x80 : 0x00) | (p.payloadType & 0x7F));
  pkt[2] = (unsigned char)(p.seq >> 8);
  pkt[3] = (unsigned char)(p.seq & 0xFF);
  pkt[4] = (unsigned char)(p.rtpTs >> 24);
  pkt[5] = (unsigned char)(p.rtpTs >> 16);
  pkt[6] = (unsigned char)(p.rtpTs >> 8);
  pkt[7] = (unsigned char)(p.rtpTs);
  pkt[8] = 0; pkt[9] = 0; pkt[10] = 0; pkt[11] = 0x7B; // SSRC
  pkt.insert(pkt.end(), p.payload.begin(), p.payload.end());
  ssize_t sent = sendto(r.sendFd, (char const*)pkt.data(), pkt.size(), 0,
			(struct sockaddr*)&r.dst, (socklen_t)sizeof r.dst);
  check(sent == (ssize_t)pkt.size(), "RTP packet sent to the local socket");
}

struct DelayedBatch {
  Receiver* r;
  std::vector<OutPacket> pkts;
};

static void sendDelayedBatch(void* clientData) {
  DelayedBatch* b = (DelayedBatch*)clientData;
  for (size_t i = 0; i < b->pkts.size(); ++i) sendPacket(*b->r, b->pkts[i]);
  // (the batch is deleted by runScenario() after the event loop finishes)
}

struct Scenario {
  std::string sdp;
  std::vector<OutPacket> immediate;
  std::vector<OutPacket> delayed; // sent "delayedUs" after the immediate ones
  long delayedUs = 250000;
  unsigned expectFrames = 0;
  unsigned bufSize = 64;
  long timeoutUs = 5000000;
};

static std::vector<Frame> runScenario(Scenario const& sc, bool& timedOut) {
  Receiver r;
  r.sched = BasicTaskScheduler::createNew();
  r.env = BasicUsageEnvironment::createNew(*r.sched);
  r.session = MediaSession::createNew(*r.env, sc.sdp.c_str());
  check(r.session != NULL, "MediaSession created from SDP");
  MediaSubsessionIterator iter(*r.session);
  r.sub = iter.next();
  check(r.sub != NULL, "subsession present");
  check(r.sub->initiate(), "subsession initiated");
  r.src = r.sub->readSource();
  check(r.src != NULL, "read source created");

  r.sendFd = socket(AF_INET, SOCK_DGRAM, 0);
  check(r.sendFd >= 0, "send socket created");
  memset(&r.dst, 0, sizeof r.dst);
  r.dst.sin_family = AF_INET;
  r.dst.sin_port = htons(r.sub->clientPortNum());
  inet_pton(AF_INET, "127.0.0.1", &r.dst.sin_addr);

  r.done = 0;
  r.expect = sc.expectFrames;
  r.bufSize = sc.bufSize;
  r.src->getNextFrame(r.buf, r.bufSize, afterFrame, &r, NULL, NULL);
  TaskToken timeoutTask = r.sched->scheduleDelayedTask(sc.timeoutUs, onTimeout, &r);
  TaskToken sendTask = NULL;
  DelayedBatch* batch = NULL;
  if (!sc.delayed.empty()) {
    batch = new DelayedBatch{&r, sc.delayed};
    sendTask = r.sched->scheduleDelayedTask(sc.delayedUs, sendDelayedBatch, batch);
  }
  for (size_t i = 0; i < sc.immediate.size(); ++i) sendPacket(r, sc.immediate[i]);

  r.sched->doEventLoop(&r.done);

  r.sched->unscheduleDelayedTask(timeoutTask);
  if (sendTask != NULL) { r.sched->unscheduleDelayedTask(sendTask); delete batch; }
  close(r.sendFd);
  Medium::close(r.session);
  r.env->reclaim();
  delete r.sched;
  timedOut = r.timedOut;
  return r.frames;
}

// For SDP parameter rejection tests: does initiating the subsession succeed?
static bool initiateSucceeds(std::string const& sdp) {
  TaskScheduler* sched = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*sched);
  bool ok = false;
  MediaSession* session = MediaSession::createNew(*env, sdp.c_str());
  if (session != NULL) {
    MediaSubsessionIterator iter(*session);
    MediaSubsession* sub = iter.next();
    if (sub != NULL && sub->initiate()) ok = true;
    Medium::close(session);
  }
  env->reclaim();
  delete sched;
  return ok;
}

static std::string aacSdp(unsigned freq, char const* fmtp) {
  char buf[600];
  snprintf(buf, sizeof buf,
	   "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=aac\r\n"
	   "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
	   "m=audio 0 RTP/AVP 96\r\n"
	   "a=rtpmap:96 MPEG4-GENERIC/%u/2\r\n"
	   "a=fmtp:96 %s\r\n", freq, fmtp);
  return std::string(buf);
}

static OutPacket auPacket(unsigned short seq, unsigned ts, bool marker,
			  AuCfg const& cfg, std::vector<AuSpec> const& aus) {
  OutPacket p;
  p.seq = seq;
  p.rtpTs = ts;
  p.marker = marker;
  p.payload = buildAuPayload(cfg, aus);
  return p;
}

static void checkFrameBytes(Frame const& f, std::vector<unsigned char> const& want,
			    char const* what) {
  if (f.bytes != want) {
    fprintf(stderr, "FAIL: %s (got %zu bytes, want %zu)\n", what, f.bytes.size(), want.size());
    exit(1);
  }
  ++gChecks;
}

////////// group 1: AU-header parsing and validation //////////

int test_aac_au_headers() {
  int const startChecks = gChecks;

  // Case 1+2: non-byte-aligned AU headers (with optional CTS fields), then
  // malformed packets that must be dropped without delivering anything,
  // then a good packet that must still be received.
  {
    AuCfg cfg{13, 3, 3, 5};
    Scenario sc;
    sc.sdp = aacSdp(48000, "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
			   "indexdeltalength=3;ctsdeltalength=5");
    sc.expectFrames = 4;

    // A good packet: 3 AUs, 61 bits of headers (not byte-aligned; the final
    // byte carries 3 unused padding bits that are not a new AU-header):
    check(auHeaderBits(cfg, {AuSpec(2, 0, {0xA0, 0xA1}),
			     AuSpec(3, 0, {0xB0, 0xB1, 0xB2}).withCts(13),
			     AuSpec(2, 1, {0xC0, 0xC1}).withCts(-3)}) == 61,
	  "test packet's AU headers are not byte-aligned");
    sc.immediate.push_back(auPacket(1, 48000, true, cfg,
				    {AuSpec(2, 0, {0xA0, 0xA1}),
				     AuSpec(3, 0, {0xB0, 0xB1, 0xB2}).withCts(13),
				     AuSpec(2, 1, {0xC0, 0xC1}).withCts(-3)}));

    // Bad packet A: the declared AU-headers-length cuts the last CTS-delta
    // short (57 bits declared instead of 61):
    {
      OutPacket bad = auPacket(2, 48000, true, cfg,
			       {AuSpec(2, 0, {0x01, 0x02}),
				AuSpec(3, 0, {0x03, 0x04, 0x05}).withCts(13),
				AuSpec(2, 1, {0x06, 0x07}).withCts(-3)});
      bad.payload[0] = 0; bad.payload[1] = 57; // AU-headers-length, in bits
      sc.immediate.push_back(bad);
    }
    // Bad packet B: AU-size (100) exceeds the payload, but the "M" bit says
    // the AU is complete:
    sc.immediate.push_back(auPacket(3, 48000, true, cfg,
				    {AuSpec(100, 0, {0x08, 0x09})}));
    // Bad packet C: the first AU carries an explicit CTS-delta (forbidden):
    sc.immediate.push_back(auPacket(4, 48000, true, cfg,
				    {AuSpec(2, 0, {0x0A, 0x0B}).withCts(5)}));
    // A good packet again, after the malformed ones:
    sc.immediate.push_back(auPacket(5, 48000, true, cfg,
				    {AuSpec(2, 0, {0xD0, 0xD1})}));

    bool timedOut;
    std::vector<Frame> frames = runScenario(sc, timedOut);
    check(!timedOut, "headers: expected frames arrived before the timeout");
    check(frames.size() == 4, "headers: only the good packets' AUs were delivered");
    checkFrameBytes(frames[0], {0xA0, 0xA1}, "headers: AU #0 bytes");
    checkFrameBytes(frames[1], {0xB0, 0xB1, 0xB2}, "headers: AU #1 bytes (after CTS header)");
    checkFrameBytes(frames[2], {0xC0, 0xC1}, "headers: AU #2 bytes (non-byte-aligned header)");
    checkFrameBytes(frames[3], {0xD0, 0xD1}, "headers: a good packet is received after bad ones");
  }

  // Case 3: with "constantDuration", a non-zero first AU-index is rejected:
  {
    AuCfg cfg{13, 3, 3, 0};
    Scenario sc;
    sc.sdp = aacSdp(48000, "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
			   "indexdeltalength=3;constantduration=1024");
    sc.expectFrames = 2;
    sc.immediate.push_back(auPacket(1, 48000, true, cfg,
				    {AuSpec(2, 1, {0xE0, 0xE1})})); // first index != 0
    sc.immediate.push_back(auPacket(2, 48000, true, cfg,
				    {AuSpec(2, 0, {0x01, 0x02}),
				     AuSpec(2, 0, {0x03, 0x04})}));
    bool timedOut;
    std::vector<Frame> frames = runScenario(sc, timedOut);
    check(!timedOut, "headers: frames arrived after a rejected first index");
    check(frames.size() == 2, "headers: packet with first AU-index != 0 was dropped");
    checkFrameBytes(frames[0], {0x01, 0x02}, "headers: first good AU after rejection");
    checkFrameBytes(frames[1], {0x03, 0x04}, "headers: second good AU after rejection");
  }

  // Case 4: the old behavior (no constantDuration, no CTS) is preserved —
  // all AUs of a packet share the packet's presentation time:
  {
    AuCfg cfg{13, 3, 3, 0};
    Scenario sc;
    sc.sdp = aacSdp(48000, "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
			   "indexdeltalength=3");
    sc.expectFrames = 2;
    sc.immediate.push_back(auPacket(1, 48000, true, cfg,
				    {AuSpec(2, 0, {0x11, 0x12}),
				     AuSpec(2, 0, {0x13, 0x14})}));
    bool timedOut;
    std::vector<Frame> frames = runScenario(sc, timedOut);
    check(!timedOut, "headers: legacy no-CTS stream still delivers");
    check(frames.size() == 2, "headers: legacy stream delivers both AUs");
    checkFrameBytes(frames[0], {0x11, 0x12}, "headers: legacy AU #0 bytes");
    checkFrameBytes(frames[1], {0x13, 0x14}, "headers: legacy AU #1 bytes");
    checkDiff(frames[1], frames[0], 0, "headers: legacy AUs share the packet's time");
  }

  // Case 5: an AU larger than the client's buffer is delivered truncated,
  // with the truncation reported:
  {
    AuCfg cfg{13, 3, 3, 0};
    Scenario sc;
    sc.sdp = aacSdp(48000, "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
			   "indexdeltalength=3");
    sc.expectFrames = 1;
    sc.bufSize = 5;
    sc.immediate.push_back(auPacket(1, 48000, true, cfg,
				    {AuSpec(8, 0, {1, 2, 3, 4, 5, 6, 7, 8})}));
    bool timedOut;
    std::vector<Frame> frames = runScenario(sc, timedOut);
    check(!timedOut, "headers: oversized AU delivered before the timeout");
    check(frames.size() == 1, "headers: one oversized AU delivered");
    checkFrameBytes(frames[0], {1, 2, 3, 4, 5}, "headers: oversized AU truncated to the buffer");
    check(frames[0].truncated == 3, "headers: truncation of 3 bytes reported");
  }

  // Case 6: out-of-range field widths and the unimplemented optional
  // AU-header fields (DTS-delta, RAP-flag, Stream-state) are rejected:
  {
    char const* base = "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;indexdeltalength=3";
    check(!initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;sizelength=33")), "headers: sizelength=33 rejected");
    check(!initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;indexlength=40")), "headers: indexlength=40 rejected");
    check(!initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;indexdeltalength=33")), "headers: indexdeltalength=33 rejected");
    check(!initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;ctsdeltalength=33")), "headers: ctsdeltalength=33 rejected");
    check(!initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;dtsdeltalength=2")), "headers: dtsdeltalength rejected");
    check(!initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;randomaccessindication=1")), "headers: randomaccessindication rejected");
    check(!initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;streamstateindication=1")), "headers: streamstateindication rejected");
    // ...while in-range and legacy configurations are still accepted:
    check(initiateSucceeds(aacSdp(48000, base)), "headers: legacy configuration accepted");
    check(initiateSucceeds(aacSdp(48000, "mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;"
					 "constantduration=1024;ctsdeltalength=32")),
	  "headers: ctsdeltalength=32 (the maximum) accepted");
  }

  return gChecks - startChecks;
}

////////// group 2: per-AU presentation times //////////

int test_aac_au_timing() {
  int const startChecks = gChecks;

  // 48 kHz, constantDuration=1024 ticks, 16-bit CTS-deltas; multiple packets,
  // an index-delta skip, explicit CTS (incl. negative), and timestamp wrap:
  {
    AuCfg cfg{13, 3, 3, 16};
    Scenario sc;
    sc.sdp = aacSdp(48000, "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
			   "indexdeltalength=3;constantduration=1024;ctsdeltalength=16");
    sc.expectFrames = 12;
    // P1: three AUs, evenly spaced (index-delta 0):
    sc.immediate.push_back(auPacket(1, 48000, true, cfg,
				    {AuSpec(2, 0, {0x01, 0x02}),
				     AuSpec(2, 0, {0x03, 0x04}),
				     AuSpec(2, 0, {0x05, 0x06})}));
    // P2: index-delta 1 on the second AU skips one AU's worth of time:
    sc.immediate.push_back(auPacket(2, 96000, true, cfg,
				    {AuSpec(2, 0, {0x07, 0x08}),
				     AuSpec(2, 1, {0x09, 0x0A})}));
    // P3: an explicit CTS-delta, then an AU timed by constantDuration:
    sc.immediate.push_back(auPacket(3, 144000, true, cfg,
				    {AuSpec(2, 0, {0x0B, 0x0C}),
				     AuSpec(2, 0, {0x0D, 0x0E}).withCts(2048),
				     AuSpec(2, 0, {0x0F, 0x10})}));
    // P4: a negative CTS-delta:
    sc.immediate.push_back(auPacket(4, 192000, true, cfg,
				    {AuSpec(2, 0, {0x11, 0x12}),
				     AuSpec(2, 0, {0x13, 0x14}).withCts(-1024)}));
    // P5+P6: the RTP timestamp wraps around between packets
    // (0xFFFFA240 == -24000 mod 2^32, so P6 is exactly 48000 ticks later):
    sc.immediate.push_back(auPacket(5, 0xFFFFA240u, true, cfg, {AuSpec(2, 0, {0x15, 0x16})}));
    sc.immediate.push_back(auPacket(6, 24000, true, cfg, {AuSpec(2, 0, {0x17, 0x18})}));

    bool timedOut;
    std::vector<Frame> f = runScenario(sc, timedOut);
    check(!timedOut, "timing: all frames arrived before the timeout");
    check(f.size() == 12, "timing: all 12 AUs were delivered");
    checkFrameBytes(f[0], {0x01, 0x02}, "timing: P1 AU #0 bytes");
    checkFrameBytes(f[4], {0x09, 0x0A}, "timing: P2 AU #1 bytes");
    checkFrameBytes(f[6], {0x0D, 0x0E}, "timing: P3 AU #1 (CTS) bytes");
    checkFrameBytes(f[9], {0x13, 0x14}, "timing: P4 AU #1 (negative CTS) bytes");
    checkFrameBytes(f[11], {0x17, 0x18}, "timing: P6 AU bytes after wrap");
    // P1: 1024 ticks at 48 kHz == 21333 us per AU:
    checkDiff(f[1], f[0], 21333, "timing: constantDuration spaces AUs (1)");
    checkDiff(f[2], f[0], 42667, "timing: constantDuration spaces AUs (2)");
    checkDiff(f[1], f[0], ticksToUs(1024, 48000), "timing: AU #1 offset in RTP ticks");
    // P2: index-delta 1 => relative index 2 => 2048 ticks:
    checkDiff(f[4], f[3], ticksToUs(2048, 48000), "timing: index-delta skip advances the time");
    // P3: explicit CTS (2048 ticks) and the duration-based AU coincide:
    checkDiff(f[6], f[5], ticksToUs(2048, 48000), "timing: explicit CTS-delta used directly");
    checkDiff(f[7], f[5], ticksToUs(2048, 48000), "timing: duration chain continues past a CTS AU");
    // P4: negative CTS-delta moves the AU earlier:
    checkDiff(f[9], f[8], -21333, "timing: negative CTS-delta");
    checkDiff(f[9], f[8], ticksToUs(-1024, 48000), "timing: negative CTS-delta in RTP ticks");
    // P5->P6: 48000 ticks across the 32-bit timestamp wrap == exactly 1 s:
    checkDiff(f[11], f[10], 1000000, "timing: RTP timestamp wrap is handled");
  }

  // 44.1 kHz, constantDuration only:
  {
    AuCfg cfg{13, 3, 3, 0};
    Scenario sc;
    sc.sdp = aacSdp(44100, "mode=AAC-hbr;config=1210;sizelength=13;indexlength=3;"
			   "indexdeltalength=3;constantduration=1024");
    sc.expectFrames = 3;
    sc.immediate.push_back(auPacket(1, 44100, true, cfg,
				    {AuSpec(2, 0, {0x21, 0x22}),
				     AuSpec(2, 0, {0x23, 0x24}),
				     AuSpec(2, 0, {0x25, 0x26})}));
    bool timedOut;
    std::vector<Frame> f = runScenario(sc, timedOut);
    check(!timedOut, "timing: 44.1 kHz frames arrived");
    check(f.size() == 3, "timing: 44.1 kHz AUs delivered");
    checkDiff(f[1], f[0], ticksToUs(1024, 44100), "timing: 44.1 kHz AU spacing (1)");
    checkDiff(f[2], f[0], ticksToUs(2048, 44100), "timing: 44.1 kHz AU spacing (2)");
  }

  // Non-byte-aligned optional CTS headers (5-bit CTS-deltas):
  {
    AuCfg cfg{13, 3, 3, 5};
    Scenario sc;
    sc.sdp = aacSdp(48000, "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
			   "indexdeltalength=3;constantduration=1024;ctsdeltalength=5");
    sc.expectFrames = 3;
    sc.immediate.push_back(auPacket(1, 48000, true, cfg,
				    {AuSpec(1, 0, {0x31}),
				     AuSpec(1, 0, {0x32}).withCts(13),
				     AuSpec(1, 0, {0x33}).withCts(-3)}));
    bool timedOut;
    std::vector<Frame> f = runScenario(sc, timedOut);
    check(!timedOut, "timing: non-byte-aligned CTS frames arrived");
    check(f.size() == 3, "timing: non-byte-aligned CTS AUs delivered");
    checkFrameBytes(f[1], {0x32}, "timing: AU bytes after a 5-bit CTS header");
    checkDiff(f[1], f[0], ticksToUs(13, 48000), "timing: 5-bit CTS-delta applied");
    checkDiff(f[2], f[0], ticksToUs(-3, 48000), "timing: negative 5-bit CTS-delta applied");
  }

  // A non-AAC MultiFramedRTPSource stream (MPA) still receives as before:
  {
    Scenario sc;
    sc.sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=mpa\r\n"
	     "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
	     "m=audio 0 RTP/AVP 14\r\na=rtpmap:14 MPA/48000\r\n";
    sc.expectFrames = 1;
    OutPacket p;
    p.seq = 1; p.rtpTs = 123456; p.marker = true; p.payloadType = 14;
    p.payload = {0, 0, 0, 0, 0xFF, 0xFB, 0x90, 0x64}; // 4-byte MPA header + frame
    sc.immediate.push_back(p);
    bool timedOut;
    std::vector<Frame> f = runScenario(sc, timedOut);
    check(!timedOut, "timing: non-AAC (MPA) stream still delivers");
    check(f.size() == 1, "timing: non-AAC frame count");
    checkFrameBytes(f[0], {0xFF, 0xFB, 0x90, 0x64}, "timing: non-AAC frame bytes");
  }

  return gChecks - startChecks;
}

////////// group 3: AU fragmentation and reassembly //////////

int test_aac_fragment_timing() {
  int const startChecks = gChecks;

  AuCfg cfg{13, 3, 3, 0};
  Scenario sc;
  sc.sdp = aacSdp(48000, "mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;"
			 "indexdeltalength=3;constantduration=1024");
  sc.expectFrames = 6;
  sc.bufSize = 64;

  // P1: one complete AU, then the start of a 6-byte AU (3 bytes here); the
  // AU-size of the fragment is the COMPLETE AU's size:
  sc.immediate.push_back(auPacket(1, 48000, false, cfg,
				  {AuSpec(2, 0, {0x41, 0x42}),
				   AuSpec(6, 0, {0x51, 0x52, 0x53})}));
  // P2: completes the fragmented AU (same RTP timestamp, same AU-size):
  sc.immediate.push_back(auPacket(2, 48000, true, cfg,
				  {AuSpec(6, 0, {0x54, 0x55, 0x56})}));
  // P3: a normal packet afterwards:
  sc.immediate.push_back(auPacket(3, 96000, true, cfg,
				  {AuSpec(2, 0, {0x61, 0x62})}));
  // P4-P6: a 9-byte AU fragmented over three packets:
  sc.immediate.push_back(auPacket(4, 144000, false, cfg,
				  {AuSpec(9, 0, {0x71, 0x72, 0x73})}));
  sc.immediate.push_back(auPacket(5, 144000, false, cfg,
				  {AuSpec(9, 0, {0x74, 0x75, 0x76})}));
  sc.immediate.push_back(auPacket(6, 144000, true, cfg,
				  {AuSpec(9, 0, {0x77, 0x78, 0x79})}));
  // P7: another fragment start; its continuation (seq 8) is LOST:
  sc.immediate.push_back(auPacket(7, 192000, false, cfg,
				  {AuSpec(8, 0, {0x81, 0x82})}));
  // P8 (seq 9): a normal packet that arrives while seq 8 is missing; it
  // cannot be the lost continuation, so the incomplete AU is dropped:
  sc.immediate.push_back(auPacket(9, 288000, true, cfg,
				  {AuSpec(2, 0, {0x91, 0x92})}));

  // Sent 250 ms later (past the reordering threshold, so the loss of seq 8
  // is acted upon):
  // P9: a normal packet — received fine after the loss:
  sc.delayed.push_back(auPacket(10, 336000, true, cfg,
				{AuSpec(2, 0, {0xA1, 0xA2})}));
  // P10+P11: a fragment whose continuation is too long (6 bytes offered,
  // 4 remaining) — the incomplete AU is dropped:
  sc.delayed.push_back(auPacket(11, 384000, false, cfg,
				{AuSpec(8, 0, {0xB1, 0xB2, 0xB3, 0xB4})}));
  sc.delayed.push_back(auPacket(12, 384000, true, cfg,
				{AuSpec(8, 0, {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6})}));
  // P12: another normal packet — the stream keeps going:
  sc.delayed.push_back(auPacket(13, 432000, true, cfg,
				{AuSpec(2, 0, {0xD1, 0xD2})}));

  bool timedOut;
  std::vector<Frame> f = runScenario(sc, timedOut);
  check(!timedOut, "fragments: expected frames arrived before the timeout");
  check(f.size() == 6, "fragments: exactly the complete AUs were delivered");

  checkFrameBytes(f[0], {0x41, 0x42}, "fragments: complete AU before a fragment");
  checkFrameBytes(f[1], {0x51, 0x52, 0x53, 0x54, 0x55, 0x56},
		  "fragments: AU reassembled from two packets, delivered once");
  check(f[1].truncated == 0, "fragments: reassembled AU not truncated");
  checkFrameBytes(f[2], {0x61, 0x62}, "fragments: normal AU after reassembly");
  checkFrameBytes(f[3], {0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79},
		  "fragments: AU reassembled from three packets");
  checkFrameBytes(f[4], {0xA1, 0xA2}, "fragments: normal packet received after a lost fragment");
  checkFrameBytes(f[5], {0xD1, 0xD2}, "fragments: normal packet received after an oversized fragment");

  // The reassembled AU keeps the presentation time computed from its FIRST
  // fragment (relative index 1 => 1024 ticks after the packet's anchor);
  // the continuation packet does not move it:
  checkDiff(f[1], f[0], 21333, "fragments: reassembled AU keeps its first fragment's time");
  checkDiff(f[1], f[0], ticksToUs(1024, 48000), "fragments: first-fragment time in RTP ticks");
  // Packet anchors advance by the RTP timestamp (exactly 1 s per 48000 ticks):
  checkDiff(f[2], f[0], 1000000, "fragments: next packet's anchor");
  checkDiff(f[3], f[2], 1000000, "fragments: three-packet AU's anchor");
  checkDiff(f[4], f[3], 4000000, "fragments: anchor after the lost fragment");
  checkDiff(f[5], f[4], 2000000, "fragments: anchor after the oversized fragment");

  return gChecks - startChecks;
}
