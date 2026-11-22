// task_tests/queued_send_example.cpp
//
// Minimal, COMPILABLE example of RTPInterface's deadline-aware queued send mode. It wires two
// "media" interfaces (RTP + RTCP) onto one plaintext TCP connection, enables the shared per-socket
// FIFO with a byte budget, configures a media clock, and lets the existing TaskScheduler drain the
// queue through the socket's writable callback -- no polling, no blocking send.
//
// Build (same include set as the gate):
//   c++ -std=c++20 -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t \
//       -I. -IliveMedia/include -Igroupsock/include \
//       -IBasicUsageEnvironment/include -IUsageEnvironment/include \
//       task_tests/queued_send_example.cpp -Wl,-all_load .task-build/production.a -o /tmp/qsend-example
//
// Applicability (the application owns this contract): a plaintext TCP connection carrying RTP/RTCP
// only. Queued mode is refused on TLS sockets and on connections with an RTSP alternative-byte
// handler; it is never used for RTSP control responses or SRTP.

#include "RTPInterface.hh"
#include "BasicUsageEnvironment.hh"
#include "Groupsock.hh"
#include "Media.hh"

#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {

class Owner : public Medium {
public:
  explicit Owner(UsageEnvironment& e) : Medium(e) {}
  virtual ~Owner() {}
};

// Real CLOCK_MONOTONIC microseconds -- the same timeline the queue's default clock uses.
long long monotonicNowUs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

void stopEventLoop(void* clientData) { *static_cast<EventLoopWatchVariable*>(clientData) = 1; }

// A V2 RTP packet whose RTP timestamp (bytes 4..7) we can set explicitly.
std::vector<unsigned char> makeRtp(u_int32_t timestamp, unsigned payloadLen) {
  std::vector<unsigned char> p(12 + payloadLen, 0xAA);
  p[0] = 0x80;
  p[1] = 96;
  p[4] = (unsigned char)(timestamp >> 24);
  p[5] = (unsigned char)(timestamp >> 16);
  p[6] = (unsigned char)(timestamp >> 8);
  p[7] = (unsigned char)(timestamp & 0xFF);
  return p;
}

} // namespace

int main() {
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 1;

  bool drained = false;
  unsigned long long pendingAtEnd = 0;
  {
    // All objects that reference "env"/"scheduler" live in this inner scope so they are destroyed
    // BEFORE the environment and scheduler below (the queue's timers/handlers need the scheduler).
    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    addr.ss_family = AF_INET;

    Owner rtpOwner(*env), rtcpOwner(*env);
    Groupsock rtpGS(*env, addr, Port(0), 1);
    Groupsock rtcpGS(*env, addr, Port(0), 1);
    rtpGS.removeAllDestinations();
    rtcpGS.removeAllDestinations();

    RTPInterface rtpIface(&rtpOwner, &rtpGS);
    RTPInterface rtcpIface(&rtcpOwner, &rtcpGS);

    // 1) Interleave the two channels onto the SAME socket (RFC 2326 sec 10.12 channels).
    rtpIface.addStreamSocket(pair[0], 0, NULL);   // RTP on channel 0
    rtcpIface.addStreamSocket(pair[0], 1, NULL);  // RTCP on channel 1

    // 2) Explicitly opt in, with a non-zero byte budget counted against bytes not yet handed to
    //    the underlying writer. Over-budget packets fail sendPacket(); earlier fresh packets are kept.
    static unsigned long long const QUEUE_BYTE_LIMIT = 256ULL * 1024;
    if (!RTPInterface::enableQueuedSendMode(*env, pair[0], QUEUE_BYTE_LIMIT)) {
      std::fprintf(stderr, "queued send mode refused (TLS socket or alternative-byte handler?)\n");
      close(pair[0]); close(pair[1]);
      env->reclaim(); delete scheduler;
      return 2;
    }

    // 3) Media clock, BEFORE the first packet. Clock units are microseconds; clockRate is Hz (> 0).
    //    deadline = localBaseUs + playBufferUs
    //             + floor(int32(rtpTs - baseRtpTs) * 1000000 / clockRate)
    //    localBaseUs is on the real monotonic timeline; the RTCP interface is intentionally left
    //    unconfigured: its frames have no deadline but still share the same FIFO.
    rtpIface.setPlaybackTiming(90000 /*Hz*/, 0 /*base RTP ts*/,
			       monotonicNowUs() /*local base us*/, 200000 /*playback buffer us*/);

    // 4) Submit frames. Success means "copied and taken over": the buffer can be reused immediately.
    //    Three small frames fit the local socketpair buffer, so the writable callback drains them.
    for (u_int32_t ts = 0; ts < 27000; ts += 9000) { // 90 kHz media, 100 ms apart
      std::vector<unsigned char> packet = makeRtp(ts, 100);
      if (!rtpIface.sendPacket(packet.data(), (unsigned)packet.size())) {
	// Rejected: over budget, already expired, or the queue terminated on a hard socket error.
	std::fprintf(stderr, "frame ts=%u rejected (back pressure or expired)\n", ts);
      }
    }
    unsigned char rtcpCompound[8] = {0x80, 0xC9, 0x00, 0x01, 0, 0, 0, 1};
    rtcpIface.sendPacket(rtcpCompound, sizeof rtcpCompound); // no deadline; queued if it fits

    // 5) Run the REAL event loop: the queue drains through the socket's writable background
    //    handler, and the deadline timer drops any frame that reaches its play time before its
    //    first byte is sent. A delayed task stops the loop after 200 ms (an app normally runs
    //    doEventLoop() for its whole lifetime). No blocking send / polling sleep is used.
    EventLoopWatchVariable stopLoop = 0;
    scheduler->scheduleDelayedTask(200000, stopEventLoop, &stopLoop);
    scheduler->doEventLoop(&stopLoop);

    drained = !RTPInterface::queuedSendHasPending(*env, pair[0]);
    pendingAtEnd = RTPInterface::queuedSendPendingBytes(*env, pair[0]);

    // Last interface leaves: the SocketDescriptor destructor cancels the deadline task/writable
    // callback and frees every queued frame, so nothing can fire on the fd after it is reused.
    rtcpIface.removeStreamSocket(pair[0], 1);
    rtpIface.removeStreamSocket(pair[0], 0);
  }

  close(pair[0]);
  close(pair[1]);
  env->reclaim();
  delete scheduler;

  std::printf("queued-send example: queue %s, pending bytes=%llu\n",
	      drained ? "drained" : "still back-pressured", pendingAtEnd);
  return drained ? 0 : 3;
}
