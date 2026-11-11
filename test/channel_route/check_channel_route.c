/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

/*
 * Offline checks for example/channel_route.c, the routing and sample moving
 * logic used by the sio_microphone example. Fake begin/end hooks and an
 * in-memory ring buffer drive the exact same pump functions the example
 * callbacks use. No audio backend is started and no sound card is needed.
 */

#include "../../example/channel_route.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_FL SoundIoChannelIdFrontLeft
#define CH_FR SoundIoChannelIdFrontRight
#define CH_FC SoundIoChannelIdFrontCenter
#define CH_LFE SoundIoChannelIdLfe
#define CH_BL SoundIoChannelIdBackLeft
#define CH_BR SoundIoChannelIdBackRight
#define CH_SL SoundIoChannelIdSideLeft
#define CH_SR SoundIoChannelIdSideRight

static int failures;

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures += 1; \
        } \
    } while (0)

static void test_panic(void *userdata, int err, const char *msg) {
    (void)userdata;
    (void)err;
    fprintf(stderr, "unexpected panic: %s\n", msg);
    exit(1);
}

static struct SoundIoChannelLayout make_layout(const char *name, int count,
        const enum SoundIoChannelId *channels)
{
    struct SoundIoChannelLayout layout;
    layout.name = name;
    layout.channel_count = count;
    for (int i = 0; i < SOUNDIO_MAX_CHANNELS; i += 1)
        layout.channels[i] = SoundIoChannelIdInvalid;
    for (int i = 0; i < count && i < SOUNDIO_MAX_CHANNELS; i += 1)
        layout.channels[i] = channels[i];
    return layout;
}

// A fake stream hands out chunks of frames according to a script. A chunk
// size of 0 makes begin return frame_count 0 ("no frames right now"). For
// input streams a chunk flagged in `holes` reports areas == NULL (dropped
// frames) and does not consume source frames.
struct FakeStream {
    int channel_count;
    struct SoundIoChannelArea base[SOUNDIO_MAX_CHANNELS];   // saved descriptors
    struct SoundIoChannelArea handed[SOUNDIO_MAX_CHANNELS]; // last handed out
    struct SoundIoChannelArea snapshot[SOUNDIO_MAX_CHANNELS];
    int total_frames;
    int pos;
    const int *chunks;
    const char *holes;
    int chunk_count;
    int chunk_index;
    int cur_chunk_frames;
    int cur_chunk_hole;
    int begin_calls;
    int end_calls;
    int areas_modified;
};

static void fake_init(struct FakeStream *f, int channel_count) {
    memset(f, 0, sizeof(*f));
    f->channel_count = channel_count;
}

static int fake_begin_read(void *userdata, struct SoundIoChannelArea **areas, int *frame_count) {
    struct FakeStream *f = (struct FakeStream *)userdata;
    f->begin_calls += 1;
    if (f->chunk_index >= f->chunk_count) {
        *frame_count = 0;
        return 0;
    }
    int chunk = f->chunks[f->chunk_index];
    int is_hole = f->holes && f->holes[f->chunk_index];
    f->chunk_index += 1;
    if (chunk <= 0) {
        *frame_count = 0;
        return 0;
    }
    int n = chunk;
    if (!is_hole && n > f->total_frames - f->pos)
        n = f->total_frames - f->pos;
    if (n > *frame_count)
        n = *frame_count;
    if (n <= 0) {
        *frame_count = 0;
        return 0;
    }
    if (is_hole) {
        *areas = NULL;
    } else {
        for (int ch = 0; ch < f->channel_count; ch += 1) {
            f->handed[ch].ptr = f->base[ch].ptr + (long)f->pos * f->base[ch].step;
            f->handed[ch].step = f->base[ch].step;
        }
        memcpy(f->snapshot, f->handed, sizeof(f->handed));
        *areas = f->handed;
        f->pos += n;
    }
    f->cur_chunk_frames = n;
    f->cur_chunk_hole = is_hole;
    *frame_count = n;
    return 0;
}

static int fake_end_read(void *userdata) {
    struct FakeStream *f = (struct FakeStream *)userdata;
    f->end_calls += 1;
    if (!f->cur_chunk_hole && memcmp(f->handed, f->snapshot, sizeof(f->handed)) != 0)
        f->areas_modified = 1;
    return 0;
}

static int fake_begin_write(void *userdata, struct SoundIoChannelArea **areas, int *frame_count) {
    struct FakeStream *f = (struct FakeStream *)userdata;
    f->begin_calls += 1;
    if (f->chunk_index >= f->chunk_count) {
        *frame_count = 0;
        return 0;
    }
    int chunk = f->chunks[f->chunk_index];
    f->chunk_index += 1;
    if (chunk <= 0) {
        *frame_count = 0;
        return 0;
    }
    int n = chunk;
    if (n > f->total_frames - f->pos)
        n = f->total_frames - f->pos;
    if (n > *frame_count)
        n = *frame_count;
    if (n <= 0) {
        *frame_count = 0;
        return 0;
    }
    for (int ch = 0; ch < f->channel_count; ch += 1) {
        f->handed[ch].ptr = f->base[ch].ptr + (long)f->pos * f->base[ch].step;
        f->handed[ch].step = f->base[ch].step;
    }
    memcpy(f->snapshot, f->handed, sizeof(f->handed));
    *areas = f->handed;
    f->cur_chunk_frames = n;
    f->cur_chunk_hole = 0;
    *frame_count = n;
    return 0;
}

static int fake_end_write(void *userdata) {
    struct FakeStream *f = (struct FakeStream *)userdata;
    f->end_calls += 1;
    if (memcmp(f->handed, f->snapshot, sizeof(f->handed)) != 0)
        f->areas_modified = 1;
    f->pos += f->cur_chunk_frames;
    return 0;
}

// In-memory ring buffer standing in for SoundIoRingBuffer.
#define RING_BYTES 8192
struct Ring {
    char buf[RING_BYTES];
    int fill; // bytes
};

static int ring_pump_read(struct ChannelRoutePipe *pipe, struct Ring *ring,
        int frame_count_min, int frame_count_max)
{
    int free_frames = (RING_BYTES - ring->fill) / pipe->bytes_per_frame;
    int done = channel_route_pump_read(pipe, ring->buf + ring->fill, free_frames,
            frame_count_min, frame_count_max);
    ring->fill += done * pipe->bytes_per_frame;
    return done;
}

static int ring_pump_write(struct ChannelRoutePipe *pipe, struct Ring *ring,
        int frame_count_min, int frame_count_max)
{
    int fill_frames = ring->fill / pipe->bytes_per_frame;
    int used = channel_route_pump_write(pipe, ring->buf, fill_frames,
            frame_count_min, frame_count_max);
    ring->fill -= used * pipe->bytes_per_frame;
    memmove(ring->buf, ring->buf + used * pipe->bytes_per_frame, ring->fill);
    return used;
}

static void pipe_init(struct ChannelRoutePipe *pipe, const struct ChannelRoute *route,
        int bytes_per_sample)
{
    memset(pipe, 0, sizeof(*pipe));
    pipe->route = *route;
    pipe->bytes_per_sample = bytes_per_sample;
    pipe->bytes_per_frame = bytes_per_sample * route->in_layout->channel_count;
    pipe->begin_read = fake_begin_read;
    pipe->end_read = fake_end_read;
    pipe->begin_write = fake_begin_write;
    pipe->end_write = fake_end_write;
    pipe->panic = test_panic;
}

// ---------------------------------------------------------------------
// Negotiation
// ---------------------------------------------------------------------

static void test_negotiate_exact_order_preferred(void) {
    enum SoundIoChannelId fl_fr[] = { CH_FL, CH_FR };
    enum SoundIoChannelId fr_fl[] = { CH_FR, CH_FL };
    struct SoundIoChannelLayout out_layouts[1];
    struct SoundIoChannelLayout in_layouts[2];
    out_layouts[0] = make_layout("stereo", 2, fl_fr);
    in_layouts[0] = make_layout("reversed", 2, fr_fl); // permuted, listed first
    in_layouts[1] = make_layout("stereo", 2, fl_fr);   // exact match

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, out_layouts, 1, in_layouts, 2));
    CHECK(route.out_layout == &out_layouts[0]);
    CHECK(route.in_layout == &in_layouts[1]);
    CHECK(route.output_to_input[0] == 0);
    CHECK(route.output_to_input[1] == 1);
}

static void test_negotiate_permutation(void) {
    enum SoundIoChannelId out_order[] = { CH_FR, CH_FL, CH_FC };
    enum SoundIoChannelId in_order[] = { CH_FL, CH_FC, CH_FR };
    struct SoundIoChannelLayout out_layouts[1];
    struct SoundIoChannelLayout in_layouts[1];
    out_layouts[0] = make_layout("out", 3, out_order);
    in_layouts[0] = make_layout("in", 3, in_order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, out_layouts, 1, in_layouts, 1));
    CHECK(route.in_layout == &in_layouts[0]);
    CHECK(route.output_to_input[0] == 2); // FR comes from input channel 2
    CHECK(route.output_to_input[1] == 0); // FL comes from input channel 0
    CHECK(route.output_to_input[2] == 1); // FC comes from input channel 1
}

static void test_negotiate_output_priority(void) {
    // The first output layout that has any match wins, even when a later
    // output layout would have matched exactly.
    enum SoundIoChannelId fl_fr[] = { CH_FL, CH_FR };
    enum SoundIoChannelId fr_fl[] = { CH_FR, CH_FL };
    struct SoundIoChannelLayout out_layouts[2];
    struct SoundIoChannelLayout in_layouts[1];
    out_layouts[0] = make_layout("reversed", 2, fr_fl);
    out_layouts[1] = make_layout("stereo", 2, fl_fr);
    in_layouts[0] = make_layout("stereo", 2, fl_fr);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, out_layouts, 2, in_layouts, 1));
    CHECK(route.out_layout == &out_layouts[0]);
    CHECK(route.in_layout == &in_layouts[0]);
    CHECK(route.output_to_input[0] == 1);
    CHECK(route.output_to_input[1] == 0);
}

static void test_negotiate_roles_not_guessed(void) {
    // BackLeft/BackRight must not be matched with SideLeft/SideRight.
    enum SoundIoChannelId back[] = { CH_BL, CH_BR };
    enum SoundIoChannelId side[] = { CH_SL, CH_SR };
    struct SoundIoChannelLayout out_layouts[1];
    struct SoundIoChannelLayout in_layouts[1];
    out_layouts[0] = make_layout("back", 2, back);
    in_layouts[0] = make_layout("side", 2, side);

    struct ChannelRoute route;
    CHECK(!channel_route_negotiate(&route, out_layouts, 1, in_layouts, 1));
}

static void test_negotiate_skips_unsupported(void) {
    enum SoundIoChannelId fl_fr[] = { CH_FL, CH_FR };
    enum SoundIoChannelId with_invalid[] = { CH_FL, SoundIoChannelIdInvalid };
    enum SoundIoChannelId duplicate[] = { CH_FL, CH_FL };
    enum SoundIoChannelId out_of_enum[] = { CH_FL, (enum SoundIoChannelId)(SoundIoChannelIdAux15 + 1) };
    enum SoundIoChannelId fr_fl[] = { CH_FR, CH_FL };
    struct SoundIoChannelLayout out_layouts[1];
    struct SoundIoChannelLayout in_layouts[6];
    out_layouts[0] = make_layout("stereo", 2, fl_fr);
    in_layouts[0] = make_layout("zero channels", 0, fl_fr);
    in_layouts[1] = make_layout("too many channels", SOUNDIO_MAX_CHANNELS + 1, fl_fr);
    in_layouts[2] = make_layout("invalid role", 2, with_invalid);
    in_layouts[3] = make_layout("duplicate role", 2, duplicate);
    in_layouts[4] = make_layout("role out of range", 2, out_of_enum);
    in_layouts[5] = make_layout("reversed", 2, fr_fl);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, out_layouts, 1, in_layouts, 6));
    CHECK(route.in_layout == &in_layouts[5]);
    CHECK(route.output_to_input[0] == 1);
    CHECK(route.output_to_input[1] == 0);

    // Same channel count but a different role set is not compatible.
    enum SoundIoChannelId fl_fc[] = { CH_FL, CH_FC };
    struct SoundIoChannelLayout other_in[1];
    other_in[0] = make_layout("other", 2, fl_fc);
    CHECK(!channel_route_negotiate(&route, out_layouts, 1, other_in, 1));
}

// ---------------------------------------------------------------------
// Sample moving
// ---------------------------------------------------------------------

// The example from the task: input [FL,FC,FR] with frames [1,10,100] and
// [2,20,200], output order [FR,FL,FC] must receive [100,1,10],[200,2,20].
static void test_pump_three_channel_permutation(void) {
    enum SoundIoChannelId in_order[] = { CH_FL, CH_FC, CH_FR };
    enum SoundIoChannelId out_order[] = { CH_FR, CH_FL, CH_FC };
    struct SoundIoChannelLayout in_layouts[1];
    struct SoundIoChannelLayout out_layouts[1];
    in_layouts[0] = make_layout("in", 3, in_order);
    out_layouts[0] = make_layout("out", 3, out_order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, out_layouts, 1, in_layouts, 1));

    struct ChannelRoutePipe pipe;
    pipe_init(&pipe, &route, 2); // S16NE

    static const unsigned short src[6] = { 1, 10, 100, 2, 20, 200 };
    struct FakeStream in;
    fake_init(&in, 3);
    for (int ch = 0; ch < 3; ch += 1) {
        in.base[ch].ptr = (char *)src + ch * 2;
        in.base[ch].step = 6;
    }
    in.total_frames = 2;
    static const int in_chunks[] = { 2 };
    in.chunks = in_chunks;
    in.chunk_count = 1;

    struct Ring ring;
    memset(&ring, 0, sizeof(ring));

    pipe.userdata = &in;
    CHECK(ring_pump_read(&pipe, &ring, 0, 2) == 2);
    CHECK(ring.fill == 2 * 6);
    CHECK(memcmp(ring.buf, src, sizeof(src)) == 0);

    static unsigned short dst[6];
    struct FakeStream out;
    fake_init(&out, 3);
    for (int ch = 0; ch < 3; ch += 1) {
        out.base[ch].ptr = (char *)dst + ch * 2;
        out.base[ch].step = 6;
    }
    out.total_frames = 2;
    static const int out_chunks[] = { 2 };
    out.chunks = out_chunks;
    out.chunk_count = 1;

    pipe.userdata = &out;
    CHECK(ring_pump_write(&pipe, &ring, 0, 2) == 2);
    CHECK(ring.fill == 0);

    static const unsigned short want[6] = { 100, 1, 10, 200, 2, 20 };
    CHECK(memcmp(dst, want, sizeof(want)) == 0);
    CHECK(!in.areas_modified);
    CHECK(!out.areas_modified);
    CHECK(in.end_calls == 1);
    CHECK(out.end_calls == 1);
}

// Same permutation, run for sample widths 1, 2, 4 and 8 bytes with raw byte
// patterns. The 4 byte width also covers the 24 bit formats, which occupy 4
// bytes per sample by library convention.
static void test_pump_sample_widths(void) {
    enum SoundIoChannelId in_order[] = { CH_FL, CH_FC, CH_FR };
    enum SoundIoChannelId out_order[] = { CH_FR, CH_FL, CH_FC };
    struct SoundIoChannelLayout in_layouts[1];
    struct SoundIoChannelLayout out_layouts[1];
    in_layouts[0] = make_layout("in", 3, in_order);
    out_layouts[0] = make_layout("out", 3, out_order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, out_layouts, 1, in_layouts, 1));
    static const int map[3] = { 2, 0, 1 };

    static const int widths[] = { 1, 2, 4, 8 };
    for (int w = 0; w < 4; w += 1) {
        int bps = widths[w];
        struct ChannelRoutePipe pipe;
        pipe_init(&pipe, &route, bps);

        unsigned char src[2 * 3 * 8];
        for (int i = 0; i < 2 * 3 * bps; i += 1)
            src[i] = (unsigned char)(i + 1);

        struct FakeStream in;
        fake_init(&in, 3);
        for (int ch = 0; ch < 3; ch += 1) {
            in.base[ch].ptr = (char *)src + ch * bps;
            in.base[ch].step = 3 * bps;
        }
        in.total_frames = 2;
        static const int chunks[] = { 2 };
        in.chunks = chunks;
        in.chunk_count = 1;

        struct Ring ring;
        memset(&ring, 0, sizeof(ring));
        pipe.userdata = &in;
        CHECK(ring_pump_read(&pipe, &ring, 0, 2) == 2);
        CHECK(ring.fill == 2 * 3 * bps);
        CHECK(memcmp(ring.buf, src, 2 * 3 * bps) == 0);

        unsigned char dst[2 * 3 * 8];
        memset(dst, 0, sizeof(dst));
        struct FakeStream out;
        fake_init(&out, 3);
        for (int ch = 0; ch < 3; ch += 1) {
            out.base[ch].ptr = (char *)dst + ch * bps;
            out.base[ch].step = 3 * bps;
        }
        out.total_frames = 2;
        out.chunks = chunks;
        out.chunk_count = 1;

        pipe.userdata = &out;
        CHECK(ring_pump_write(&pipe, &ring, 0, 2) == 2);
        CHECK(ring.fill == 0);

        unsigned char want[2 * 3 * 8];
        for (int frame = 0; frame < 2; frame += 1)
            for (int out_ch = 0; out_ch < 3; out_ch += 1)
                memcpy(want + (frame * 3 + out_ch) * bps,
                        src + (frame * 3 + map[out_ch]) * bps, bps);
        CHECK(memcmp(dst, want, 2 * 3 * bps) == 0);
        CHECK(!in.areas_modified);
        CHECK(!out.areas_modified);
    }
}

// Six channels, planar input and planar output areas (step == sample width),
// frames delivered in 2+1 segments.
static void test_pump_six_channel_planar(void) {
    enum SoundIoChannelId in_order[] = { CH_FL, CH_FR, CH_FC, CH_LFE, CH_BL, CH_BR };
    enum SoundIoChannelId out_order[] = { CH_BL, CH_BR, CH_FL, CH_FR, CH_FC, CH_LFE };
    struct SoundIoChannelLayout in_layouts[1];
    struct SoundIoChannelLayout out_layouts[1];
    in_layouts[0] = make_layout("5.1", 6, in_order);
    out_layouts[0] = make_layout("reordered", 6, out_order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, out_layouts, 1, in_layouts, 1));
    static const int map[6] = { 4, 5, 0, 1, 2, 3 };
    for (int ch = 0; ch < 6; ch += 1)
        CHECK(route.output_to_input[ch] == map[ch]);

    const int bps = 8; // Float64NE
    const int frames = 3;
    struct ChannelRoutePipe pipe;
    pipe_init(&pipe, &route, bps);

    unsigned char src[6][3 * 8];
    for (int ch = 0; ch < 6; ch += 1)
        for (int i = 0; i < frames * bps; i += 1)
            src[ch][i] = (unsigned char)(ch * 24 + i + 1);

    struct FakeStream in;
    fake_init(&in, 6);
    for (int ch = 0; ch < 6; ch += 1) {
        in.base[ch].ptr = (char *)src[ch];
        in.base[ch].step = bps; // planar
    }
    in.total_frames = frames;
    static const int in_chunks[] = { 2, 1 };
    in.chunks = in_chunks;
    in.chunk_count = 2;

    struct Ring ring;
    memset(&ring, 0, sizeof(ring));
    pipe.userdata = &in;
    CHECK(ring_pump_read(&pipe, &ring, 0, frames) == frames);
    CHECK(ring.fill == frames * 6 * bps);
    CHECK(in.end_calls == 2);

    // Ring holds frames interleaved in the input's native channel order.
    unsigned char want_ring[3 * 6 * 8];
    for (int frame = 0; frame < frames; frame += 1)
        for (int ch = 0; ch < 6; ch += 1)
            memcpy(want_ring + (frame * 6 + ch) * bps, src[ch] + frame * bps, bps);
    CHECK(memcmp(ring.buf, want_ring, sizeof(want_ring)) == 0);

    unsigned char dst[6][3 * 8];
    memset(dst, 0, sizeof(dst));
    struct FakeStream out;
    fake_init(&out, 6);
    for (int ch = 0; ch < 6; ch += 1) {
        out.base[ch].ptr = (char *)dst[ch];
        out.base[ch].step = bps; // planar
    }
    out.total_frames = frames;
    static const int out_chunks[] = { 2, 1 };
    out.chunks = out_chunks;
    out.chunk_count = 2;

    pipe.userdata = &out;
    CHECK(ring_pump_write(&pipe, &ring, 0, frames) == frames);
    CHECK(ring.fill == 0);
    CHECK(out.end_calls == 2);

    for (int out_ch = 0; out_ch < 6; out_ch += 1)
        CHECK(memcmp(dst[out_ch], src[map[out_ch]], frames * bps) == 0);
    CHECK(!in.areas_modified);
    CHECK(!out.areas_modified);
}

// Interleaved output with padding between samples: step > sample width.
// The padding bytes must be left untouched.
static void test_pump_interleaved_with_padding(void) {
    enum SoundIoChannelId order[] = { CH_FL, CH_FR };
    struct SoundIoChannelLayout layouts[1];
    layouts[0] = make_layout("stereo", 2, order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, layouts, 1, layouts, 1));

    const int bps = 1; // S8
    const int frames = 3;
    const int step = 3; // 2 channels * 1 byte + 1 byte padding
    struct ChannelRoutePipe pipe;
    pipe_init(&pipe, &route, bps);

    unsigned char src[3] = { 7, 8, 9 };
    unsigned char src2[3] = { 70, 80, 90 };
    struct FakeStream in;
    fake_init(&in, 2);
    in.base[0].ptr = (char *)src;
    in.base[0].step = 1;
    in.base[1].ptr = (char *)src2;
    in.base[1].step = 1;
    in.total_frames = frames;
    static const int in_chunks[] = { 3 };
    in.chunks = in_chunks;
    in.chunk_count = 1;

    struct Ring ring;
    memset(&ring, 0, sizeof(ring));
    pipe.userdata = &in;
    CHECK(ring_pump_read(&pipe, &ring, 0, frames) == frames);
    static const unsigned char want_ring[6] = { 7, 70, 8, 80, 9, 90 };
    CHECK(memcmp(ring.buf, want_ring, sizeof(want_ring)) == 0);

    unsigned char dst[3 * 3];
    memset(dst, 0xAA, sizeof(dst));
    struct FakeStream out;
    fake_init(&out, 2);
    out.base[0].ptr = (char *)dst;
    out.base[0].step = step;
    out.base[1].ptr = (char *)dst + 1;
    out.base[1].step = step;
    out.total_frames = frames;
    static const int out_chunks[] = { 3 };
    out.chunks = out_chunks;
    out.chunk_count = 1;

    pipe.userdata = &out;
    CHECK(ring_pump_write(&pipe, &ring, 0, frames) == frames);
    CHECK(ring.fill == 0);

    static const unsigned char want_dst[9] = {
        7, 70, 0xAA,
        8, 80, 0xAA,
        9, 90, 0xAA,
    };
    CHECK(memcmp(dst, want_dst, sizeof(want_dst)) == 0);
    CHECK(!in.areas_modified);
    CHECK(!out.areas_modified);
}

// A hole (areas == NULL) occupies frames in the ring as silence, and the
// following normal frames are written right after it. Uses Float32NE.
static void test_pump_read_hole_then_normal(void) {
    enum SoundIoChannelId order[] = { CH_FL, CH_FR };
    struct SoundIoChannelLayout layouts[1];
    layouts[0] = make_layout("stereo", 2, order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, layouts, 1, layouts, 1));

    const int bps = 4; // Float32NE
    struct ChannelRoutePipe pipe;
    pipe_init(&pipe, &route, bps);

    unsigned char src[2 * 4];
    for (int i = 0; i < 8; i += 1)
        src[i] = (unsigned char)(0x40 + i);

    struct FakeStream in;
    fake_init(&in, 2);
    in.base[0].ptr = (char *)src;
    in.base[0].step = 8;
    in.base[1].ptr = (char *)src + 4;
    in.base[1].step = 8;
    in.total_frames = 1;
    static const int chunks[] = { 2, 1 };
    static const char holes[] = { 1, 0 };
    in.chunks = chunks;
    in.holes = holes;
    in.chunk_count = 2;

    struct Ring ring;
    memset(&ring, 0, sizeof(ring));
    pipe.userdata = &in;
    CHECK(ring_pump_read(&pipe, &ring, 0, 3) == 3);
    CHECK(ring.fill == 3 * 8);

    unsigned char want[3 * 8];
    memset(want, 0, 16); // the hole is all-zero silence
    memcpy(want + 16, src, 8);
    CHECK(memcmp(ring.buf, want, sizeof(want)) == 0);
    CHECK(in.pos == 1); // the hole did not consume source frames
    CHECK(in.end_calls == 2);
}

// begin_read may return 0 frames mid-request; only the completed frames
// advance and the next callback continues without skipping or replaying.
static void test_pump_read_zero_chunk(void) {
    enum SoundIoChannelId order[] = { CH_FL, CH_FR };
    struct SoundIoChannelLayout layouts[1];
    layouts[0] = make_layout("stereo", 2, order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, layouts, 1, layouts, 1));

    struct ChannelRoutePipe pipe;
    pipe_init(&pipe, &route, 2); // S16NE

    static const unsigned short src[6] = { 1, 2, 3, 4, 5, 6 };
    struct FakeStream in;
    fake_init(&in, 2);
    in.base[0].ptr = (char *)src;
    in.base[0].step = 4;
    in.base[1].ptr = (char *)src + 2;
    in.base[1].step = 4;
    in.total_frames = 3;
    static const int chunks[] = { 2, 0, 1 };
    in.chunks = chunks;
    in.chunk_count = 3;

    struct Ring ring;
    memset(&ring, 0, sizeof(ring));
    pipe.userdata = &in;
    CHECK(ring_pump_read(&pipe, &ring, 0, 3) == 2);
    CHECK(ring.fill == 2 * 4);
    CHECK(ring_pump_read(&pipe, &ring, 0, 3) == 1);
    CHECK(ring.fill == 3 * 4);
    CHECK(memcmp(ring.buf, src, sizeof(src)) == 0);
    CHECK(in.end_calls == 2); // the 0-frame begin has no matching end
}

// begin_write may split a request into 2+1 frames and may return 0 frames;
// only completed frames are consumed from the ring.
static void test_pump_write_segmented_and_zero(void) {
    enum SoundIoChannelId order[] = { CH_FL, CH_FR };
    struct SoundIoChannelLayout layouts[1];
    layouts[0] = make_layout("stereo", 2, order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, layouts, 1, layouts, 1));

    struct ChannelRoutePipe pipe;
    pipe_init(&pipe, &route, 2);

    static const unsigned short src[6] = { 11, 12, 13, 14, 15, 16 };
    struct Ring ring;
    memset(&ring, 0, sizeof(ring));
    memcpy(ring.buf, src, sizeof(src));
    ring.fill = sizeof(src);

    unsigned short dst[6];
    memset(dst, 0, sizeof(dst));
    struct FakeStream out;
    fake_init(&out, 2);
    out.base[0].ptr = (char *)dst;
    out.base[0].step = 4;
    out.base[1].ptr = (char *)dst + 2;
    out.base[1].step = 4;
    out.total_frames = 3;
    static const int chunks[] = { 2, 0, 1 };
    out.chunks = chunks;
    out.chunk_count = 3;

    pipe.userdata = &out;
    CHECK(ring_pump_write(&pipe, &ring, 0, 3) == 2);
    CHECK(ring.fill == 1 * 4);
    CHECK(ring_pump_write(&pipe, &ring, 0, 3) == 1);
    CHECK(ring.fill == 0);
    CHECK(memcmp(dst, src, sizeof(src)) == 0);
    CHECK(out.end_calls == 2);
    CHECK(!out.areas_modified);
}

// Underrun: not enough data in the ring, so silence is written and no
// unplayed ring data is consumed.
static void test_pump_write_underrun_silence(void) {
    enum SoundIoChannelId order[] = { CH_FL, CH_FR };
    struct SoundIoChannelLayout layouts[1];
    layouts[0] = make_layout("stereo", 2, order);

    struct ChannelRoute route;
    CHECK(channel_route_negotiate(&route, layouts, 1, layouts, 1));

    struct ChannelRoutePipe pipe;
    pipe_init(&pipe, &route, 2);

    static const unsigned short src[2] = { 0x1234, 0x5678 };
    struct Ring ring;
    memset(&ring, 0, sizeof(ring));
    memcpy(ring.buf, src, sizeof(src));
    ring.fill = sizeof(src); // exactly 1 frame

    unsigned short dst[6];
    memset(dst, 0xFF, sizeof(dst));
    struct FakeStream out;
    fake_init(&out, 2);
    out.base[0].ptr = (char *)dst;
    out.base[0].step = 4;
    out.base[1].ptr = (char *)dst + 2;
    out.base[1].step = 4;
    out.total_frames = 3;
    static const int chunks[] = { 2, 1 };
    out.chunks = chunks;
    out.chunk_count = 2;

    pipe.userdata = &out;
    CHECK(ring_pump_write(&pipe, &ring, 3, 5) == 0);
    CHECK(ring.fill == sizeof(src)); // nothing consumed
    static const unsigned short zeros[3 * 2] = { 0, 0, 0, 0, 0, 0 };
    CHECK(memcmp(dst, zeros, sizeof(zeros)) == 0);

    // The buffered frame is still played afterwards.
    out.pos = 0;
    out.chunk_index = 0;
    static const int one_chunk[] = { 1 };
    out.chunks = one_chunk;
    out.chunk_count = 1;
    out.total_frames = 1;
    memset(dst, 0xFF, sizeof(dst));
    CHECK(ring_pump_write(&pipe, &ring, 0, 5) == 1);
    CHECK(ring.fill == 0);
    CHECK(memcmp(dst, src, sizeof(src)) == 0);
}

int main(void) {
    test_negotiate_exact_order_preferred();
    test_negotiate_permutation();
    test_negotiate_output_priority();
    test_negotiate_roles_not_guessed();
    test_negotiate_skips_unsupported();

    test_pump_three_channel_permutation();
    test_pump_sample_widths();
    test_pump_six_channel_planar();
    test_pump_interleaved_with_padding();
    test_pump_read_hole_then_normal();
    test_pump_read_zero_chunk();
    test_pump_write_segmented_and_zero();
    test_pump_write_underrun_silence();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all channel_route checks passed\n");
    return 0;
}
