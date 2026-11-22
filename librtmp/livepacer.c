/*
 *      Copyright (C) 2026 Howard Chu
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
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with librtmp see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *  http://www.gnu.org/copyleft/lgpl.html
 */

/* Live pacer: a media-time driven, low-latency send mode for FLV AVC/AAC
 * push.  Complete FLV messages are queued with a monotonic clock; each
 * pump call emits at most one media packet (preceded by any config
 * messages it needs) once its media time is due.  Media that falls more
 * than late_us behind its due time is dropped, and video drops are
 * recovered by jumping forward to the next keyframe that can still make
 * its playout time, re-sending the AVC config that keyframe carries.
 *
 * Sending reuses the production chunking of RTMP_SendPacket; nothing is
 * staged into the channel compression state at queue time, so dropped
 * media never affects the wire channel history.
 */

#include <stdlib.h>
#include <string.h>

#include "rtmp.h"
#include "log.h"

/* the two tracks use their own chunk stream IDs and share one message
 * stream ID (taken from the queued packets) */
#define LIVE_AUDIO_CSID	4
#define LIVE_VIDEO_CSID	6

#define FLV_SOUND_AAC	10	/* SoundFormat in the upper nibble */
#define FLV_CODEC_AVC	7	/* CodecID in the lower nibble */
#define FLV_FRAME_KEY	1
#define FLV_FRAME_INTER	2

#define DTS_HALF	0x80000000u	/* 2^31 ms: wrap/regression boundary */

typedef struct LiveConfig
{
  int refs;			/* latest-config hold + queued media holds */
  uint64_t serial;		/* version identity, grows on every change */
  uint32_t size;
  char *data;			/* opaque full message body */
} LiveConfig;

typedef struct LiveMedia
{
  struct LiveMedia *next;
  LiveConfig *config;		/* config version held at submit time */
  int64_t due_us;		/* send no earlier than this clock time */
  uint32_t dts;			/* raw 32-bit FLV timestamp */
  int32_t msid;			/* message stream ID snapshot */
  uint32_t size;
  uint8_t is_video;
  uint8_t is_keyframe;
  char *body;			/* payload snapshot, sent verbatim */
} LiveMedia;

typedef struct RTMPLivePacer
{
  int64_t delay_us;		/* target send delay behind arrival */
  int64_t late_us;		/* extra slack before a packet is dropped */
  uint32_t max_queued;		/* media queue capacity (configs not counted) */
  uint32_t queued;
  LiveMedia *head, *tail;

  /* shared media timeline: due = anchor_arrival + delay_us +
   *   1000 * (expanded_dts - anchor_dts) */
  int anchored;
  uint64_t anchor_dts;		/* expanded DTS of the first media, ms */
  int64_t anchor_arrival;	/* now_us of the first media */
  uint64_t last_dts;		/* expanded DTS of the last accepted media */
  uint32_t last_raw;		/* its raw 32-bit timestamp */

  LiveConfig *audio_cfg;	/* latest submitted config per track */
  LiveConfig *video_cfg;
  uint64_t next_serial;

  uint64_t audio_cfg_sent;	/* config serial actually on the wire, 0=none */
  uint64_t video_cfg_sent;

  int waiting_keyframe;		/* video suppressed until next keyframe */
} RTMPLivePacer;

static void
config_unref(LiveConfig *c)
{
  if (c && --c->refs <= 0)
    {
      free(c->data);
      free(c);
    }
}

static void
media_free(LiveMedia *m)
{
  config_unref(m->config);
  free(m->body);
  free(m);
}

/* detach m from the queue; caller frees */
static void
media_unlink(RTMPLivePacer *p, LiveMedia *m)
{
  LiveMedia **pp = &p->head;
  while (*pp && *pp != m)
    pp = &(*pp)->next;
  if (*pp)
    *pp = m->next;
  if (p->tail == m)
    {
      LiveMedia *t = p->head;
      while (t && t->next)
	t = t->next;
      p->tail = t;
    }
  p->queued--;
}

int
RTMP_EnableLivePacer(RTMP *r, int64_t delay_us, int64_t late_us,
		     uint32_t max_queued)
{
  RTMPLivePacer *p;

  if (!r)
    return FALSE;
  if (r->m_livePacer)
    return FALSE;		/* already enabled: keep existing state */
  if (delay_us < 0 || late_us < 0 || !max_queued)
    return FALSE;

  p = calloc(1, sizeof(RTMPLivePacer));
  if (!p)
    return FALSE;
  p->delay_us = delay_us;
  p->late_us = late_us;
  p->max_queued = max_queued;
  p->waiting_keyframe = TRUE;	/* video starts out waiting for a keyframe */
  r->m_livePacer = p;
  return TRUE;
}

uint32_t
RTMP_LivePacerPending(const RTMP *r)
{
  if (!r || !r->m_livePacer)
    return 0;
  return ((RTMPLivePacer *)r->m_livePacer)->queued;
}

/* store a new config version for the track; identical bytes are a no-op */
static int
queue_config(RTMPLivePacer *p, int is_video, const char *body, uint32_t size)
{
  LiveConfig **slot = is_video ? &p->video_cfg : &p->audio_cfg;
  LiveConfig *nc;

  if (*slot && (*slot)->size == size && !memcmp((*slot)->data, body, size))
    return TRUE;		/* identical bytes: no new version */

  nc = malloc(sizeof(LiveConfig));
  if (!nc)
    return FALSE;
  nc->data = malloc(size);
  if (!nc->data)
    {
      free(nc);
      return FALSE;
    }
  memcpy(nc->data, body, size);
  nc->size = size;
  nc->refs = 1;
  nc->serial = ++p->next_serial;
  config_unref(*slot);
  *slot = nc;
  return TRUE;
}

int
RTMP_QueueLivePacket(RTMP *r, const RTMPPacket *packet, int64_t now_us)
{
  RTMPLivePacer *p;
  LiveConfig *cfg;
  LiveMedia *m;
  const unsigned char *b;
  uint64_t expanded;
  uint32_t size, delta;
  int is_video, is_config, is_keyframe = 0;

  if (!r || !packet || !packet->m_body)
    return FALSE;
  p = r->m_livePacer;
  if (!p || now_us < 0)
    return FALSE;

  b = (const unsigned char *)packet->m_body;
  size = packet->m_nBodySize;
  is_video = is_config = 0;

  if (packet->m_packetType == RTMP_PACKET_TYPE_AUDIO)
    {
      if (size < 3)		/* 2-byte header + non-empty content */
	return FALSE;
      if ((b[0] >> 4) != FLV_SOUND_AAC)
	return FALSE;
      if (b[1] == 0)
	is_config = 1;
      else if (b[1] != 1)
	return FALSE;
    }
  else if (packet->m_packetType == RTMP_PACKET_TYPE_VIDEO)
    {
      int ft;
      is_video = 1;
      if (size < 6)		/* 5-byte header + non-empty content */
	return FALSE;
      if ((b[0] & 0x0f) != FLV_CODEC_AVC)
	return FALSE;
      ft = b[0] >> 4;
      if (b[1] == 0)
	{
	  if (ft != FLV_FRAME_KEY)
	    return FALSE;	/* video config must be marked keyframe */
	  is_config = 1;
	}
      else if (b[1] == 1)
	{
	  if (ft != FLV_FRAME_KEY && ft != FLV_FRAME_INTER)
	    return FALSE;
	  is_keyframe = (ft == FLV_FRAME_KEY);
	}
      else
	return FALSE;
    }
  else
    return FALSE;

  if (is_config)
    /* configs only update the track's latest version: no media queue, no
     * time anchor, nothing sent on the wire yet */
    return queue_config(p, is_video, (const char *)b, size);

  cfg = is_video ? p->video_cfg : p->audio_cfg;
  if (!cfg)
    return FALSE;		/* every media packet needs its track config */

  /* expand the 32-bit DTS on the shared non-decreasing timeline */
  if (!p->anchored)
    {
      expanded = packet->m_nTimeStamp;
    }
  else
    {
      delta = (uint32_t)(packet->m_nTimeStamp - p->last_raw);
      if (delta >= DTS_HALF)
	return FALSE;		/* timestamp regression; last DTS untouched */
      expanded = p->last_dts + delta;
    }

  if (p->queued >= p->max_queued)
    return FALSE;		/* full: reject wholesale, anchor untouched */

  m = malloc(sizeof(LiveMedia));
  if (!m)
    return FALSE;
  m->body = malloc(size);
  if (!m->body)
    {
      free(m);
      return FALSE;
    }
  memcpy(m->body, b, size);

  /* all validation and allocation done: commit, leaving no half-update */
  m->next = NULL;
  m->config = cfg;
  cfg->refs++;
  m->dts = packet->m_nTimeStamp;
  m->msid = packet->m_nInfoField2;
  m->size = size;
  m->is_video = is_video;
  m->is_keyframe = is_keyframe;

  if (!p->anchored)
    {
      p->anchored = TRUE;
      p->anchor_dts = expanded;
      p->anchor_arrival = now_us;
    }
  p->last_raw = packet->m_nTimeStamp;
  p->last_dts = expanded;
  m->due_us = p->anchor_arrival + p->delay_us
    + (int64_t)((expanded - p->anchor_dts) * 1000);

  if (p->tail)
    p->tail->next = m;
  else
    p->head = m;
  p->tail = m;
  p->queued++;
  return TRUE;
}

/* send one message through the production chunking path */
static int
pump_send(RTMP *r, int csid, uint8_t ptype, uint32_t ts, int32_t msid,
	  const char *body, uint32_t size)
{
  RTMPPacket pkt;
  int ok;

  memset(&pkt, 0, sizeof(pkt));
  if (!RTMPPacket_Alloc(&pkt, size))
    return FALSE;
  pkt.m_headerType = RTMP_PACKET_SIZE_LARGE;
  pkt.m_packetType = ptype;
  pkt.m_nChannel = csid;
  pkt.m_nTimeStamp = ts;
  pkt.m_nInfoField2 = msid;
  pkt.m_nBodySize = size;
  memcpy(pkt.m_body, body, size);
  ok = RTMP_SendPacket(r, &pkt, FALSE);
  RTMPPacket_Free(&pkt);
  return ok;
}

/* the config a media carries goes out with the media's own DTS/MSID */
static int
pump_send_config(RTMP *r, LiveMedia *m)
{
  return pump_send(r, m->is_video ? LIVE_VIDEO_CSID : LIVE_AUDIO_CSID,
		   m->is_video ? RTMP_PACKET_TYPE_VIDEO : RTMP_PACKET_TYPE_AUDIO,
		   m->dts, m->msid, m->config->data, m->config->size);
}

static int
pump_send_media(RTMP *r, LiveMedia *m)
{
  return pump_send(r, m->is_video ? LIVE_VIDEO_CSID : LIVE_AUDIO_CSID,
		   m->is_video ? RTMP_PACKET_TYPE_VIDEO : RTMP_PACKET_TYPE_AUDIO,
		   m->dts, m->msid, m->body, m->size);
}

int
RTMP_PumpLive(RTMP *r, int64_t now_us)
{
  RTMPLivePacer *p;

  if (!r)
    return RTMP_LIVE_PUMP_ERROR;
  p = r->m_livePacer;
  if (!p || now_us < 0)
    return RTMP_LIVE_PUMP_ERROR;

  for (;;)
    {
      LiveMedia *m;

      /* due times are non-decreasing, so expired media form a prefix */
      while (p->head && now_us > p->head->due_us + p->late_us)
	{
	  m = p->head;
	  media_unlink(p, m);
	  if (m->is_video)
	    p->waiting_keyframe = TRUE;	/* expired video forces recovery */
	  media_free(m);		/* audio expiry only costs itself */
	}

      if (p->waiting_keyframe)
	{
	  LiveMedia *k, **pp;

	  /* earliest keyframe that can still make its playout time */
	  for (k = p->head; k && !(k->is_video && k->is_keyframe); k = k->next)
	    ;
	  if (!k)
	    return RTMP_LIVE_PUMP_IDLE;

	  /* clear the way to it: non-keyframe video is undecodable,
	   * earlier audio must not hold the keyframe back; audio at the
	   * same DTS stays and follows the keyframe */
	  pp = &p->head;
	  while (*pp != k)
	    {
	      LiveMedia *h = *pp;
	      if (!h->is_video && h->dts == k->dts)
		{
		  pp = &h->next;
		  continue;
		}
	      *pp = h->next;
	      p->queued--;
	      media_free(h);
	    }

	  if (now_us < k->due_us)
	    return RTMP_LIVE_PUMP_IDLE;	/* keyframe and audio may wait */

	  /* recovery: the keyframe's held AVC config goes out first */
	  if (!pump_send_config(r, k))
	    return RTMP_LIVE_PUMP_ERROR;
	  p->video_cfg_sent = k->config->serial;
	  if (!pump_send_media(r, k))
	    return RTMP_LIVE_PUMP_ERROR;
	  media_unlink(p, k);
	  media_free(k);
	  p->waiting_keyframe = FALSE;
	  return RTMP_LIVE_PUMP_SENT;
	}

      m = p->head;
      if (!m || now_us < m->due_us)
	return RTMP_LIVE_PUMP_IDLE;

      if (m->is_video && m->config->serial != p->video_cfg_sent)
	{
	  if (!m->is_keyframe)
	    {
	      /* non-keyframes cannot switch config: drop and wait */
	      media_unlink(p, m);
	      media_free(m);
	      p->waiting_keyframe = TRUE;
	      continue;
	    }
	  /* a keyframe may recover onto its own config version */
	  if (!pump_send_config(r, m))
	    return RTMP_LIVE_PUMP_ERROR;
	  p->video_cfg_sent = m->config->serial;
	}
      else if (!m->is_video && m->config->serial != p->audio_cfg_sent)
	{
	  /* top up the AAC config the media was submitted with */
	  if (!pump_send_config(r, m))
	    return RTMP_LIVE_PUMP_ERROR;
	  p->audio_cfg_sent = m->config->serial;
	}

      if (!pump_send_media(r, m))
	return RTMP_LIVE_PUMP_ERROR;
      media_unlink(p, m);
      media_free(m);
      return RTMP_LIVE_PUMP_SENT;
    }
}

void
RTMP_LivePacerClose(RTMP *r)
{
  RTMPLivePacer *p;
  LiveMedia *m;

  if (!r)
    return;
  p = r->m_livePacer;
  if (!p)
    return;
  r->m_livePacer = NULL;

  while ((m = p->head))
    {
      p->head = m->next;
      media_free(m);
    }
  config_unref(p->audio_cfg);
  config_unref(p->video_cfg);
  free(p);
}
