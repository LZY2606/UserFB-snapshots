/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 *
 * Tests for the incremental event reader (nestegg_read_event).  Fixtures are
 * generated in memory so the expected reassembly and metadata are computed
 * independently of the parser.
 */
#define main indexing_fixture_main
#include "indexing.c"
#undef main

#include <stdio.h>
#include <dlfcn.h>
#include <malloc/malloc.h>

#include "nestegg/nestegg-events.h"
#include "task_support/check.h"

#define ID_CLUSTER          0x1f43b675U
#define ID_TIMECODE         0xe7U
#define ID_INFO             0x1549a966U
#define ID_TIMECODE_SCALE   0x2ad7b1U
#define ID_BLOCK_GROUP      0xa0U
#define ID_BLOCK            0xa1U
#define ID_BLOCK_DURATION   0x9bU
#define ID_DISCARD_PADDING  0x75a2U
#define ID_REFERENCE_BLOCK  0xfbU
#define ID_BLOCK_ADDITIONS  0x75a1U
#define ID_BLOCK_MORE       0xa6U
#define ID_BLOCK_ADDITIONAL 0xa5U

#define LACE_NONE  0
#define LACE_XIPH  1
#define LACE_FIXED 2
#define LACE_EBML  3
#define FRAG_MAX   1024

/* ---------------------------------------------------------------- fixtures */

static void
buffer_put_svint(struct buffer * buffer, int64_t value)
{
  int width;
  uint64_t bias, encoded;

  for (width = 1; width <= 8; ++width) {
    bias = (UINT64_C(1) << (7 * width - 1)) - 1;
    if (value >= -(int64_t) bias &&
        value <= (int64_t) (UINT64_C(1) << (7 * width)) - 2 - (int64_t) bias)
      break;
  }
  assert(width <= 8);
  encoded = (uint64_t) (value + (int64_t) bias) |
            (UINT64_C(1) << (7 * width));
  buffer_put_be(buffer, encoded, (unsigned int) width);
}

static void
buffer_put_unknown_size(struct buffer * buffer)
{
  buffer_put_byte(buffer, 0xff);
}

struct frame {
  struct buffer bytes;
};

static void
frame_fill(struct frame * f, size_t length, unsigned int seed)
{
  size_t i;
  buffer_clear(&f->bytes);
  for (i = 0; i < length; ++i)
    buffer_put_byte(&f->bytes, (unsigned char) ((seed * 37 + i * 11 + 5) & 0xff));
}

static void
frame_destroy(struct frame * f)
{
  buffer_destroy(&f->bytes);
}

/* Append a block payload: track, timecode, flags, lacing and frame bytes. */
static void
put_block_payload(struct buffer * payload, uint64_t track_number,
                  int64_t timecode, unsigned int lacing, int keyframe,
                  struct frame const * frames, unsigned int count)
{
  unsigned int i;
  unsigned char flags = (unsigned char) (lacing << 1);

  assert(count >= 1 && count <= 256);
  if (keyframe)
    flags |= 0x80;

  buffer_put_vint(payload, track_number);
  buffer_put_be(payload, (uint64_t) (uint16_t) timecode, 2);
  buffer_put_byte(payload, flags);

  if (lacing != LACE_NONE)
    buffer_put_byte(payload, (unsigned char) (count - 1));

  switch (lacing) {
  case LACE_NONE:
    assert(count == 1);
    break;
  case LACE_XIPH:
    for (i = 0; i + 1 < count; ++i) {
      size_t left = frames[i].bytes.length;
      while (left >= 255) {
        buffer_put_byte(payload, 255);
        left -= 255;
      }
      buffer_put_byte(payload, (unsigned char) left);
    }
    break;
  case LACE_FIXED: {
    size_t size = frames[0].bytes.length;
    for (i = 1; i < count; ++i)
      assert(frames[i].bytes.length == size);
    break;
  }
  case LACE_EBML:
    buffer_put_vint(payload, frames[0].bytes.length);
    for (i = 1; i + 1 < count; ++i) {
      int64_t delta = (int64_t) frames[i].bytes.length -
                      (int64_t) frames[i - 1].bytes.length;
      buffer_put_svint(payload, delta);
    }
    break;
  }

  for (i = 0; i < count; ++i)
    buffer_put(payload, frames[i].bytes.data, frames[i].bytes.length);
}

/* Describe one block to place in a generated stream. */
struct block_spec {
  uint64_t track_number;
  int64_t timecode;
  unsigned int lacing;
  int keyframe;
  unsigned int frame_count;
  size_t frame_size;
  unsigned int seed;
  int group;
  int has_duration;
  uint64_t duration_ticks;
  int has_discard;
  int64_t discard_padding;
  int has_reference;
  int64_t reference;
  int metadata_first;
  int block_additions;
  size_t * frame_sizes; /* optional per-frame sizes, overriding frame_size */
};

struct track_spec {
  uint64_t number;
  unsigned int type;
  char const * codec;
};

static void
put_track_entry_ex(struct buffer * tracks, uint64_t number,
                   unsigned int type, char const * codec)
{
  struct buffer entry = { 0 };

  buffer_put_uint(&entry, ID_TRACK_NUMBER, number);
  buffer_put_uint(&entry, ID_TRACK_UID, number);
  buffer_put_uint(&entry, ID_TRACK_TYPE, type);
  buffer_put_string(&entry, ID_CODEC_ID, codec);
  buffer_put_master(tracks, ID_TRACK_ENTRY, &entry);
  buffer_destroy(&entry);
}

static void
put_track_set(struct buffer * segment, struct track_spec const * tracks,
              unsigned int count)
{
  struct buffer element = { 0 };
  unsigned int i;

  for (i = 0; i < count; ++i)
    put_track_entry_ex(&element, tracks[i].number, tracks[i].type,
                       tracks[i].codec);
  buffer_put_master(segment, ID_TRACKS, &element);
  buffer_destroy(&element);
}

static void
put_cluster(struct buffer * segment, uint64_t timecode,
            struct block_spec const * specs, unsigned int count)
{
  struct buffer cluster = { 0 };
  unsigned int i;

  buffer_put_uint(&cluster, ID_TIMECODE, timecode);
  for (i = 0; i < count; ++i) {
    struct block_spec const * s = &specs[i];
    struct frame frames[256];
    unsigned int j;

    memset(frames, 0, sizeof(frames));
    for (j = 0; j < s->frame_count; ++j)
      frame_fill(&frames[j], s->frame_sizes ? s->frame_sizes[j]
                                           : s->frame_size,
                 s->seed + j);

    if (s->group) {
      struct buffer group = { 0 };
      struct buffer block = { 0 };

      put_block_payload(&block, s->track_number, s->timecode, s->lacing,
                        s->keyframe, frames, s->frame_count);

      if (s->metadata_first) {
        if (s->has_duration)
          buffer_put_uint(&group, ID_BLOCK_DURATION, s->duration_ticks);
        if (s->has_discard)
          buffer_put_uint(&group, ID_DISCARD_PADDING, (uint64_t) s->discard_padding);
        if (s->has_reference)
          buffer_put_uint(&group, ID_REFERENCE_BLOCK, (uint64_t) s->reference);
      }

      buffer_put_master(&group, ID_BLOCK, &block);

      if (!s->metadata_first) {
        if (s->has_duration)
          buffer_put_uint(&group, ID_BLOCK_DURATION, s->duration_ticks);
        if (s->has_discard)
          buffer_put_uint(&group, ID_DISCARD_PADDING, (uint64_t) s->discard_padding);
        if (s->has_reference)
          buffer_put_uint(&group, ID_REFERENCE_BLOCK, (uint64_t) s->reference);
      }

      if (s->block_additions) {
        struct buffer additions = { 0 };
        struct buffer more = { 0 };
        buffer_put(&more, "add", 3);
        buffer_put_master(&more, ID_BLOCK_ADDITIONAL, &more);
        buffer_put_master(&additions, ID_BLOCK_MORE, &more);
        buffer_put_master(&group, ID_BLOCK_ADDITIONS, &additions);
        buffer_destroy(&more);
        buffer_destroy(&additions);
      }

      buffer_put_master(&cluster, ID_BLOCK_GROUP, &group);
      buffer_destroy(&block);
      buffer_destroy(&group);
    } else {
      struct buffer block = { 0 };
      put_block_payload(&block, s->track_number, s->timecode, s->lacing,
                        s->keyframe, frames, s->frame_count);
      buffer_put_master(&cluster, ID_SIMPLE_BLOCK, &block);
      buffer_destroy(&block);
    }

    for (j = 0; j < s->frame_count; ++j)
      frame_destroy(&frames[j]);
  }

  buffer_put_master(segment, ID_CLUSTER, &cluster);
  buffer_destroy(&cluster);
}

/* Build a complete file: EBML header, Info, Tracks and one Cluster. */
static void
build_file(struct buffer * file, struct track_spec const * tracks,
           unsigned int track_count, uint64_t timecode,
           struct block_spec const * specs, unsigned int count)
{
  struct buffer segment = { 0 };
  struct buffer info = { 0 };

  buffer_put_uint(&info, ID_TIMECODE_SCALE, 1000000);
  buffer_put_master(&segment, ID_INFO, &info);
  put_track_set(&segment, tracks, track_count);
  put_cluster(&segment, timecode, specs, count);

  put_ebml_header(file);
  buffer_put_master(file, ID_SEGMENT, &segment);

  buffer_destroy(&info);
  buffer_destroy(&segment);
}

/* ----------------------------------------------------------------- capture */

struct capture {
  nestegg_event * events;   /* DATA metadata by value */
  size_t count;
  size_t capacity;
  struct buffer fragments;  /* immediate copy of every DATA payload */
  nestegg_event * commits;  /* COMMIT metadata by value */
  size_t commit_count;
  size_t commit_capacity;
};

static void
capture_reset(struct capture * cap)
{
  free(cap->events);
  free(cap->commits);
  buffer_destroy(&cap->fragments);
  memset(cap, 0, sizeof(*cap));
}

/* Copy the current event by value and, for DATA, immediately copy the
   borrowed payload; the borrow expires on the next read. */
static void
capture_take(struct capture * cap, const nestegg_event * ev)
{
  if (ev->type == NESTEGG_EVENT_DATA) {
    if (cap->count == cap->capacity) {
      size_t capacity = cap->capacity ? cap->capacity * 2 : 128;
      nestegg_event * events = realloc(cap->events, capacity * sizeof(*events));
      assert(events);
      cap->events = events;
      cap->capacity = capacity;
    }
    cap->events[cap->count++] = *ev;
    if (ev->length)
      buffer_put(&cap->fragments, ev->data, ev->length);
  } else {
    assert(ev->type == NESTEGG_EVENT_COMMIT);
    if (cap->commit_count == cap->commit_capacity) {
      size_t capacity = cap->commit_capacity ? cap->commit_capacity * 2 : 64;
      nestegg_event * commits = realloc(cap->commits, capacity * sizeof(*commits));
      assert(commits);
      cap->commits = commits;
      cap->commit_capacity = capacity;
    }
    cap->commits[cap->commit_count++] = *ev;
  }
}

/* Drive the reader to the end, capturing everything.  Returns the final
   return value (0 or -1). */
static int
run_events(nestegg * ctx, struct capture * cap)
{
  const nestegg_event * ev;
  int r;

  memset(cap, 0, sizeof(*cap));
  for (;;) {
    ev = (nestegg_event *) (void *) 1;
    r = nestegg_read_event(ctx, &ev);
    if (r != 1) {
      TASK_CHECK(ev == NULL);
      return r;
    }
    TASK_CHECK(ev != NULL);
    TASK_CHECK(ev->type == NESTEGG_EVENT_DATA ||
               ev->type == NESTEGG_EVENT_COMMIT);
    capture_take(cap, ev);
  }
}

/* Record where one packet's fragments begin in the copied buffer. */
static size_t packet_fragment_offset(struct capture const * cap, size_t first)
{
  size_t offset = 0, k;
  for (k = 0; k < first; ++k)
    offset += cap->events[k].length;
  return offset;
}

/* Verify the DATA events of one packet and compare with a whole-packet read.
   @a first is the index of the packet's first DATA event. */
static int
verify_packet(struct capture const * cap, size_t first, size_t count,
              nestegg_packet * pkt)
{
  struct buffer rebuilt[256];
  unsigned int frame_count = 0, rebuilt_count = 0;
  unsigned char * data;
  size_t length, offset = packet_fragment_offset(cap, first);
  size_t k;
  int have_first;

  if (nestegg_packet_count(pkt, &frame_count) != 0)
    return 0;

  memset(rebuilt, 0, sizeof(rebuilt));
  have_first = 0;
  for (k = 0; k < count; ++k) {
    nestegg_event const * ev = &cap->events[first + k];

    TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
    TASK_CHECK(ev->frame_count == frame_count);
    TASK_CHECK(ev->frame_index < frame_count);
    TASK_CHECK(ev->length <= FRAG_MAX);
    TASK_CHECK(ev->data != NULL || ev->length == 0);
    TASK_CHECK(ev->keyframe == NESTEGG_PACKET_HAS_KEYFRAME_UNKNOWN);
    TASK_CHECK(ev->has_duration == 0);
    TASK_CHECK(ev->has_discard_padding == 0);
    TASK_CHECK(ev->has_reference == 0);
    TASK_CHECK(ev->duration == 0);
    TASK_CHECK(ev->discard_padding == 0);
    TASK_CHECK(ev->reference == 0);
    TASK_CHECK(!(ev->first && !have_first && ev->frame_index != rebuilt_count));
    if (ev->first) {
      TASK_CHECK(ev->frame_index == rebuilt_count);
      rebuilt_count++;
      have_first = 1;
    }
    TASK_CHECK(have_first);
    TASK_CHECK(ev->length == 0 ? (ev->first && ev->last) : 1);
    TASK_CHECK(ev->first || ev->last || ev->length == FRAG_MAX);

    if (ev->length)
      buffer_put(&rebuilt[ev->frame_index], cap->fragments.data + offset,
                 ev->length);
    offset += ev->length;
  }

  if (rebuilt_count != frame_count) {
    size_t z;
    for (z = 0; z < frame_count; ++z)
      buffer_destroy(&rebuilt[z]);
    return 0;
  }

  for (k = 0; k < frame_count; ++k) {
    if (nestegg_packet_data(pkt, (unsigned int) k, &data, &length) != 0 ||
        length != rebuilt[k].length ||
        (length && memcmp(data, rebuilt[k].data, length))) {
      size_t z;
      for (z = 0; z < frame_count; ++z)
        buffer_destroy(&rebuilt[z]);
      return 0;
    }
  }

  for (k = 0; k < frame_count; ++k)
    buffer_destroy(&rebuilt[k]);
  return 1;
}

/* ------------------------------------------------------------------- cases */

static int
case_lacing(void)
{
  static const unsigned int lacings[] = { LACE_NONE, LACE_XIPH,
                                          LACE_FIXED, LACE_EBML };
  struct block_spec specs[4];
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer file = { 0 };
  struct buffer_io io, ref_io;
  nestegg * ctx = NULL, * ref = NULL;
  struct capture cap;
  nestegg_packet * pkt = NULL;
  unsigned int i;
  size_t first = 0;

  memset(specs, 0, sizeof(specs));
  for (i = 0; i < 4; ++i) {
    specs[i].track_number = 1;
    specs[i].timecode = 10 + (int64_t) i * 10;
    specs[i].lacing = lacings[i];
    specs[i].keyframe = 1;
    specs[i].seed = i + 1;
  }
  specs[0].frame_count = 1; specs[0].frame_size = 300;
  specs[1].frame_count = 4; specs[1].frame_size = 129;
  specs[2].frame_count = 3; specs[2].frame_size = 1;
  specs[3].frame_count = 5; specs[3].frame_size = 7;

  build_file(&file, tracks, 1, 0, specs, 4);
  assert(init_from_buffer(&ctx, &file, &io) == 0);
  assert(init_from_buffer(&ref, &file, &ref_io) == 0);

  TASK_CHECK(run_events(ctx, &cap) == 0);
  TASK_CHECK(cap.commit_count == 4);

  for (i = 0; i < 4; ++i) {
    size_t count = 0;
    while (first + count < cap.count &&
           cap.events[first + count].packet_id == i + 1)
      count++;
    TASK_CHECK(count > 0);
    TASK_CHECK(nestegg_read_packet(ref, &pkt) == 1);
    TASK_CHECK(verify_packet(&cap, first, count, pkt));
    nestegg_free_packet(pkt);
    pkt = NULL;
    first += count;
  }
  TASK_CHECK(first == cap.count);
  TASK_CHECK(nestegg_read_packet(ref, &pkt) == 0 && pkt == NULL);

  capture_reset(&cap);
  nestegg_destroy(ref);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* Locate the contiguous DATA events of one packet.  Returns 0 if none. */
static int
packet_events(struct capture const * cap, uint64_t packet_id,
              size_t * first, size_t * count)
{
  size_t k = 0;
  while (k < cap->count && cap->events[k].packet_id < packet_id)
    ++k;
  if (k == cap->count || cap->events[k].packet_id != packet_id)
    return 0;
  *first = k;
  *count = 0;
  while (k + *count < cap->count &&
         cap->events[k + *count].packet_id == packet_id)
    ++*count;
  return 1;
}

/* Cross-track, cross-cluster stream with a negative relative timecode and
   BlockGroups whose metadata follows the Block. */
static int
case_cross(void)
{
  struct track_spec tracks[] = {
    { 1, 1, "V_VP8" },
    { 7, 2, "A_OPUS" },
  };
  struct block_spec cluster_a[3], cluster_b[2];
  struct buffer file = { 0 };
  struct buffer_io io, ref_io;
  nestegg * ctx = NULL, * ref = NULL;
  struct capture cap;
  nestegg_packet * pkt = NULL;
  unsigned int i;

  memset(cluster_a, 0, sizeof(cluster_a));
  memset(cluster_b, 0, sizeof(cluster_b));

  /* Cluster A has timecode 100.  The first block is at -40 ticks => 60 ns. */
  cluster_a[0].track_number = 7;
  cluster_a[0].timecode = -40;
  cluster_a[0].lacing = LACE_XIPH;
  cluster_a[0].frame_count = 3;
  cluster_a[0].frame_size = 33;
  cluster_a[0].seed = 10;
  cluster_a[0].group = 1;
  cluster_a[0].has_duration = 1;
  cluster_a[0].duration_ticks = 5;
  cluster_a[0].has_discard = 1;
  cluster_a[0].discard_padding = -1234;
  cluster_a[0].has_reference = 1;
  cluster_a[0].reference = -3;

  cluster_a[1].track_number = 1;
  cluster_a[1].timecode = 0;
  cluster_a[1].lacing = LACE_EBML;
  cluster_a[1].keyframe = 1;
  cluster_a[1].frame_count = 4;
  cluster_a[1].frame_size = 12;
  cluster_a[1].seed = 20;
  cluster_a[1].group = 1;
  cluster_a[1].has_duration = 1;
  cluster_a[1].duration_ticks = 40;
  cluster_a[1].metadata_first = 1;

  cluster_a[2].track_number = 7;
  cluster_a[2].timecode = 50;
  cluster_a[2].lacing = LACE_FIXED;
  cluster_a[2].keyframe = 1;
  cluster_a[2].frame_count = 2;
  cluster_a[2].frame_size = 9;
  cluster_a[2].seed = 30;

  /* Cluster B has timecode 200. */
  cluster_b[0].track_number = 1;
  cluster_b[0].timecode = 3;
  cluster_b[0].lacing = LACE_XIPH;
  cluster_b[0].keyframe = 1;
  cluster_b[0].frame_count = 2;
  cluster_b[0].frame_size = 1;
  cluster_b[0].seed = 40;
  cluster_b[0].group = 1;
  cluster_b[0].has_duration = 1;
  cluster_b[0].duration_ticks = 1;
  cluster_b[0].has_discard = 1;
  cluster_b[0].discard_padding = -9;

  cluster_b[1].track_number = 7;
  cluster_b[1].timecode = 10;
  cluster_b[1].lacing = LACE_NONE;
  cluster_b[1].keyframe = 1;
  cluster_b[1].frame_count = 1;
  cluster_b[1].frame_size = 1000;
  cluster_b[1].seed = 50;

  {
    struct buffer segment = { 0 };
    struct buffer info = { 0 };

    buffer_put_uint(&info, ID_TIMECODE_SCALE, 1000000);
    buffer_put_master(&segment, ID_INFO, &info);
    put_track_set(&segment, tracks, 2);
    put_cluster(&segment, 100, cluster_a, 3);
    put_cluster(&segment, 200, cluster_b, 2);
    put_ebml_header(&file);
    buffer_put_master(&file, ID_SEGMENT, &segment);
    buffer_destroy(&info);
    buffer_destroy(&segment);
  }

  assert(init_from_buffer(&ctx, &file, &io) == 0);
  assert(init_from_buffer(&ref, &file, &ref_io) == 0);

  TASK_CHECK(run_events(ctx, &cap) == 0);
  TASK_CHECK(cap.commit_count == 5);
  TASK_CHECK(cap.count > 0);

  /* Expected metadata, computed independently. */
  {
    struct {
      unsigned int track;
      uint64_t timestamp;
      int keyframe;
      int has_duration;
      uint64_t duration;
      int has_discard;
      int64_t discard;
      int has_reference;
      int64_t reference;
    } expected[5] = {
      { 1, 60000000, 0, 1, 5000000, 1, -1234, 1, -3 },   /* track index 1 */
      { 0, 100000000, 1, 1, 40000000, 0, 0, 0, 0 },      /* track index 0 */
      { 1, 150000000, 1, 0, 0, 0, 0, 0, 0 },             /* SimpleBlock */
      { 0, 203000000, 1, 1, 1000000, 1, -9, 0, 0 },      /* track index 0 */
      { 1, 210000000, 1, 0, 0, 0, 0, 0, 0 },
    };

    for (i = 0; i < 5; ++i) {
      size_t first = 0, count = 0;
      nestegg_event const * c = &cap.commits[i];

      TASK_CHECK(packet_events(&cap, i + 1, &first, &count));
      TASK_CHECK(c->type == NESTEGG_EVENT_COMMIT);
      TASK_CHECK(c->packet_id == i + 1);
      TASK_CHECK(c->track == expected[i].track);
      TASK_CHECK(c->timestamp == expected[i].timestamp);
      TASK_CHECK(c->keyframe == expected[i].keyframe);
      TASK_CHECK(c->has_duration == expected[i].has_duration);
      TASK_CHECK(c->duration == expected[i].duration);
      TASK_CHECK(c->has_discard_padding == expected[i].has_discard);
      TASK_CHECK(c->discard_padding == expected[i].discard);
      TASK_CHECK(c->has_reference == expected[i].has_reference);
      TASK_CHECK(c->reference == expected[i].reference);
      TASK_CHECK(c->data == NULL && c->length == 0);
      TASK_CHECK(c->frame_index == 0 && c->first == 0 && c->last == 0);
      TASK_CHECK(c->frame_count ==
                 (i == 0 || i == 1 ? (i == 0 ? 3u : 4u) :
                  i == 2 ? 2u : (i == 3 ? 2u : 1u)));

      TASK_CHECK(nestegg_read_packet(ref, &pkt) == 1);
      TASK_CHECK(verify_packet(&cap, first, count, pkt));
      nestegg_free_packet(pkt);
      pkt = NULL;
    }
    TASK_CHECK(nestegg_read_packet(ref, &pkt) == 0 && pkt == NULL);
  }

  capture_reset(&cap);
  nestegg_destroy(ref);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}



/* Clusters with unknown sizes keep track and block clocks across boundaries;
   the next top-level element terminates them without being consumed. */
static int
case_unknown_cluster(void)
{
  struct block_spec a[2], b[1];
  struct track_spec tracks[] = { { 3, 1, "V_VP8" } };
  struct buffer segment = { 0 };
  struct buffer info = { 0 };
  struct buffer cluster = { 0 };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  struct capture cap;

  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  a[0].track_number = 3; a[0].lacing = LACE_NONE; a[0].keyframe = 1;
  a[0].frame_count = 1; a[0].frame_size = 3; a[0].seed = 1;
  a[1] = a[0]; a[1].timecode = 4; a[1].seed = 2;
  b[0] = a[0]; b[0].timecode = 2; b[0].seed = 3;

  buffer_put_uint(&info, ID_TIMECODE_SCALE, 1000000);
  buffer_put_master(&segment, ID_INFO, &info);
  put_track_set(&segment, tracks, 1);

  /* First cluster: unknown size. */
  buffer_put_id(&cluster, ID_CLUSTER);
  buffer_put_unknown_size(&cluster);
  buffer_put_uint(&cluster, ID_TIMECODE, 10);
  {
    struct buffer block = { 0 };
    struct frame frames[1];
    memset(frames, 0, sizeof(frames));
    frame_fill(&frames[0], 3, 1);
    put_block_payload(&block, 3, 0, LACE_NONE, 1, frames, 1);
    buffer_put_master(&cluster, ID_SIMPLE_BLOCK, &block);
    buffer_destroy(&block);
    frame_destroy(&frames[0]);
    memset(&block, 0, sizeof(block));
    memset(frames, 0, sizeof(frames));
    frame_fill(&frames[0], 3, 2);
    memset(&block, 0, sizeof(block));
    put_block_payload(&block, 3, 4, LACE_NONE, 1, frames, 1);
    buffer_put_master(&cluster, ID_SIMPLE_BLOCK, &block);
    buffer_destroy(&block);
    frame_destroy(&frames[0]);
  }
  buffer_put(&segment, cluster.data, cluster.length);
  buffer_clear(&cluster);

  /* Second cluster: known size. */
  put_cluster(&segment, 100, b, 1);

  put_ebml_header(&file);
  buffer_put_master(&file, ID_SEGMENT, &segment);

  assert(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(run_events(ctx, &cap) == 0);
  TASK_CHECK(cap.commit_count == 3);
  TASK_CHECK(cap.commits[0].timestamp == 10000000);
  TASK_CHECK(cap.commits[1].timestamp == 14000000);
  TASK_CHECK(cap.commits[2].timestamp == 102000000);

  capture_reset(&cap);
  nestegg_destroy(ctx);
  buffer_destroy(&info);
  buffer_destroy(&cluster);
  buffer_destroy(&segment);
  buffer_destroy(&file);
  return 0;
}


/* A frame with no payload still produces one DATA event with length 0 and
   both first and last set.  Xiph lacing makes an empty middle frame. */
static int
case_empty_frame(void)
{
  struct block_spec spec;
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  struct capture cap;
  size_t first = 0, count = 0, k;
  int empty_seen = 0;

  memset(&spec, 0, sizeof(spec));
  spec.track_number = 1;
  spec.lacing = LACE_XIPH;
  spec.keyframe = 1;
  spec.frame_count = 3;
  spec.frame_size = 8;
  spec.seed = 80;
  /* Xiph lacing lets the middle frame be empty; the last frame is the
     remainder. */
  {
    static size_t sizes[] = { 8, 0, 8 };
    spec.frame_sizes = sizes;
  }

  build_file(&file, tracks, 1, 0, &spec, 1);
  assert(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(run_events(ctx, &cap) == 0);
  TASK_CHECK(cap.commit_count == 1);
  TASK_CHECK(packet_events(&cap, 1, &first, &count));

  /* The three frames are 8, 0 and 8 bytes: one full fragment, one empty
     fragment, one full fragment. */
  TASK_CHECK(count == 3);
  for (k = 0; k < count; ++k) {
    nestegg_event const * ev = &cap.events[first + k];
    TASK_CHECK(ev->frame_count == 3);
    if (ev->length == 0) {
      empty_seen = 1;
      TASK_CHECK(ev->frame_index == 1);
      TASK_CHECK(ev->first && ev->last);
      TASK_CHECK(ev->data == NULL);
    }
  }
  TASK_CHECK(empty_seen);

  capture_reset(&cap);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* --------------------------------------------------------------- metadata */

/* Presence and value rules for BlockGroup metadata, and the keyframe rule. */
static int
case_metadata(void)
{
  struct block_spec specs[5];
  struct track_spec tracks[] = { { 2, 1, "V_VP8" } };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  struct capture cap;
  unsigned int i;

  memset(specs, 0, sizeof(specs));
  for (i = 0; i < 5; ++i) {
    specs[i].track_number = 2;
    specs[i].timecode = (int64_t) i;
    specs[i].lacing = LACE_NONE;
    specs[i].keyframe = 1;
    specs[i].frame_count = 1;
    specs[i].frame_size = 4 + i;
    specs[i].seed = 60 + i;
  }

  /* 0: SimpleBlock, cannot carry BlockGroup metadata. */
  /* 1: BlockGroup with all metadata, trailing. */
  specs[1].group = 1;
  specs[1].has_duration = 1; specs[1].duration_ticks = 7;
  specs[1].has_discard = 1; specs[1].discard_padding = -77;
  specs[1].has_reference = 1; specs[1].reference = -1;
  /* 2: BlockGroup, zero-valued metadata still counts as present. */
  specs[2].group = 1;
  specs[2].has_duration = 1; specs[2].duration_ticks = 0;
  specs[2].has_discard = 1; specs[2].discard_padding = 0;
  specs[2].has_reference = 1; specs[2].reference = 0;
  /* 3: BlockGroup with no reference: a keyframe. */
  specs[3].group = 1;
  specs[3].has_duration = 1; specs[3].duration_ticks = 3;
  /* 4: BlockGroup with metadata before the Block. */
  specs[4].group = 1;
  specs[4].metadata_first = 1;
  specs[4].has_discard = 1;
  specs[4].discard_padding = 99;

  build_file(&file, tracks, 1, 0, specs, 5);
  assert(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(run_events(ctx, &cap) == 0);
  TASK_CHECK(cap.commit_count == 5);

  /* SimpleBlock uses its own flag. */
  TASK_CHECK(cap.commits[0].keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);
  TASK_CHECK(cap.commits[0].has_duration == 0);
  TASK_CHECK(cap.commits[0].has_discard_padding == 0);
  TASK_CHECK(cap.commits[0].has_reference == 0);

  TASK_CHECK(cap.commits[1].has_duration == 1);
  TASK_CHECK(cap.commits[1].duration == 7000000);
  TASK_CHECK(cap.commits[1].has_discard_padding == 1);
  TASK_CHECK(cap.commits[1].discard_padding == -77);
  TASK_CHECK(cap.commits[1].has_reference == 1);
  TASK_CHECK(cap.commits[1].reference == -1);
  TASK_CHECK(cap.commits[1].keyframe == NESTEGG_PACKET_HAS_KEYFRAME_FALSE);

  /* Present but zero is still present. */
  TASK_CHECK(cap.commits[2].has_duration == 1);
  TASK_CHECK(cap.commits[2].duration == 0);
  TASK_CHECK(cap.commits[2].has_discard_padding == 1);
  TASK_CHECK(cap.commits[2].discard_padding == 0);
  TASK_CHECK(cap.commits[2].has_reference == 1);
  TASK_CHECK(cap.commits[2].reference == 0);
  TASK_CHECK(cap.commits[2].keyframe == NESTEGG_PACKET_HAS_KEYFRAME_FALSE);

  /* No ReferenceBlock means a keyframe. */
  TASK_CHECK(cap.commits[3].has_reference == 0);
  TASK_CHECK(cap.commits[3].keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);

  /* Metadata appearing before the Block still applies. */
  TASK_CHECK(cap.commits[4].has_discard_padding == 1);
  TASK_CHECK(cap.commits[4].discard_padding == 99);
  TASK_CHECK(cap.commits[4].has_duration == 0);
  TASK_CHECK(cap.commits[4].keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);

  /* Every COMMIT repeats the block's track, timestamp and frame count. */
  for (i = 0; i < 5; ++i) {
    TASK_CHECK(cap.commits[i].packet_id == i + 1);
    TASK_CHECK(cap.commits[i].track == 0);
    TASK_CHECK(cap.commits[i].timestamp == (uint64_t) i * 1000000);
    TASK_CHECK(cap.commits[i].frame_count == 1);
    TASK_CHECK(cap.commits[i].data == NULL);
    TASK_CHECK(cap.commits[i].length == 0);
  }

  capture_reset(&cap);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* A BlockGroup whose tail metadata is cut off must fail without a COMMIT. */
static int
case_truncated_group(void)
{
  struct block_spec spec;
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  struct capture cap;
  size_t whole;

  memset(&spec, 0, sizeof(spec));
  spec.track_number = 1;
  spec.timecode = 0;
  spec.lacing = LACE_NONE;
  spec.keyframe = 1;
  spec.frame_count = 1;
  spec.frame_size = 16;
  spec.seed = 70;
  spec.group = 1;
  spec.has_duration = 1;
  spec.duration_ticks = 3;

  build_file(&file, tracks, 1, 0, &spec, 1);
  whole = file.length;
  /* Trim the final byte of the trailing BlockDuration so the group is cut. */
  file.length = whole - 1;

  assert(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(run_events(ctx, &cap) == -1);
  TASK_CHECK(cap.commit_count == 0);
  /* The frame fragments were delivered before the fault. */
  TASK_CHECK(cap.count > 0);
  TASK_CHECK(cap.fragments.length == 16);

  /* Later calls keep reporting the error and emit nothing. */
  {
    const nestegg_event * ev = (const nestegg_event *) (void *) 1;
    TASK_CHECK(nestegg_read_event(ctx, &ev) == -1);
    TASK_CHECK(ev == NULL);
    TASK_CHECK(nestegg_read_event(ctx, &ev) == -1);
    TASK_CHECK(ev == NULL);
  }

  capture_reset(&cap);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* Unsupported media is rejected; already-sent DATA stays uncommitted. */
static int
case_unsupported(void)
{
  struct block_spec spec;
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  struct capture cap;

  memset(&spec, 0, sizeof(spec));
  spec.track_number = 1;
  spec.lacing = LACE_NONE;
  spec.keyframe = 1;
  spec.frame_count = 1;
  spec.frame_size = 8;
  spec.group = 1;
  spec.block_additions = 1;

  build_file(&file, tracks, 1, 0, &spec, 1);
  assert(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(run_events(ctx, &cap) == -1);
  TASK_CHECK(cap.commit_count == 0);
  TASK_CHECK(cap.fragments.length == 8);

  capture_reset(&cap);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* A clean end of stream at a block boundary is not an error. */
static int
case_clean_end(void)
{
  struct block_spec spec;
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  struct capture cap;
  const nestegg_event * ev;
  unsigned int i;

  memset(&spec, 0, sizeof(spec));
  spec.track_number = 1;
  spec.lacing = LACE_NONE;
  spec.keyframe = 1;
  spec.frame_count = 1;
  spec.frame_size = 5;

  build_file(&file, tracks, 1, 0, &spec, 1);
  assert(init_from_buffer(&ctx, &file, &io) == 0);
  TASK_CHECK(run_events(ctx, &cap) == 0);
  TASK_CHECK(cap.commit_count == 1);

  for (i = 0; i < 3; ++i) {
    ev = (const nestegg_event *) (void *) 1;
    TASK_CHECK(nestegg_read_event(ctx, &ev) == 0);
    TASK_CHECK(ev == NULL);
  }

  capture_reset(&cap);
  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}


/* ------------------------------------------------------------ seek/memory */

/* ------------------------------------------------------------ seek/memory */

/* The I/O wrapper records the stream position at which the parser first
   produces a DATA fragment, so a frame can be shown to be delivered before
   it has been read in full. */
struct progress_io {
  unsigned char const * data;
  size_t length;
  size_t offset;
  size_t position_at_first_data;
  size_t position_before_data;
  size_t max_request;
  int saw_data;
};

static int64_t
progress_read(void * destination, size_t length, void * userdata)
{
  struct progress_io * io = userdata;
  size_t available = io->length - io->offset;

  if (length > available)
    length = available;
  if (length == 0)
    return 0;
  if (length > io->max_request)
    io->max_request = length;
  memcpy(destination, io->data + io->offset, length);
  io->offset += length;
  return (int64_t) length;
}

static int
progress_seek(int64_t offset, int whence, void * userdata)
{
  struct progress_io * io = userdata;
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
progress_tell(void * userdata)
{
  struct progress_io * io = userdata;
  return (int64_t) io->offset;
}

static void
progress_init(nestegg_io * callbacks, struct progress_io * io,
              struct buffer const * file)
{
  memset(callbacks, 0, sizeof(*callbacks));
  callbacks->read = progress_read;
  callbacks->seek = progress_seek;
  callbacks->tell = progress_tell;
  callbacks->userdata = io;
  io->data = file->data;
  io->length = file->length;
  io->offset = 0;
  io->position_at_first_data = 0;
  io->position_before_data = 0;
  io->max_request = 0;
  io->saw_data = 0;
}

/* A frame larger than 32 KiB must have its first fragment delivered before
   the whole frame has been read.  The I/O callback records the largest
   single read the library requests while producing that first fragment; a
   reader that buffered the frame would ask for the whole frame at once. */
static int
case_early_delivery(void)
{
  struct block_spec spec;
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer file = { 0 };
  struct progress_io io;
  nestegg_io callbacks;
  nestegg * ctx = NULL;
  const nestegg_event * ev;
  size_t large = 40000;
  int r;

  memset(&spec, 0, sizeof(spec));
  spec.track_number = 1;
  spec.lacing = LACE_NONE;
  spec.keyframe = 1;
  spec.frame_count = 1;
  spec.frame_size = large;
  spec.seed = 11;

  build_file(&file, tracks, 1, 0, &spec, 1);
  progress_init(&callbacks, &io, &file);

  TASK_CHECK(nestegg_init(&ctx, callbacks, NULL, -1) == 0);
  for (;;) {
    r = nestegg_read_event(ctx, &ev);
    if (r != 1)
      break;
    if (ev->type == NESTEGG_EVENT_DATA && !io.saw_data) {
      io.saw_data = 1;
      io.position_at_first_data = io.offset;
      TASK_CHECK(ev->first);
      TASK_CHECK(ev->length == FRAG_MAX);
      TASK_CHECK(ev->frame_index == 0);
      /* No single read has asked for the whole 40 KiB frame. */
      TASK_CHECK(io.max_request < large);
    }
  }
  TASK_CHECK(r == 0);
  TASK_CHECK(io.saw_data);
  /* The first fragment was produced without reading the frame's tail. */
  TASK_CHECK(io.position_at_first_data + large / 2 <= file.length);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* A large frame arrives as ceil(size / 1024) fragments, in order. */
static int
case_incremental(void)
{
  struct block_spec spec;
  struct track_spec tracks[] = { { 1, 1, "L" } };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev;
  size_t large = 64 * 1024;
  size_t expect = large / FRAG_MAX + (large % FRAG_MAX ? 1 : 0);
  size_t fragments = 0;
  int r;

  memset(&spec, 0, sizeof(spec));
  spec.track_number = 1;
  spec.lacing = LACE_NONE;
  spec.keyframe = 1;
  spec.frame_count = 1;
  spec.frame_size = large;
  spec.seed = 12;

  build_file(&file, tracks, 1, 0, &spec, 1);
  assert(init_from_buffer(&ctx, &file, &io) == 0);
  for (;;) {
    r = nestegg_read_event(ctx, &ev);
    if (r != 1)
      break;
    if (ev->type == NESTEGG_EVENT_DATA) {
      TASK_CHECK(ev->frame_index == 0);
      TASK_CHECK(ev->first == (fragments == 0));
      TASK_CHECK(ev->last == (fragments + 1 == expect));
      TASK_CHECK(ev->length == (fragments + 1 == expect ?
                                large - fragments * FRAG_MAX : FRAG_MAX));
      ++fragments;
    }
  }
  TASK_CHECK(r == 0);
  TASK_CHECK(fragments == expect);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* Seek discards uncommitted fragment state and packet ids keep increasing. */
/* Find the Nth (zero-based) Cluster start offset by scanning the built
   segment.  The generated files put the clusters last, so a raw ID scan is a
   reliable independent locator. */
static size_t
find_cluster(struct buffer const * file, unsigned int nth)
{
  size_t i;
  unsigned int seen = 0;

  for (i = 0; i + 4 <= file->length; ++i) {
    if (file->data[i] == 0x1f && file->data[i + 1] == 0x43 &&
        file->data[i + 2] == 0xb6 && file->data[i + 3] == 0x75) {
      if (seen == nth)
        return i;
      seen++;
    }
  }
  return (size_t) -1;
}

/* A seek in the middle of a block drops the uncommitted fragments: no COMMIT
   for the old packet appears, its fragments do not leak into the new read,
   and packet ids keep increasing across the seek. */
static int
case_seek(void)
{
  struct block_spec specs[2];
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer segment = { 0 };
  struct buffer info = { 0 };
  struct buffer file = { 0 };
  struct buffer_io io;
  nestegg * ctx = NULL;
  const nestegg_event * ev;
  uint64_t id_before = 0, id_after = 0;
  int r, saw_commit, saw_data_before;

  memset(specs, 0, sizeof(specs));
  specs[0].track_number = 1;
  specs[0].lacing = LACE_NONE;
  specs[0].keyframe = 1;
  specs[0].frame_count = 1;
  specs[0].frame_size = 3000;
  specs[0].seed = 4;
  specs[1] = specs[0];
  specs[1].timecode = 100;
  specs[1].seed = 5;

  buffer_put_uint(&info, ID_TIMECODE_SCALE, 1000000);
  buffer_put_master(&segment, ID_INFO, &info);
  put_track_set(&segment, tracks, 1);
  put_cluster(&segment, 0, specs, 1);
  put_cluster(&segment, 100, &specs[1], 1);
  put_ebml_header(&file);
  buffer_put_master(&file, ID_SEGMENT, &segment);

  assert(init_from_buffer(&ctx, &file, &io) == 0);

  /* Read only one DATA fragment of the first block, then seek away. */
  saw_data_before = 0;
  saw_commit = 0;
  while ((r = nestegg_read_event(ctx, &ev)) == 1) {
    TASK_CHECK(ev->type == NESTEGG_EVENT_DATA);
    if (!saw_data_before) {
      saw_data_before = 1;
      id_before = ev->packet_id;
      TASK_CHECK(id_before == 1);
      TASK_CHECK(ev->first && !ev->last);
      break;
    }
    if (ev->type == NESTEGG_EVENT_COMMIT) {
      saw_commit = 1;
      break;
    }
  }
  TASK_CHECK(saw_data_before);
  TASK_CHECK(!saw_commit);

  {
    size_t offset = find_cluster(&file, 1);
    TASK_CHECK(offset != (size_t) -1);
    TASK_CHECK(nestegg_offset_seek(ctx, offset) == 0);
  }

  saw_commit = 0;
  for (;;) {
    r = nestegg_read_event(ctx, &ev);
    if (r != 1)
      break;
    /* The abandoned packet's id must never reappear. */
    TASK_CHECK(ev->packet_id != id_before || ev->type != NESTEGG_EVENT_DATA);
    TASK_CHECK(ev->packet_id == 2);
    if (ev->type == NESTEGG_EVENT_DATA)
      id_after = ev->packet_id;
    if (ev->type == NESTEGG_EVENT_COMMIT)
      saw_commit = 1;
  }
  TASK_CHECK(r == 0);
  TASK_CHECK(saw_commit);
  TASK_CHECK(id_after == 2);

  nestegg_destroy(ctx);
  buffer_destroy(&info);
  buffer_destroy(&segment);
  buffer_destroy(&file);
  return 0;
}

/* --------------------------------------------------------- memory tracking */

/* Interpose calloc so the event path's heap footprint can be measured.  The
   library allocates its event state and its transient buffers through
   calloc, and reads frame fragments into that state, so tracking live bytes
   while events are read gives the added-memory peak directly. */

static void * (*real_calloc)(size_t, size_t);
static int track_memory;
static size_t live_bytes;
static size_t peak_bytes;
static size_t live_blocks[4096];
static size_t block_sizes[4096];
static size_t block_count;

void *
calloc(size_t count, size_t size)
{
  size_t total = count * size;
  void * p;

  if (!real_calloc)
    real_calloc = (void * (*)(size_t, size_t)) dlsym(RTLD_NEXT, "calloc");
  p = real_calloc(count, size);

  if (track_memory && p) {
    if (block_count < sizeof(live_blocks) / sizeof(live_blocks[0])) {
      live_blocks[block_count] = (size_t) p;
      block_sizes[block_count] = total;
      block_count++;
      live_bytes += total;
      if (live_bytes > peak_bytes)
        peak_bytes = live_bytes;
    }
  }
  return p;
}

void *
realloc(void * ptr, size_t size)
{
  void * (*real_realloc)(void *, size_t);
  void * p;
  size_t i;

  real_realloc = (void * (*)(void *, size_t)) dlsym(RTLD_NEXT, "realloc");
  p = real_realloc(ptr, size);

  if (track_memory) {
    for (i = 0; i < block_count; ++i) {
      if (live_blocks[i] == (size_t) ptr) {
        if (size >= block_sizes[i]) {
          live_bytes += size - block_sizes[i];
          if (live_bytes > peak_bytes)
            peak_bytes = live_bytes;
        } else {
          live_bytes -= block_sizes[i] - size;
        }
        block_sizes[i] = size;
        live_blocks[i] = (size_t) p;
        break;
      }
    }
  }
  return p;
}

/* A frame of @a frame_size bytes repeated @a frame_count times must not cause
   the event path to hold more than the documented budget, and the peak must
   not grow with the media length. */
static size_t
measure_event_peak(struct block_spec const * spec, unsigned int count,
                   struct progress_io * progress, nestegg_io * callbacks)
{
  struct track_spec tracks[] = { { 1, 1, "V_VP8" } };
  struct buffer file = { 0 };
  nestegg * ctx = NULL;
  const nestegg_event * ev;
  size_t peak;
  int r;

  build_file(&file, tracks, 1, 0, spec, count);
  progress_init(callbacks, progress, &file);
  assert(nestegg_init(&ctx, *callbacks, NULL, -1) == 0);

  block_count = 0;
  live_bytes = 0;
  peak_bytes = 0;
  track_memory = 1;
  for (;;) {
    r = nestegg_read_event(ctx, &ev);
    if (r != 1)
      break;
  }
  track_memory = 0;
  peak = peak_bytes;
  TASK_CHECK(r == 0);

  nestegg_destroy(ctx);

  /* Forget every surviving block (all should be freed by destroy). */
  block_count = 0;
  live_bytes = 0;
  peak_bytes = 0;
  buffer_destroy(&file);
  return peak;
}

static int
case_memory(void)
{
  struct block_spec small, large;
  struct progress_io progress;
  nestegg_io callbacks;
  size_t peak_small, peak_large;

  memset(&small, 0, sizeof(small));
  small.track_number = 1;
  small.lacing = LACE_EBML;
  small.keyframe = 1;
  small.frame_count = 200;
  small.frame_size = 36;
  small.seed = 90;

  memset(&large, 0, sizeof(large));
  large.track_number = 1;
  large.lacing = LACE_XIPH;
  large.keyframe = 1;
  large.frame_count = 256;
  large.frame_size = 200;
  large.seed = 91;

  peak_small = measure_event_peak(&small, 1, &progress, &callbacks);
  peak_large = measure_event_peak(&large, 1, &progress, &callbacks);

  /* The added footprint stays inside 8192 bytes. */
  TASK_CHECK(peak_small > 0);
  TASK_CHECK(peak_small <= 8192);
  TASK_CHECK(peak_large <= 8192);
  /* And does not grow with the amount of media read: the state is fixed. */
  TASK_CHECK(peak_large == peak_small);

  return 0;
}
const struct task_case task_cases[] = {
  { "fragments", "lacing", case_lacing },
  { "fragments", "cross", case_cross },
  { "fragments", "unknown_cluster", case_unknown_cluster },
  { "fragments", "empty_frame", case_empty_frame },
  { "metadata", "values", case_metadata },
  { "metadata", "truncated_group", case_truncated_group },
  { "metadata", "unsupported", case_unsupported },
  { "metadata", "clean_end", case_clean_end },
  { "seek_memory", "early_delivery", case_early_delivery },
  { "seek_memory", "incremental", case_incremental },
  { "seek_memory", "seek", case_seek },
  { "seek_memory", "memory", case_memory },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
