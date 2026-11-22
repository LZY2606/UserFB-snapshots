/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 *
 * Tests for nestegg_init_chained: sequential playback of complete WebM
 * members concatenated back to back, presented as one continuous track
 * set and timeline.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"

/* Reuse the buffer and memory-IO helpers from the indexing regression. */
#define main indexing_regression_main
#include "indexing.c"
#undef main

#define ID_INFO             0x1549a966U
#define ID_TIMECODE_SCALE   0x2ad7b1U
#define ID_DURATION         0x4489U
#define ID_BLOCK_GROUP      0xa0U
#define ID_BLOCK            0xa1U
#define ID_BLOCK_DURATION   0x9bU
#define ID_REFERENCE_BLOCK  0xfbU
#define ID_DISCARD_PADDING  0x75a2U
#define ID_TAGS             0x1254c367U
#define ID_VOID             0xecU

enum { BS_SIMPLE, BS_XIPH3, BS_GROUP };

struct tspec {
  uint64_t number;
  uint64_t uid;
  int audio;
  char const * codec;
  int omit_uid;
};

struct bspec {
  uint64_t track;
  int rel;
  int kind;
  unsigned char payload[3];
  size_t payload_len;
  uint64_t duration;
  int64_t reference;
  int64_t discard;
};

struct mspec {
  uint64_t scale;
  double duration;
  int omit_duration;
  struct tspec const * tracks;
  size_t ntracks;
  uint64_t cluster_tc;
  struct bspec const * blocks;
  size_t nblocks;
  int unknown_size;
  int tail_tags;
  int tail_cues;
  size_t void_pad;
};

static void
buffer_put_sint(struct buffer * buffer, uint32_t id, int64_t value)
{
  unsigned int width = 1;

  while (width < 8) {
    int64_t min = -(INT64_C(1) << (width * 8 - 1));
    int64_t max = (INT64_C(1) << (width * 8 - 1)) - 1;
    if (value >= min && value <= max)
      break;
    width += 1;
  }

  buffer_put_id(buffer, id);
  buffer_put_size(buffer, width);
  buffer_put_be(buffer, (uint64_t) value, width);
}

static void
put_info(struct buffer * segment, uint64_t scale, double duration,
         int omit_duration)
{
  struct buffer info = { 0 };
  uint64_t bits;

  buffer_put_uint(&info, ID_TIMECODE_SCALE, scale);
  if (!omit_duration) {
    memcpy(&bits, &duration, sizeof(bits));
    buffer_put_id(&info, ID_DURATION);
    buffer_put_size(&info, 8);
    buffer_put_be(&info, bits, 8);
  }
  buffer_put_master(segment, ID_INFO, &info);
  buffer_destroy(&info);
}

static void
put_tracks_spec(struct buffer * segment, struct tspec const * tracks,
                size_t count)
{
  struct buffer payload = { 0 };
  size_t i;

  for (i = 0; i < count; ++i) {
    struct buffer entry = { 0 };

    buffer_put_uint(&entry, ID_TRACK_NUMBER, tracks[i].number);
    if (!tracks[i].omit_uid)
      buffer_put_uint(&entry, ID_TRACK_UID, tracks[i].uid);
    buffer_put_uint(&entry, ID_TRACK_TYPE, tracks[i].audio ? 2 : 1);
    buffer_put_string(&entry, ID_CODEC_ID, tracks[i].codec);
    buffer_put_master(&payload, ID_TRACK_ENTRY, &entry);
    buffer_destroy(&entry);
  }

  buffer_put_master(segment, ID_TRACKS, &payload);
  buffer_destroy(&payload);
}

static void
put_block_payload(struct buffer * out, uint64_t track, int rel,
                  unsigned char flags, unsigned char const * data,
                  size_t length)
{
  buffer_put_vint(out, track);
  buffer_put_be(out, (uint16_t) (int16_t) rel, 2);
  buffer_put_byte(out, flags);
  buffer_put(out, data, length);
}

static void
put_simple(struct buffer * cluster, uint64_t track, int rel,
           unsigned char const * data, size_t length)
{
  struct buffer block = { 0 };

  put_block_payload(&block, track, rel, 0x80, data, length);
  buffer_put_master(cluster, ID_SIMPLE_BLOCK, &block);
  buffer_destroy(&block);
}

/* SimpleBlock with Xiph lacing holding three one-byte frames. */
static void
put_simple_xiph3(struct buffer * cluster, uint64_t track, int rel,
                 unsigned char const * data)
{
  struct buffer block = { 0 };

  buffer_put_vint(&block, track);
  buffer_put_be(&block, (uint16_t) (int16_t) rel, 2);
  buffer_put_byte(&block, 0x80 | 0x02); /* keyframe, Xiph lacing */
  buffer_put_byte(&block, 2);           /* three frames */
  buffer_put_byte(&block, 1);           /* frame 1 size */
  buffer_put_byte(&block, 1);           /* frame 2 size */
  buffer_put(&block, data, 3);
  buffer_put_master(cluster, ID_SIMPLE_BLOCK, &block);
  buffer_destroy(&block);
}

static void
put_block_group(struct buffer * cluster, uint64_t track, int rel,
                unsigned char data, uint64_t duration, int64_t reference,
                int64_t discard)
{
  struct buffer group = { 0 };
  struct buffer block = { 0 };

  put_block_payload(&block, track, rel, 0x00, &data, 1);
  buffer_put_master(&group, ID_BLOCK, &block);
  buffer_put_uint(&group, ID_BLOCK_DURATION, duration);
  buffer_put_sint(&group, ID_REFERENCE_BLOCK, reference);
  buffer_put_sint(&group, ID_DISCARD_PADDING, discard);
  buffer_put_master(cluster, ID_BLOCK_GROUP, &group);
  buffer_destroy(&block);
  buffer_destroy(&group);
}

static void
put_member(struct buffer * file, struct mspec const * m)
{
  struct buffer segment = { 0 };
  struct buffer cluster = { 0 };
  size_t i;

  put_info(&segment, m->scale, m->duration, m->omit_duration);
  put_tracks_spec(&segment, m->tracks, m->ntracks);

  buffer_put_uint(&cluster, ID_TIMECODE, m->cluster_tc);
  for (i = 0; i < m->nblocks; ++i) {
    struct bspec const * b = &m->blocks[i];

    if (b->kind == BS_SIMPLE)
      put_simple(&cluster, b->track, b->rel, b->payload, b->payload_len);
    else if (b->kind == BS_XIPH3)
      put_simple_xiph3(&cluster, b->track, b->rel, b->payload);
    else
      put_block_group(&cluster, b->track, b->rel, b->payload[0],
                      b->duration, b->reference, b->discard);
  }
  buffer_put_master(&segment, ID_CLUSTER, &cluster);

  if (m->tail_tags) {
    struct buffer tags = { 0 };
    buffer_put_master(&segment, ID_TAGS, &tags);
    buffer_destroy(&tags);
  }

  if (m->tail_cues) {
    uint64_t numbers[2];
    numbers[0] = m->tracks[0].number;
    numbers[1] = m->tracks[1].number;
    put_cues(&segment, numbers, 2, 0);
  }

  if (m->void_pad) {
    size_t left = m->void_pad;
    buffer_put_id(&segment, ID_VOID);
    buffer_put_size(&segment, m->void_pad);
    while (left-- > 0)
      buffer_put_byte(&segment, 0);
  }

  put_ebml_header(file);
  if (m->unknown_size) {
    buffer_put_id(file, ID_SEGMENT);
    buffer_put_byte(file, 0x01); /* 8-byte unknown size: all data bits 1 */
    for (i = 0; i < 7; ++i)
      buffer_put_byte(file, 0xff);
    buffer_put(file, segment.data, segment.length);
  } else {
    buffer_put_master(file, ID_SEGMENT, &segment);
  }

  buffer_destroy(&cluster);
  buffer_destroy(&segment);
}

/* Track sets: the first member establishes the public order (video uid 100
   -> track 0, audio uid 200 -> track 1); later members reorder the Track
   elements and use different TrackNumbers. */
static struct tspec const TRACKS_FIRST[] = {
  { 42, 100, 0, "V_VP8", 0 },
  { 7, 200, 1, "A_OPUS", 0 },
};
static struct tspec const TRACKS_SECOND[] = {
  { 9, 200, 1, "A_OPUS", 0 },
  { 77, 100, 0, "V_VP8", 0 },
};
static struct tspec const TRACKS_THIRD[] = {
  { 5, 100, 0, "V_VP8", 0 },
  { 3, 200, 1, "A_OPUS", 0 },
};
static struct tspec const TRACKS_SECOND_BAD_CODEC[] = {
  { 9, 200, 1, "A_OPUS", 0 },
  { 77, 100, 0, "V_VP9", 0 },
};
static struct tspec const TRACKS_SECOND_BAD_UID[] = {
  { 9, 201, 1, "A_OPUS", 0 },
  { 77, 100, 0, "V_VP8", 0 },
};
static struct tspec const TRACKS_FIRST_NO_UID[] = {
  { 42, 0, 0, "V_VP8", 1 },
  { 7, 200, 1, "A_OPUS", 0 },
};

static struct bspec const BLOCKS_A[] = {
  { 42, 0, BS_SIMPLE, { 0xa1 }, 1, 0, 0, 0 },
  { 7, 10, BS_SIMPLE, { 0xb1 }, 1, 0, 0, 0 },
};
static struct bspec const BLOCKS_C[] = {
  { 3, 0, BS_SIMPLE, { 0xc3 }, 1, 0, 0, 0 },
  { 5, 2, BS_XIPH3, { 0x01, 0x02, 0x03 }, 3, 0, 0, 0 },
  { 5, 4, BS_GROUP, { 0xc5 }, 1, 4, -1, 6 },
};

/* The standard three-member stream:
   - member A: known-size Segment, scale 1e6, declared duration 1e8 ns
     (longer than its last packet), two SimpleBlocks;
   - member B: unknown-size Segment, empty (a Cluster with no blocks),
     declared duration 5e7 ns, trailing Tags;
   - member C: known-size Segment, scale 2e6, declared duration 5e7 ns,
     laced and BlockGroup packets, trailing Cues. */
static void
build_standard(struct buffer * file, size_t void_pad,
               size_t * offset_b, size_t * offset_c)
{
  struct mspec ma = { 1000000, 100.0, 0, TRACKS_FIRST, 2, 40,
                      BLOCKS_A, 2, 0, 0, 0, void_pad };
  struct mspec mb = { 1000000, 50.0, 0, TRACKS_SECOND, 2, 0,
                      NULL, 0, 1, 1, 0, 0 };
  struct mspec mc = { 2000000, 25.0, 0, TRACKS_THIRD, 2, 5,
                      BLOCKS_C, 3, 0, 0, 1, 0 };

  put_member(file, &ma);
  if (offset_b)
    *offset_b = file->length;
  put_member(file, &mb);
  if (offset_c)
    *offset_c = file->length;
  put_member(file, &mc);
}

static void
build_bad_second(struct buffer * file, struct tspec const * bad_tracks)
{
  struct mspec ma = { 1000000, 100.0, 0, TRACKS_FIRST, 2, 40,
                      BLOCKS_A, 2, 0, 0, 0, 0 };
  struct mspec mb = { 1000000, 50.0, 0, bad_tracks, 2, 0,
                      NULL, 0, 1, 0, 0, 0 };
  struct mspec mc = { 2000000, 25.0, 0, TRACKS_THIRD, 2, 5,
                      BLOCKS_C, 3, 0, 0, 0, 0 };

  put_member(file, &ma);
  put_member(file, &mb);
  put_member(file, &mc);
}

static int
init_chained_from_buffer(nestegg ** context, struct buffer const * file,
                         struct buffer_io * userdata)
{
  nestegg_io io;

  userdata->data = file->data;
  userdata->length = file->length;
  userdata->offset = 0;

  memset(&io, 0, sizeof(io));
  io.read = memory_read;
  io.seek = memory_seek;
  io.tell = memory_tell;
  io.userdata = userdata;
  return nestegg_init_chained(context, io, NULL, -1);
}

struct pkt_summary {
  unsigned int track;
  uint64_t tstamp;
  unsigned int frames;
  unsigned char first;
};

/* Expected packet sequence for the standard stream on the continuous
   timeline: bases are 0, 1e8 and 1.5e8 ns from the declared durations. */
static struct pkt_summary const EXPECTED[] = {
  { 0, 40000000, 1, 0xa1 },
  { 1, 50000000, 1, 0xb1 },
  { 1, 160000000, 1, 0xc3 },
  { 0, 164000000, 3, 0x01 },
  { 0, 168000000, 1, 0xc5 },
};
#define EXPECTED_COUNT (sizeof(EXPECTED) / sizeof(EXPECTED[0]))

static int
summaries_match(struct pkt_summary const * got,
                struct pkt_summary const * expected, size_t count)
{
  size_t i;

  for (i = 0; i < count; ++i) {
    if (got[i].track != expected[i].track ||
        got[i].tstamp != expected[i].tstamp ||
        got[i].frames != expected[i].frames ||
        got[i].first != expected[i].first)
      return 0;
  }
  return 1;
}

/* Read packets to end of stream; returns the final nestegg_read_packet
   status and the number of packets seen. */
static int
read_all_summaries(nestegg * ctx, struct pkt_summary * out, size_t max,
                   size_t * count)
{
  size_t n = 0;

  for (;;) {
    nestegg_packet * p = NULL;
    int status = nestegg_read_packet(ctx, &p);

    if (status == 0)
      break;
    if (status != 1) {
      *count = n;
      return status;
    }
    if (n < max) {
      unsigned char * data = NULL;
      size_t length = 0;

      out[n].frames = 0;
      out[n].first = 0;
      if (nestegg_packet_track(p, &out[n].track) != 0 ||
          nestegg_packet_tstamp(p, &out[n].tstamp) != 0 ||
          nestegg_packet_count(p, &out[n].frames) != 0) {
        nestegg_free_packet(p);
        *count = n;
        return -1;
      }
      if (out[n].frames > 0) {
        if (nestegg_packet_data(p, 0, &data, &length) != 0 || length < 1) {
          nestegg_free_packet(p);
          *count = n;
          return -1;
        }
        out[n].first = data[0];
      }
    }
    ++n;
    nestegg_free_packet(p);
  }

  *count = n;
  return 0;
}

/* identity: the public track set comes from the first member and packet
   track indexes follow it even when later members reorder Tracks and
   renumber TrackNumbers. */
static int
case_public_track_identity(void)
{
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  struct pkt_summary got[8];
  uint64_t frames = 0;
  size_t count = 0;
  unsigned int tracks = 0;

  build_standard(&file, 0, NULL, NULL);
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);

  TASK_CHECK(nestegg_track_count(ctx, &tracks) == 0);
  TASK_CHECK(tracks == 2);
  TASK_CHECK(nestegg_track_type(ctx, 0) == NESTEGG_TRACK_VIDEO);
  TASK_CHECK(nestegg_track_type(ctx, 1) == NESTEGG_TRACK_AUDIO);
  TASK_CHECK(nestegg_track_codec_id(ctx, 0) == NESTEGG_CODEC_VP8);
  TASK_CHECK(nestegg_track_codec_id(ctx, 1) == NESTEGG_CODEC_OPUS);

  /* Seeking and whole-stream scans are not supported in chained mode and
     must not disturb sequential reading. */
  TASK_CHECK(nestegg_track_seek(ctx, 0, 0) == -1);
  TASK_CHECK(nestegg_offset_seek(ctx, 0) == -1);
  TASK_CHECK(nestegg_read_last_packet(ctx, 0, &p) == -1);
  TASK_CHECK(p == NULL);
  TASK_CHECK(nestegg_read_total_frames_count(ctx, &frames) == -1);

  TASK_CHECK(read_all_summaries(ctx, got, 8, &count) == 0);
  TASK_CHECK(count == EXPECTED_COUNT);
  TASK_CHECK(summaries_match(got, EXPECTED, EXPECTED_COUNT));

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* identity: plain nestegg_init keeps its old behavior on the same
   concatenated input: first member's packets, then end of stream. */
static int
case_legacy_entry_unchanged(void)
{
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  struct pkt_summary got[4];
  size_t count = 0;
  static struct pkt_summary const expected_legacy[] = {
    { 0, 40000000, 1, 0xa1 },
    { 1, 50000000, 1, 0xb1 },
  };

  build_standard(&file, 0, NULL, NULL);
  TASK_CHECK(init_from_buffer(&ctx, &file, &userdata) == 0);
  TASK_CHECK(read_all_summaries(ctx, got, 4, &count) == 0);
  TASK_CHECK(count == 2);
  TASK_CHECK(summaries_match(got, expected_legacy, 2));

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* identity: a member whose track configuration does not match the public
   set is rejected at the boundary; already delivered packets stand. */
static int
case_member_compatibility(void)
{
  struct tspec const * variants[] = { TRACKS_SECOND_BAD_CODEC,
                                      TRACKS_SECOND_BAD_UID };
  size_t v;

  for (v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
    struct buffer file = { 0 };
    struct buffer_io userdata;
    nestegg * ctx = NULL;
    nestegg_packet * p = NULL;
    unsigned int track = 0;
    uint64_t tstamp = 0;

    build_bad_second(&file, variants[v]);
    TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);

    TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
    TASK_CHECK(nestegg_packet_track(p, &track) == 0 && track == 0);
    TASK_CHECK(nestegg_packet_tstamp(p, &tstamp) == 0 &&
               tstamp == 40000000);
    nestegg_free_packet(p);
    p = NULL;

    TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
    nestegg_free_packet(p);
    p = NULL;

    /* The incompatible member header is an error, not end of stream. */
    TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
    TASK_CHECK(p == NULL);
    TASK_CHECK(nestegg_read_reset(ctx) == 0);
    TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
    TASK_CHECK(p == NULL);

    nestegg_destroy(ctx);
    buffer_destroy(&file);
  }
  return 0;
}

/* identity: the first member must satisfy the chained contract (declared
   positive duration, nonzero unique TrackUIDs); plain nestegg_init is
   unaffected. */
static int
case_first_member_contract(void)
{
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  struct mspec member = { 1000000, 100.0, 0, TRACKS_FIRST, 2, 40,
                          BLOCKS_A, 1, 0, 0, 0, 0 };

  /* Missing Info.Duration: chained init fails, legacy init still works. */
  member.omit_duration = 1;
  put_member(&file, &member);
  ctx = (nestegg *) 1;
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == -1);
  TASK_CHECK(ctx == NULL);
  TASK_CHECK(init_from_buffer(&ctx, &file, &userdata) == 0);
  nestegg_destroy(ctx);
  buffer_destroy(&file);

  /* Zero declared duration. */
  member.omit_duration = 0;
  member.duration = 0.0;
  put_member(&file, &member);
  ctx = (nestegg *) 1;
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == -1);
  TASK_CHECK(ctx == NULL);
  buffer_destroy(&file);

  /* Non-integer declared duration. */
  member.duration = 100.5;
  put_member(&file, &member);
  ctx = (nestegg *) 1;
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == -1);
  TASK_CHECK(ctx == NULL);
  buffer_destroy(&file);

  /* Zero TimecodeScale. */
  member.duration = 100.0;
  member.scale = 0;
  put_member(&file, &member);
  ctx = (nestegg *) 1;
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == -1);
  TASK_CHECK(ctx == NULL);
  buffer_destroy(&file);
  member.scale = 1000000;

  /* Track without a TrackUID. */
  member.duration = 100.0;
  member.tracks = TRACKS_FIRST_NO_UID;
  put_member(&file, &member);
  ctx = (nestegg *) 1;
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == -1);
  TASK_CHECK(ctx == NULL);
  buffer_destroy(&file);

  return 0;
}

/* identity: a known-size Segment must end exactly at its declared end,
   and packet timecodes must stay below the member's declared duration. */
static int
case_segment_bounds(void)
{
  struct buffer file = { 0 };
  struct buffer segment = { 0 };
  struct buffer cluster = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  struct mspec mb = { 1000000, 50.0, 0, TRACKS_SECOND, 2, 0,
                      NULL, 0, 1, 0, 0, 0 };
  struct mspec mshort = { 1000000, 30.0, 0, TRACKS_FIRST, 2, 40,
                          BLOCKS_A, 1, 0, 0, 0, 0 };

  /* Member A whose Segment declares four bytes more than its real
     content: the next member's EBML header does not start at the
     declared end, so the boundary is an error. */
  put_info(&segment, 1000000, 100.0, 0);
  put_tracks_spec(&segment, TRACKS_FIRST, 2);
  buffer_put_uint(&cluster, ID_TIMECODE, 40);
  put_simple(&cluster, 42, 0, BLOCKS_A[0].payload, 1);
  buffer_put_master(&segment, ID_CLUSTER, &cluster);
  put_ebml_header(&file);
  buffer_put_id(&file, ID_SEGMENT);
  buffer_put_size(&file, segment.length + 4);
  buffer_put(&file, segment.data, segment.length);
  put_member(&file, &mb);
  buffer_destroy(&segment);
  buffer_destroy(&cluster);

  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);
  TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
  nestegg_free_packet(p);
  p = NULL;
  TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
  TASK_CHECK(p == NULL);
  nestegg_destroy(ctx);
  buffer_destroy(&file);

  /* Member whose only packet starts at 4e7 ns while the declared
     duration is 3e7 ns: the packet is rejected. */
  put_member(&file, &mshort);
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);
  TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
  TASK_CHECK(p == NULL);
  nestegg_destroy(ctx);
  buffer_destroy(&file);

  return 0;
}

/* identity: a following Segment without its own EBML header is not a
   member and must not be skipped over. */
static int
case_bare_segment_rejected(void)
{
  struct buffer file = { 0 };
  struct buffer bare = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  struct mspec ma = { 1000000, 100.0, 0, TRACKS_FIRST, 2, 40,
                      BLOCKS_A, 2, 0, 0, 0, 0 };
  struct mspec mb = { 1000000, 50.0, 0, TRACKS_SECOND, 2, 0,
                      NULL, 0, 0, 0, 0, 0 };
  size_t ebml_size;

  put_member(&file, &ma);
  /* Build the second member separately and append only its Segment. */
  put_member(&bare, &mb);
  ebml_size = bare.length;
  TASK_CHECK(ebml_size > 0);
  {
    /* The EBML header is the first element; find where it ends by
       re-encoding it on its own. */
    struct buffer hdr = { 0 };
    put_ebml_header(&hdr);
    ebml_size = hdr.length;
    buffer_destroy(&hdr);
  }
  buffer_put(&file, bare.data + ebml_size, bare.length - ebml_size);
  buffer_destroy(&bare);

  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);
  TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
  nestegg_free_packet(p);
  p = NULL;
  TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
  nestegg_free_packet(p);
  p = NULL;
  TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
  TASK_CHECK(p == NULL);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* timeline: packet timestamps are local nanoseconds by each member's own
   TimecodeScale plus the sum of previous members' declared durations;
   trailing gaps and empty members are preserved. */
static int
case_continuous_timeline(void)
{
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  struct pkt_summary got[8];
  uint64_t scale = 0, duration = 0;
  size_t count = 0;

  build_standard(&file, 0, NULL, NULL);
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);

  TASK_CHECK(nestegg_tstamp_scale(ctx, &scale) == 0);
  TASK_CHECK(scale == 1000000);
  TASK_CHECK(nestegg_duration(ctx, &duration) == 0);
  TASK_CHECK(duration == 100000000);

  TASK_CHECK(read_all_summaries(ctx, got, 8, &count) == 0);
  TASK_CHECK(count == EXPECTED_COUNT);
  TASK_CHECK(summaries_match(got, EXPECTED, EXPECTED_COUNT));

  /* The gap between the last packet of member A (5e7 ns) and the first of
     member C (1.6e8 ns) proves the declared durations set the base, not
     the last packet: 1e8 (A, longer than its media) + 5e7 (empty B). */
  TASK_CHECK(got[1].tstamp + 100000000 == 150000000);
  TASK_CHECK(got[2].tstamp == 150000000 + 10000000);

  /* End of stream at the final member boundary repeats cleanly. */
  TASK_CHECK(nestegg_read_packet(ctx, &p) == 0);
  TASK_CHECK(p == NULL);
  TASK_CHECK(nestegg_read_packet(ctx, &p) == 0);
  TASK_CHECK(p == NULL);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* timeline: lacing, BlockDuration, ReferenceBlock and DiscardPadding keep
   their meaning on the continuous timeline. */
static int
case_blockgroup_and_lace(void)
{
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  unsigned char * data = NULL;
  size_t length = 0, i;
  unsigned int count = 0;
  uint64_t uvalue = 0;
  int64_t svalue = 0;
  size_t n;

  build_standard(&file, 0, NULL, NULL);
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);

  /* Skip the three non-laced packets. */
  for (n = 0; n < 3; ++n) {
    TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
    nestegg_free_packet(p);
    p = NULL;
  }

  /* Xiph-laced packet: three one-byte frames. */
  TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
  TASK_CHECK(nestegg_packet_count(p, &count) == 0);
  TASK_CHECK(count == 3);
  TASK_CHECK(nestegg_packet_has_keyframe(p) == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);
  for (i = 0; i < 3; ++i) {
    TASK_CHECK(nestegg_packet_data(p, (unsigned int) i, &data, &length) == 0);
    TASK_CHECK(length == 1);
    TASK_CHECK(data[0] == (unsigned char) (i + 1));
  }
  nestegg_free_packet(p);
  p = NULL;

  /* BlockGroup packet: duration scaled by the member's TimecodeScale,
     reference and discard padding unshifted by the timeline base. */
  TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
  TASK_CHECK(nestegg_packet_tstamp(p, &uvalue) == 0);
  TASK_CHECK(uvalue == 168000000);
  TASK_CHECK(nestegg_packet_duration(p, &uvalue) == 0);
  TASK_CHECK(uvalue == 8000000);
  TASK_CHECK(nestegg_packet_reference_block(p, &svalue) == 0);
  TASK_CHECK(svalue == -1);
  TASK_CHECK(nestegg_packet_discard_padding(p, &svalue) == 0);
  TASK_CHECK(svalue == 6);
  TASK_CHECK(nestegg_packet_has_keyframe(p) == NESTEGG_PACKET_HAS_KEYFRAME_FALSE);
  TASK_CHECK(nestegg_packet_data(p, 0, &data, &length) == 0);
  TASK_CHECK(length == 1 && data[0] == 0xc5);
  nestegg_free_packet(p);
  p = NULL;

  TASK_CHECK(nestegg_read_packet(ctx, &p) == 0);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* timeline: many members accumulate the base from declared durations;
   track mappings and Segment size kinds alternate per member. */
static int
case_many_members(void)
{
  static struct tspec const track_a[] = { { 42, 100, 0, "V_VP8", 0 } };
  static struct tspec const track_b[] = { { 77, 100, 0, "V_VP8", 0 } };
  static struct bspec const block_a[] = {
    { 42, 0, BS_SIMPLE, { 0x5a }, 1, 0, 0, 0 }
  };
  static struct bspec const block_b[] = {
    { 77, 0, BS_SIMPLE, { 0x5a }, 1, 0, 0, 0 }
  };
  enum { MEMBERS = 100 };
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  unsigned int track = 0;
  uint64_t tstamp = 0;
  unsigned char * data = NULL;
  size_t length = 0;
  int i, status = 0;

  for (i = 0; i < MEMBERS; ++i) {
    int odd = i % 2;
    struct mspec m = { 1000000, 10.0, 0, odd ? track_b : track_a, 1, 5,
                       odd ? block_b : block_a, 1,
                       i % 3 == 0 || i == MEMBERS - 1, 0, 0, 0 };
    put_member(&file, &m);
  }

  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);
  for (i = 0; i < MEMBERS; ++i) {
    TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
    TASK_CHECK(nestegg_packet_track(p, &track) == 0 && track == 0);
    TASK_CHECK(nestegg_packet_tstamp(p, &tstamp) == 0);
    TASK_CHECK(tstamp == (uint64_t) i * 10000000 + 5000000);
    TASK_CHECK(nestegg_packet_data(p, 0, &data, &length) == 0);
    TASK_CHECK(length == 1 && data[0] == 0x5a);
    nestegg_free_packet(p);
    p = NULL;
  }
  /* The final member has an unknown-size Segment ending at clean EOF. */
  status = nestegg_read_packet(ctx, &p);
  TASK_CHECK(status == 0);
  TASK_CHECK(p == NULL);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* timeline: a packet returned from an earlier member stays valid while
   reading crosses later members and after the context is destroyed. */
static int
case_held_packet_survives(void)
{
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * held = NULL;
  nestegg_packet * p = NULL;
  unsigned char * data = NULL;
  size_t length = 0;
  unsigned int track = 0;
  uint64_t tstamp = 0;
  size_t n = 0;
  int status;

  build_standard(&file, 0, NULL, NULL);
  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);

  TASK_CHECK(nestegg_read_packet(ctx, &held) == 1);

  while ((status = nestegg_read_packet(ctx, &p)) == 1) {
    ++n;
    nestegg_free_packet(p);
    p = NULL;
  }
  TASK_CHECK(status == 0);
  TASK_CHECK(n == EXPECTED_COUNT - 1);

  nestegg_destroy(ctx);

  TASK_CHECK(nestegg_packet_track(held, &track) == 0);
  TASK_CHECK(track == 0);
  TASK_CHECK(nestegg_packet_tstamp(held, &tstamp) == 0);
  TASK_CHECK(tstamp == 40000000);
  TASK_CHECK(nestegg_packet_data(held, 0, &data, &length) == 0);
  TASK_CHECK(length == 1 && data[0] == 0xa1);
  nestegg_free_packet(held);

  buffer_destroy(&file);
  return 0;
}

/* IO that injects a single one-shot read error at a chosen stream
   offset. */
struct fault_io {
  struct buffer_io mem;
  size_t chunk;
  size_t fail_at;
  int armed;
  int fired;
};

static int64_t
fault_read(void * destination, size_t length, void * userdata)
{
  struct fault_io * f = userdata;
  size_t available;

  if (f->chunk && length > f->chunk)
    length = f->chunk;
  available = f->mem.length - f->mem.offset;
  if (length > available)
    length = available;

  if (f->armed && !f->fired && length > 0 &&
      f->mem.offset <= f->fail_at &&
      f->fail_at < f->mem.offset + length) {
    f->fired = 1;
    return -1;
  }

  if (length == 0)
    return 0;
  memcpy(destination, f->mem.data + f->mem.offset, length);
  f->mem.offset += length;
  return (int64_t) length;
}

static int
fault_seek(int64_t offset, int whence, void * userdata)
{
  struct fault_io * f = userdata;
  return memory_seek(offset, whence, &f->mem);
}

static int64_t
fault_tell(void * userdata)
{
  struct fault_io * f = userdata;
  return memory_tell(&f->mem);
}

/* Read the standard stream (padded so member headers are read after
   initialization) with a one-shot fault inside the header of member
   index 1 (empty member) or 2.  Every failure is retried after
   nestegg_read_reset. */
static int
fault_run(unsigned int member, struct pkt_summary * out, size_t max,
          size_t * count, int * resets)
{
  struct buffer file = { 0 };
  size_t offset_b = 0, offset_c = 0;
  struct fault_io fault;
  nestegg_io io;
  nestegg * ctx = NULL;
  size_t n = 0;
  int status;

  build_standard(&file, 9000, &offset_b, &offset_c);

  memset(&fault, 0, sizeof(fault));
  fault.mem.data = file.data;
  fault.mem.length = file.length;
  fault.chunk = 512;
  fault.fail_at = (member == 0 ? offset_b : offset_c) + 1;

  memset(&io, 0, sizeof(io));
  io.read = fault_read;
  io.seek = fault_seek;
  io.tell = fault_tell;
  io.userdata = &fault;

  if (nestegg_init_chained(&ctx, io, NULL, -1) != 0) {
    buffer_destroy(&file);
    return -1;
  }
  fault.armed = 1;

  *resets = 0;
  for (;;) {
    nestegg_packet * p = NULL;

    status = nestegg_read_packet(ctx, &p);
    if (status == 0)
      break;
    if (status < 0) {
      if (!fault.fired || *resets >= 4)
        break;
      if (nestegg_read_reset(ctx) != 0)
        break;
      ++*resets;
      continue;
    }
    if (n < max) {
      unsigned char * data = NULL;
      size_t length = 0;

      out[n].frames = 0;
      out[n].first = 0;
      if (nestegg_packet_track(p, &out[n].track) != 0 ||
          nestegg_packet_tstamp(p, &out[n].tstamp) != 0 ||
          nestegg_packet_count(p, &out[n].frames) != 0 ||
          nestegg_packet_data(p, 0, &data, &length) != 0 || length < 1) {
        nestegg_free_packet(p);
        break;
      }
      out[n].first = data[0];
    }
    ++n;
    nestegg_free_packet(p);
  }

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  *count = n;
  return 0;
}

/* recovery: after a one-shot IO fault while crossing a member boundary
   (including an empty member), read_reset plus a retry reproduces the
   fault-free packet sequence exactly, with no doubled member duration. */
static int
case_transient_io_fault(void)
{
  struct pkt_summary got[8];
  size_t count = 0;
  int resets = 0;
  unsigned int member;

  for (member = 0; member < 2; ++member) {
    memset(got, 0, sizeof(got));
    TASK_CHECK(fault_run(member, got, 8, &count, &resets) == 0);
    TASK_CHECK(resets >= 1);
    TASK_CHECK(count == EXPECTED_COUNT);
    TASK_CHECK(summaries_match(got, EXPECTED, EXPECTED_COUNT));
  }
  return 0;
}

/* recovery: a member header truncated at the boundary is an error;
   read_reset restores the pre-call position and retries fail cleanly. */
static int
case_truncated_member_header(void)
{
  struct buffer full = { 0 };
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  size_t offset_c = 0;
  unsigned int track = 0;
  uint64_t tstamp = 0;

  build_standard(&full, 0, NULL, &offset_c);
  /* Keep only the first three bytes of the final member's EBML header. */
  buffer_put(&file, full.data, offset_c + 3);
  buffer_destroy(&full);

  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);

  TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
  TASK_CHECK(nestegg_packet_track(p, &track) == 0 && track == 0);
  TASK_CHECK(nestegg_packet_tstamp(p, &tstamp) == 0 &&
             tstamp == 40000000);
  nestegg_free_packet(p);
  p = NULL;

  TASK_CHECK(nestegg_read_packet(ctx, &p) == 1);
  nestegg_free_packet(p);
  p = NULL;

  TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
  TASK_CHECK(p == NULL);
  TASK_CHECK(nestegg_read_reset(ctx) == 0);
  TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
  TASK_CHECK(p == NULL);
  TASK_CHECK(nestegg_read_reset(ctx) == 0);
  TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
  TASK_CHECK(p == NULL);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

/* recovery: truncation inside a trailing element is an error, not a clean
   end of stream, and read_reset leaves the context reusable. */
static int
case_truncated_tail_retry(void)
{
  struct buffer full = { 0 };
  struct buffer file = { 0 };
  struct buffer_io userdata;
  nestegg * ctx = NULL;
  nestegg_packet * p = NULL;
  struct pkt_summary got[8];
  size_t count = 0;

  build_standard(&full, 0, NULL, NULL);
  TASK_CHECK(full.length > 3);
  buffer_put(&file, full.data, full.length - 3);
  buffer_destroy(&full);

  TASK_CHECK(init_chained_from_buffer(&ctx, &file, &userdata) == 0);

  TASK_CHECK(read_all_summaries(ctx, got, 8, &count) == -1);
  TASK_CHECK(count == EXPECTED_COUNT);
  TASK_CHECK(summaries_match(got, EXPECTED, EXPECTED_COUNT));

  TASK_CHECK(nestegg_read_reset(ctx) == 0);
  TASK_CHECK(nestegg_read_packet(ctx, &p) == -1);
  TASK_CHECK(p == NULL);

  nestegg_destroy(ctx);
  buffer_destroy(&file);
  return 0;
}

const struct task_case task_cases[] = {
  { "identity", "public_track_identity", case_public_track_identity },
  { "identity", "legacy_entry_unchanged", case_legacy_entry_unchanged },
  { "identity", "member_compatibility", case_member_compatibility },
  { "identity", "first_member_contract", case_first_member_contract },
  { "identity", "segment_bounds", case_segment_bounds },
  { "identity", "bare_segment_rejected", case_bare_segment_rejected },
  { "timeline", "continuous_timeline", case_continuous_timeline },
  { "timeline", "blockgroup_and_lace", case_blockgroup_and_lace },
  { "timeline", "many_members", case_many_members },
  { "timeline", "held_packet_survives", case_held_packet_survives },
  { "recovery", "transient_io_fault", case_transient_io_fault },
  { "recovery", "truncated_member_header", case_truncated_member_header },
  { "recovery", "truncated_tail_retry", case_truncated_tail_retry },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
