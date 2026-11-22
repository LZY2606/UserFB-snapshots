// task_tests/deadline_tcp.cpp
//
// Unit tests for the optional deadline-aware queued send mode of RTPInterface (see
// RTPInterface.hh). Everything runs against a scripted IN-MEMORY writer and a manually advanceable
// clock: no real network, no sleeps, so short writes, EAGAIN, EINTR, hard errors and play-time
// expiry are all replayed deterministically. The production entry points (sendPacket /
// enableQueuedSendMode / pumpQueuedSend / ...) are exercised end to end and the exact bytes handed
// to the writer are asserted.
//
// Registered through task_support/check.h's task_cases[] in three groups:
//   framing    - RFC 2326 sec 10.12 frame layout, FIFO order, short/EINTR/EAGAIN/hard-error writes
//   deadlines  - play-deadline math (incl. RTP wrap), expiry vs. already-started frames
//   lifecycle  - byte budget, channel removal, fd reuse, alternative-byte-handler exclusion

#include "task_support/check.h"

#include "RTPInterface.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include "Groupsock.hh"
#include "Media.hh"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---- A manually advanceable monotonic clock (microseconds), injected via setQueuedSendHooks() ----
struct ManualClock {
  long long nowUs;
  explicit ManualClock(long long start = 100000) : nowUs(start) {}
  void advance(long long us) { nowUs += us; }
};

long long manualClockThunk(void* clientData) {
  return static_cast<ManualClock*>(clientData)->nowUs;
}

// ---- Scripted underlying writer: records every emitted byte and replays a fixed write script ----
enum WriteKind { WRITE_ACCEPT, WRITE_EAGAIN, WRITE_EINTR, WRITE_ERROR };

struct WriteAction {
  WriteKind kind;
  unsigned calls;      // how many successive write() calls this action applies to
  ssize_t acceptBytes; // for WRITE_ACCEPT: cap on bytes taken per call
  int err;             // for WRITE_ERROR
};

struct MemoryWriter {
  std::vector<WriteAction> script;
  size_t actionIndex = 0;
  bool initialized = false;
  unsigned actionCallsLeft = 0;
  std::vector<unsigned char> emitted;
  unsigned hardErrorCount = 0;

  RTPQueueWriteResult callWrite(int /*socketNum*/, unsigned char const* data, unsigned dataSize) {
    RTPQueueWriteResult result;
    result.bytesAccepted = 0;
    result.err = 0;

    // Lazily bind to script[0]: tests assign the script (and sometimes clear it later) after
    // constructing the h.writer.
    if (!initialized) {
      actionIndex = 0;
      actionCallsLeft = script.empty() ? 0 : script[0].calls;
      initialized = true;
    }

    if (actionIndex >= script.size()) {
      // Script exhausted (or cleared): behave as a perfectly drainable non-blocking socket.
      emitted.insert(emitted.end(), data, data + dataSize);
      result.bytesAccepted = (ssize_t)dataSize;
      return result;
    }
    WriteAction const& action = script[actionIndex];
    if (actionCallsLeft > 0) --actionCallsLeft;

    switch (action.kind) {
      case WRITE_ACCEPT: {
        ssize_t n = action.acceptBytes < (ssize_t)dataSize ? action.acceptBytes : (ssize_t)dataSize;
        if (n < 0) n = 0;
        emitted.insert(emitted.end(), data, data + n);
        result.bytesAccepted = n;
        break;
      }
      case WRITE_EAGAIN: result.err = EAGAIN; break;
      case WRITE_EINTR:  result.err = EINTR;  break;
      case WRITE_ERROR:  result.err = action.err != 0 ? action.err : EPIPE; ++hardErrorCount; break;
    }

    if (actionCallsLeft == 0) ++actionIndex;
    return result;
  }
};

RTPQueueWriteResult memoryWriteThunk(void* clientData, int socketNum,
				     unsigned char const* data, unsigned dataSize) {
  return static_cast<MemoryWriter*>(clientData)->callWrite(socketNum, data, dataSize);
}

// BasicTaskScheduler::SingleStep() is protected; expose it so a test can prove the writable
// callback is really wired into the existing scheduler (select() on the fd, no polling sleep).
class ExposedScheduler : public BasicTaskScheduler {
public:
  static ExposedScheduler* createNew() { return new ExposedScheduler(); }
  void stepOnce() { SingleStep(0); }
protected:
  ExposedScheduler() : BasicTaskScheduler(10000) {}
};

struct TestEnv {
  ExposedScheduler* scheduler;
  UsageEnvironment* env;
  // Owned here (the longest-lived object in a case) so a queue's injected clock/writer hooks can
  // never outlive their targets when TestInterfaces are torn down at the end of a case.
  ManualClock clock;
  MemoryWriter writer;

  TestEnv() : scheduler(NULL), env(NULL), clock(100000) {
    scheduler = ExposedScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*scheduler);
  }
  ~TestEnv() {
    env->reclaim();
    delete scheduler;
  }
};

class TestOwner : public Medium {
public:
  explicit TestOwner(UsageEnvironment& e) : Medium(e) {}
  virtual ~TestOwner() {}
};

// One RTPInterface plus the owner/groupsock it needs.
struct TestInterface {
  TestEnv& harness;
  TestOwner owner;
  Groupsock gs;
  RTPInterface iface;

  TestInterface(TestEnv& h)
    : harness(h), owner(*h.env),
      gs(*h.env,
      [] {
        struct sockaddr_storage a;
        memset(&a, 0, sizeof(a));
        a.ss_family = AF_INET;
        return a;
      }(),
      Port(0), 1),
      iface(&owner, &gs) {
    gs.removeAllDestinations();
  }
};

struct SocketPair {
  int fds[2];
  SocketPair() {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) { fds[0] = fds[1] = -1; }
  }
  ~SocketPair() {
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
  }
};

// Build a minimal RTP packet (V2, PT=96) with the given timestamp, sequence number and payload fill.
std::vector<unsigned char> rtpPacket(u_int32_t timestamp, unsigned char seq,
				     unsigned payloadLen, unsigned char fill) {
  std::vector<unsigned char> p(12 + payloadLen, fill);
  p[0] = 0x80;
  p[1] = 96;
  p[2] = (unsigned char)(seq >> 8);
  p[3] = (unsigned char)(seq & 0xFF);
  p[4] = (unsigned char)(timestamp >> 24);
  p[5] = (unsigned char)(timestamp >> 16);
  p[6] = (unsigned char)(timestamp >> 8);
  p[7] = (unsigned char)(timestamp & 0xFF);
  return p;
}

// Assert that one complete interleaved frame sits at emitted[pos] and matches channel/payload.
bool expectFrame(std::vector<unsigned char> const& emitted, size_t& pos,
		 unsigned char channel, std::vector<unsigned char> const& payload) {
  if (pos + 4 + payload.size() > emitted.size()) return false;
  if (emitted[pos] != '$' || emitted[pos + 1] != channel) return false;
  unsigned len = ((unsigned)emitted[pos + 2] << 8) | emitted[pos + 3];
  if (len != payload.size()) return false;
  if (memcmp(&emitted[pos + 4], payload.data(), payload.size()) != 0) return false;
  pos += 4 + payload.size();
  return true;
}

void attachQueue(TestEnv& h, int fd,
		 ManualClock* clock = NULL, MemoryWriter* writer = NULL) {
  // Default to the harness-owned clock/writer, which outlive every TestInterface in the case.
  // Hooks must be installed before the first packet (a queue freezes its hooks once traffic flows).
  RTPInterface::setQueuedSendHooks(*h.env, fd,
				   manualClockThunk, clock != NULL ? clock : &h.clock,
				   memoryWriteThunk, writer != NULL ? writer : &h.writer);
}

} // namespace

// ============================ framing group ============================

static int framing_fifo_two_channels() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h), b(h);
  a.iface.addStreamSocket(sp.fds[0], 2, NULL);
  b.iface.addStreamSocket(sp.fds[0], 3, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> p1 = rtpPacket(1000, 1, 10, 0xA1);
  std::vector<unsigned char> p2 = rtpPacket(2000, 2, 8,  0xB2);
  std::vector<unsigned char> p3 = rtpPacket(3000, 3, 12, 0xC3);
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(b.iface.sendPacket(p2.data(), (unsigned)p2.size()));
  TASK_CHECK(a.iface.sendPacket(p3.data(), (unsigned)p3.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));

  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 2, p1));
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 3, p2));
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 2, p3));
  TASK_CHECK(pos == h.writer.emitted.size());
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 0);
  return 0;
}

static int framing_short_writes_continue_at_offset() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  h.writer.script = {
    {WRITE_ACCEPT, 1, 2, 0},  // first call takes only the first 2 header bytes
    {WRITE_ACCEPT, 1, 3, 0},  // next call takes 3 more
    // afterwards the script drains everything
  };
  h.writer.actionCallsLeft = 1;
  a.iface.addStreamSocket(sp.fds[0], 4, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> p1 = rtpPacket(11, 1, 20, 0x11);
  std::vector<unsigned char> p2 = rtpPacket(22, 2, 16, 0x22);
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(a.iface.sendPacket(p2.data(), (unsigned)p2.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  // A second pump must be a no-op now that everything drained.
  size_t before = h.writer.emitted.size();
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  TASK_CHECK(h.writer.emitted.size() == before);

  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 4, p1));
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 4, p2));
  TASK_CHECK(pos == h.writer.emitted.size());
  return 0;
}

static int framing_eintr_consumes_nothing() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  h.writer.script = {
    {WRITE_EINTR, 2, 0, 0},
    {WRITE_ACCEPT, 1, 1, 0}, // one byte, to prove retry resumes at offset 0
  };
  h.writer.actionCallsLeft = 2;
  a.iface.addStreamSocket(sp.fds[0], 5, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> p1 = rtpPacket(33, 9, 30, 0x33);
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));

  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 5, p1));
  TASK_CHECK(pos == h.writer.emitted.size());
  return 0;
}

static int framing_oversize_rejected_no_truncated_header() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  a.iface.addStreamSocket(sp.fds[0], 6, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1ULL << 30));
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> tooBig(65536, 0x77);
  TASK_CHECK(!a.iface.sendPacket(tooBig.data(), (unsigned)tooBig.size()));
  TASK_CHECK(h.writer.emitted.empty());
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 0);

  std::vector<unsigned char> maxed(65535, 0x88);
  TASK_CHECK(a.iface.sendPacket(maxed.data(), (unsigned)maxed.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  TASK_CHECK(h.writer.emitted.size() == 4 + 65535);
  TASK_CHECK(h.writer.emitted[0] == '$' && h.writer.emitted[1] == 6);
  TASK_CHECK(h.writer.emitted[2] == 0xFF && h.writer.emitted[3] == 0xFF);
  TASK_CHECK(memcmp(&h.writer.emitted[4], maxed.data(), 65535) == 0);
  return 0;
}

static int framing_source_buffer_is_copied_and_owned() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  h.writer.script = {{WRITE_EAGAIN, 1, 0, 0}}; // park the frame in the queue first
  h.writer.actionCallsLeft = 1;
  a.iface.addStreamSocket(sp.fds[0], 7, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> packet = rtpPacket(444, 4, 40, 0x44);
  std::vector<unsigned char> original = packet;
  TASK_CHECK(a.iface.sendPacket(packet.data(), (unsigned)packet.size()));
  // The queue took ownership of a copy: the caller may now reuse/overwrite its buffer.
  memset(packet.data(), 0xAA, packet.size());
  TASK_CHECK(RTPInterface::queuedSendHasPending(*h.env, sp.fds[0]));

  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // EAGAIN: stays queued
  TASK_CHECK(RTPInterface::queuedSendHasPending(*h.env, sp.fds[0]));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // script exhausted: drains fully

  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 7, original));
  TASK_CHECK(pos == h.writer.emitted.size());
  return 0;
}

static int framing_default_mode_still_sends_inline() {
  // Control case: WITHOUT enableQueuedSendMode() the legacy behaviour is unchanged -- an immediate
  // inline send over the real socket, no queue involved.
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  a.iface.addStreamSocket(sp.fds[0], 8, NULL);
  TASK_CHECK(!RTPInterface::queuedSendIsActive(*h.env, sp.fds[0]));

  std::vector<unsigned char> packet = rtpPacket(555, 5, 14, 0x55);
  TASK_CHECK(a.iface.sendPacket(packet.data(), (unsigned)packet.size()));

  unsigned char got[64] = {};
  size_t total = 0;
  while (total < 4 + packet.size()) {
    ssize_t r = recv(sp.fds[1], got + total, sizeof(got) - total, 0);
    if (r <= 0) break;
    total += (size_t)r;
  }
  TASK_CHECK(total == 4 + packet.size());
  TASK_CHECK(got[0] == '$' && got[1] == 8);
  TASK_CHECK(got[2] == 0 && got[3] == packet.size());
  TASK_CHECK(memcmp(got + 4, packet.data(), packet.size()) == 0);
  return 0;
}

// ============================ deadlines group ============================

static int deadlines_basic_expiry_releases_capacity() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  a.iface.addStreamSocket(sp.fds[0], 9, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);
  // rate 90000 Hz; ts 9000 is exactly 100000 us after the base.
  TASK_CHECK(a.iface.setPlaybackTiming(90000, 0, 1000, 500000));

  std::vector<unsigned char> p1 = rtpPacket(9000, 1, 10, 0x61);
  h.clock.nowUs = 600000; // deadline = 1000 + 500000 + 100000 = 601000, still due
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 4 + p1.size());

  h.clock.advance(1000); // now == deadline: an unstarted frame at/after its deadline is dropped
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  TASK_CHECK(h.writer.emitted.empty());
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 0);

  // Already-expired on arrival: rejected, never takes capacity.
  std::vector<unsigned char> p2 = rtpPacket(18000, 2, 10, 0x62); // deadline 701000, now 601000 -> future
  TASK_CHECK(a.iface.sendPacket(p2.data(), (unsigned)p2.size()));
  h.clock.nowUs = 800000;
  std::vector<unsigned char> p3 = rtpPacket(9000, 3, 10, 0x63);
  TASK_CHECK(!a.iface.sendPacket(p3.data(), (unsigned)p3.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // p2 also expires and is dropped
  TASK_CHECK(h.writer.emitted.empty());
  return 0;
}

static int deadlines_rtp_wrap_and_negative_delta() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  a.iface.addStreamSocket(sp.fds[0], 10, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);
  TASK_CHECK(a.iface.setPlaybackTiming(90000, 0xFFFFFFF0u, 10000, 5000));

  // Forward wrap: delta = int32(0x0000000A - 0xFFFFFFF0) = 26 -> floor(26e6/90000) = 288 us.
  std::vector<unsigned char> forward = rtpPacket(0x0000000Au, 1, 6, 0x0A);
  h.clock.nowUs = 15287; // deadline 15288
  TASK_CHECK(a.iface.sendPacket(forward.data(), (unsigned)forward.size()));
  h.clock.nowUs = 15288;
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  TASK_CHECK(h.writer.emitted.empty()); // expired exactly on the deadline

  // Negative delta: ts = base - 1 -> floor(-1e6/90000) = -12 us -> deadline 14988.
  std::vector<unsigned char> backward = rtpPacket(0xFFFFFFEFu, 2, 6, 0x0B);
  h.clock.nowUs = 14987;
  TASK_CHECK(a.iface.sendPacket(backward.data(), (unsigned)backward.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 10, backward));
  TASK_CHECK(pos == h.writer.emitted.size());
  return 0;
}

static int deadlines_same_timestamp_fragments_share_one_deadline() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  h.writer.script = {
    {WRITE_ACCEPT, 1, 1, 0}, // frame 1: exactly one header byte goes out, then EAGAIN
    {WRITE_EAGAIN, 1, 0, 0},
  };
  h.writer.actionCallsLeft = 1;
  a.iface.addStreamSocket(sp.fds[0], 11, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);
  TASK_CHECK(a.iface.setPlaybackTiming(90000, 0, 0, 100000));

  std::vector<unsigned char> f1 = rtpPacket(9000, 1, 20, 0x01); // 100000 us of media -> deadline 200000
  std::vector<unsigned char> f2 = rtpPacket(9000, 2, 20, 0x02); // same timestamp -> same deadline
  TASK_CHECK(a.iface.sendPacket(f1.data(), (unsigned)f1.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // 1 byte out, then blocked
  TASK_CHECK(a.iface.sendPacket(f2.data(), (unsigned)f2.size()));

  h.clock.nowUs = 200000; // both deadlines reached; f1 started, f2 did not
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 11, f1)); // started frame completed
  TASK_CHECK(pos == h.writer.emitted.size());            // f2 was discarded
  return 0;
}

static int deadlines_expired_packets_behind_blocked_head_release() {
  TestEnv h;
  h.clock.nowUs = 0; // deadlines below are absolute from zero
  SocketPair sp;
  TestInterface a(h);
  h.writer.script = {{WRITE_EAGAIN, 10, 0, 0}}; // blocked until the test flips the script
  h.writer.actionCallsLeft = 10;
  a.iface.addStreamSocket(sp.fds[0], 12, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);
  TASK_CHECK(a.iface.setPlaybackTiming(90000, 0, 0, 0));

  std::vector<unsigned char> pa = rtpPacket(9000, 1, 10, 0xA); // deadline 100000
  std::vector<unsigned char> pb = rtpPacket(18000, 2, 10, 0xB); // deadline 200000
  std::vector<unsigned char> pc = rtpPacket(27000, 3, 10, 0xC); // deadline 300000
  TASK_CHECK(a.iface.sendPacket(pa.data(), (unsigned)pa.size()));
  TASK_CHECK(a.iface.sendPacket(pb.data(), (unsigned)pb.size()));
  TASK_CHECK(a.iface.sendPacket(pc.data(), (unsigned)pc.size()));
  unsigned long long fullBytes = 3ULL * 4 + pa.size() + pb.size() + pc.size();
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == fullBytes);

  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // head EAGAIN; nothing sent
  h.clock.nowUs = 150000;
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // pa expires behind the blocked head
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0])
	     == 2ULL * 4 + pb.size() + pc.size());
  h.clock.nowUs = 250000;
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // pb expires too
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 4ULL + pc.size());

  h.writer.script.clear(); // writer drains again
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 12, pc)); // only the fresh survivor is emitted
  TASK_CHECK(pos == h.writer.emitted.size());
  return 0;
}

static int deadlines_partial_header_survives_its_own_deadline() {
  TestEnv h;
  h.clock.nowUs = 0;
  SocketPair sp;
  TestInterface a(h), b(h);
  h.writer.script = {
    {WRITE_ACCEPT, 1, 1, 0}, // one header byte of frame A, then blocked
    {WRITE_EAGAIN, 10, 0, 0},
  };
  h.writer.actionCallsLeft = 1;
  a.iface.addStreamSocket(sp.fds[0], 13, NULL);
  b.iface.addStreamSocket(sp.fds[0], 14, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);
  TASK_CHECK(a.iface.setPlaybackTiming(90000, 0, 0, 0));
  TASK_CHECK(b.iface.setPlaybackTiming(90000, 0, 0, 0));

  std::vector<unsigned char> fa = rtpPacket(9000, 1, 12, 0xA); // deadline 100000
  std::vector<unsigned char> fb = rtpPacket(18000, 2, 12, 0xB); // deadline 200000
  TASK_CHECK(a.iface.sendPacket(fa.data(), (unsigned)fa.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // 1 byte out, EAGAIN
  TASK_CHECK(b.iface.sendPacket(fb.data(), (unsigned)fb.size()));

  h.clock.nowUs = 500000; // both deadlines long past
  h.writer.script.clear();
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 13, fa)); // remnant completed, never concatenated over
  TASK_CHECK(pos == h.writer.emitted.size());            // unstarted fb was dropped
  return 0;
}

static int deadlines_rtcp_interface_has_no_deadline_shares_fifo() {
  TestEnv h;
  h.clock.nowUs = 0;
  SocketPair sp;
  TestInterface media(h), rtcp(h);
  media.iface.addStreamSocket(sp.fds[0], 0, NULL);
  rtcp.iface.addStreamSocket(sp.fds[0], 1, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);
  TASK_CHECK(media.iface.setPlaybackTiming(90000, 0, 0, 0));
  // Deliberately NO setPlaybackTiming() on rtcp: RTCP has no play deadline.

  std::vector<unsigned char> m1 = rtpPacket(9000, 1, 8, 0x10);  // deadline 100000
  std::vector<unsigned char> r1(8, 0xC8); // tiny RTCP compound, no play deadline
  std::vector<unsigned char> m2 = rtpPacket(27000, 2, 8, 0x20); // deadline 300000
  TASK_CHECK(media.iface.sendPacket(m1.data(), (unsigned)m1.size()));
  TASK_CHECK(rtcp.iface.sendPacket(r1.data(), (unsigned)r1.size()));
  TASK_CHECK(media.iface.sendPacket(m2.data(), (unsigned)m2.size()));

  h.clock.nowUs = 100000000000LL; // absurdly far ahead: both media frames expire, RTCP never does
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 1, r1)); // only the deadline-free RTCP frame survives
  TASK_CHECK(pos == h.writer.emitted.size());
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 0);
  return 0;
}

// ============================ lifecycle group ============================

static int lifecycle_byte_budget_enforced_and_released() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  h.writer.script = {{WRITE_EAGAIN, 10, 0, 0}}; // keep everything queued
  h.writer.actionCallsLeft = 10;
  a.iface.addStreamSocket(sp.fds[0], 15, NULL);
  std::vector<unsigned char> p1 = rtpPacket(1, 1, 10, 0x01);
  unsigned const frameSize = 4u + (unsigned)p1.size(); // 4-byte interleaved header + whole RTP packet
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], frameSize)); // exactly one frame fits
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> p2 = rtpPacket(2, 2, 10, 0x02);
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(!a.iface.sendPacket(p2.data(), (unsigned)p2.size())); // over budget: rejected, not evicting p1
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == frameSize);

  h.writer.script.clear();
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // drain p1
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 0);
  TASK_CHECK(a.iface.sendPacket(p2.data(), (unsigned)p2.size())); // capacity returned
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 15, p1));
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 15, p2));
  return 0;
}

static int lifecycle_channel_removal_completes_started_frame() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h), b(h);
  h.writer.script = {
    {WRITE_ACCEPT, 1, 1, 0},
    {WRITE_EAGAIN, 10, 0, 0},
  };
  h.writer.actionCallsLeft = 1;
  a.iface.addStreamSocket(sp.fds[0], 16, NULL);
  b.iface.addStreamSocket(sp.fds[0], 17, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> a1 = rtpPacket(1, 1, 10, 0xA1); // will be started
  std::vector<unsigned char> a2 = rtpPacket(2, 2, 10, 0xA2); // stays unstarted
  std::vector<unsigned char> b1 = rtpPacket(3, 3, 10, 0xB1);
  TASK_CHECK(a.iface.sendPacket(a1.data(), (unsigned)a1.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  TASK_CHECK(a.iface.sendPacket(a2.data(), (unsigned)a2.size()));
  TASK_CHECK(b.iface.sendPacket(b1.data(), (unsigned)b1.size()));

  a.iface.removeStreamSocket(sp.fds[0], 16); // a2 withdrawn; a1 must survive
  // a1 already handed one byte to the writer, so its remaining (frame-1) bytes stay queued
  // alongside b1's full frame.
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0])
	     == (4ULL + a1.size()) - 1 + (4ULL + b1.size()));

  h.writer.script.clear();
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 16, a1)); // started frame finished first
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 17, b1)); // other channel untouched
  TASK_CHECK(pos == h.writer.emitted.size());            // a2 never went out
  return 0;
}

static int lifecycle_hard_error_kills_one_socket_only() {
  TestEnv h;
  SocketPair s1, s2;
  // Declared BEFORE the interfaces so reverse-order destruction tears the interfaces down first
  // and their queues never call hooks into freed writer memory.
  ManualClock clock;
  MemoryWriter w1, w2;
  w1.script = {{WRITE_ERROR, 5, 0, EPIPE}};
  w1.actionCallsLeft = 5;
  TestInterface x(h), y(h);
  x.iface.addStreamSocket(s1.fds[0], 18, NULL);
  y.iface.addStreamSocket(s2.fds[0], 19, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, s1.fds[0], 1 << 20));
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, s2.fds[0], 1 << 20));
  RTPInterface::setQueuedSendHooks(*h.env, s1.fds[0], manualClockThunk, &clock,
				   memoryWriteThunk, &w1);
  RTPInterface::setQueuedSendHooks(*h.env, s2.fds[0], manualClockThunk, &clock,
				   memoryWriteThunk, &w2);

  std::vector<unsigned char> px = rtpPacket(1, 1, 12, 0x58);
  std::vector<unsigned char> py = rtpPacket(2, 2, 12, 0x59);
  TASK_CHECK(x.iface.sendPacket(px.data(), (unsigned)px.size()));
  TASK_CHECK(y.iface.sendPacket(py.data(), (unsigned)py.size()));

  TASK_CHECK(!RTPInterface::pumpQueuedSend(*h.env, s1.fds[0]));
  TASK_CHECK(!RTPInterface::queuedSendIsActive(*h.env, s1.fds[0]));
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, s1.fds[0]) == 0);
  TASK_CHECK(w1.hardErrorCount == 1);
  // New packets on the dead queue are refused; its callbacks no longer do anything.
  TASK_CHECK(!x.iface.sendPacket(px.data(), (unsigned)px.size()));

  // The other socket is completely unaffected.
  TASK_CHECK(RTPInterface::queuedSendIsActive(*h.env, s2.fds[0]));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, s2.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(w2.emitted, pos, 19, py));
  TASK_CHECK(pos == w2.emitted.size());

  // Teardown on the terminated socket must still run cleanly.
  x.iface.removeStreamSocket(s1.fds[0], 18);
  TASK_CHECK(!RTPInterface::queuedSendIsActive(*h.env, s1.fds[0]));
  return 0;
}

static int lifecycle_eagain_arms_scheduler_writable_callback() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  h.writer.script = {{WRITE_EAGAIN, 3, 0, 0}}; // three blocked calls, then drain
  h.writer.actionCallsLeft = 3;
  a.iface.addStreamSocket(sp.fds[0], 20, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
  attachQueue(h, sp.fds[0]);

  std::vector<unsigned char> p1 = rtpPacket(7, 7, 24, 0x77);
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp.fds[0])); // EAGAIN: parked
  TASK_CHECK(RTPInterface::queuedSendHasPending(*h.env, sp.fds[0]));
  TASK_CHECK(h.writer.emitted.empty());

  // Drive the REAL scheduler: select() finds the socketpair writable and dispatches the queue's
  // registered handler (no sleep/poll inside the production code). The first step still sees EAGAIN.
  h.scheduler->stepOnce();
  TASK_CHECK(RTPInterface::queuedSendHasPending(*h.env, sp.fds[0]));
  h.writer.script.clear();
  h.scheduler->stepOnce(); // handler fires again; writer now drains the whole frame
  TASK_CHECK(!RTPInterface::queuedSendHasPending(*h.env, sp.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 20, p1));
  TASK_CHECK(pos == h.writer.emitted.size());
  return 0;
}

static void dummyAlternativeByteHandler(void*, u_int8_t) {}

static int lifecycle_alternative_byte_handler_mutual_exclusion() {
  TestEnv h;
  SocketPair s1, s2;
  ManualClock clock; // must outlive the interfaces below
  MemoryWriter w1;
  TestInterface a(h), b(h);
  a.iface.addStreamSocket(s1.fds[0], 21, NULL);
  b.iface.addStreamSocket(s2.fds[0], 22, NULL);

  // Handler registered FIRST: enabling queued mode must be refused and leave state unchanged.
  RTPInterface::setServerRequestAlternativeByteHandler(*h.env, s2.fds[0],
						       dummyAlternativeByteHandler, NULL);
  TASK_CHECK(!RTPInterface::enableQueuedSendMode(*h.env, s2.fds[0], 1 << 20));
  TASK_CHECK(!RTPInterface::queuedSendIsActive(*h.env, s2.fds[0]));

  // Queued mode FIRST: a later handler registration is refused, the queue stays in control.
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, s1.fds[0], 1 << 20));
  RTPInterface::setQueuedSendHooks(*h.env, s1.fds[0], manualClockThunk, &clock,
				   memoryWriteThunk, &w1);
  RTPInterface::setServerRequestAlternativeByteHandler(*h.env, s1.fds[0],
						       dummyAlternativeByteHandler, NULL);
  TASK_CHECK(RTPInterface::queuedSendIsActive(*h.env, s1.fds[0]));
  std::vector<unsigned char> p1 = rtpPacket(8, 8, 9, 0x88);
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, s1.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(w1.emitted, pos, 21, p1));
  TASK_CHECK(pos == w1.emitted.size());
  return 0;
}

static int lifecycle_zero_budget_and_bad_clock_rate_rejected() {
  TestEnv h;
  SocketPair sp;
  TestInterface a(h);
  a.iface.addStreamSocket(sp.fds[0], 23, NULL);
  TASK_CHECK(!RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 0));
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 777));
  TASK_CHECK(!a.iface.setPlaybackTiming(0, 0, 0, 0));       // rate must be positive
  TASK_CHECK(a.iface.setPlaybackTiming(48000, 1234, 100, 200000));
  // Enabling twice is a no-op failure that leaves the original budget in place.
  TASK_CHECK(!RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1));
  attachQueue(h, sp.fds[0]);
  std::vector<unsigned char> p1(777 - 4, 0x42); // raw packet sized so the full interleaved frame is exactly 777
  TASK_CHECK(p1.size() + 4 == 777);
  TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 777);
  return 0;
}

static int lifecycle_last_interface_leave_and_fd_reuse() {
  TestEnv h;
  int oldFd = -1;
  {
    SocketPair sp;
    oldFd = sp.fds[0];
    TestInterface a(h), b(h);
    a.iface.addStreamSocket(sp.fds[0], 24, NULL);
    b.iface.addStreamSocket(sp.fds[0], 25, NULL);
    TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp.fds[0], 1 << 20));
    attachQueue(h, sp.fds[0]);
    std::vector<unsigned char> p1 = rtpPacket(1, 1, 10, 0x99);
    TASK_CHECK(a.iface.sendPacket(p1.data(), (unsigned)p1.size()));

    // Last interface leaves: descriptor destructor must cancel tasks and free every queue state.
    a.iface.removeStreamSocket(sp.fds[0], 24);
    b.iface.removeStreamSocket(sp.fds[0], 25);
    TASK_CHECK(!RTPInterface::queuedSendIsActive(*h.env, sp.fds[0]));
    TASK_CHECK(!RTPInterface::queuedSendHasPending(*h.env, sp.fds[0]));
    TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp.fds[0]) == 0);
    // Spin the scheduler: no stale timer/handler may fire on the doomed fd.
    for (int i = 0; i < 5; ++i) h.scheduler->stepOnce();
  }

  // A brand-new connection (possibly reusing the same fd number): no state from the old queue may
  // survive -- a fresh queue starts empty and only frames submitted to it are ever emitted.
  SocketPair sp2;
  TestInterface c(h);
  c.iface.addStreamSocket(sp2.fds[0], 26, NULL);
  TASK_CHECK(RTPInterface::enableQueuedSendMode(*h.env, sp2.fds[0], 1 << 20));
  attachQueue(h, sp2.fds[0]);
  TASK_CHECK(RTPInterface::queuedSendPendingBytes(*h.env, sp2.fds[0]) == 0);
  std::vector<unsigned char> p2 = rtpPacket(9, 9, 11, 0x66);
  TASK_CHECK(c.iface.sendPacket(p2.data(), (unsigned)p2.size()));
  TASK_CHECK(RTPInterface::pumpQueuedSend(*h.env, sp2.fds[0]));
  size_t pos = 0;
  TASK_CHECK(expectFrame(h.writer.emitted, pos, 26, p2));
  TASK_CHECK(pos == h.writer.emitted.size());
  for (int i = 0; i < 3; ++i) h.scheduler->stepOnce();
  TASK_CHECK(pos == h.writer.emitted.size()); // nothing stale appended
  // The old fd number carries no queue anymore, whether or not the kernel handed the number back.
  TASK_CHECK(!RTPInterface::queuedSendIsActive(*h.env, oldFd)
	     || oldFd == sp2.fds[0] || oldFd == sp2.fds[1]);
  return 0;
}

const struct task_case task_cases[] = {
  // framing
  {"framing", "two_channel_fifo_order",              framing_fifo_two_channels},
  {"framing", "short_writes_continue_at_offset",     framing_short_writes_continue_at_offset},
  {"framing", "eintr_consumes_nothing",              framing_eintr_consumes_nothing},
  {"framing", "oversize_rejected_no_truncated_header", framing_oversize_rejected_no_truncated_header},
  {"framing", "source_buffer_copied_and_owned",      framing_source_buffer_is_copied_and_owned},
  {"framing", "default_mode_still_sends_inline",     framing_default_mode_still_sends_inline},
  // deadlines
  {"deadlines", "basic_expiry_releases_capacity",    deadlines_basic_expiry_releases_capacity},
  {"deadlines", "rtp_wrap_and_negative_delta",       deadlines_rtp_wrap_and_negative_delta},
  {"deadlines", "same_timestamp_fragments_share_deadline", deadlines_same_timestamp_fragments_share_one_deadline},
  {"deadlines", "expired_packets_behind_blocked_head", deadlines_expired_packets_behind_blocked_head_release},
  {"deadlines", "partial_header_survives_deadline",  deadlines_partial_header_survives_its_own_deadline},
  {"deadlines", "rtcp_interface_has_no_deadline",    deadlines_rtcp_interface_has_no_deadline_shares_fifo},
  // lifecycle
  {"lifecycle", "byte_budget_enforced_and_released", lifecycle_byte_budget_enforced_and_released},
  {"lifecycle", "channel_removal_completes_started", lifecycle_channel_removal_completes_started_frame},
  {"lifecycle", "hard_error_kills_one_socket_only",  lifecycle_hard_error_kills_one_socket_only},
  {"lifecycle", "eagain_arms_scheduler_writable",    lifecycle_eagain_arms_scheduler_writable_callback},
  {"lifecycle", "alternative_byte_handler_exclusion", lifecycle_alternative_byte_handler_mutual_exclusion},
  {"lifecycle", "zero_budget_and_bad_rate_rejected", lifecycle_zero_budget_and_bad_clock_rate_rejected},
  {"lifecycle", "last_interface_leave_and_fd_reuse", lifecycle_last_interface_leave_and_fd_reuse},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
