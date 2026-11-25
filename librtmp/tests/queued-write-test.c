/*
 *      Copyright (C) 2026
 *
 *  This file is part of librtmp.
 *
 *  librtmp is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation; either version 2.1,
 *  or (at your option) any later version.
 *
 *  librtmp is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 * Queued, non-blocking write tests for RTMP_QueuePacket(),
 * RTMP_WriteQueue() and RTMP_HasQueuedPackets().
 *
 * A scripted RTMPDebugSend adapter replays short writes, EINTR and
 * repeated EAGAIN on top of a local socketpair, records every push and
 * captures the exact wire bytes without sleeping.  Captured bytes are
 * both compared against hand-written chunk expectations and replayed
 * through the stock RTMP_ReadPacket() parser.
 */

#include "rtmp.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(cond)	do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return -(asserts + 1); \
	} \
	asserts++; \
} while (0)

/* ---- growable byte buffer -------------------------------------- */

typedef struct WireBuf
{
  unsigned char *data;
  size_t len;
  size_t cap;
} WireBuf;

static int
wire_append(WireBuf *w, const void *p, size_t n)
{
  if (n > w->cap - w->len)
    {
      size_t ncap = w->cap ? w->cap : 128;
      unsigned char *ndata;
      while (ncap < w->len + n)
	ncap *= 2;
      ndata = realloc(w->data, ncap);
      if (!ndata)
	return 0;
      w->data = ndata;
      w->cap = ncap;
    }
  memcpy(w->data + w->len, p, n);
  w->len += n;
  return 1;
}

/* ---- scripted send adapter ------------------------------------- */

#define CAP_EINTR	1
#define CAP_EAGAIN	2
#define CAP_SHORT	3
#define CAP_ERROR	4

typedef struct SendCap
{
  int kind;
  int count;		/* how many send() calls this cap lasts */
  int short_len;	/* bytes accepted per CAP_SHORT call */
  int err;		/* errno to stage, 0 means the default */
} SendCap;

typedef struct SendScript
{
  SendCap *caps;
  int ncaps;
  int step;		/* current cap index */
  int incap;		/* calls already spent in the current cap */
  int calls;
  int pushes;		/* RTMP_WriteQueue() entries observed */
  int int_count;
  int again_count;
  int short_count;
  int error_count;
  int socket_fd;	/* real socket mirroring accepted bytes */
  WireBuf captured;	/* every accepted wire byte, in order */
} SendScript;

static SendScript g_script;

static int
scripted_send(RTMPSockBuf *sb, const char *buf, int len)
{
  SendScript *s = &g_script;
  SendCap *cap;
  (void)sb;
  s->calls++;

  for (;;)
    {
      if (s->step >= s->ncaps)
	{
	  /* script exhausted: behave like the real socket, still
	   * recording the exact bytes that leave the library */
	  int n = send(s->socket_fd, buf, len, 0);
	  if (n > 0 && !wire_append(&s->captured, buf, (size_t)n))
	    {
	      errno = ENOMEM;
	      return -1;
	    }
	  return n;
	}
      cap = &s->caps[s->step];
      if (cap->count >= 0 && s->incap >= cap->count)
	{
	  s->step++;
	  s->incap = 0;
	  continue;
	}
      s->incap++;
      switch (cap->kind)
	{
	case CAP_EINTR:
	  s->int_count++;
	  errno = cap->err ? cap->err : EINTR;
	  return -1;
	case CAP_EAGAIN:
	  s->again_count++;
	  errno = cap->err ? cap->err : EAGAIN;
	  return -1;
	case CAP_ERROR:
	  s->error_count++;
	  errno = cap->err ? cap->err : EPIPE;
	  return -1;
	case CAP_SHORT:
	  {
	    int n = cap->short_len > 0 ? cap->short_len : 1;
	    if (n > len)
	      n = len;
	    s->short_count++;
	    /* mirror the accepted fragment so the peer can parse it */
	    if (send(s->socket_fd, buf, n, 0) != n
		|| !wire_append(&s->captured, buf, (size_t)n))
	      {
		errno = EIO;
		return -1;
	      }
	    return n;
	  }
	default:
	  errno = EIO;
	  return -1;
	}
    }
}

static void
script_reset(SendCap *caps, int ncaps, int socket_fd)
{
  free(g_script.captured.data);
  memset(&g_script, 0, sizeof(g_script));
  g_script.caps = caps;
  g_script.ncaps = ncaps;
  g_script.socket_fd = socket_fd;
  RTMP_SetDebugSend(scripted_send);
}

static void
script_done(void)
{
  free(g_script.captured.data);
  memset(&g_script, 0, sizeof(g_script));
  RTMP_SetDebugSend(NULL);
}

/* Drive RTMP_WriteQueue() until DRAINED, counting pushes.  EAGAIN is
 * expected between pushes; anything else fails the test. */
static int
drain_queue(RTMP *r, int *pushes)
{
  int res;
  *pushes = 0;
  do
    {
      g_script.pushes++;
      res = RTMP_WriteQueue(r);
      (*pushes)++;
      if (res != RTMP_QUEUE_WOULDBLOCK && res != RTMP_QUEUE_DRAINED)
	return res;
      if (*pushes > 1000)
	return 99;
    }
  while (res == RTMP_QUEUE_WOULDBLOCK);
  return res;
}

static void
fill_body(RTMPPacket *p, uint8_t first)
{
  uint32_t i;
  for (i = 0; i < p->m_nBodySize; i++)
    p->m_body[i] = (char)(first + i);
}

static void
make_packet(RTMPPacket *p, int channel, int htype, int ptype,
	    uint32_t ts, int32_t info, uint32_t body_size, uint8_t first)
{
  RTMPPacket_Reset(p);
  if (!RTMPPacket_Alloc(p, body_size))
    {
      fprintf(stderr, "RTMPPacket_Alloc failed\n");
      exit(5);
    }
  /* RTMPPacket_Alloc() only reserves the storage; the caller owns the
   * logical body size. */
  p->m_nBodySize = body_size;
  p->m_nChannel = channel;
  p->m_headerType = (uint8_t)htype;
  p->m_packetType = (uint8_t)ptype;
  p->m_nTimeStamp = ts;
  p->m_nInfoField2 = info;
  fill_body(p, first);
}

/* Read one fully reassembled packet from a reader that keeps the
 * per-channel compression state.  The stock reader consumes a single
 * chunk per call, so loop until the packet is complete. */
static int
read_one_packet(RTMP *rr, RTMPPacket *out)
{
  memset(out, 0, sizeof(*out));
  do
    {
      if (!RTMP_ReadPacket(rr, out))
	return 0;
    }
  while (!RTMPPacket_IsReady(out));
  return 1;
}

/* ---- test 1: short writes, EINTR and repeated EAGAIN ------------ */

int
test_rtmp_write_resume(void)
{
  int asserts = 0;
  int f[2];
  RTMP r;
  RTMPPacket p;
  int pushes, res;
  SendCap caps[] = {
    { CAP_SHORT, 1, 3, 0 },	/* first call accepts 3 bytes */
    { CAP_EINTR, 1, 0, 0 },	/* one interrupted send */
    { CAP_SHORT, 2, 5, 0 },	/* two 5-byte short writes */
    { CAP_EAGAIN, 3, 0, 0 },	/* backpressure across 3 pushes */
  };
  const unsigned char body[20] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3
  };

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);
  script_reset(caps, (int)(sizeof(caps) / sizeof(caps[0])), f[0]);

  RTMP_Init(&r);
  r.m_sb.sb_socket = f[0];

  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      100, 1, sizeof(body), 0);
  memcpy(p.m_body, body, sizeof(body));

  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  CHECK(RTMP_HasQueuedPackets(&r) == 1);
  /* the caller still owns its packet unchanged */
  CHECK(p.m_headerType == RTMP_PACKET_SIZE_LARGE);
  CHECK(memcmp(p.m_body, body, sizeof(body)) == 0);
  RTMPPacket_Free(&p);

  res = drain_queue(&r, &pushes);
  CHECK(res == RTMP_QUEUE_DRAINED);
  CHECK(RTMP_HasQueuedPackets(&r) == 0);
  CHECK(RTMP_IsConnected(&r) == 1);

  /* recorded adapter behavior: no sleep, exact script replay */
  CHECK(g_script.again_count == 3);
  CHECK(g_script.int_count == 1);
  CHECK(g_script.short_count == 3);
  CHECK(pushes == 4);		/* 3 EAGAIN pushes + final drain */

  /* one 12-byte header plus one 20-byte body chunk (chunk size 128) */
  CHECK(g_script.captured.len == 32);
  /* hand-written expectation for the fmt0 header on channel 4 */
  CHECK(g_script.captured.data[0] == 0x04);
  CHECK(g_script.captured.data[1] == 0x00);
  CHECK(g_script.captured.data[2] == 0x00);
  CHECK(g_script.captured.data[3] == 0x64);	/* ts 100 */
  CHECK(g_script.captured.data[4] == 0x00);
  CHECK(g_script.captured.data[5] == 0x00);
  CHECK(g_script.captured.data[6] == 0x14);	/* body size 20 */
  CHECK(g_script.captured.data[7] == 0x08);	/* audio */
  CHECK(g_script.captured.data[8] == 0x01);	/* info field 2, LE */
  CHECK(g_script.captured.data[9] == 0x00);
  CHECK(g_script.captured.data[10] == 0x00);
  CHECK(g_script.captured.data[11] == 0x00);
  CHECK(memcmp(g_script.captured.data + 12, body, sizeof(body)) == 0);

  /* cross-check with the stock parser on the peer socket */
  {
    RTMP rr;
    RTMPPacket got;
    RTMP_Init(&rr);
    rr.m_sb.sb_socket = f[1];
    rr.m_inChunkSize = RTMP_DEFAULT_CHUNKSIZE;
    CHECK(read_one_packet(&rr, &got) == 1);
    CHECK(got.m_nChannel == 4);
    CHECK(got.m_packetType == RTMP_PACKET_TYPE_AUDIO);
    CHECK(got.m_nTimeStamp == 100);
    CHECK(got.m_nBodySize == 20);
    CHECK(memcmp(got.m_body, body, sizeof(body)) == 0);
    RTMPPacket_Free(&got);
  }

  RTMP_Close(&r);
  close(f[0]);
  close(f[1]);
  script_done();
  return asserts;
}

/* ---- test 2: two channels, compressed headers, ext timestamp ---- */

/* Append an expected wire fragment while building the hand-written
 * reference byte stream. */
static int
exp_put(WireBuf *w, const void *p, size_t n)
{
  return wire_append(w, p, n);
}

int
test_rtmp_write_headers(void)
{
  int asserts = 0;
  int f[2];
  RTMP r, rr;
  RTMPPacket pa, pb, pc, pd, pe;
  int pushes;
  WireBuf expect = { 0, 0, 0 };
  const uint32_t big_ts = 2000 + 0x01000000;	/* delta >= 0xffffff */
  unsigned char hdr[12];
  /* fixed, independent reference bodies (0xa0..0xa9 etc.) */
  const unsigned char ea[10] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9
  };
  const unsigned char eb[10] = {
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9
  };
  const unsigned char ec[10] = {
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9
  };
  const unsigned char ed[10] = {
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9
  };
  const unsigned char ee[10] = {
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9
  };

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);
  script_reset(NULL, 0, f[0]);

  RTMP_Init(&r);
  r.m_sb.sb_socket = f[0];
  r.m_outChunkSize = 4;		/* set before queueing anything */

  /* A: ch4 fmt0 audio ts1000 */
  make_packet(&pa, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      1000, 1, 10, 0xa0);
  /* B: ch6 fmt0 video ts2000 */
  make_packet(&pb, 6, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_VIDEO,
	      2000, 2, 10, 0xb0);
  /* C: ch4 medium -> compressed to small, delta 100 */
  make_packet(&pc, 4, RTMP_PACKET_SIZE_MEDIUM, RTMP_PACKET_TYPE_AUDIO,
	      1100, 1, 10, 0xc0);
  /* D: ch4 small with same ts -> compressed to minimum (fmt3) */
  make_packet(&pd, 4, RTMP_PACKET_SIZE_SMALL, RTMP_PACKET_TYPE_AUDIO,
	      1100, 1, 10, 0xd0);
  /* E: ch6 medium -> small; delta 0x01000000 needs ext timestamp */
  make_packet(&pe, 6, RTMP_PACKET_SIZE_MEDIUM, RTMP_PACKET_TYPE_VIDEO,
	      big_ts, 2, 10, 0xe0);

  CHECK(RTMP_QueuePacket(&r, &pa) == RTMP_QUEUE_OK);
  CHECK(RTMP_QueuePacket(&r, &pb) == RTMP_QUEUE_OK);
  CHECK(RTMP_QueuePacket(&r, &pc) == RTMP_QUEUE_OK);
  CHECK(pc.m_headerType == RTMP_PACKET_SIZE_MEDIUM);	/* caller's copy */
  CHECK(RTMP_QueuePacket(&r, &pd) == RTMP_QUEUE_OK);
  CHECK(pd.m_headerType == RTMP_PACKET_SIZE_SMALL);
  CHECK(RTMP_QueuePacket(&r, &pe) == RTMP_QUEUE_OK);
  CHECK(RTMP_HasQueuedPackets(&r) == 1);

  /* the serializer must never overwrite the caller's body bytes or
   * header type, even though fmt3 headers are assembled in gaps */
  CHECK(memcmp(pa.m_body, ea, 10) == 0);
  CHECK(memcmp(pb.m_body, eb, 10) == 0);
  CHECK(memcmp(pc.m_body, ec, 10) == 0);
  CHECK(memcmp(pd.m_body, ed, 10) == 0);
  CHECK(memcmp(pe.m_body, ee, 10) == 0);

  CHECK(drain_queue(&r, &pushes) == RTMP_QUEUE_DRAINED);
  CHECK(pushes == 1);
  CHECK(RTMP_HasQueuedPackets(&r) == 0);

  /* bodies are still intact after the flush */
  CHECK(memcmp(pa.m_body, ea, 10) == 0);
  CHECK(memcmp(pe.m_body, ee, 10) == 0);

  /* --- hand-built reference stream, chunk size 4 --- */
  /* A: fmt0 ch4 header + 4 / fmt3+4 / fmt3+2 */
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = 0x04; hdr[2] = 0x03; hdr[3] = 0xe8;	/* 1000 */
  hdr[6] = 0x0a; hdr[7] = 0x08; hdr[8] = 0x01;
  CHECK(exp_put(&expect, hdr, 12));
  CHECK(exp_put(&expect, ea, 4));
  CHECK(exp_put(&expect, "\xc4", 1));
  CHECK(exp_put(&expect, ea + 4, 4));
  CHECK(exp_put(&expect, "\xc4", 1));
  CHECK(exp_put(&expect, ea + 8, 2));

  /* B: fmt0 ch6 */
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = 0x06; hdr[2] = 0x07; hdr[3] = 0xd0;	/* 2000 */
  hdr[6] = 0x0a; hdr[7] = 0x09; hdr[8] = 0x02;
  CHECK(exp_put(&expect, hdr, 12));
  CHECK(exp_put(&expect, eb, 4));
  CHECK(exp_put(&expect, "\xc6", 1));
  CHECK(exp_put(&expect, eb + 4, 4));
  CHECK(exp_put(&expect, "\xc6", 1));
  CHECK(exp_put(&expect, eb + 8, 2));

  /* C: medium was further compressed to fmt2 (0x80|ch4=0x84) +
   * delta 100 */
  CHECK(exp_put(&expect, "\x84\x00\x00\x64", 4));
  CHECK(exp_put(&expect, ec, 4));
  CHECK(exp_put(&expect, "\xc4", 1));
  CHECK(exp_put(&expect, ec + 4, 4));
  CHECK(exp_put(&expect, "\xc4", 1));
  CHECK(exp_put(&expect, ec + 8, 2));

  /* D: fmt3 ch4 (0xc4), no message header at all */
  CHECK(exp_put(&expect, "\xc4", 1));
  CHECK(exp_put(&expect, ed, 4));
  CHECK(exp_put(&expect, "\xc4", 1));
  CHECK(exp_put(&expect, ed + 4, 4));
  CHECK(exp_put(&expect, "\xc4", 1));
  CHECK(exp_put(&expect, ed + 8, 2));

  /* E: medium compressed to fmt2 (0x80|ch6=0x86); 24-bit field
   * 0xffffff + ext 0x01000000; every fmt3 continuation (0xc6)
   * repeats the extended timestamp */
  CHECK(exp_put(&expect, "\x86\xff\xff\xff\x01\x00\x00\x00", 8));
  CHECK(exp_put(&expect, ee, 4));
  CHECK(exp_put(&expect, "\xc6\x01\x00\x00\x00", 5));
  CHECK(exp_put(&expect, ee + 4, 4));
  CHECK(exp_put(&expect, "\xc6\x01\x00\x00\x00", 5));
  CHECK(exp_put(&expect, ee + 8, 2));

  CHECK(g_script.captured.len == expect.len);
  if (memcmp(g_script.captured.data, expect.data, expect.len) != 0)
    {
      size_t i;
      fprintf(stderr, "got:");
      for (i = 0; i < g_script.captured.len && i < 64; i++)
	fprintf(stderr, " %02x", g_script.captured.data[i]);
      fprintf(stderr, "\nwant:");
      for (i = 0; i < expect.len && i < 64; i++)
	fprintf(stderr, " %02x", expect.data[i]);
      fprintf(stderr, "\n");
      for (i = 0; i < expect.len; i++)
	if (g_script.captured.data[i] != expect.data[i])
	  {
	    fprintf(stderr, "wire mismatch at byte %zu: got %02x want %02x\n",
		    i, g_script.captured.data[i], expect.data[i]);
	    break;
	  }
      CHECK(0);
    }
  asserts++;
  free(expect.data);

  /* parse everything back with the unmodified reader; one shared
   * reader keeps the per-channel compression history, just like a real
   * peer would. */
  RTMP_Init(&rr);
  rr.m_sb.sb_socket = f[1];
  rr.m_inChunkSize = 4;
  {
    RTMPPacket got;
    CHECK(read_one_packet(&rr, &got) == 1);
    CHECK(got.m_nChannel == 4 && got.m_packetType == 0x08
	  && got.m_nTimeStamp == 1000 && got.m_nBodySize == 10);
    CHECK(memcmp(got.m_body, pa.m_body, 10) == 0);
    RTMPPacket_Free(&got);

    CHECK(read_one_packet(&rr, &got) == 1);
    CHECK(got.m_nChannel == 6 && got.m_packetType == 0x09
	  && got.m_nTimeStamp == 2000 && got.m_nBodySize == 10);
    CHECK(memcmp(got.m_body, pb.m_body, 10) == 0);
    RTMPPacket_Free(&got);

    CHECK(read_one_packet(&rr, &got) == 1);
    CHECK(got.m_nChannel == 4 && got.m_nTimeStamp == 1100);
    CHECK(memcmp(got.m_body, pc.m_body, 10) == 0);
    RTMPPacket_Free(&got);

    /* The stock reader gives a fmt3 message the previous message's
     * timestamp delta (100), so 1100 + 100 = 1200; the hand-written
     * expectation above is what pins the actual on-wire fmt3 header. */
    CHECK(read_one_packet(&rr, &got) == 1);
    CHECK(got.m_nChannel == 4 && got.m_nTimeStamp == 1200);
    CHECK(memcmp(got.m_body, pd.m_body, 10) == 0);
    RTMPPacket_Free(&got);

    CHECK(read_one_packet(&rr, &got) == 1);
    CHECK(got.m_nChannel == 6 && got.m_nTimeStamp == big_ts);
    CHECK(memcmp(got.m_body, pe.m_body, 10) == 0);
    RTMPPacket_Free(&got);
  }

  RTMPPacket_Free(&pa);
  RTMPPacket_Free(&pb);
  RTMPPacket_Free(&pc);
  RTMPPacket_Free(&pd);
  RTMPPacket_Free(&pe);
  RTMP_Close(&r);
  close(f[0]);
  close(f[1]);
  script_done();
  return asserts;
}

/* ---- test 3: validation, snapshots, close/reset, hard error ----- */

int
test_rtmp_write_lifecycle(void)
{
  int asserts = 0;
  int f[2], f2[2];
  RTMP r;
  RTMPPacket p;
  int pushes;
  SendCap partial[] = {
    { CAP_SHORT, 1, 2, 0 },
    { CAP_EAGAIN, 1, 0, 0 },
  };
  SendCap fatal[] = {
    { CAP_ERROR, 1, 0, EPIPE },
  };

  RTMP_LogSetLevel(RTMP_LOGCRIT);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);
  script_reset(NULL, 0, f[0]);
  RTMP_Init(&r);
  r.m_sb.sb_socket = f[0];

  /* empty queue reports DRAINED, not an error */
  CHECK(RTMP_HasQueuedPackets(&r) == 0);
  CHECK(RTMP_WriteQueue(&r) == RTMP_QUEUE_DRAINED);

  /* invalid arguments must fail without queueing anything */
  {
    RTMPPacket dummy;
    RTMPPacket_Reset(&dummy);
    CHECK(RTMP_QueuePacket(NULL, &dummy) == RTMP_QUEUE_ERROR);
    CHECK(RTMP_QueuePacket(&r, NULL) == RTMP_QUEUE_ERROR);
  }

  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_INVOKE,
	      0, 0, 4, 0x10);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  p.m_packetType = RTMP_PACKET_TYPE_CONTROL;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  p.m_packetType = RTMP_PACKET_TYPE_CHUNK_SIZE;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  p.m_packetType = RTMP_PACKET_TYPE_AUDIO;
  p.m_headerType = 4;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  p.m_headerType = RTMP_PACKET_SIZE_LARGE;
  p.m_nChannel = 70000;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  p.m_nChannel = -1;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  p.m_nChannel = 4;
  RTMPPacket_Free(&p);
  RTMPPacket_Reset(&p);
  p.m_nChannel = 4;
  p.m_headerType = RTMP_PACKET_SIZE_LARGE;
  p.m_packetType = RTMP_PACKET_TYPE_AUDIO;
  p.m_nBodySize = 8;
  p.m_body = NULL;
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_ERROR);
  CHECK(RTMP_HasQueuedPackets(&r) == 0);

  /* INFO metadata is an accepted type */
  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_INFO,
	      0, 0, 6, 0x20);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  CHECK(RTMP_HasQueuedPackets(&r) == 1);

  /* snapshot survives the caller mutating and freeing its body */
  fill_body(&p, 0x51);
  {
    unsigned char saved[6];
    memcpy(saved, p.m_body, 6);
    make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
		50, 0, 6, 0);
    memcpy(p.m_body, saved, 6);
    CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
    memset(p.m_body, 0xff, 6);
    CHECK(p.m_headerType == RTMP_PACKET_SIZE_LARGE);
    RTMPPacket_Free(&p);
    CHECK(drain_queue(&r, &pushes) == RTMP_QUEUE_DRAINED);
    /* INFO packet: 12-byte header then the 6 metadata bytes (0x20..) */
    CHECK(g_script.captured.data[7] == RTMP_PACKET_TYPE_INFO);
    CHECK(g_script.captured.data[12] == 0x20);
    /* the freed audio body still carries the original snapshot bytes
     * at offset 12 (first header+body) + 18 (second header) = 30 */
    CHECK(memcmp(g_script.captured.data + 30, saved, 6) == 0);
    CHECK(g_script.captured.len == 36);
  }

  /* partial flush followed by RTMP_Close drops every leftover snapshot */
  make_packet(&p, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      70, 0, 10, 0x70);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  make_packet(&p, 6, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_VIDEO,
	      80, 0, 10, 0x80);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  RTMPPacket_Free(&p);
  script_reset(partial, (int)(sizeof(partial) / sizeof(partial[0])), f[0]);
  CHECK(RTMP_WriteQueue(&r) == RTMP_QUEUE_WOULDBLOCK);
  CHECK(RTMP_HasQueuedPackets(&r) == 1);
  RTMP_Close(&r);
  CHECK(RTMP_HasQueuedPackets(&r) == 0);
  CHECK(RTMP_IsConnected(&r) == 0);
  close(f[0]);
  close(f[1]);

  /* re-initialize on a fresh socket: no stale queued byte survives */
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f2) == 0);
  script_reset(NULL, 0, f2[0]);
  RTMP_Init(&r);
  r.m_sb.sb_socket = f2[0];
  CHECK(RTMP_HasQueuedPackets(&r) == 0);
  make_packet(&p, 5, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_VIDEO,
	      90, 0, 7, 0x90);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  RTMPPacket_Free(&p);
  CHECK(drain_queue(&r, &pushes) == RTMP_QUEUE_DRAINED);
  CHECK(g_script.captured.len == 19);	/* 12 header + 7 body, only */
  CHECK(g_script.captured.data[0] == 0x05);
  CHECK(g_script.captured.data[7] == RTMP_PACKET_TYPE_VIDEO);
  {
    RTMP rr2;
    RTMPPacket got;
    RTMP_Init(&rr2);
    rr2.m_sb.sb_socket = f2[1];
    rr2.m_inChunkSize = RTMP_DEFAULT_CHUNKSIZE;
    CHECK(read_one_packet(&rr2, &got) == 1);
    CHECK(got.m_nChannel == 5 && got.m_nTimeStamp == 90);
    CHECK(got.m_body[0] == (char)0x90 && got.m_body[6] == (char)0x96);
    RTMPPacket_Free(&got);
  }

  /* end-to-end backpressure on a genuine non-blocking socket, with
   * the debug hook removed so the real send() path is exercised */
  {
    int f3[2], flags, iters = 0;
    char fillbuf[4096];
    RTMP r3;
    RTMPPacket p3;
    int res2;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, f3) == 0);
    RTMP_SetDebugSend(NULL);
    flags = fcntl(f3[0], F_GETFL);
    CHECK(flags >= 0);
    CHECK(fcntl(f3[0], F_SETFL, flags | O_NONBLOCK) == 0);
    memset(fillbuf, 1, sizeof(fillbuf));
    while (send(f3[0], fillbuf, sizeof(fillbuf), 0) > 0)
      ;
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

    RTMP_Init(&r3);
    r3.m_sb.sb_socket = f3[0];
    make_packet(&p3, 4, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
		1, 0, 16, 0xaa);
    CHECK(RTMP_QueuePacket(&r3, &p3) == RTMP_QUEUE_OK);
    RTMPPacket_Free(&p3);

    /* EAGAIN must report WOULDBLOCK, keep the connection and keep the
     * full snapshot queued; no busy waiting is done inside the call */
    CHECK(RTMP_WriteQueue(&r3) == RTMP_QUEUE_WOULDBLOCK);
    CHECK(RTMP_IsConnected(&r3) == 1);
    CHECK(RTMP_HasQueuedPackets(&r3) == 1);

    /* once the peer drains, every queued byte goes out */
    do
      {
	ssize_t rd = read(f3[1], fillbuf, sizeof(fillbuf));
	(void)rd;
	res2 = RTMP_WriteQueue(&r3);
	CHECK(++iters < 100000);
      }
    while (res2 == RTMP_QUEUE_WOULDBLOCK);
    CHECK(res2 == RTMP_QUEUE_DRAINED);
    CHECK(RTMP_HasQueuedPackets(&r3) == 0);
    CHECK(RTMP_IsConnected(&r3) == 1);
    RTMP_Close(&r3);
    close(f3[0]);
    close(f3[1]);
  }

  /* a real socket error fails the flush and runs the close path */
  script_reset(fatal, (int)(sizeof(fatal) / sizeof(fatal[0])), f2[0]);
  make_packet(&p, 5, RTMP_PACKET_SIZE_LARGE, RTMP_PACKET_TYPE_AUDIO,
	      91, 0, 3, 0x60);
  CHECK(RTMP_QueuePacket(&r, &p) == RTMP_QUEUE_OK);
  RTMPPacket_Free(&p);
  CHECK(RTMP_WriteQueue(&r) == RTMP_QUEUE_FAILED);
  CHECK(RTMP_IsConnected(&r) == 0);
  CHECK(RTMP_HasQueuedPackets(&r) == 0);
  close(f2[0]);
  close(f2[1]);
  script_done();
  return asserts;
}
