/* Live pacer regression tests: schedule, keyframe recovery and config
 * wire format.  All clocks are manual; the wire bytes produced by the
 * real production chunking code are captured through task_support
 * memory_io and dechunked again with RTMP_ReadPacket, with a few
 * hand-written header offsets checked alongside.
 */
#include "librtmp/rtmp.h"
#include "librtmp/log.h"
#include "task_support/check.h"
#include "task_support/memory_io.h"
#include <string.h>

#define MSID 1

static RTMP writer, reader;

/* FLV message bodies; configs are opaque but distinct per version */
static const unsigned char VCFG1[] = {0x17, 0x00, 0, 0, 0, 0x01, 0x4d, 0x00, 0x1f};
static const unsigned char VCFG2[] = {0x17, 0x00, 0, 0, 0, 0x02, 0x64, 0x00, 0x28};
static const unsigned char ACFG1[] = {0xaf, 0x00, 0x12, 0x10};
static const unsigned char ACFG2[] = {0xaf, 0x00, 0x13, 0x90};

static void
setup(int64_t delay_us, int64_t late_us, uint32_t max_queued)
{
  RTMP_LogSetLevel(RTMP_LOGERROR);
  RTMP_Init(&writer);
  task_io_reset();
  RTMP_EnableLivePacer(&writer, delay_us, late_us, max_queued);
}

static void
teardown(void)
{
  RTMP_Close(&writer);
}

static uint32_t
mkvid(unsigned char *buf, int keyframe, unsigned char tag)
{
  buf[0] = keyframe ? 0x17 : 0x27;
  buf[1] = 1;			/* AVCPacketType: NALU */
  buf[2] = buf[3] = buf[4] = 0;	/* CTS stays in the body */
  buf[5] = keyframe ? 0x65 : 0x41;
  buf[6] = tag;
  return 7;
}

static uint32_t
mkaud(unsigned char *buf, unsigned char tag)
{
  buf[0] = 0xaf;
  buf[1] = 1;			/* AACPacketType: raw AAC */
  buf[2] = 0x21;
  buf[3] = tag;
  return 4;
}

static int
queue(uint8_t ptype, uint32_t dts, const unsigned char *body, uint32_t size,
      int64_t now_us)
{
  RTMPPacket p;
  int ok;
  memset(&p, 0, sizeof(p));
  if (!RTMPPacket_Alloc(&p, size))
    return -1;
  p.m_headerType = RTMP_PACKET_SIZE_LARGE;
  p.m_nChannel = 3;		/* producer channel; pacer re-maps */
  p.m_packetType = ptype;
  p.m_nTimeStamp = dts;
  p.m_nInfoField2 = MSID;
  p.m_nBodySize = size;
  memcpy(p.m_body, body, size);
  ok = RTMP_QueueLivePacket(&writer, &p, now_us);
  RTMPPacket_Free(&p);
  return ok;
}

static int
queue_video(uint32_t dts, int keyframe, unsigned char tag, int64_t now_us)
{
  unsigned char b[8];
  return queue(RTMP_PACKET_TYPE_VIDEO, dts, b, mkvid(b, keyframe, tag), now_us);
}

static int
queue_audio(uint32_t dts, unsigned char tag, int64_t now_us)
{
  unsigned char b[4];
  return queue(RTMP_PACKET_TYPE_AUDIO, dts, b, mkaud(b, tag), now_us);
}

struct got
{
  uint8_t type;
  uint32_t ts;
  int32_t msid;
  uint32_t size;
  unsigned char body[16];
};

/* dechunk everything the production code wrote, via RTMP_ReadPacket */
static int
drain(struct got *g, int max)
{
  RTMPPacket p;
  int n = 0, steps = 0;
  memset(&p, 0, sizeof(p));
  RTMP_Init(&reader);
  reader.m_inChunkSize = writer.m_outChunkSize;
  task_io_rewind();
  while (n < max && ++steps < 1000)
    {
      if (!RTMP_ReadPacket(&reader, &p))
	break;
      if (!RTMPPacket_IsReady(&p))
	continue;
      g[n].type = p.m_packetType;
      g[n].ts = p.m_nTimeStamp;
      g[n].msid = p.m_nInfoField2;
      g[n].size = p.m_nBodySize;
      memcpy(g[n].body, p.m_body, p.m_nBodySize < 16 ? p.m_nBodySize : 16);
      n++;
      RTMPPacket_Free(&p);
      memset(&p, 0, sizeof(p));
    }
  RTMP_Close(&reader);
  return n;
}

static int
expect(const struct got *g, uint8_t type, uint32_t ts,
       const unsigned char *body, uint32_t size)
{
  TASK_CHECK(g->type == type);
  TASK_CHECK(g->ts == ts);
  TASK_CHECK(g->msid == MSID);
  TASK_CHECK(g->size == size);
  TASK_CHECK(!memcmp(g->body, body, size));
  return 1;
}

/* ---- schedule ---- */

/* due gating: nothing before due, send at due, still sendable at
 * due+late, dropped right after */
static int
case_due_then_late(void)
{
  struct got g[8];
  unsigned char b[8];
  const unsigned char *w;
  int n;

  setup(2000, 1000, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 0);	/* configs not counted */
  TASK_CHECK(queue_video(0, 1, 0x01, 0) == 1);		/* anchor A=0 T=0 */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 1);

  TASK_CHECK(RTMP_PumpLive(&writer, 1999) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(task_io_size() == 0);			/* nothing early */
  TASK_CHECK(RTMP_PumpLive(&writer, 2000) == RTMP_LIVE_PUMP_SENT);
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 0);

  TASK_CHECK(queue_video(40, 0, 0x02, 2000) == 1);	/* due 42000 */
  TASK_CHECK(RTMP_PumpLive(&writer, 41999) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(RTMP_PumpLive(&writer, 42000) == RTMP_LIVE_PUMP_SENT);

  TASK_CHECK(queue_video(80, 0, 0x03, 42000) == 1);	/* due 82000 */
  TASK_CHECK(RTMP_PumpLive(&writer, 83000) == RTMP_LIVE_PUMP_SENT); /* ==due+late */

  TASK_CHECK(queue_video(120, 0, 0x04, 83000) == 1);	/* due 122000 */
  TASK_CHECK(RTMP_PumpLive(&writer, 123001) == RTMP_LIVE_PUMP_IDLE); /* expired */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 0);

  w = task_io_data();
  TASK_CHECK(w[0] == 0x06);		/* video track CSID, large header */
  TASK_CHECK(w[7] == RTMP_PACKET_TYPE_VIDEO);
  n = drain(g, 8);
  TASK_CHECK(n == 4);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x01);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7));
  mkvid(b, 0, 0x02);
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_VIDEO, 40, b, 7));
  mkvid(b, 0, 0x03);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_VIDEO, 80, b, 7));
  teardown();
  return 0;
}

/* audio submitted before a same-DTS keyframe is held and sent after it */
static int
case_same_dts_audio(void)
{
  struct got g[8];
  unsigned char b[8];
  const unsigned char *w;
  int n;

  setup(2000, 1000, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, ACFG1, sizeof(ACFG1), 0) == 1);
  TASK_CHECK(queue_audio(0, 0x0a, 0) == 1);	/* audio anchors the timeline */
  TASK_CHECK(queue_video(0, 1, 0x0b, 0) == 1);
  TASK_CHECK(queue_audio(40, 0x0c, 0) == 1);

  TASK_CHECK(RTMP_PumpLive(&writer, 2000) == RTMP_LIVE_PUMP_SENT); /* keyframe */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 2);	/* same-DTS audio kept */
  TASK_CHECK(RTMP_PumpLive(&writer, 2000) == RTMP_LIVE_PUMP_SENT); /* audio@0 */
  TASK_CHECK(RTMP_PumpLive(&writer, 42000) == RTMP_LIVE_PUMP_SENT); /* audio@40 */

  n = drain(g, 8);
  TASK_CHECK(n == 5);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x0b);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7)); /* key first */
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_AUDIO, 0, ACFG1, sizeof(ACFG1)));
  mkaud(b, 0x0a);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_AUDIO, 0, b, 4));
  mkaud(b, 0x0c);
  TASK_CHECK(expect(&g[4], RTMP_PACKET_TYPE_AUDIO, 40, b, 4));

  /* hand-written check of the audio track header on the wire */
  w = task_io_data();
  TASK_CHECK(w[0] == 0x06 && w[7] == 0x09);		/* video csid/type */
  /* vcfg(12+9) + key(12+7) = 40 bytes, then audio config header */
  TASK_CHECK(w[40] == 0x04);				/* audio csid */
  TASK_CHECK(w[44] == 0 && w[45] == 0 && w[46] == 4);	/* body size 4 */
  TASK_CHECK(w[47] == RTMP_PACKET_TYPE_AUDIO);
  TASK_CHECK(w[48] == MSID && w[49] == 0 && w[50] == 0 && w[51] == 0);
  teardown();
  return 0;
}

/* DTS expansion across 2^32, regression rejected without moving state */
static int
case_dts_wrap(void)
{
  struct got g[8];
  unsigned char b[8];
  const unsigned char *w;
  int n;

  setup(1000, 500, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue_video(0xfffffff0u, 1, 0x11, 0) == 1);
  TASK_CHECK(queue_video(20, 0, 0x12, 0) == 1);		/* +36ms across wrap */
  TASK_CHECK(queue_video(10, 0, 0x13, 0) == 0);		/* regression */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 2);
  TASK_CHECK(queue_video(30, 0, 0x14, 0) == 1);		/* last DTS still 20 */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 3);

  TASK_CHECK(RTMP_PumpLive(&writer, 1000) == RTMP_LIVE_PUMP_SENT);
  TASK_CHECK(RTMP_PumpLive(&writer, 36999) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(RTMP_PumpLive(&writer, 37000) == RTMP_LIVE_PUMP_SENT);
  TASK_CHECK(RTMP_PumpLive(&writer, 47000) == RTMP_LIVE_PUMP_SENT);

  n = drain(g, 8);
  TASK_CHECK(n == 4);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0xfffffff0u, VCFG1,
		    sizeof(VCFG1)));
  mkvid(b, 1, 0x11);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0xfffffff0u, b, 7));
  mkvid(b, 0, 0x12);
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_VIDEO, 20, b, 7));
  mkvid(b, 0, 0x14);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_VIDEO, 30, b, 7));

  /* extended timestamp on the wire for the wrapped DTS */
  w = task_io_data();
  TASK_CHECK(w[1] == 0xff && w[2] == 0xff && w[3] == 0xff);
  TASK_CHECK(w[12] == 0xff && w[13] == 0xff && w[14] == 0xff && w[15] == 0xf0);
  teardown();
  return 0;
}

/* invalid input, double enable and a full queue all reject cleanly */
static int
case_reject_continue(void)
{
  struct got g[8];
  unsigned char bad[8];
  unsigned char b[8];
  int n;

  setup(1000, 500, 2);
  TASK_CHECK(RTMP_EnableLivePacer(&writer, 0, 0, 100) == 0); /* already on */
  TASK_CHECK(RTMP_EnableLivePacer(&writer, -1, 0, 1) == 0);
  TASK_CHECK(RTMP_EnableLivePacer(&writer, 0, -1, 1) == 0);
  TASK_CHECK(RTMP_EnableLivePacer(&writer, 0, 0, 0) == 0);

  /* malformed messages are rejected before any state moves */
  bad[0] = 0x97; bad[1] = 1; bad[2] = 0x21;		/* SoundFormat 9 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, bad, 3, 0) == 0);
  bad[0] = 0xaf; bad[1] = 2;				/* AACPacketType 2 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, bad, 3, 0) == 0);
  bad[1] = 1;
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, bad, 2, 0) == 0); /* no content */
  bad[0] = 0x15; bad[1] = 1; bad[5] = 0x65;		/* CodecID 5 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, bad, 6, 0) == 0);
  bad[0] = 0x17; bad[1] = 2;				/* AVCPacketType 2 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, bad, 6, 0) == 0);
  bad[0] = 0x37; bad[1] = 1;				/* FrameType 3 media */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, bad, 6, 0) == 0);
  bad[0] = 0x27; bad[1] = 0;				/* config not keyframe */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, bad, 6, 0) == 0);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_INFO, 0, bad, 6, 0) == 0);
  TASK_CHECK(queue_video(0, 1, 0x21, 0) == 0);		/* no config yet */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 0);

  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x22, 0) == 1);		/* anchor A=0 */
  TASK_CHECK(queue_video(40, 0, 0x23, 0) == 1);
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 2);
  TASK_CHECK(queue_video(80, 0, 0x24, 0) == 0);		/* full (max kept: 2) */
  TASK_CHECK(queue_video(0xffffffffu, 0, 0x25, 0) == 0); /* regression */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 2);

  TASK_CHECK(RTMP_PumpLive(&writer, 1000) == RTMP_LIVE_PUMP_SENT);
  /* last DTS survived the rejections: 50 expands from 40, due 51000 */
  TASK_CHECK(queue_video(50, 0, 0x26, 1000) == 1);
  TASK_CHECK(RTMP_PumpLive(&writer, 41000) == RTMP_LIVE_PUMP_SENT);
  TASK_CHECK(RTMP_PumpLive(&writer, 50999) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(RTMP_PumpLive(&writer, 51000) == RTMP_LIVE_PUMP_SENT);

  n = drain(g, 8);
  TASK_CHECK(n == 4);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x22);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7));
  mkvid(b, 0, 0x23);
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_VIDEO, 40, b, 7));
  mkvid(b, 0, 0x26);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_VIDEO, 50, b, 7));
  teardown();
  return 0;
}

/* ---- recovery ---- */

/* expired P frames are dropped and the stream resumes at the next
 * keyframe that still makes its playout time */
static int
case_pframe_drop_keyframe(void)
{
  struct got g[8];
  unsigned char b[8];
  int n;

  setup(2000, 1000, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x31, 0) == 1);
  TASK_CHECK(queue_video(40, 0, 0x32, 0) == 1);
  TASK_CHECK(queue_video(80, 0, 0x33, 0) == 1);
  TASK_CHECK(queue_video(120, 1, 0x34, 0) == 1);
  TASK_CHECK(queue_video(160, 0, 0x35, 0) == 1);

  TASK_CHECK(RTMP_PumpLive(&writer, 2000) == RTMP_LIVE_PUMP_SENT); /* key@0 */
  /* P@40 and P@80 expire; recovery keyframe@120 is not due yet */
  TASK_CHECK(RTMP_PumpLive(&writer, 90000) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 2);
  TASK_CHECK(RTMP_PumpLive(&writer, 121999) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(RTMP_PumpLive(&writer, 122000) == RTMP_LIVE_PUMP_SENT); /* key@120 */
  TASK_CHECK(RTMP_PumpLive(&writer, 162000) == RTMP_LIVE_PUMP_SENT); /* P@160 */

  n = drain(g, 8);
  TASK_CHECK(n == 5);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x31);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7));
  /* recovery re-sends the held config with the keyframe's own DTS */
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_VIDEO, 120, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x34);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_VIDEO, 120, b, 7));
  mkvid(b, 0, 0x35);
  TASK_CHECK(expect(&g[4], RTMP_PACKET_TYPE_VIDEO, 160, b, 7));
  teardown();
  return 0;
}

/* an expired audio packet costs only itself; video keeps flowing */
static int
case_audio_expiry_alone(void)
{
  struct got g[8];
  unsigned char b[8];
  int n;

  setup(2000, 1000, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, ACFG1, sizeof(ACFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x41, 0) == 1);
  TASK_CHECK(queue_audio(40, 0x42, 0) == 1);
  TASK_CHECK(queue_video(80, 0, 0x43, 0) == 1);

  TASK_CHECK(RTMP_PumpLive(&writer, 2000) == RTMP_LIVE_PUMP_SENT); /* key@0 */
  TASK_CHECK(RTMP_PumpLive(&writer, 50000) == RTMP_LIVE_PUMP_IDLE); /* au@40 gone */
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 1);
  TASK_CHECK(RTMP_PumpLive(&writer, 82000) == RTMP_LIVE_PUMP_SENT); /* P@80 */

  n = drain(g, 8);
  TASK_CHECK(n == 3);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x41);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7));
  mkvid(b, 0, 0x43);
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_VIDEO, 80, b, 7));
  teardown();
  return 0;
}

/* Close frees the pacer; a fresh RTMP_Init inherits no clock or
 * recovery state */
static int
case_close_rebuild(void)
{
  struct got g[4];
  int n;

  setup(1000, 500, 4);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x51, 0) == 1);
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 1);
  RTMP_Close(&writer);
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 0);
  TASK_CHECK(RTMP_PumpLive(&writer, 5000) == RTMP_LIVE_PUMP_ERROR);
  TASK_CHECK(queue_video(0, 1, 0x52, 0) == 0);

  RTMP_Init(&writer);
  task_io_reset();
  TASK_CHECK(RTMP_EnableLivePacer(&writer, 3000, 100, 4) == 1);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG2, sizeof(VCFG2), 100000) == 1);
  TASK_CHECK(queue_video(500, 1, 0x53, 100000) == 1);
  /* fresh anchor: due = 100000 + 3000, not inherited from the old clock */
  TASK_CHECK(RTMP_PumpLive(&writer, 102999) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(RTMP_PumpLive(&writer, 103000) == RTMP_LIVE_PUMP_SENT);

  n = drain(g, 4);
  TASK_CHECK(n == 2);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 500, VCFG2, sizeof(VCFG2)));
  {
    unsigned char b[8];
    mkvid(b, 1, 0x53);
    TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 500, b, 7));
  }
  teardown();
  return 0;
}

/* ---- config_wire ---- */

/* a new config submitted before old-version media is sent does not
 * interrupt it; versions interleave on the wire in media order */
static int
case_config_interleave(void)
{
  struct got g[8];
  unsigned char b[8];
  int n;

  setup(2000, 1000, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x61, 0) == 1);		/* holds v1 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG2, sizeof(VCFG2), 0) == 1);
  TASK_CHECK(queue_video(40, 1, 0x62, 0) == 1);		/* holds v2 */
  TASK_CHECK(queue_video(80, 0, 0x63, 0) == 1);		/* holds v2 */

  TASK_CHECK(RTMP_PumpLive(&writer, 2000) == RTMP_LIVE_PUMP_SENT);
  TASK_CHECK(RTMP_PumpLive(&writer, 42000) == RTMP_LIVE_PUMP_SENT);
  TASK_CHECK(RTMP_PumpLive(&writer, 82000) == RTMP_LIVE_PUMP_SENT);

  n = drain(g, 8);
  TASK_CHECK(n == 5);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x61);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7));
  /* keyframe switches the wire to v2, config stamped with its DTS */
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_VIDEO, 40, VCFG2, sizeof(VCFG2)));
  mkvid(b, 1, 0x62);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_VIDEO, 40, b, 7));
  mkvid(b, 0, 0x63);
  TASK_CHECK(expect(&g[4], RTMP_PACKET_TYPE_VIDEO, 80, b, 7));
  teardown();
  return 0;
}

/* a non-keyframe cannot switch config: it is dropped and the stream
 * waits for the next keyframe carrying the new version */
static int
case_config_nonkey_waits(void)
{
  struct got g[8];
  unsigned char b[8];
  int n;

  setup(2000, 1000, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x71, 0) == 1);
  TASK_CHECK(RTMP_PumpLive(&writer, 2000) == RTMP_LIVE_PUMP_SENT);

  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG2, sizeof(VCFG2), 2000) == 1);
  TASK_CHECK(queue_video(40, 0, 0x72, 2000) == 1);	/* P holding v2 */
  TASK_CHECK(queue_video(80, 1, 0x73, 2000) == 1);	/* key holding v2 */
  TASK_CHECK(queue_video(120, 0, 0x74, 2000) == 1);

  /* P@40 is due but cannot switch config: dropped, wait for key@80 */
  TASK_CHECK(RTMP_PumpLive(&writer, 42000) == RTMP_LIVE_PUMP_IDLE);
  TASK_CHECK(RTMP_LivePacerPending(&writer) == 2);
  TASK_CHECK(RTMP_PumpLive(&writer, 82000) == RTMP_LIVE_PUMP_SENT);
  TASK_CHECK(RTMP_PumpLive(&writer, 122000) == RTMP_LIVE_PUMP_SENT);

  n = drain(g, 8);
  TASK_CHECK(n == 5);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x71);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7));
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_VIDEO, 80, VCFG2, sizeof(VCFG2)));
  mkvid(b, 1, 0x73);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_VIDEO, 80, b, 7));
  mkvid(b, 0, 0x74);
  TASK_CHECK(expect(&g[4], RTMP_PACKET_TYPE_VIDEO, 120, b, 7));
  teardown();
  return 0;
}

/* AAC config switches are topped up before the media that carries the
 * new version; byte-identical configs create no new version */
static int
case_config_audio_switch(void)
{
  struct got g[10];
  unsigned char b[8];
  int n;

  setup(1000, 500, 8);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, ACFG1, sizeof(ACFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x81, 0) == 1);
  TASK_CHECK(queue_audio(0, 0x82, 0) == 1);		/* holds a1 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, ACFG2, sizeof(ACFG2), 0) == 1);
  TASK_CHECK(queue_audio(40, 0x83, 0) == 1);		/* holds a2 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, ACFG2, sizeof(ACFG2), 0) == 1);
  TASK_CHECK(queue_audio(80, 0x84, 0) == 1);		/* still a2 */
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue_video(120, 0, 0x85, 0) == 1);	/* still v1 */

  TASK_CHECK(RTMP_PumpLive(&writer, 1000) == RTMP_LIVE_PUMP_SENT); /* key */
  TASK_CHECK(RTMP_PumpLive(&writer, 1000) == RTMP_LIVE_PUMP_SENT); /* au@0 */
  TASK_CHECK(RTMP_PumpLive(&writer, 41000) == RTMP_LIVE_PUMP_SENT); /* au@40 */
  TASK_CHECK(RTMP_PumpLive(&writer, 81000) == RTMP_LIVE_PUMP_SENT); /* au@80 */
  TASK_CHECK(RTMP_PumpLive(&writer, 121000) == RTMP_LIVE_PUMP_SENT); /* P@120 */

  n = drain(g, 10);
  TASK_CHECK(n == 8);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  mkvid(b, 1, 0x81);
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, b, 7));
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_AUDIO, 0, ACFG1, sizeof(ACFG1)));
  mkaud(b, 0x82);
  TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_AUDIO, 0, b, 4));
  /* audio config switch: a2 goes out with the media's DTS */
  TASK_CHECK(expect(&g[4], RTMP_PACKET_TYPE_AUDIO, 40, ACFG2, sizeof(ACFG2)));
  mkaud(b, 0x83);
  TASK_CHECK(expect(&g[5], RTMP_PACKET_TYPE_AUDIO, 40, b, 4));
  /* identical resubmits made no new versions: no configs before these */
  mkaud(b, 0x84);
  TASK_CHECK(expect(&g[6], RTMP_PACKET_TYPE_AUDIO, 80, b, 4));
  mkvid(b, 0, 0x85);
  TASK_CHECK(expect(&g[7], RTMP_PACKET_TYPE_VIDEO, 120, b, 7));
  teardown();
  return 0;
}

/* hand-written chunk layout check: per-track CSIDs, shared MSID,
 * config stamped with the following media's DTS, continuation chunks */
static int
case_wire_format(void)
{
  struct got g[4];
  unsigned char key[8];
  const unsigned char *w;
  int n;

  setup(1000, 500, 4);
  writer.m_outChunkSize = 4;
  mkvid(key, 1, 0x91);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1), 0) == 1);
  TASK_CHECK(queue(RTMP_PACKET_TYPE_AUDIO, 0, ACFG1, sizeof(ACFG1), 0) == 1);
  TASK_CHECK(queue_video(0, 1, 0x91, 0) == 1);
  TASK_CHECK(queue_audio(0, 0x92, 0) == 1);
  TASK_CHECK(RTMP_PumpLive(&writer, 1000) == RTMP_LIVE_PUMP_SENT); /* key */
  TASK_CHECK(RTMP_PumpLive(&writer, 1000) == RTMP_LIVE_PUMP_SENT); /* audio */

  w = task_io_data();
  TASK_CHECK(task_io_size() == 75);
  /* video config: 12-byte header on csid 6, 9-byte body, 2 continuations */
  TASK_CHECK(w[0] == 0x06);
  TASK_CHECK(w[1] == 0 && w[2] == 0 && w[3] == 0);	/* DTS of keyframe */
  TASK_CHECK(w[4] == 0 && w[5] == 0 && w[6] == 9);
  TASK_CHECK(w[7] == RTMP_PACKET_TYPE_VIDEO);
  TASK_CHECK(w[8] == MSID && w[9] == 0 && w[10] == 0 && w[11] == 0);
  TASK_CHECK(!memcmp(w + 12, VCFG1, 4));
  TASK_CHECK(w[16] == 0xc6);				/* continuation, csid 6 */
  TASK_CHECK(!memcmp(w + 17, VCFG1 + 4, 4));
  TASK_CHECK(w[21] == 0xc6);
  TASK_CHECK(w[22] == VCFG1[8]);
  /* keyframe: 12-byte header, 7-byte body, 1 continuation */
  TASK_CHECK(w[23] == 0x06);
  TASK_CHECK(w[27] == 0 && w[28] == 0 && w[29] == 7);
  TASK_CHECK(w[30] == RTMP_PACKET_TYPE_VIDEO);
  TASK_CHECK(!memcmp(w + 35, key, 4));
  TASK_CHECK(w[39] == 0xc6);
  TASK_CHECK(!memcmp(w + 40, key + 4, 3));
  /* audio config: csid 4, same MSID, DTS of the audio media */
  TASK_CHECK(w[43] == 0x04);
  TASK_CHECK(w[44] == 0 && w[45] == 0 && w[46] == 0);
  TASK_CHECK(w[47] == 0 && w[48] == 0 && w[49] == 4);
  TASK_CHECK(w[50] == RTMP_PACKET_TYPE_AUDIO);
  TASK_CHECK(w[51] == MSID && w[52] == 0 && w[53] == 0 && w[54] == 0);
  TASK_CHECK(!memcmp(w + 55, ACFG1, 4));
  /* audio media */
  TASK_CHECK(w[59] == 0x04);
  TASK_CHECK(w[66] == RTMP_PACKET_TYPE_AUDIO);
  TASK_CHECK(w[71] == 0xaf && w[72] == 1 && w[73] == 0x21 && w[74] == 0x92);

  n = drain(g, 4);
  TASK_CHECK(n == 4);
  TASK_CHECK(expect(&g[0], RTMP_PACKET_TYPE_VIDEO, 0, VCFG1, sizeof(VCFG1)));
  TASK_CHECK(expect(&g[1], RTMP_PACKET_TYPE_VIDEO, 0, key, 7));
  TASK_CHECK(expect(&g[2], RTMP_PACKET_TYPE_AUDIO, 0, ACFG1, sizeof(ACFG1)));
  {
    unsigned char a[4];
    mkaud(a, 0x92);
    TASK_CHECK(expect(&g[3], RTMP_PACKET_TYPE_AUDIO, 0, a, 4));
  }
  teardown();
  return 0;
}

const struct task_case task_cases[] = {
  {"schedule", "due_then_late", case_due_then_late},
  {"schedule", "same_dts_audio", case_same_dts_audio},
  {"schedule", "dts_wrap", case_dts_wrap},
  {"schedule", "reject_continue", case_reject_continue},
  {"recovery", "pframe_drop_keyframe", case_pframe_drop_keyframe},
  {"recovery", "audio_expiry_alone", case_audio_expiry_alone},
  {"recovery", "close_rebuild", case_close_rebuild},
  {"config_wire", "config_interleave", case_config_interleave},
  {"config_wire", "config_nonkey_waits", case_config_nonkey_waits},
  {"config_wire", "config_audio_switch", case_config_audio_switch},
  {"config_wire", "wire_format", case_wire_format},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
