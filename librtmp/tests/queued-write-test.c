/*
 * Offline tests for the queued, non-blocking RTMP write API.
 *
 * No remote peer is involved: a weak RTMP_SockSend() interposition provides
 * a controlled write adapter that scripts short writes, EINTR and repeated
 * EAGAIN while recording every wire byte.  A local socketpair feeds the
 * captured stream back through the existing RTMP_ReadPacket() parser.
 *
 * Each test returns the number of successful positive assertions, or a
 * non-positive value on failure.
 */
#include "rtmp.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <dlfcn.h>

/* ------------------------------------------------------------------ */
/* Deterministic allocation-failure injection.                        */
/* Only calloc() failures are triggered, and only while armed, so the */
/* snapshot allocation inside RTMP_QueuePacket() can be forced to fail*/
/* without touching unrelated allocations.                            */
/* ------------------------------------------------------------------ */
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);
static int calloc_fault_armed;
static int in_bootstrap;
static char boot_buf[512];
static size_t boot_used;

static void *
boot_alloc(size_t n)
{
  void *p;
  if (boot_used + n > sizeof(boot_buf))
    return NULL;
  p = boot_buf + boot_used;
  boot_used += (n + 15) & ~(size_t)15;
  return p;
}

static void
alloc_bootstrap(void)
{
  if (real_malloc || in_bootstrap)
    return;
  in_bootstrap = 1;
  real_malloc = dlsym(RTLD_NEXT, "malloc");
  real_calloc = dlsym(RTLD_NEXT, "calloc");
  real_realloc = dlsym(RTLD_NEXT, "realloc");
  real_free = dlsym(RTLD_NEXT, "free");
  in_bootstrap = 0;
}

void *
malloc(size_t n)
{
  if (!real_malloc)
    {
      if (in_bootstrap)
	return boot_alloc(n);
      alloc_bootstrap();
    }
  return real_malloc(n);
}

void *
calloc(size_t a, size_t b)
{
  if (!real_calloc)
    {
      if (in_bootstrap)
	return boot_alloc(a * b);
      alloc_bootstrap();
    }
  if (calloc_fault_armed)
    {
      calloc_fault_armed = 0;
      return NULL;
    }
  return real_calloc(a, b);
}

void *
realloc(void *p, size_t n)
{
  if (!real_realloc)
    {
      if (in_bootstrap)
	return boot_alloc(n);
      alloc_bootstrap();
    }
  return real_realloc(p, n);
}

void
free(void *p)
{
  if (!p)
    return;
  if (p >= (void *)boot_buf && p < (void *)(boot_buf + sizeof(boot_buf)))
    return;	/* bootstrap scratch storage */
  if (!real_free)
    return;
  real_free(p);
}

/* ------------------------------------------------------------------ */
/* Controlled write adapter (overrides the weak RTMP_SockSend seam).  */
/* ------------------------------------------------------------------ */
enum send_kind { SEND_ACCEPT, SEND_EINTR, SEND_EAGAIN, SEND_FAULT };

struct send_op
{
  enum send_kind kind;
  int n;		/* SEND_ACCEPT: maximum bytes accepted */
  int err;		/* SEND_FAULT: errno to report */
};

struct send_ctl
{
  const struct send_op *ops;
  int nops;
  int idx;
  int calls;		/* number of entries into the adapter */
  int armed;		/* 0: pass through to the real socket send */

  unsigned char *cap;	/* captured wire bytes */
  int caplen;
  int capcap;
} g_send;

static void
send_reset(const struct send_op *ops, int nops)
{
  real_free(g_send.cap);
  g_send.cap = NULL;
  g_send.caplen = 0;
  g_send.capcap = 0;
  g_send.ops = ops;
  g_send.nops = nops;
  g_send.idx = 0;
  g_send.calls = 0;
  g_send.armed = 1;
}

static int
send_capture(const char *buf, int n)
{
  if (g_send.caplen + n > g_send.capcap)
    {
      int nc = g_send.capcap ? g_send.capcap : 64;
      while (nc < g_send.caplen + n)
	nc *= 2;
      unsigned char *p = real_realloc(g_send.cap, nc);
      if (!p)
	return -1;
      g_send.cap = p;
      g_send.capcap = nc;
    }
  memcpy(g_send.cap + g_send.caplen, buf, n);
  g_send.caplen += n;
  return 0;
}

/* Strong override of the librtmp weak seam. */
int
RTMP_SockSend(RTMPSockBuf *sb, const char *buf, int len)
{
  /* Until a test arms the adapter, behave exactly like the default seam so
   * the synchronous normal_rtmp() comparison still hits the real socket. */
  if (!g_send.armed)
    return RTMPSockBuf_Send(sb, buf, len);

  g_send.calls++;

  if (g_send.idx < g_send.nops)
    {
      struct send_op op = g_send.ops[g_send.idx++];
      switch (op.kind)
	{
	case SEND_ACCEPT:
	  {
	    int n = op.n < len ? op.n : len;
	    if (n <= 0)
	      n = len;
	    if (send_capture(buf, n))
	      return -1;
	    return n;
	  }
	case SEND_EINTR:
	  errno = EINTR;
	  return -1;
	case SEND_EAGAIN:
	  errno = EAGAIN;
	  return -1;
	case SEND_FAULT:
	  errno = op.err ? op.err : EPIPE;
	  return -1;
	}
    }
  /* script exhausted: accept everything */
  if (send_capture(buf, len))
    return -1;
  return len;
}

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                    */
/* ------------------------------------------------------------------ */
static int g_asserts;
static int g_failed;

#define CHECK(cond) do { \
  if (cond) { g_asserts++; } \
  else { g_failed = 1; printf("  CHECK failed at %s:%d: %s\n", \
			      __FILE__, __LINE__, #cond); } \
} while (0)

static void
make_packet(RTMPPacket *p, int channel, int htype, int ptype,
	    uint32_t ts, int32_t stream, uint32_t bodylen, unsigned char seed)
{
  RTMPPacket_Reset(p);
  if (!RTMPPacket_Alloc(p, bodylen))
    {
      p->m_body = NULL;
      return;
    }
  p->m_nChannel = channel;
  p->m_headerType = (uint8_t)htype;
  p->m_packetType = (uint8_t)ptype;
  p->m_nTimeStamp = ts;
  p->m_nInfoField2 = stream;
  p->m_nBodySize = bodylen;
  for (uint32_t i = 0; i < bodylen; i++)
    p->m_body[i] = (char)(unsigned char)(seed + (unsigned char)i);
}

/* Independent, hand-written RTMP chunk encoder used only as a test oracle;
 * it shares no code with the library encoder under test. */
static int
oracle_encode(const RTMPPacket *p, int outChunkSize,
	      unsigned char *out, int outcap)
{
  int pos = 0;
  int cSize = p->m_nChannel > 319 ? 2 : (p->m_nChannel > 63 ? 1 : 0);
  uint32_t last = 0;	/* oracle is only asked for the first packet on a
			   channel in these tests */
  uint32_t t = p->m_nTimeStamp - last;
  int ext = t >= 0xffffff;

  /* first header (always type 0 for the packets handed to the oracle);
   * an extended channel id leaves the low six bits at 0 (cSize 1) or 1 (2) */
  #define PUT(b) do { if (pos >= outcap) return -1; out[pos++] = (unsigned char)(b); } while (0)
  PUT((p->m_headerType << 6)
      | (cSize == 2 ? 1 : (cSize == 1 ? 0 : p->m_nChannel)));
  if (cSize)
    {
      int tmp = p->m_nChannel - 64;
      PUT(tmp & 0xff);
      if (cSize == 2)
	PUT(tmp >> 8);
    }
  {
    uint32_t tv = ext ? 0xffffff : t;
    PUT((tv >> 16) & 0xff);
    PUT((tv >> 8) & 0xff);
    PUT(tv & 0xff);
  }
  PUT((p->m_nBodySize >> 16) & 0xff);
  PUT((p->m_nBodySize >> 8) & 0xff);
  PUT(p->m_nBodySize & 0xff);
  PUT(p->m_packetType);
  PUT(p->m_nInfoField2 & 0xff);
  PUT((p->m_nInfoField2 >> 8) & 0xff);
  PUT((p->m_nInfoField2 >> 16) & 0xff);
  PUT((p->m_nInfoField2 >> 24) & 0xff);
  if (ext)
    {
      PUT((t >> 24) & 0xff);
      PUT((t >> 16) & 0xff);
      PUT((t >> 8) & 0xff);
      PUT(t & 0xff);
    }
  #undef PUT

  {
    uint32_t rem = p->m_nBodySize;
    uint32_t off = 0;
    while (rem > 0)
      {
	int n = rem < (uint32_t)outChunkSize ? (int)rem : outChunkSize;
	for (int i = 0; i < n; i++)
	  {
	    if (pos >= outcap)
	      return -1;
	    out[pos++] = (unsigned char)p->m_body[off++];
	  }
	rem -= n;
	if (rem > 0)
	  {
	    if (pos >= outcap)
	      return -1;
	    out[pos++] = (unsigned char)(0xc0
		| (cSize == 2 ? 1 : (cSize == 1 ? 0 : p->m_nChannel)));
	    if (cSize)
	      {
		int tmp = p->m_nChannel - 64;
		out[pos++] = tmp & 0xff;
		if (cSize == 2)
		  out[pos++] = tmp >> 8;
	      }
	    if (ext)
	      {
		out[pos++] = (t >> 24) & 0xff;
		out[pos++] = (t >> 16) & 0xff;
		out[pos++] = (t >> 8) & 0xff;
		out[pos++] = t & 0xff;
	      }
	  }
      }
  }
  return pos;
}

/* Reassemble one complete message through the real RTMP_ReadPacket().
 * A freshly reset packet reports "ready" (0 of 0 bytes), so the first read
 * must happen unconditionally.  Zero the whole packet (not just Reset) so
 * the optional m_chunk raw-chunk pointer is NULL. */
static int
read_full_message(RTMP *rd, RTMPPacket *rp)
{
  int guard = 0;
  memset(rp, 0, sizeof(*rp));
  RTMPPacket_Reset(rp);
  do
    {
      if (!RTMP_ReadPacket(rd, rp))
	return -1;
      if (++guard > 1000)
	return -2;
    }
  while (!RTMPPacket_IsReady(rp));
  return 0;
}

/* Drain whatever is queued using the scripted adapter; returns the number of
 * RTMP_WriteQueue() invocations made, or -1 on an unexpected result. */
static int
drain_queue(RTMP *r, int expect_again)
{
  int pushes = 0;
  int again_seen = 0;
  for (;;)
    {
      int rc = RTMP_WriteQueue(r);
      pushes++;
      if (rc == RTMP_WRITE_AGAIN)
	{
	  again_seen = 1;
	  if (pushes > 100000)
	    return -1;	/* would be a busy loop / stuck */
	  continue;
	}
      if (rc == RTMP_WRITE_DONE)
	break;
      return -1;	/* RTMP_WRITE_ERROR was not part of the plan */
    }
  if (expect_again && !again_seen)
    return -1;
  return pushes;
}

/* ------------------------------------------------------------------ */
/* test_rtmp_write_resume: short writes, EINTR, repeated EAGAIN,       */
/* resumption at arbitrary offsets, byte-exact capture.                */
/* ------------------------------------------------------------------ */
int
test_rtmp_write_resume(void)
{
  int f[2];
  RTMP r;
  RTMPPacket p;
  unsigned char expected[256];
  int explen;
  int pushes;
  int cap_before_again;

  g_asserts = 0;
  g_failed = 0;

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);

  RTMP_Init(&r);
  r.m_sb.sb_socket = f[0];
  r.m_outChunkSize = 4;

  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      100, 1, 10, 0xa0);
  CHECK(p.m_body != NULL);

  explen = oracle_encode(&p, r.m_outChunkSize, expected, sizeof(expected));
  CHECK(explen == 24);	/* 12 header + 4 + 1 + 4 + 1 + 2 */
  /* hand-written header expectations */
  CHECK(expected[0] == 0x04);
  CHECK(expected[1] == 0x00 && expected[2] == 0x00 && expected[3] == 0x64);
  CHECK(expected[4] == 0x00 && expected[5] == 0x00 && expected[6] == 0x0a);
  CHECK(expected[7] == 0x08);
  CHECK(expected[8] == 0x01);
  CHECK(expected[16] == 0xc4);	/* fmt-3 continuation header */
  CHECK(expected[21] == 0xc4);

  /* fragment awkwardly: inside the basic header, across a continuation,
   * with EINTR retries and two back-to-back EAGAIN stalls */
  static const struct send_op ops[] = {
    { SEND_ACCEPT, 1, 0 },	/* stop after the basic header byte */
    { SEND_ACCEPT, 2, 0 },
    { SEND_EINTR, 0, 0 },
    { SEND_ACCEPT, 3, 0 },
    { SEND_EAGAIN, 0, 0 },
    { SEND_EAGAIN, 0, 0 },
    { SEND_ACCEPT, 5, 0 },
    { SEND_EINTR, 0, 0 },
    { SEND_ACCEPT, 1, 0 },
    { SEND_ACCEPT, 7, 0 },	/* cross a media fragment boundary */
    { SEND_EAGAIN, 0, 0 },
  };
  send_reset(ops, (int)(sizeof(ops) / sizeof(ops[0])));

  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  CHECK(RTMP_HasQueuedData(&r) == 1);

  /* drive until the first EAGAIN is observed, then prove it consumed nothing */
  cap_before_again = -1;
  for (int i = 0; i < 6; i++)
    {
      int rc = RTMP_WriteQueue(&r);
      if (rc == RTMP_WRITE_AGAIN)
	{
	  cap_before_again = g_send.caplen;
	  CHECK(RTMP_HasQueuedData(&r) == 1);
	  CHECK(RTMP_IsConnected(&r) == 1);
	  break;
	}
    }
  CHECK(cap_before_again >= 0);
  /* another immediate EAGAIN must not consume or close */
  {
    int rc = RTMP_WriteQueue(&r);
    CHECK(rc == RTMP_WRITE_AGAIN);
    CHECK(g_send.caplen == cap_before_again);
    CHECK(RTMP_IsConnected(&r) == 1);
  }

  pushes = drain_queue(&r, 1);
  CHECK(pushes > 0);
  CHECK(RTMP_HasQueuedData(&r) == 0);
  CHECK(RTMP_WriteQueue(&r) == RTMP_WRITE_DONE);	/* empty queue */

  /* every byte captured exactly once, in order */
  CHECK(g_send.caplen == explen);
  CHECK(memcmp(g_send.cap, expected, explen) == 0);
  /* the adapter was entered many times (no single big write hid the seams) */
  CHECK(g_send.calls >= 10);

  RTMPPacket_Free(&p);

  /* wide channel ids: one-byte (64..319) and two-byte (>=320) basic headers
   * must match the independent oracle byte-for-byte through the queued API */
  {
    RTMPPacket w1, w2;
    unsigned char e1[256], e2[256];
    int l1, l2;

    make_packet(&w1, 64, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
		50, 1, 7, 0x60);
    CHECK(w1.m_body != NULL);
    make_packet(&w2, 320, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_VIDEO,
		1000, 1, 10, 0x70);
    CHECK(w2.m_body != NULL);
    l1 = oracle_encode(&w1, r.m_outChunkSize, e1, sizeof(e1));
    l2 = oracle_encode(&w2, r.m_outChunkSize, e2, sizeof(e2));
    CHECK(l1 > 0 && l2 > 0);
    /* hand-written basic-header expectations for the extended ids */
    CHECK(e1[0] == 0x00);		/* fmt0, marker 0 */
    CHECK(e1[1] == (64 - 64));
    CHECK(e2[0] == 0x01);		/* fmt0, marker 1 */
    CHECK(e2[1] == ((320 - 64) & 0xff));
    CHECK(e2[2] == ((320 - 64) >> 8));

    send_reset(NULL, 0);
    CHECK(RTMP_QueuePacket(&r, &w1) == RTMP_QUEUE_OK);
    CHECK(RTMP_QueuePacket(&r, &w2) == RTMP_QUEUE_OK);
    CHECK(drain_queue(&r, 0) > 0);
    CHECK(g_send.caplen == l1 + l2);
    CHECK(memcmp(g_send.cap, e1, l1) == 0);
    CHECK(memcmp(g_send.cap + l1, e2, l2) == 0);

    RTMPPacket_Free(&w1);
    RTMPPacket_Free(&w2);
  }

  RTMP_Close(&r);
  close(f[1]);
  return g_failed ? -1 : g_asserts;
}

/* ------------------------------------------------------------------ */
/* test_rtmp_write_headers: two channels, compressed history, extended */
/* timestamps, fmt-3 continuation, small chunks, RTMP_ReadPacket parse */
/* plus hand-written expectations.                                     */
/* ------------------------------------------------------------------ */
int
test_rtmp_write_headers(void)
{
  int f[2];
  RTMP w, rd;
  RTMPPacket a1, v1, a2;
  int pushes;
  int a1_len, v1_len, a2_len;
  unsigned char expect_a1[64];
  int off;

  g_asserts = 0;
  g_failed = 0;

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);

  RTMP_Init(&w);
  w.m_sb.sb_socket = f[0];
  w.m_outChunkSize = 3;

  /* channel 4 audio, timestamp over 24 bits -> extended timestamp */
  make_packet(&a1, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      0x1000000, 1, 6, 0x10);
  CHECK(a1.m_body != NULL);
  /* channel 6 video, independent history, also extended */
  make_packet(&v1, 6, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_VIDEO,
	      0x1000010, 1, 4, 0x30);
  CHECK(v1.m_body != NULL);
  /* second channel-4 packet, requests MEDIUM; must compress against a1,
   * not against the interleaved v1. Different ts -> stays fmt 2 (small) */
  make_packet(&a2, 4, RTMP_PACKET_SIZE_MEDIUM, RTMP_PACKET_TYPE_AUDIO,
	      0x1000020, 1, 6, 0x50);
  CHECK(a2.m_body != NULL);

  a1_len = oracle_encode(&a1, w.m_outChunkSize, expect_a1, sizeof(expect_a1));
  CHECK(a1_len > 0);

  /* heavy fragmentation so sends land inside headers, extended timestamps
   * and fragment interiors */
  static const struct send_op ops[] = {
    { SEND_ACCEPT, 1, 0 },
    { SEND_EINTR, 0, 0 },
    { SEND_ACCEPT, 2, 0 },
    { SEND_EAGAIN, 0, 0 },
    { SEND_ACCEPT, 3, 0 },
    { SEND_ACCEPT, 1, 0 },
    { SEND_EINTR, 0, 0 },
    { SEND_ACCEPT, 4, 0 },
    { SEND_EAGAIN, 0, 0 },
    { SEND_ACCEPT, 2, 0 },
  };
  send_reset(ops, (int)(sizeof(ops) / sizeof(ops[0])));

  CHECK(RTMP_QueuePacket(&w, &a1) == RTMP_QUEUE_OK);
  CHECK(RTMP_QueuePacket(&w, &v1) == RTMP_QUEUE_OK);
  CHECK(RTMP_QueuePacket(&w, &a2) == RTMP_QUEUE_OK);
  CHECK(RTMP_HasQueuedData(&w) == 1);

  pushes = drain_queue(&w, 1);
  CHECK(pushes > 0);
  CHECK(RTMP_HasQueuedData(&w) == 0);

  /* ---- hand-written expectations over the captured stream ---- */
  /* a1: type 0 on csid 4, 24-bit field 0xffffff + extended 0x01000000 */
  CHECK(g_send.cap[0] == 0x04);
  CHECK(g_send.cap[1] == 0xff && g_send.cap[2] == 0xff
	&& g_send.cap[3] == 0xff);
  CHECK(g_send.cap[7] == RTMP_PACKET_TYPE_AUDIO);
  CHECK(g_send.cap[12] == 0x01 && g_send.cap[13] == 0x00
	&& g_send.cap[14] == 0x00 && g_send.cap[15] == 0x00);
  CHECK(memcmp(g_send.cap, expect_a1, a1_len) == 0);
  /* fmt-3 continuation of a1 carries the extended timestamp too */
  /* layout: 16-byte header, 3 body, then 0xc4 + 4 ext bytes */
  CHECK(g_send.cap[19] == 0xc4);
  CHECK(g_send.cap[20] == 0x01 && g_send.cap[23] == 0x00);

  /* v1 starts right after a1; independent channel, its own type-0 header */
  v1_len = 16 + 3 + 5 + 1;	/* header + chunk + (0xc6+4 ext) + chunk */
  off = a1_len;
  CHECK(g_send.cap[off] == 0x06);
  CHECK(g_send.cap[off + 7] == RTMP_PACKET_TYPE_VIDEO);
  CHECK(g_send.cap[off + 12] == 0x01 && g_send.cap[off + 13] == 0x00
	&& g_send.cap[off + 14] == 0x00 && g_send.cap[off + 15] == 0x10);

  /* a2 compressed against a1 (same channel): equal size/type upgrades the
   * requested MEDIUM to SMALL (fmt 2) -> basic 0x84, 3-byte relative delta
   * 0x000020, no extended timestamp (delta fits in 24 bits) */
  a2_len = 4 + 3 + 1 + 3;	/* small header + chunk + 0xc4 + chunk */
  off = a1_len + v1_len;
  CHECK(g_send.cap[off] == 0x84);
  CHECK(g_send.cap[off + 1] == 0x00 && g_send.cap[off + 2] == 0x00
	&& g_send.cap[off + 3] == 0x20);
  CHECK(g_send.caplen == a1_len + v1_len + a2_len);

  /* ---- round-trip through the unmodified RTMP_ReadPacket() ---- */
  RTMP_Init(&rd);
  rd.m_sb.sb_socket = f[1];
  rd.m_inChunkSize = w.m_outChunkSize;
  {
    int woff = 0;
    while (woff < g_send.caplen)
      {
	int n = (int)send(f[0], g_send.cap + woff, g_send.caplen - woff, 0);
	CHECK(n > 0);
	if (n <= 0)
	  break;
	woff += n;
      }
  }

  /* message 1: a1 */
  {
    RTMPPacket rp;
    CHECK(read_full_message(&rd, &rp) == 0);
    CHECK(rp.m_nChannel == 4);
    CHECK(rp.m_packetType == RTMP_PACKET_TYPE_AUDIO);
    CHECK(rp.m_nTimeStamp == 0x1000000);	/* extended survived */
    CHECK(rp.m_nBodySize == 6);
    CHECK(memcmp(rp.m_body, a1.m_body, 6) == 0);
    RTMPPacket_Free(&rp);
  }
  /* message 2: v1 */
  {
    RTMPPacket rp;
    CHECK(read_full_message(&rd, &rp) == 0);
    CHECK(rp.m_nChannel == 6);
    CHECK(rp.m_packetType == RTMP_PACKET_TYPE_VIDEO);
    CHECK(rp.m_nTimeStamp == 0x1000010);
    CHECK(rp.m_nBodySize == 4);
    CHECK(memcmp(rp.m_body, v1.m_body, 4) == 0);
    RTMPPacket_Free(&rp);
  }
  /* message 3: a2, absolute ts reconstructed from relative delta */
  {
    RTMPPacket rp;
    CHECK(read_full_message(&rd, &rp) == 0);
    CHECK(rp.m_nChannel == 4);
    CHECK(rp.m_packetType == RTMP_PACKET_TYPE_AUDIO);
    CHECK(rp.m_nTimeStamp == 0x1000020);
    CHECK(rp.m_nBodySize == 6);
    CHECK(memcmp(rp.m_body, a2.m_body, 6) == 0);
    RTMPPacket_Free(&rp);
  }

  RTMPPacket_Free(&a1);
  RTMPPacket_Free(&v1);
  RTMPPacket_Free(&a2);
  RTMP_Close(&w);
  close(rd.m_sb.sb_socket);
  close(f[0]);
  return g_failed ? -1 : g_asserts;
}

/* ------------------------------------------------------------------ */
/* test_rtmp_write_lifecycle: snapshot ownership, close/discard/reinit,*/
/* type and argument rejection, allocation failure, fatal send error.  */
/* ------------------------------------------------------------------ */
int
test_rtmp_write_lifecycle(void)
{
  int f[2];
  RTMP r;
  RTMPPacket p;
  unsigned char expected[256];
  int explen;

  g_asserts = 0;
  g_failed = 0;

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);
  RTMP_Init(&r);
  r.m_sb.sb_socket = f[0];
  r.m_outChunkSize = 4;
  RTMPPacket_Reset(&p);
  p.m_body = NULL;

  /* ---- invalid arguments and unsupported types ---- */
  CHECK(RTMP_QueuePacket(NULL, &p) == RTMP_QUEUE_ERROR);
  CHECK(RTMP_QueuePacket(&r, NULL) == RTMP_QUEUE_ERROR);
  CHECK(RTMP_HasQueuedData(&r) == 0);

  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_INVOKE,
	      10, 1, 4, 0x00);
  CHECK(p.m_body != NULL);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);	/* invoke rejected */
  p.m_packetType = RTMP_PACKET_TYPE_CONTROL;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);	/* control rejected */
  p.m_packetType = RTMP_PACKET_TYPE_AUDIO;
  p.m_nChannel = RTMP_CHANNELS;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);	/* bad channel */
  p.m_nChannel = 4;
  RTMPPacket_Free(&p);
  p.m_body = NULL;
  p.m_nBodySize = 4;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);	/* body missing */
  CHECK(RTMP_HasQueuedData(&r) == 0);			/* nothing half-queued */

  /* ---- snapshot independence: clobber and free the source immediately */
  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      7, 1, 6, 0x70);
  CHECK(p.m_body != NULL);
  {
    /* remember the caller's original header type and body bytes */
    uint8_t orig_type = p.m_headerType;
    unsigned char orig_body[6];
    memcpy(orig_body, p.m_body, 6);
    explen = oracle_encode(&p, r.m_outChunkSize, expected, sizeof(expected));
    CHECK(explen > 0);
    send_reset(NULL, 0);
    CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
    /* the source packet's header type and body must be untouched */
    CHECK(p.m_headerType == orig_type);
    CHECK(memcmp(p.m_body, orig_body, 6) == 0);
    /* now mutate and release the caller's storage */
    p.m_headerType = RTMP_PACKET_SIZE_MINIMUM;
    memset(p.m_body, 0xee, p.m_nBodySize);
    RTMPPacket_Free(&p);
  }
  CHECK(RTMP_HasQueuedData(&r) == 1);
  CHECK(RTMP_WriteQueue(&r) == RTMP_WRITE_DONE);
  CHECK(g_send.caplen == explen);
  CHECK(memcmp(g_send.cap, expected, explen) == 0);	/* wire unchanged */
  CHECK(RTMP_HasQueuedData(&r) == 0);

  /* ---- allocation failure leaves no half packet ---- */
  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_VIDEO,
	      9, 1, 5, 0x90);
  CHECK(p.m_body != NULL);
  send_reset(NULL, 0);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);	/* established item */
  calloc_fault_armed = 1;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  CHECK(calloc_fault_armed == 0);			/* fault was consumed */
  CHECK(RTMP_HasQueuedData(&r) == 1);			/* only first item */
  CHECK(RTMP_WriteQueue(&r) == RTMP_WRITE_DONE);
  {
    unsigned char expect2[256];
    int len2 = oracle_encode(&p, r.m_outChunkSize, expect2, sizeof(expect2));
    CHECK(len2 > 0);
    CHECK(g_send.caplen == len2);
    CHECK(memcmp(g_send.cap, expect2, len2) == 0);	/* first item intact */
  }
  RTMPPacket_Free(&p);

  /* ---- fatal send error: WRITE_ERROR and normal connection teardown ---- */
  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      11, 1, 3, 0x40);
  CHECK(p.m_body != NULL);
  {
    static const struct send_op ops[] = { { SEND_FAULT, 0, EPIPE } };
    send_reset(ops, 1);
  }
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  CHECK(RTMP_WriteQueue(&r) == RTMP_WRITE_ERROR);
  CHECK(RTMP_IsConnected(&r) == 0);
  CHECK(RTMP_HasQueuedData(&r) == 0);			/* snapshots discarded */
  RTMPPacket_Free(&p);
  close(f[1]);
  close(f[0]);

  /* ---- close with unsent data, then re-init must carry no old bytes ---- */
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);
  RTMP_Init(&r);
  r.m_sb.sb_socket = f[0];
  r.m_outChunkSize = 4;
  make_packet(&p, 5, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_VIDEO,
	     12, 1, 4, 0x60);
  CHECK(p.m_body != NULL);
  send_reset(NULL, 0);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  RTMP_Close(&r);					/* never drained */
  CHECK(RTMP_HasQueuedData(&r) == 0);

  RTMP_Init(&r);
  r.m_sb.sb_socket = f[0];
  r.m_outChunkSize = 4;
  {
    RTMPPacket q;
    unsigned char expect3[256];
    int len3;
    make_packet(&q, 7, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_INFO,
		13, 1, 5, 0x20);
    CHECK(q.m_body != NULL);
    len3 = oracle_encode(&q, r.m_outChunkSize, expect3, sizeof(expect3));
    CHECK(len3 > 0);
    CHECK(RTMP_QueuePacket(&r, &q) == RTMP_QUEUE_OK);
    CHECK(RTMP_WriteQueue(&r) == RTMP_WRITE_DONE);
    CHECK(g_send.caplen == len3);			/* no stale bytes */
    CHECK(memcmp(g_send.cap, expect3, len3) == 0);
    RTMPPacket_Free(&q);
  }

  RTMPPacket_Free(&p);
  RTMP_Close(&r);
  close(f[1]);
  return g_failed ? -1 : g_asserts;
}
