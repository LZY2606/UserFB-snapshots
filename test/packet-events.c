/*
 * Task tests for the fragment-first nestegg_read_event() API.
 *
 * These cases mux small, legal WebM streams entirely in C, compare the
 * reassembled event bodies byte for byte against the legacy
 * nestegg_read_packet() entry point and independently check the committed
 * metadata, streaming and fixed memory behaviour.
 */
#include "../task_support/check.h"

#include "nestegg/nestegg-events.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reuse the ISC licensed C buffer muxer / memory IO fixtures from
   test/indexing.c (and rename its main the same way task_support/baseline.c
   does). */
#define main indexing_fixture_main
#include "indexing.c"
#undef main

/* White box fixed-memory accounting helpers provided by src/nestegg.c. */
size_t ne_event_extra_bytes(void);
size_t ne_event_live_bytes(nestegg * ctx);

#define EV_ID_BLOCK_GROUP     0xa0U
#define EV_ID_BLOCK           0xa1U
#define EV_ID_BLOCK_DURATION  0x9bU
#define EV_ID_REFERENCE       0xfbU
#define EV_ID_DISCARD_PADDING 0x75a2U
#define EV_ID_BLOCK_ADDITIONS 0x75a1U
#define EV_ID_VOID            0xecU
#define EV_ID_UNKNOWN3        0x20deadU

#define EV_CHUNK_MAX          1024U
#define EV_MAX_FRAMES         256U

/* ------------------------------------------------------------------ */
/* C WebM muxer                                                       */
/* ------------------------------------------------------------------ */

struct packet_spec {
  unsigned int cluster;
  uint64_t track_number;
  int rel_tc;
  int group;
  int keyframe;
  unsigned int lace;              /* 0 none, 1 xiph, 2 fixed, 3 ebml */
  size_t lens[EV_MAX_FRAMES];
  unsigned int nframes;
  unsigned int seed;

  int has_duration;
  uint64_t duration_ticks;
  int has_padding;
  int64_t padding;
  int has_reference;
  int64_t reference;

  int empty_duration;             /* emit a zero length element */
  int empty_padding;
  int empty_reference;

  int metadata_before;
  int additions;
};

static unsigned
ev_size_width(uint64_t size)
{
  unsigned int width;

  for (width = 1; width <= 8; ++width) {
    uint64_t limit = (UINT64_C(1) << (7 * width)) - 1;
    if (size < limit)
      break;
  }
  return width;
}

static void
put_empty_element(struct buffer * b, uint32_t id)
{
  buffer_put_id(b, id);
  buffer_put_byte(b, 0x80);       /* length vint encoding zero */
}

static void
put_signed_element(struct buffer * b, uint32_t id, int64_t value)
{
  if (value >= 0) {
    buffer_put_uint(b, id, (uint64_t) value);
    return;
  }
  buffer_put_id(b, id);
  buffer_put_size(b, 8);
  buffer_put_be(b, (uint64_t) value, 8);
}

static void
put_xiph_size_table(struct buffer * b, struct packet_spec const * p)
{
  unsigned int i;

  /* n-1 explicit sizes; the last frame is the remainder. */
  for (i = 0; i + 1 < p->nframes; ++i) {
    size_t size = p->lens[i];
    while (size >= 255) {
      buffer_put_byte(b, 255);
      size -= 255;
    }
    buffer_put_byte(b, (unsigned char) size);
  }
}

static void
put_ebml_delta(struct buffer * b, int64_t delta)
{
  static int64_t const bias[8] = {
    0x3f, 0x1fff, 0xfffff, 0x7ffffff,
    0x3ffffffffLL, 0x1ffffffffffLL,
    0xffffffffffffLL, 0x7fffffffffffffLL
  };
  unsigned int width;

  for (width = 1; width <= 8; ++width) {
    uint64_t u = (uint64_t) (delta + bias[width - 1]);
    uint64_t mask = (UINT64_C(1) << (7 * width)) - 1;
    if (u <= mask) {
      uint64_t encoded = u | (UINT64_C(1) << (7 * width));
      buffer_put_be(b, encoded, width);
      return;
    }
  }
  assert(0 && "signed vint too large");
}

static void
fill_pattern(unsigned char * data, size_t length, unsigned int seed)
{
  uint32_t x = seed ? (uint32_t) seed : 1U;
  size_t i;

  for (i = 0; i < length; ++i) {
    x = x * 1103515245U + 12345U;
    data[i] = (unsigned char) (x >> 16);
  }
}

static void
build_block_payload(struct buffer * block, struct packet_spec const * p)
{
  unsigned char flags;
  unsigned int i;

  buffer_put_vint(block, p->track_number);
  buffer_put_be(block, (uint64_t) (uint16_t) (int16_t) p->rel_tc, 2);

  flags = (unsigned char) ((p->lace & 3U) << 1);
  if (!p->group && p->keyframe)
    flags |= 0x80;
  buffer_put_byte(block, flags);

  if (p->lace != 0)
    buffer_put_byte(block, (unsigned char) (p->nframes - 1));

  if (p->lace == 1) {
    put_xiph_size_table(block, p);
  } else if (p->lace == 3) {
    /* First frame size unsigned, then deltas up to the second to last
       frame; the final frame is the block remainder. */
    buffer_put_vint(block, p->lens[0]);
    for (i = 1; i + 1 < p->nframes; ++i)
      put_ebml_delta(block, (int64_t) p->lens[i] - (int64_t) p->lens[i - 1]);
  }

  for (i = 0; i < p->nframes; ++i) {
    unsigned char * body;
    if (p->lens[i] == 0)
      continue;
    body = malloc(p->lens[i]);
    assert(body);
    fill_pattern(body, p->lens[i], p->seed + i * 17U + 1U);
    buffer_put(block, body, p->lens[i]);
    free(body);
  }
}

static void
put_group_metadata(struct buffer * group, struct packet_spec const * p)
{
  if (p->empty_duration)
    put_empty_element(group, EV_ID_BLOCK_DURATION);
  else if (p->has_duration)
    buffer_put_uint(group, EV_ID_BLOCK_DURATION, p->duration_ticks);

  if (p->empty_padding)
    put_empty_element(group, EV_ID_DISCARD_PADDING);
  else if (p->has_padding)
    put_signed_element(group, EV_ID_DISCARD_PADDING, p->padding);

  if (p->empty_reference)
    put_empty_element(group, EV_ID_REFERENCE);
  else if (p->has_reference)
    put_signed_element(group, EV_ID_REFERENCE, p->reference);
}

static void
put_packet(struct buffer * cluster, struct packet_spec const * p)
{
  struct buffer block = { 0 };
  struct buffer group = { 0 };
  struct buffer additions = { 0 };

  build_block_payload(&block, p);

  if (!p->group) {
    buffer_put_master(cluster, ID_SIMPLE_BLOCK, &block);
  } else {
    if (p->metadata_before)
      put_group_metadata(&group, p);
    buffer_put_master(&group, EV_ID_BLOCK, &block);
    if (!p->metadata_before)
      put_group_metadata(&group, p);
    if (p->additions)
      buffer_put_master(&group, EV_ID_BLOCK_ADDITIONS, &additions);
    buffer_put_master(cluster, EV_ID_BLOCK_GROUP, &group);
  }

  buffer_destroy(&block);
  buffer_destroy(&group);
  buffer_destroy(&additions);
}

/* Build a multi cluster file.  When cluster_abs is non-NULL it receives
   the absolute offset of each Cluster element. */
static void
build_scenario(struct buffer * file, uint64_t timecode_scale,
               uint64_t const * track_numbers, unsigned int track_count,
               uint64_t const * cluster_tc, int const * cluster_unknown,
               unsigned int cluster_count,
               struct packet_spec const * packets, unsigned int packet_count,
               uint64_t * cluster_abs)
{
  struct buffer info = { 0 };
  struct buffer seg = { 0 };
  unsigned int c, i;
  size_t * rel = NULL;
  unsigned int size_width;
  uint64_t seg_payload_abs;

  buffer_put_uint(&info, 0x2ad7b1, timecode_scale);
  buffer_put_master(&seg, 0x1549a966U, &info);
  put_tracks(&seg, track_numbers, track_count);
  buffer_destroy(&info);

  rel = calloc(cluster_count ? cluster_count : 1, sizeof(*rel));
  assert(rel);

  for (c = 0; c < cluster_count; ++c) {
    struct buffer cluster = { 0 };
    static unsigned char const void_payload[5] = { 1, 2, 3, 4, 5 };

    /* Void before Timecode must be skipped while hunting the Timecode. */
    buffer_put_id(&cluster, EV_ID_VOID);
    buffer_put_size(&cluster, sizeof(void_payload));
    buffer_put(&cluster, void_payload, sizeof(void_payload));
    buffer_put_uint(&cluster, ID_TIMECODE, cluster_tc[c]);

    /* Unknown fixed length element after Timecode. */
    buffer_put_id(&cluster, EV_ID_UNKNOWN3);
    buffer_put_size(&cluster, 3);
    buffer_put_byte(&cluster, 0xaa);
    buffer_put_byte(&cluster, 0xbb);
    buffer_put_byte(&cluster, 0xcc);

    for (i = 0; i < packet_count; ++i)
      if (packets[i].cluster == c)
        put_packet(&cluster, &packets[i]);

    rel[c] = seg.length;
    buffer_put_id(&seg, ID_CLUSTER);
    if (cluster_unknown && cluster_unknown[c])
      buffer_put_byte(&seg, 0xff);   /* one byte unknown size */
    else
      buffer_put_size(&seg, cluster.length);
    buffer_put(&seg, cluster.data, cluster.length);
    buffer_destroy(&cluster);

    /* Top level Void between clusters. */
    if (c + 1 < cluster_count) {
      buffer_put_id(&seg, EV_ID_VOID);
      buffer_put_size(&seg, 2);
      buffer_put_byte(&seg, 0);
      buffer_put_byte(&seg, 0);
    }
  }

  put_ebml_header(file);
  size_width = ev_size_width(seg.length);
  seg_payload_abs = (uint64_t) file->length + 4U + size_width;
  if (cluster_abs)
    for (c = 0; c < cluster_count; ++c)
      cluster_abs[c] = seg_payload_abs + (uint64_t) rel[c];

  buffer_put_master(file, ID_SEGMENT, &seg);
  buffer_destroy(&seg);
  free(rel);
}

/* ------------------------------------------------------------------ */
/* Legacy entry point collector for byte comparisons                  */
/* ------------------------------------------------------------------ */

struct old_packet {
  unsigned int track;
  uint64_t tstamp;
  unsigned int nframes;
  unsigned char * data[EV_MAX_FRAMES];
  size_t length[EV_MAX_FRAMES];
  int keyframe;
  uint64_t duration;
  int has_duration;
  int64_t discard;
  int has_discard;
  int64_t reference;
  int has_reference;
};

static void
free_old_packets(struct old_packet * pkts, size_t count)
{
  size_t i;
  unsigned int j;

  if (!pkts)
    return;
  for (i = 0; i < count; ++i)
    for (j = 0; j < pkts[i].nframes; ++j)
      free(pkts[i].data[j]);
  free(pkts);
}

static int
collect_old(struct buffer const * file, struct old_packet ** out,
            size_t * out_count)
{
  struct buffer_io io;
  nestegg * ctx = NULL;
  nestegg_packet * pkt = NULL;
  struct old_packet * pkts = NULL;
  size_t count = 0, capacity = 0;
  int r;

  *out = NULL;
  *out_count = 0;

  if (init_from_buffer(&ctx, file, &io) != 0)
    return -1;

  for (;;) {
    unsigned int track = 0, nframes = 0, j;
    struct old_packet op;

    r = nestegg_read_packet(ctx, &pkt);
    if (r == 0)
      break;
    if (r < 0)
      goto fail_ctx;

    memset(&op, 0, sizeof(op));
    if (nestegg_packet_track(pkt, &track) != 0)
      goto fail_pkt;
    if (nestegg_packet_tstamp(pkt, &op.tstamp) != 0)
      goto fail_pkt;
    if (nestegg_packet_count(pkt, &nframes) != 0 || nframes > EV_MAX_FRAMES)
      goto fail_pkt;
    op.track = track;
    op.nframes = nframes;
    op.keyframe = nestegg_packet_has_keyframe(pkt);

    for (j = 0; j < nframes; ++j) {
      unsigned char * data = NULL;
      size_t length = 0;
      if (nestegg_packet_data(pkt, j, &data, &length) != 0)
        goto fail_pkt;
      op.length[j] = length;
      if (length) {
        op.data[j] = malloc(length);
        if (!op.data[j])
          goto fail_pkt;
        memcpy(op.data[j], data, length);
      }
    }

    {
      uint64_t duration;
      int64_t value;
      if (nestegg_packet_duration(pkt, &duration) == 0) {
        op.has_duration = 1;
        op.duration = duration;
      }
      if (nestegg_packet_discard_padding(pkt, &value) == 0) {
        op.has_discard = 1;
        op.discard = value;
      }
      if (nestegg_packet_reference_block(pkt, &value) == 0) {
        op.has_reference = 1;
        op.reference = value;
      }
    }

    if (count == capacity) {
      struct old_packet * grown;
      capacity = capacity ? capacity * 2 : 8;
      grown = realloc(pkts, capacity * sizeof(*pkts));
      if (!grown)
        goto fail_pkt;
      pkts = grown;
    }
    pkts[count++] = op;
    nestegg_free_packet(pkt);
    pkt = NULL;
  }

  nestegg_destroy(ctx);
  *out = pkts;
  *out_count = count;
  return 0;

fail_pkt:
  nestegg_free_packet(pkt);
fail_ctx:
  nestegg_destroy(ctx);
  free_old_packets(pkts, count);
  return -1;
}

/* ------------------------------------------------------------------ */
/* Rich fragments + metadata cross check                              */
/* ------------------------------------------------------------------ */

static int
case_rich(void)
{
  uint64_t numbers[3] = { 1, 5, 17 };
  uint64_t tcs[3] = { 20, 100, 0 };
  int unknown[3] = { 0, 1, 0 };
  uint64_t const scale = 1000000ULL;

  struct packet_spec packets[8];
  struct buffer file = { 0 };
  struct old_packet * old = NULL;
  size_t old_count = 0;

  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;

  unsigned char * acc[EV_MAX_FRAMES];
  size_t alen[EV_MAX_FRAMES];
  unsigned int frags[EV_MAX_FRAMES];
  int last_seen[EV_MAX_FRAMES];

  size_t pi = 0;
  unsigned int k;
  uint64_t active_id = 0, next_id = 1;
  unsigned int commits = 0;
  int terminal;

  memset(packets, 0, sizeof(packets));
  memset(acc, 0, sizeof(acc));
  memset(alen, 0, sizeof(alen));
  memset(frags, 0, sizeof(frags));
  memset(last_seen, 0, sizeof(last_seen));

  /* p0: negative relative timecode, SimpleBlock keyframe, no lacing. */
  packets[0].cluster = 0;
  packets[0].track_number = 1;
  packets[0].rel_tc = -2;
  packets[0].keyframe = 1;
  packets[0].lace = 0;
  packets[0].nframes = 1;
  packets[0].lens[0] = 64;
  packets[0].seed = 11;

  /* p1: Xiph lacing on track index 1. */
  packets[1].cluster = 0;
  packets[1].track_number = 5;
  packets[1].rel_tc = 5;
  packets[1].lace = 1;
  packets[1].nframes = 3;
  packets[1].lens[0] = 100;
  packets[1].lens[1] = 300;
  packets[1].lens[2] = 7;
  packets[1].seed = 23;

  /* p2: EBML lacing with a >1024 frame, metadata after the Block. */
  packets[2].cluster = 0;
  packets[2].track_number = 17;
  packets[2].rel_tc = 0;
  packets[2].group = 1;
  packets[2].lace = 3;
  packets[2].nframes = 3;
  packets[2].lens[0] = 50;
  packets[2].lens[1] = 1200;
  packets[2].lens[2] = 33;
  packets[2].seed = 37;
  packets[2].has_duration = 1;
  packets[2].duration_ticks = 4;
  packets[2].has_padding = 1;
  packets[2].padding = 200000;
  packets[2].has_reference = 1;
  packets[2].reference = 0;

  /* p3: Fixed lacing, metadata before the Block, no reference. */
  packets[3].cluster = 1;
  packets[3].track_number = 1;
  packets[3].rel_tc = -10;
  packets[3].group = 1;
  packets[3].lace = 2;
  packets[3].nframes = 4;
  packets[3].lens[0] = packets[3].lens[1] = 700;
  packets[3].lens[2] = packets[3].lens[3] = 700;
  packets[3].seed = 41;
  packets[3].has_duration = 1;
  packets[3].duration_ticks = 2;
  packets[3].metadata_before = 1;

  /* p4: EBML lacing containing empty frames, SimpleBlock keyframe. */
  packets[4].cluster = 1;
  packets[4].track_number = 5;
  packets[4].rel_tc = 3;
  packets[4].keyframe = 1;
  packets[4].lace = 3;
  packets[4].nframes = 4;
  packets[4].lens[0] = 0;
  packets[4].lens[1] = 15;
  packets[4].lens[2] = 0;
  packets[4].lens[3] = 8;
  packets[4].seed = 53;

  /* p5: Xiph lacing with empty and 255 continuation sizes, signed
     metadata. */
  packets[5].cluster = 1;
  packets[5].track_number = 17;
  packets[5].rel_tc = 0;
  packets[5].group = 1;
  packets[5].lace = 1;
  packets[5].nframes = 4;
  packets[5].lens[0] = 0;
  packets[5].lens[1] = 255;
  packets[5].lens[2] = 300;
  packets[5].lens[3] = 1;
  packets[5].seed = 67;
  packets[5].has_duration = 1;
  packets[5].duration_ticks = 3;
  packets[5].has_padding = 1;
  packets[5].padding = -123456789;
  packets[5].has_reference = 1;
  packets[5].reference = -5;

  /* p6: Fixed lacing on the final known size cluster. */
  packets[6].cluster = 2;
  packets[6].track_number = 1;
  packets[6].rel_tc = 0;
  packets[6].lace = 2;
  packets[6].nframes = 3;
  packets[6].lens[0] = packets[6].lens[1] = 10;
  packets[6].lens[2] = 10;
  packets[6].seed = 71;

  /* p7: plain BlockGroup with no metadata at all. */
  packets[7].cluster = 2;
  packets[7].track_number = 5;
  packets[7].rel_tc = 12;
  packets[7].group = 1;
  packets[7].lace = 0;
  packets[7].nframes = 1;
  packets[7].lens[0] = 123;
  packets[7].seed = 83;

  build_scenario(&file, scale, numbers, 3, tcs, unknown, 3,
                 packets, 8, NULL);

  TASK_CHECK(collect_old(&file, &old, &old_count) == 0);
  TASK_CHECK(old_count == 8);

  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);

  for (;;) {
    int r = nestegg_read_event(ctx, &ev);
    if (r <= 0) {
      terminal = r;
      TASK_CHECK(r == 0);
      TASK_CHECK(ev == NULL);
      break;
    }
    TASK_CHECK(r == 1);
    TASK_CHECK(ev != NULL);

    if (ev->type == NESTEGG_EVENT_DATA) {
      unsigned int fi = ev->frame_index;
      struct packet_spec const * p;

      TASK_CHECK(pi < 8);
      p = &packets[pi];

      if (active_id == 0) {
        active_id = ev->packet_id;
        TASK_CHECK(active_id == next_id);
      } else {
        TASK_CHECK(ev->packet_id == active_id);
      }

      TASK_CHECK(ev->frame_count == p->nframes);
      TASK_CHECK(ev->frame_count == old[pi].nframes);
      TASK_CHECK(fi < ev->frame_count);
      TASK_CHECK(fi < EV_MAX_FRAMES);
      TASK_CHECK(!last_seen[fi]);

      /* Fragment geometry. */
      TASK_CHECK(ev->length <= EV_CHUNK_MAX);
      TASK_CHECK(ev->first == (frags[fi] == 0));
      if (ev->length == 0) {
        TASK_CHECK(ev->first == 1);
        TASK_CHECK(ev->last == 1);
      } else if (!ev->last) {
        TASK_CHECK(ev->length == EV_CHUNK_MAX);
      }
      TASK_CHECK(ev->data != NULL);

      /* Final metadata must stay cleared on DATA. */
      TASK_CHECK(ev->keyframe == NESTEGG_PACKET_HAS_KEYFRAME_UNKNOWN);
      TASK_CHECK(ev->duration == 0);
      TASK_CHECK(ev->has_duration == 0);
      TASK_CHECK(ev->has_discard_padding == 0);
      TASK_CHECK(ev->has_reference == 0);
      TASK_CHECK(ev->discard_padding == 0);
      TASK_CHECK(ev->reference == 0);

      if (ev->length) {
        unsigned char * grown = realloc(acc[fi], alen[fi] + ev->length);
        TASK_CHECK(grown != NULL);
        acc[fi] = grown;
        memcpy(acc[fi] + alen[fi], ev->data, ev->length);
        alen[fi] += ev->length;
      }
      frags[fi]++;
      if (ev->last)
        last_seen[fi] = 1;
    } else {
      struct packet_spec const * p;
      unsigned int fi;

      TASK_CHECK(ev->type == NESTEGG_EVENT_COMMIT);
      TASK_CHECK(pi < 8);
      p = &packets[pi];
      TASK_CHECK(active_id == ev->packet_id);

      /* COMMIT shape. */
      TASK_CHECK(ev->length == 0);
      TASK_CHECK(ev->data == NULL);
      TASK_CHECK(ev->frame_index == 0);
      TASK_CHECK(ev->first == 0);
      TASK_CHECK(ev->last == 0);
      TASK_CHECK(ev->frame_count == p->nframes);

      /* Identity fields. */
      TASK_CHECK(ev->track == old[pi].track);
      TASK_CHECK(ev->timestamp == old[pi].tstamp);
      TASK_CHECK(ev->track == (p->track_number == 1 ? 0 :
                               p->track_number == 5 ? 1 : 2));

      /* Every frame completed and reassembles byte identically. */
      for (fi = 0; fi < ev->frame_count; ++fi) {
        TASK_CHECK(last_seen[fi]);
        TASK_CHECK(alen[fi] == old[pi].length[fi]);
        TASK_CHECK(alen[fi] == p->lens[fi]);
        if (alen[fi])
          TASK_CHECK(memcmp(acc[fi], old[pi].data[fi], alen[fi]) == 0);
        free(acc[fi]);
        acc[fi] = NULL;
        alen[fi] = 0;
        frags[fi] = 0;
        last_seen[fi] = 0;
      }

      /* Metadata versus independent expectations. */
      TASK_CHECK(ev->has_duration == p->has_duration);
      TASK_CHECK(ev->has_discard_padding == p->has_padding);
      TASK_CHECK(ev->has_reference == p->has_reference);
      if (p->has_duration)
        TASK_CHECK(ev->duration == p->duration_ticks * scale);
      if (p->has_padding)
        TASK_CHECK(ev->discard_padding == p->padding);
      if (p->has_reference)
        TASK_CHECK(ev->reference == p->reference);

      /* Metadata agrees with the legacy accessors where they expose
         presence. */
      TASK_CHECK(ev->has_duration == old[pi].has_duration);
      TASK_CHECK(ev->has_discard_padding == old[pi].has_discard);
      TASK_CHECK(ev->has_reference == old[pi].has_reference);
      if (old[pi].has_duration)
        TASK_CHECK(ev->duration == old[pi].duration);
      if (old[pi].has_discard)
        TASK_CHECK(ev->discard_padding == old[pi].discard);
      if (old[pi].has_reference)
        TASK_CHECK(ev->reference == old[pi].reference);

      if (!p->group) {
        TASK_CHECK(ev->has_duration == 0);
        TASK_CHECK(ev->has_discard_padding == 0);
        TASK_CHECK(ev->has_reference == 0);
        TASK_CHECK(ev->keyframe == (p->keyframe ?
                                    NESTEGG_PACKET_HAS_KEYFRAME_TRUE :
                                    NESTEGG_PACKET_HAS_KEYFRAME_FALSE));
      } else {
        TASK_CHECK(ev->keyframe == (p->has_reference ?
                                    NESTEGG_PACKET_HAS_KEYFRAME_FALSE :
                                    NESTEGG_PACKET_HAS_KEYFRAME_TRUE));
      }

      for (k = 0; k < EV_MAX_FRAMES; ++k) {
        TASK_CHECK(acc[k] == NULL);
        TASK_CHECK(alen[k] == 0);
      }

      commits++;
      pi++;
      active_id = 0;
      next_id++;
    }
  }

  TASK_CHECK(commits == 8);
  TASK_CHECK(pi == old_count);

  /* Sticky normal end. */
  ev = (void *) 1;
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 0);
  TASK_CHECK(ev == NULL);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 0);
  TASK_CHECK(ev == NULL);

  nestegg_destroy(ctx);
  free_old_packets(old, old_count);
  buffer_destroy(&file);
  (void) terminal;
  return 0;
}

/* ------------------------------------------------------------------ */
/* >32 KiB early delivery + IO high water mark                        */
/* ------------------------------------------------------------------ */

struct wm_io {
  unsigned char const * data;
  size_t length;
  size_t offset;
  size_t peak;
};

static int64_t
wm_read(void * destination, size_t length, void * userdata)
{
  struct wm_io * io = userdata;
  size_t available;

  available = io->length - io->offset;
  if (length > available)
    length = available;
  if (length == 0)
    return 0;
  memcpy(destination, io->data + io->offset, length);
  io->offset += length;
  if (io->offset > io->peak)
    io->peak = io->offset;
  return (int64_t) length;
}

static int
wm_seek(int64_t offset, int whence, void * userdata)
{
  struct wm_io * io = userdata;
  int64_t position;

  if (whence == NESTEGG_SEEK_SET)
    position = offset;
  else if (whence == NESTEGG_SEEK_CUR)
    position = (int64_t) io->offset + offset;
  else if (whence == NESTEGG_SEEK_END)
    position = (int64_t) io->length + offset;
  else
    return -1;
  if (position < 0 || (uint64_t) position > io->length)
    return -1;
  io->offset = (size_t) position;
  return 0;
}

static int64_t
wm_tell(void * userdata)
{
  struct wm_io * io = userdata;
  return (int64_t) io->offset;
}

static int
case_large_early(void)
{
  enum { FRAME_LEN = 40000 };
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[1] = { 0 };
  int unknown[1] = { 0 };
  struct packet_spec p;
  struct buffer file = { 0 };
  struct wm_io wm;
  nestegg_io nio;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;
  unsigned char * expected;
  unsigned char * rebuilt = NULL;
  size_t rebuilt_len = 0;
  int chunks = 0, commits = 0;

  memset(&p, 0, sizeof(p));
  p.cluster = 0;
  p.track_number = 1;
  p.keyframe = 1;
  p.lace = 0;
  p.nframes = 1;
  p.lens[0] = FRAME_LEN;
  p.seed = 99;

  build_scenario(&file, 1000000, numbers, 1, tcs, unknown, 1, &p, 1, NULL);
  TASK_CHECK(file.length > FRAME_LEN);

  expected = malloc(FRAME_LEN);
  TASK_CHECK(expected != NULL);
  fill_pattern(expected, FRAME_LEN, 100);

  memset(&wm, 0, sizeof(wm));
  wm.data = file.data;
  wm.length = file.length;
  memset(&nio, 0, sizeof(nio));
  nio.read = wm_read;
  nio.seek = wm_seek;
  nio.tell = wm_tell;
  nio.userdata = &wm;
  TASK_CHECK(nestegg_init(&ctx, nio, NULL, -1) == 0);

  /* The very first call must hand over the first fragment without having
     pulled the whole 40 KiB frame through the user IO callback. */
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->length == EV_CHUNK_MAX);
  TASK_CHECK(ev->first == 1);
  TASK_CHECK(ev->last == 0);
  TASK_CHECK(ev->frame_index == 0);
  TASK_CHECK(ev->frame_count == 1);
  TASK_CHECK(wm.peak < file.length);
  TASK_CHECK(file.length - wm.peak >= 16384);
  TASK_CHECK(memcmp(ev->data, expected, EV_CHUNK_MAX) == 0);
  rebuilt = malloc(EV_CHUNK_MAX);
  TASK_CHECK(rebuilt != NULL);
  memcpy(rebuilt, ev->data, EV_CHUNK_MAX);
  rebuilt_len = EV_CHUNK_MAX;
  chunks++;

  for (;;) {
    int r = nestegg_read_event(ctx, &ev);
    TASK_CHECK(r == 1 || r == 0);
    if (r == 0) {
      TASK_CHECK(ev == NULL);
      break;
    }
    if (ev->type == NESTEGG_EVENT_DATA) {
      unsigned char * grown = realloc(rebuilt, rebuilt_len + ev->length);
      TASK_CHECK(grown != NULL);
      rebuilt = grown;
      memcpy(rebuilt + rebuilt_len, ev->data, ev->length);
      rebuilt_len += ev->length;
      chunks++;
    } else {
      TASK_CHECK(ev->type == NESTEGG_EVENT_COMMIT);
      TASK_CHECK(ev->keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);
      commits++;
    }
  }

  /* 40000 = 39*1024 + 64 -> 40 fragments. */
  TASK_CHECK(chunks == 40);
  TASK_CHECK(commits == 1);
  TASK_CHECK(rebuilt_len == FRAME_LEN);
  TASK_CHECK(memcmp(rebuilt, expected, FRAME_LEN) == 0);

  nestegg_destroy(ctx);
  free(rebuilt);
  free(expected);
  buffer_destroy(&file);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Truncation and unsupported groups                                  */
/* ------------------------------------------------------------------ */

static int
run_expect_uncommitted_error(struct buffer const * file)
{
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;
  int data_events = 0;
  int commits = 0;
  int r;

  if (init_from_buffer(&ctx, file, &io) != 0)
    return -100;

  for (;;) {
    r = nestegg_read_event(ctx, &ev);
    if (r == -1)
      break;
    if (r == 0) {
      nestegg_destroy(ctx);
      return -101;          /* expected an error, got clean end */
    }
    if (ev->type == NESTEGG_EVENT_DATA)
      data_events++;
    else if (ev->type == NESTEGG_EVENT_COMMIT)
      commits++;
  }

  if (ev != NULL) {
    nestegg_destroy(ctx);
    return -102;
  }
  if (data_events < 1) {
    nestegg_destroy(ctx);
    return -103;
  }
  if (commits != 0) {
    nestegg_destroy(ctx);
    return -104;
  }

  /* Error is sticky. */
  if (nestegg_read_event(ctx, &ev) != -1 || ev != NULL) {
    nestegg_destroy(ctx);
    return -105;
  }
  if (nestegg_read_event(ctx, &ev) != -1 || ev != NULL) {
    nestegg_destroy(ctx);
    return -106;
  }

  nestegg_destroy(ctx);
  return 0;
}

static int
case_truncated_tail(void)
{
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[1] = { 0 };
  struct packet_spec p;
  struct buffer full = { 0 };
  struct buffer truncated;

  memset(&p, 0, sizeof(p));
  p.track_number = 1;
  p.group = 1;
  p.lace = 0;
  p.nframes = 1;
  p.lens[0] = 64;
  p.seed = 7;
  p.has_padding = 1;
  p.padding = -987654321LL;     /* encoded as 8 bytes at file tail */

  build_scenario(&full, 1000000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);
  TASK_CHECK(full.length > 3);

  /* Remove the last three bytes of the eight byte DiscardPadding value. */
  truncated.data = full.data;
  truncated.length = full.length - 3;
  truncated.capacity = full.capacity;

  TASK_CHECK(run_expect_uncommitted_error(&truncated) == 0);

  buffer_destroy(&full);
  return 0;
}

static int
case_truncated_body(void)
{
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[1] = { 0 };
  struct packet_spec p;
  struct buffer full = { 0 };
  struct buffer truncated;

  memset(&p, 0, sizeof(p));
  p.track_number = 1;
  p.lace = 0;
  p.nframes = 1;
  p.lens[0] = 5000;
  p.seed = 8;

  build_scenario(&full, 1000000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);

  /* Drop the final 2000 frame bytes. */
  truncated.data = full.data;
  truncated.length = full.length - 2000;
  truncated.capacity = full.capacity;

  TASK_CHECK(run_expect_uncommitted_error(&truncated) == 0);

  buffer_destroy(&full);
  return 0;
}

static int
case_unsupported_additions(void)
{
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[1] = { 0 };
  struct packet_spec p;
  struct buffer file = { 0 };

  memset(&p, 0, sizeof(p));
  p.track_number = 1;
  p.group = 1;
  p.lace = 0;
  p.nframes = 1;
  p.lens[0] = 32;
  p.seed = 9;
  p.additions = 1;

  build_scenario(&file, 1000000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);
  TASK_CHECK(run_expect_uncommitted_error(&file) == 0);

  buffer_destroy(&file);
  return 0;
}

static int
case_unsupported_encoding(void)
{
  struct buffer file = { 0 };
  struct buffer segment = { 0 };
  struct buffer info = { 0 };
  struct buffer tracks = { 0 };
  struct buffer entry = { 0 };
  struct buffer encodings = { 0 };
  struct buffer encoding = { 0 };
  struct buffer cluster = { 0 };
  struct buffer block = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;

  /* TrackEntry carrying an empty-but-present ContentEncoding. */
  buffer_put_uint(&entry, ID_TRACK_NUMBER, 1);
  buffer_put_uint(&entry, ID_TRACK_UID, 1);
  buffer_put_uint(&entry, ID_TRACK_TYPE, 1);
  buffer_put_string(&entry, ID_CODEC_ID, "V_VP8");
  buffer_put_master(&encodings, 0x6240U, &encoding);
  buffer_put_master(&entry, 0x6d80U, &encodings);
  buffer_put_master(&tracks, ID_TRACK_ENTRY, &entry);

  buffer_put_uint(&info, 0x2ad7b1, 1000000);
  buffer_put_master(&segment, 0x1549a966U, &info);
  buffer_put_master(&segment, ID_TRACKS, &tracks);

  buffer_put_uint(&cluster, ID_TIMECODE, 0);
  buffer_put_vint(&block, 1);
  buffer_put_be(&block, 0, 2);
  buffer_put_byte(&block, 0x80);
  buffer_put(&block, "abc", 3);
  buffer_put_master(&cluster, ID_SIMPLE_BLOCK, &block);
  buffer_put_master(&segment, ID_CLUSTER, &cluster);

  put_ebml_header(&file);
  buffer_put_master(&file, ID_SEGMENT, &segment);

  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == -1);
  TASK_CHECK(ev == NULL);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == -1);
  TASK_CHECK(ev == NULL);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  buffer_destroy(&segment);
  buffer_destroy(&info);
  buffer_destroy(&tracks);
  buffer_destroy(&entry);
  buffer_destroy(&encodings);
  buffer_destroy(&encoding);
  buffer_destroy(&cluster);
  buffer_destroy(&block);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Seek drops uncommitted fragments                                   */
/* ------------------------------------------------------------------ */

static void
build_seek_file(struct buffer * file, uint64_t * cluster_abs)
{
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[3] = { 0, 40, 90 };
  struct packet_spec packets[3];

  memset(packets, 0, sizeof(packets));

  packets[0].cluster = 0;
  packets[0].track_number = 1;
  packets[0].keyframe = 1;
  packets[0].nframes = 1;
  packets[0].lens[0] = 100;
  packets[0].seed = 201;

  packets[1].cluster = 1;
  packets[1].track_number = 1;
  packets[1].keyframe = 1;
  packets[1].nframes = 1;
  packets[1].lens[0] = 3000;
  packets[1].seed = 202;

  packets[2].cluster = 2;
  packets[2].track_number = 1;
  packets[2].keyframe = 1;
  packets[2].nframes = 1;
  packets[2].lens[0] = 120;
  packets[2].seed = 203;

  build_scenario(file, 1000000, numbers, 1, tcs, NULL, 3,
                 packets, 3, cluster_abs);
}

static int
read_single_packet(nestegg * ctx, uint64_t expect_id,
                   unsigned char const * expect, size_t expect_len,
                   uint64_t expect_tstamp)
{
  const nestegg_event * ev = NULL;
  unsigned char rebuilt[4096];
  size_t rebuilt_len = 0;
  int got_data = 0, r;

  memset(rebuilt, 0, sizeof(rebuilt));

  for (;;) {
    r = nestegg_read_event(ctx, &ev);
    if (r != 1)
      return -1;
    if (ev->packet_id != expect_id)
      return -2;
    if (ev->timestamp != expect_tstamp)
      return -3;
    if (ev->track != 0 || ev->frame_count != 1)
      return -4;

    if (ev->type == NESTEGG_EVENT_DATA) {
      if (ev->frame_index != 0)
        return -5;
      if (rebuilt_len + ev->length > sizeof(rebuilt))
        return -6;
      memcpy(rebuilt + rebuilt_len, ev->data, ev->length);
      rebuilt_len += ev->length;
      got_data = 1;
    } else {
      if (ev->type != NESTEGG_EVENT_COMMIT)
        return -7;
      if (!got_data || rebuilt_len != expect_len)
        return -8;
      if (memcmp(rebuilt, expect, expect_len) != 0)
        return -9;
      return 0;
    }
  }
}

static int
case_seek_offset(void)
{
  struct buffer file = { 0 };
  uint64_t abs[3] = { 0, 0, 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;
  unsigned char small[100], big[3000], last[120];

  build_seek_file(&file, abs);
  /* The muxer seeds frame i with p.seed + i*17 + 1. */
  fill_pattern(small, sizeof(small), 201 + 1);
  fill_pattern(big, sizeof(big), 202 + 1);
  fill_pattern(last, sizeof(last), 203 + 1);

  TASK_CHECK(abs[0] < abs[1] && abs[1] < abs[2]);
  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);

  /* Packet A. */
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->packet_id == 1);
  TASK_CHECK(ev->first == 1 && ev->last == 1);
  TASK_CHECK(ev->length == 100);
  TASK_CHECK(memcmp(ev->data, small, 100) == 0);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_COMMIT);
  TASK_CHECK(ev->packet_id == 1);

  /* Start packet B, deliver only its first fragment, then seek away
     mid-frame without reading its COMMIT. */
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->packet_id == 2);
  TASK_CHECK(ev->length == 1024);
  TASK_CHECK(ev->first == 1 && ev->last == 0);

  TASK_CHECK(nestegg_offset_seek(ctx, abs[2]) == 0);

  /* First event after seek must be a fresh first fragment of packet C with
     a strictly increasing id, never a stale B continuation. */
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->packet_id == 3);
  TASK_CHECK(ev->frame_index == 0);
  TASK_CHECK(ev->first == 1);
  TASK_CHECK(ev->last == 1);
  TASK_CHECK(ev->length == 120);
  TASK_CHECK(ev->timestamp == 90 * 1000000ULL);
  TASK_CHECK(memcmp(ev->data, last, 120) == 0);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_COMMIT);
  TASK_CHECK(ev->packet_id == 3);

  /* Seek back into the middle file and ensure B restarts from its first
     fragment and reassembles completely. */
  TASK_CHECK(nestegg_offset_seek(ctx, abs[1]) == 0);
  TASK_CHECK(read_single_packet(ctx, 4, big, sizeof(big),
                                40 * 1000000ULL) == 0);

  /* Seek back to the first cluster. */
  TASK_CHECK(nestegg_offset_seek(ctx, abs[0]) == 0);
  TASK_CHECK(read_single_packet(ctx, 5, small, sizeof(small),
                                0) == 0);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

static int
case_seek_track_cue(void)
{
  uint64_t numbers[3] = { 1, 17, UINT64_C(0x100000001) };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;
  uint64_t first_id;

  build_indexed_file(&file, numbers, 3);
  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);

  /* First packet belongs to track index 2. */
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->track == 2);
  first_id = ev->packet_id;

  /* Seek without consuming the fragment or COMMIT. */
  TASK_CHECK(nestegg_track_seek(ctx, 2, 0) == 0);

  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->track == 2);
  TASK_CHECK(ev->packet_id > first_id);
  TASK_CHECK(ev->frame_index == 0);
  TASK_CHECK(ev->first == 1);
  TASK_CHECK(ev->length >= 1);
  TASK_CHECK(ev->data[0] == 0);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Fixed memory bound                                                 */
/* ------------------------------------------------------------------ */

static int
case_memory_bound(void)
{
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[1] = { 0 };
  struct packet_spec p;
  struct buffer small = { 0 };
  struct buffer huge = { 0 };
  nestegg * ctx = NULL;
  struct buffer_io io;
  const nestegg_event * ev = NULL;
  size_t live_small, live_huge;
  unsigned int i;

  /* Event state plus conservative new stack usage stays under 8 KiB. */
  TASK_CHECK(ne_event_extra_bytes() <= 8192);
  TASK_CHECK(ne_event_extra_bytes() >= sizeof(nestegg_event));

  memset(&p, 0, sizeof(p));
  p.track_number = 1;
  p.keyframe = 1;
  p.nframes = 1;
  p.lens[0] = 64;
  p.seed = 1;
  build_scenario(&small, 1000000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);

  TASK_CHECK(init_from_buffer(&ctx, &small, &io) == 0);
  TASK_CHECK(ne_event_live_bytes(ctx) == 0);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  live_small = ne_event_live_bytes(ctx);
  TASK_CHECK(live_small > 0);
  TASK_CHECK(live_small <= 8192);
  TASK_CHECK(live_small <= ne_event_extra_bytes());

  /* Drain everything; the live fixed state does not grow. */
  for (i = 0; i < 100; ++i) {
    int r = nestegg_read_event(ctx, &ev);
    if (r == 0)
      break;
    TASK_CHECK(r == 1 || r == 0);
  }
  TASK_CHECK(ne_event_live_bytes(ctx) == live_small);
  nestegg_destroy(ctx);
  buffer_destroy(&small);

  /* A much larger frame uses the same fixed size state. */
  p.lens[0] = 200000;
  build_scenario(&huge, 1000000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);
  TASK_CHECK(init_from_buffer(&ctx, &huge, &io) == 0);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  live_huge = ne_event_live_bytes(ctx);
  TASK_CHECK(live_huge == live_small);
  TASK_CHECK(live_huge <= 8192);
  nestegg_destroy(ctx);
  buffer_destroy(&huge);

  return 0;
}

/* ------------------------------------------------------------------ */
/* Existing indexed fixture (256 frame fixed lace) cross check        */
/* ------------------------------------------------------------------ */

static int
case_indexed_fixture(void)
{
  uint64_t numbers[3] = { 1, 17, UINT64_C(0x100000001) };
  struct buffer file = { 0 };
  struct old_packet * old = NULL;
  size_t old_count = 0;
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;
  size_t pi = 0;
  unsigned char * acc = NULL;
  size_t acc_len = 0;
  unsigned int fi_seen = 0;
  uint64_t id = 0;
  int in_packet = 0;

  build_indexed_file(&file, numbers, 3);
  TASK_CHECK(collect_old(&file, &old, &old_count) == 0);
  TASK_CHECK(old_count == 2);

  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);

  for (;;) {
    int r = nestegg_read_event(ctx, &ev);
    TASK_CHECK(r == 1 || r == 0);
    if (r == 0) {
      TASK_CHECK(ev == NULL);
      break;
    }

    if (ev->type == NESTEGG_EVENT_DATA) {
      TASK_CHECK(pi < old_count);
      if (!in_packet) {
        in_packet = 1;
        id = ev->packet_id;
        fi_seen = 0;
        acc_len = 0;
      }
      TASK_CHECK(ev->packet_id == id);
      TASK_CHECK(ev->frame_count == old[pi].nframes);
      TASK_CHECK(ev->frame_index < ev->frame_count);
      TASK_CHECK(ev->frame_index == fi_seen);

      if (ev->length) {
        unsigned char * grown = realloc(acc, acc_len + ev->length);
        TASK_CHECK(grown != NULL);
        acc = grown;
        memcpy(acc + acc_len, ev->data, ev->length);
        acc_len += ev->length;
      }

      if (ev->last) {
        unsigned int fi = ev->frame_index;
        TASK_CHECK(ev->first || old[pi].length[fi] > EV_CHUNK_MAX);
        TASK_CHECK(acc_len == old[pi].length[fi]);
        if (acc_len)
          TASK_CHECK(memcmp(acc, old[pi].data[fi], acc_len) == 0);
        free(acc);
        acc = NULL;
        acc_len = 0;
        fi_seen++;
      } else {
        TASK_CHECK(ev->length == EV_CHUNK_MAX);
      }
    } else {
      TASK_CHECK(ev->type == NESTEGG_EVENT_COMMIT);
      TASK_CHECK(in_packet);
      TASK_CHECK(ev->packet_id == id);
      TASK_CHECK(fi_seen == old[pi].nframes);
      TASK_CHECK(ev->track == old[pi].track);
      TASK_CHECK(ev->timestamp == old[pi].tstamp);
      pi++;
      in_packet = 0;
    }
  }

  TASK_CHECK(pi == old_count);
  free(acc);
  nestegg_destroy(ctx);
  free_old_packets(old, old_count);
  buffer_destroy(&file);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Zero length bodies and zero length metadata elements               */
/* ------------------------------------------------------------------ */

static int
case_zero_edges(void)
{
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[1] = { 7 };
  struct packet_spec p;
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;

  /* Zero payload SimpleBlock: one empty DATA fragment. */
  memset(&p, 0, sizeof(p));
  p.track_number = 1;
  p.keyframe = 1;
  p.nframes = 1;
  p.lens[0] = 0;
  build_scenario(&file, 1000000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);
  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);

  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->length == 0);
  TASK_CHECK(ev->first == 1 && ev->last == 1);
  TASK_CHECK(ev->frame_index == 0 && ev->frame_count == 1);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_COMMIT);
  TASK_CHECK(ev->keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 0 && ev == NULL);
  nestegg_destroy(ctx);
  buffer_destroy(&file);

  /* All three metadata elements present but zero length: present with
     value zero; an empty ReferenceBlock still suppresses keyframe. */
  tcs[0] = 3;
  memset(&p, 0, sizeof(p));
  p.track_number = 1;
  p.group = 1;
  p.nframes = 1;
  p.lens[0] = 4;
  p.empty_duration = 1;
  p.empty_padding = 1;
  p.empty_reference = 1;
  p.metadata_before = 1;
  build_scenario(&file, 500000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);
  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);

  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_COMMIT);
  TASK_CHECK(ev->has_duration == 1);
  TASK_CHECK(ev->has_discard_padding == 1);
  TASK_CHECK(ev->has_reference == 1);
  TASK_CHECK(ev->duration == 0);
  TASK_CHECK(ev->discard_padding == 0);
  TASK_CHECK(ev->reference == 0);
  TASK_CHECK(ev->keyframe == NESTEGG_PACKET_HAS_KEYFRAME_FALSE);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 0);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* A >32 KiB frame inside a BlockGroup must also stream its first fragment
   early and then pick up trailing metadata on COMMIT. */
static int
case_large_group_frame(void)
{
  enum { LEN = 33000 };
  uint64_t numbers[1] = { 1 };
  uint64_t tcs[1] = { 1 };
  struct packet_spec p;
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev = NULL;
  unsigned char * expected;
  size_t total = 0;
  int data_events = 0;

  memset(&p, 0, sizeof(p));
  p.track_number = 1;
  p.group = 1;
  p.nframes = 1;
  p.lens[0] = LEN;
  p.seed = 313;
  p.has_duration = 1;
  p.duration_ticks = 2;
  build_scenario(&file, 1000000, numbers, 1, tcs, NULL, 1, &p, 1, NULL);

  expected = malloc(LEN);
  TASK_CHECK(expected != NULL);
  fill_pattern(expected, LEN, p.seed + 1);

  TASK_CHECK(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(nestegg_read_event(ctx, &ev) == 1);
  TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
  TASK_CHECK(ev->length == 1024 && ev->first == 1 && ev->last == 0);
  TASK_CHECK(memcmp(ev->data, expected, 1024) == 0);
  total += ev->length;
  data_events++;

  for (;;) {
    int r = nestegg_read_event(ctx, &ev);
    TASK_CHECK(r == 1 || r == 0);
    if (r == 0)
      break;
    if (ev->type == NESTEGG_EVENT_DATA) {
      TASK_CHECK(total + ev->length <= (size_t) LEN);
      TASK_CHECK(memcmp(ev->data, expected + total, ev->length) == 0);
      total += ev->length;
      data_events++;
    } else {
      TASK_CHECK(total == LEN);
      TASK_CHECK(data_events == 33); /* 32*1024 + 232 */
      TASK_CHECK(ev->has_duration == 1);
      TASK_CHECK(ev->duration == 2000000);
      TASK_CHECK(ev->keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);
    }
  }

  nestegg_destroy(ctx);
  free(expected);
  buffer_destroy(&file);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Task case registry                                                 */
/* ------------------------------------------------------------------ */

struct task_case const task_cases[] = {
  { "fragments", "lacing_reassembly", case_rich },
  { "fragments", "indexed_256_lace", case_indexed_fixture },
  { "fragments", "large_frame_early_delivery", case_large_early },
  { "fragments", "large_group_frame_streaming", case_large_group_frame },
  { "fragments", "zero_length_edges", case_zero_edges },
  { "metadata", "truncated_group_tail_no_commit", case_truncated_tail },
  { "metadata", "truncated_body_no_commit", case_truncated_body },
  { "metadata", "block_additions_rejected", case_unsupported_additions },
  { "metadata", "content_encoding_rejected", case_unsupported_encoding },
  { "seek_memory", "offset_seek_drops_fragments", case_seek_offset },
  { "seek_memory", "track_seek_drops_fragments", case_seek_track_cue },
  { "seek_memory", "fixed_memory_bound", case_memory_bound }
};

size_t const task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
