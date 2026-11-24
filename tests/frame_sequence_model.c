/*
 * frame_sequence_model.c - replayable structured frame-sequence regression.
 *
 * This test is intentionally self-contained and deterministic.  It builds a
 * small corpus of concatenated LZ4 frames (real LZ4F-compressed frames,
 * format-written skippable frames, and checksum-damaged frames), keeps an
 * independent byte/output model, and replays it through one short output
 * buffer with stableDst disabled.
 *
 * The focused integrity and recovery cases below pin the cross-call checksum
 * and reset behaviour, including the current compressed-block exception
 * documented in tests/frame_sequence_model.md.
 */

#include "task_support/check.h"

#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_SEED 1681U
#define GENERATED_SEQUENCE_COUNT 120
#define SHORT_OUT_CAP 40U
#define GUARD_FILL 0xA5U
#define GUARD_PRE 0x71U
#define GUARD_POST 0x83U

enum frame_kind {
    FRAME_NORMAL = 0,
    FRAME_SKIPPABLE = 1,
    FRAME_BAD_CONTENT = 2
};

enum chunk_mode {
    CHUNK_BYTE = 0,
    CHUNK_FIELD = 1,
    CHUNK_RANDOM = 2
};

enum skip_mode {
    SKIP_NONE = 0,
    SKIP_FIRST_ONLY = 1,
    SKIP_ALL = 2
};

typedef struct Buf {
    unsigned char *p;
    size_t len;
    size_t cap;
} Buf;

typedef struct FrameModel {
    Buf raw;
    Buf payload;
    int kind;
    int block_mode;
    int block_checksum;
    int content_checksum;
    int first_block_uncompressed;
    size_t header_size;
    size_t content_crc_offset;
    size_t first_block_header_offset;
    size_t first_block_data_offset;
    size_t first_block_size;
    size_t first_block_crc_offset;
    size_t stream_start;
    size_t *marks;
    size_t mark_count;
    size_t mark_cap;
} FrameModel;

typedef struct Sequence {
    FrameModel *frames;
    size_t frame_count;
    size_t frame_cap;
    Buf stream;
    Buf expected;
    size_t *marks;
    size_t mark_count;
    size_t mark_cap;
    size_t *random_ends;
    size_t random_count;
    size_t random_cap;
    int chunk_mode;
    int expected_bad_frame;
    int expected_error;
} Sequence;

typedef struct StepRecord {
    size_t step;
    size_t offer;
    size_t used;
    size_t produced;
    size_t cum_out;
    size_t cursor;
    size_t hint;
    int skip_option;
    int is_error;
    int error_code;
    int completed_after;
} StepRecord;

typedef struct RunResult {
    Buf out;
    StepRecord *steps;
    size_t step_count;
    size_t step_cap;
    size_t end_pos;
    size_t exhaust_hint;
    int completed_frames;
    int zero_returns;
    int got_error;
    int error_code;
    int incomplete;
} RunResult;

typedef struct DecodeOptions {
    int skip_mode;
} DecodeOptions;

typedef struct GuardBuffer {
    unsigned char pre[16];
    unsigned char out[SHORT_OUT_CAP];
    unsigned char post[32];
} GuardBuffer;

static uint64_t model_rng_state = MODEL_SEED;

static uint32_t
rng_next(void)
{
    uint64_t z = (model_rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return (uint32_t)(z ^ (z >> 31));
}

static unsigned
rng_below(unsigned n)
{
    return n ? (unsigned)(rng_next() % n) : 0U;
}

static int
buf_reserve(Buf *b, size_t needed)
{
    if (needed > b->cap) {
        size_t cap = b->cap ? b->cap : 64U;
        unsigned char *q;
        while (cap < needed) {
            if (cap > ((size_t)-1) / 2U) return 0;
            cap *= 2U;
        }
        q = (unsigned char *)realloc(b->p, cap);
        if (q == NULL) return 0;
        b->p = q;
        b->cap = cap;
    }
    return 1;
}

static int
buf_append(Buf *b, const void *p, size_t n)
{
    if (n == 0) return 1;
    if (!buf_reserve(b, b->len + n)) return 0;
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 1;
}

static void
buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void
put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t
get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int
size_list_add(size_t **list, size_t *count, size_t *cap, size_t value)
{
    size_t *next;
    if (*count == *cap) {
        size_t new_cap = *cap ? *cap * 2U : 16U;
        next = (size_t *)realloc(*list, new_cap * sizeof(*next));
        if (next == NULL) return 0;
        *list = next;
        *cap = new_cap;
    }
    (*list)[(*count)++] = value;
    return 1;
}

static int
frame_add_mark(FrameModel *f, size_t mark)
{
    if (f->mark_count && f->marks[f->mark_count - 1] == mark) return 1;
    return size_list_add(&f->marks, &f->mark_count, &f->mark_cap, mark);
}

static void
frame_init(FrameModel *f)
{
    memset(f, 0, sizeof(*f));
    f->first_block_uncompressed = -1;
}

static void
frame_free(FrameModel *f)
{
    if (f == NULL) return;
    buf_free(&f->raw);
    buf_free(&f->payload);
    free(f->marks);
    memset(f, 0, sizeof(*f));
}

static int
frame_parse_layout(FrameModel *f)
{
    const unsigned char *b = f->raw.p;
    size_t n = f->raw.len;
    size_t off;
    unsigned block_checksum;
    unsigned content_checksum;

    if (b == NULL || n < 8U) return 0;
    if ((get_le32(b) & 0xFFFFFFF0U) == LZ4F_MAGIC_SKIPPABLE_START) {
        if (n != (size_t)8U + get_le32(b + 4)) return 0;
        f->header_size = 8;
        if (!frame_add_mark(f, 0) || !frame_add_mark(f, 4) ||
            !frame_add_mark(f, 8) || !frame_add_mark(f, n))
            return 0;
        return 1;
    }

    if (get_le32(b) != LZ4F_MAGICNUMBER || n < 7U) return 0;
    f->header_size = LZ4F_headerSize(b, n);
    if (LZ4F_isError(f->header_size) || f->header_size < 7U ||
        f->header_size > n)
        return 0;

    block_checksum = (b[4] >> 4) & 1U;
    content_checksum = (b[4] >> 2) & 1U;
    f->content_checksum = (int)content_checksum;
    if (!frame_add_mark(f, 0) || !frame_add_mark(f, 4) ||
        !frame_add_mark(f, f->header_size))
        return 0;

    off = f->header_size;
    while (off + 4U <= n) {
        uint32_t header = get_le32(b + off);
        uint32_t block_size = header & 0x7FFFFFFFU;
        size_t header_offset = off;
        int first_block = (f->first_block_uncompressed < 0);

        if (!frame_add_mark(f, off) || !frame_add_mark(f, off + 4U))
            return 0;
        off += 4U;

        if (header == 0U) {
            if (!frame_add_mark(f, off)) return 0;
            if (content_checksum) {
                f->content_crc_offset = off;
                if (off + 4U > n) return 0;
                off += 4U;
                if (!frame_add_mark(f, off)) return 0;
            }
            return off == n;
        }

        if ((size_t)block_size > n - off) return 0;
        if (first_block) {
            f->first_block_header_offset = header_offset;
            f->first_block_data_offset = off;
            f->first_block_size = block_size;
            f->first_block_uncompressed =
                (header & 0x80000000U) ? 1 : 0;
        }
        off += block_size;
        if (!frame_add_mark(f, off)) return 0;

        if (block_checksum) {
            if (off + 4U > n) return 0;
            if (first_block) f->first_block_crc_offset = off;
            off += 4U;
            if (!frame_add_mark(f, off)) return 0;
        }
    }
    return 0;
}

static int
payload_make(Buf *out, size_t size, int mixed, unsigned salt)
{
    static const char pattern[] =
        "frame-sequence-model/0123456789/ABCDEFGHIJKLMNOPQRSTUVWXYZ/abcdefghijklmnopqrstuvwxyz";
    size_t plen = sizeof(pattern) - 1U;
    size_t i;

    if (!buf_reserve(out, size)) return 0;
    out->len = size;
    if (size == 0) return 1;

    for (i = 0; i < size; ++i) {
        if (!mixed) {
            unsigned char v = (unsigned char)pattern[(i + salt) % plen];
            if ((i % 997U) == 0U) v ^= (unsigned char)salt;
            out->p[i] = v;
        } else {
            size_t third = size / 3U;
            if (i < third) {
                out->p[i] = (unsigned char)pattern[(i * 3U + salt) % plen];
            } else if (i < 2U * third) {
                uint64_t z = (uint64_t)(i + salt + 1U) *
                             UINT64_C(0x9E3779B97F4A7C15);
                z ^= z >> 29;
                z *= UINT64_C(0xBF58476D1CE4E5B9);
                out->p[i] = (unsigned char)(z >> 17);
            } else {
                out->p[i] = (unsigned char)((i / 11U) + salt * 17U);
            }
        }
    }
    return 1;
}

static int
frame_make_normal(FrameModel *f, const Buf *payload,
                  LZ4F_blockMode_t block_mode,
                  LZ4F_blockChecksum_t block_checksum,
                  LZ4F_contentChecksum_t content_checksum)
{
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t bound;
    size_t compressed_size;

    frame_init(f);
    f->kind = FRAME_NORMAL;
    f->block_mode = (int)block_mode;
    f->block_checksum = (int)block_checksum;
    f->content_checksum = (int)content_checksum;

    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockMode = block_mode;
    prefs.frameInfo.blockChecksumFlag = block_checksum;
    prefs.frameInfo.contentChecksumFlag = content_checksum;
    prefs.compressionLevel = 0;

    bound = LZ4F_compressFrameBound(payload->len, &prefs) + 64U;
    if (!buf_reserve(&f->raw, bound)) return 0;
    compressed_size = LZ4F_compressFrame(
        f->raw.p, f->raw.cap,
        payload->p, payload->len, &prefs);
    if (LZ4F_isError(compressed_size)) return 0;
    f->raw.len = compressed_size;

    if (!buf_append(&f->payload, payload->p, payload->len)) return 0;
    return frame_parse_layout(f);
}

static int
frame_make_skippable(FrameModel *f, const Buf *payload, unsigned variant)
{
    frame_init(f);
    f->kind = FRAME_SKIPPABLE;
    if (!buf_reserve(&f->raw, (size_t)8U + payload->len)) return 0;
    f->raw.len = (size_t)8U + payload->len;
    put_le32(f->raw.p, LZ4F_MAGIC_SKIPPABLE_START + (variant & 15U));
    put_le32(f->raw.p + 4U, (uint32_t)payload->len);
    if (payload->len) memcpy(f->raw.p + 8U, payload->p, payload->len);
    return frame_parse_layout(f);
}

static int
frame_flip_content_crc(FrameModel *f)
{
    if (!f->content_checksum || f->content_crc_offset + 4U > f->raw.len)
        return 0;
    f->kind = FRAME_BAD_CONTENT;
    f->raw.p[f->content_crc_offset] ^= 0x01U;
    return 1;
}

static int
frame_flip_first_block_crc(FrameModel *f)
{
    if (!f->block_checksum || f->first_block_crc_offset + 4U > f->raw.len)
        return 0;
    f->raw.p[f->first_block_crc_offset] ^= 0x01U;
    return 1;
}

static void
sequence_init(Sequence *s)
{
    memset(s, 0, sizeof(*s));
    s->chunk_mode = CHUNK_RANDOM;
    s->expected_bad_frame = -1;
}

static void
sequence_free(Sequence *s)
{
    size_t i;
    for (i = 0; i < s->frame_count; ++i) frame_free(&s->frames[i]);
    free(s->frames);
    buf_free(&s->stream);
    buf_free(&s->expected);
    free(s->marks);
    free(s->random_ends);
    memset(s, 0, sizeof(*s));
}

static int
sequence_add_mark(Sequence *s, size_t mark)
{
    return size_list_add(&s->marks, &s->mark_count, &s->mark_cap, mark);
}

static int
sequence_add_frame(Sequence *s, FrameModel *f)
{
    FrameModel *stored;
    size_t i;

    if (s->frame_count == s->frame_cap) {
        size_t new_cap = s->frame_cap ? s->frame_cap * 2U : 8U;
        FrameModel *next = (FrameModel *)realloc(
            s->frames, new_cap * sizeof(*next));
        if (next == NULL) return 0;
        s->frames = next;
        s->frame_cap = new_cap;
    }

    f->stream_start = s->stream.len;
    stored = &s->frames[s->frame_count++];
    *stored = *f;
    memset(f, 0, sizeof(*f));

    if (!buf_append(&s->stream, stored->raw.p, stored->raw.len)) return 0;
    if (stored->kind != FRAME_SKIPPABLE) {
        if (!buf_append(&s->expected, stored->payload.p,
                        stored->payload.len))
            return 0;
    }
    for (i = 0; i < stored->mark_count; ++i) {
        if (!sequence_add_mark(s, stored->stream_start + stored->marks[i]))
            return 0;
    }
    return sequence_add_mark(s, s->stream.len);
}

static int
sequence_make_random_plan(Sequence *s, uint32_t seed)
{
    uint32_t state = seed ? seed : 1U;
    size_t pos = 0;
    s->random_count = 0;

    while (pos < s->stream.len) {
        uint32_t word;
        unsigned span;
        size_t end;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        word = state;
        span = 1U + (unsigned)(word % 96U);
        end = pos + span;
        if (end > s->stream.len || end < pos) end = s->stream.len;
        if (!size_list_add(&s->random_ends, &s->random_count,
                           &s->random_cap, end))
            return 0;
        pos = end;
    }
    return 1;
}

static size_t
plan_boundary(const Sequence *s, size_t *plan_index, size_t pos)
{
    const size_t *ends = NULL;
    size_t count = 0;

    if (s->chunk_mode == CHUNK_BYTE) return pos + 1U;
    if (s->chunk_mode == CHUNK_FIELD) {
        ends = s->marks;
        count = s->mark_count;
    } else {
        ends = s->random_ends;
        count = s->random_count;
    }

    while (*plan_index < count && ends[*plan_index] <= pos)
        ++*plan_index;
    if (*plan_index < count) return ends[*plan_index];
    return s->stream.len;
}

static void
run_result_init(RunResult *r)
{
    memset(r, 0, sizeof(*r));
}

static void
run_result_free(RunResult *r)
{
    buf_free(&r->out);
    free(r->steps);
    memset(r, 0, sizeof(*r));
}

static int
run_result_add_step(RunResult *r, const StepRecord *step)
{
    if (r->step_count == r->step_cap) {
        size_t new_cap = r->step_cap ? r->step_cap * 2U : 32U;
        StepRecord *next = (StepRecord *)realloc(
            r->steps, new_cap * sizeof(*next));
        if (next == NULL) return 0;
        r->steps = next;
        r->step_cap = new_cap;
    }
    r->steps[r->step_count++] = *step;
    return 1;
}

static struct diag_context {
    unsigned seed;
    int sequence_index;
    int frame_index;
    const Sequence *sequence;
    const Buf *hex;
    const char *hex_label;
    const RunResult *run;
    char first_diff[512];
    char switches[4096];
    int last_switch;
} diag;

static void
diag_reset(void)
{
    memset(&diag, 0, sizeof(diag));
    diag.seed = MODEL_SEED;
    diag.sequence_index = -1;
    diag.frame_index = -1;
    diag.last_switch = -1;
}

static void
diag_switch(size_t step, int skip)
{
    char item[96];
    int n;
    size_t used;
    if (diag.last_switch == skip) return;
    used = strlen(diag.switches);
    n = snprintf(item, sizeof(item), "%sstep=%zu:skipChecksums=%d",
                 used ? "; " : "", step, skip);
    if (n < 0) return;
    if (used + (size_t)n + 1U >= sizeof(diag.switches)) return;
    memcpy(diag.switches + used, item, (size_t)n + 1U);
    diag.last_switch = skip;
}

static void
dump_hex(const char *label, const unsigned char *p, size_t n)
{
    size_t i;
    fprintf(stderr, "%s len=%zu hex=", label, n);
    for (i = 0; i < n; ++i) fprintf(stderr, "%02x", p[i]);
    fputc('\n', stderr);
}

static const char *
chunk_name(int mode)
{
    if (mode == CHUNK_BYTE) return "single-byte";
    if (mode == CHUNK_FIELD) return "field-boundary";
    return "fixed-random";
}

static void
dump_model_diag(void)
{
    fprintf(stderr,
            "MODEL_FAILURE seed=%u sequence=%d frame=%d first_difference=%s\n",
            diag.seed, diag.sequence_index, diag.frame_index,
            diag.first_diff[0] ? diag.first_diff : "(assertion)");
    fprintf(stderr, "switch_operations: %s\n",
            diag.switches[0] ? diag.switches :
            "all calls skipChecksums=0, stableDst=0");

    if (diag.run != NULL) {
        const RunResult *r = diag.run;
        size_t first = r->step_count > 12U ? r->step_count - 12U : 0U;
        size_t i;
        fprintf(stderr,
                "run_state steps=%zu end_pos=%zu output=%zu completed=%d "
                "zero_returns=%d error=%d/%d incomplete=%d hint=%zu\n",
                r->step_count, r->end_pos, r->out.len,
                r->completed_frames, r->zero_returns, r->got_error,
                r->error_code, r->incomplete, r->exhaust_hint);
        for (i = first; i < r->step_count; ++i) {
            const StepRecord *s = &r->steps[i];
            fprintf(stderr,
                    "  step=%zu offer=%zu used=%zu produced=%zu cum_out=%zu "
                    "cursor=%zu hint=%zu skip=%d err=%d/%d completed=%d\n",
                    s->step, s->offer, s->used, s->produced, s->cum_out,
                    s->cursor, s->hint, s->skip_option, s->is_error,
                    s->error_code, s->completed_after);
        }
    }

    if (diag.sequence != NULL) {
        const Sequence *s = diag.sequence;
        size_t i;
        fprintf(stderr, "chunk_arrangement mode=%s plan_entries=%zu\n",
                chunk_name(s->chunk_mode),
                s->chunk_mode == CHUNK_RANDOM ? s->random_count :
                s->mark_count);
        if (s->chunk_mode == CHUNK_FIELD) {
            fprintf(stderr, "field_boundaries=");
            for (i = 0; i < s->mark_count; ++i)
                fprintf(stderr, "%s%zu", i ? "," : "[", s->marks[i]);
            fputs("]\n", stderr);
        } else if (s->chunk_mode == CHUNK_RANDOM) {
            size_t prev = 0;
            fprintf(stderr, "random_chunk_sizes=");
            for (i = 0; i < s->random_count; ++i) {
                fprintf(stderr, "%s%zu", i ? "," : "[",
                        s->random_ends[i] - prev);
                prev = s->random_ends[i];
            }
            fputs("]\n", stderr);
        } else {
            fputs("single_byte_chunks=all offers are one byte\n", stderr);
        }
        dump_hex("REPLAY_SEQUENCE", s->stream.p, s->stream.len);
        for (i = 0; i < s->frame_count; ++i) {
            const FrameModel *f = &s->frames[i];
            fprintf(stderr, "FRAME model_number=%zu stream_start=%zu kind=%d "
                    "block_mode=%d block_checksum=%d content_checksum=%d "
                    "first_block_uncompressed=%d\n",
                    i, f->stream_start, f->kind, f->block_mode,
                    f->block_checksum, f->content_checksum,
                    f->first_block_uncompressed);
            dump_hex("FRAME_HEX", f->raw.p, f->raw.len);
        }
    }
    if (diag.hex != NULL)
        dump_hex(diag.hex_label ? diag.hex_label : "REPLAY_HEX",
                 diag.hex->p, diag.hex->len);
}

#define CHECK(cond) do { \
    int model_check_ok_ = !!(cond); \
    if (!model_check_ok_) dump_model_diag(); \
    TASK_CHECK(model_check_ok_); \
} while (0)

#define CHECK_INT_EQ(actual, expected) do { \
    long long model_actual_ = (long long)(actual); \
    long long model_expected_ = (long long)(expected); \
    if (model_actual_ != model_expected_) { \
        snprintf(diag.first_diff, sizeof(diag.first_diff), \
                 "%s vs %s: actual=%lld expected=%lld", \
                 #actual, #expected, model_actual_, model_expected_); \
    } \
    CHECK(model_actual_ == model_expected_); \
} while (0)

static void
guard_init(GuardBuffer *g)
{
    memset(g->pre, GUARD_PRE, sizeof(g->pre));
    memset(g->post, GUARD_POST, sizeof(g->post));
    memset(g->out, GUARD_FILL, sizeof(g->out));
}

static int
guard_ok(const GuardBuffer *g)
{
    size_t i;
    for (i = 0; i < sizeof(g->pre); ++i)
        if (g->pre[i] != GUARD_PRE) return 0;
    for (i = 0; i < sizeof(g->post); ++i)
        if (g->post[i] != GUARD_POST) return 0;
    return 1;
}

/*
 * The step cap is deliberately linear: every continuing successful call must
 * consume at least one input byte or report at least one output byte.  Add
 * 256 calls of state-machine slack.  A no-progress successful call is handled
 * as input exhaustion instead of spinning.
 */
static int
decode_with_dctx(LZ4F_dctx *dctx, const Sequence *seq,
                 const DecodeOptions *options, RunResult *result)
{
    size_t const step_cap = seq->stream.len + seq->expected.len + 256U;
    size_t pos = 0;
    size_t plan_index = 0;
    size_t step = 0;
    GuardBuffer guard;

    guard_init(&guard);
    diag.run = result;

    for (;;) {
        size_t offer = 0;
        size_t boundary = 0;
        size_t in_size;
        size_t out_size = SHORT_OUT_CAP;
        size_t used, produced;
        size_t ret;
        int is_error, error_code = 0;
        int skip = 0;
        size_t output_before;
        size_t i;
        StepRecord record;

        diag.frame_index = result->completed_frames;
        if (step >= step_cap) {
            snprintf(diag.first_diff, sizeof(diag.first_diff),
                     "step cap %zu exceeded", step_cap);
            CHECK(0);
        }

        if (pos < seq->stream.len) {
            boundary = plan_boundary(seq, &plan_index, pos);
            if (boundary <= pos || boundary > seq->stream.len) {
                snprintf(diag.first_diff, sizeof(diag.first_diff),
                         "bad chunk boundary %zu at pos %zu", boundary, pos);
                CHECK(0);
            }
            offer = boundary - pos;
        }

        if (!guard_ok(&guard)) {
            snprintf(diag.first_diff, sizeof(diag.first_diff),
                     "output sentinel corrupted before call %zu", step);
            CHECK(0);
        }
        memset(guard.out, GUARD_FILL, sizeof(guard.out));

        if (options != NULL) {
            if (options->skip_mode == SKIP_ALL) skip = 1;
            if (options->skip_mode == SKIP_FIRST_ONLY && step == 0) skip = 1;
        }
        diag_switch(step, skip);

        in_size = offer;
        {
            LZ4F_decompressOptions_t decompress_options;
            memset(&decompress_options, 0, sizeof(decompress_options));
            decompress_options.stableDst = 0;
            decompress_options.skipChecksums = (unsigned)skip;
            ret = LZ4F_decompress(
                dctx, guard.out, &out_size,
                seq->stream.p + pos, &in_size,
                &decompress_options);
        }
        used = in_size;
        produced = out_size;
        is_error = LZ4F_isError(ret);
        if (is_error) error_code = (int)LZ4F_getErrorCode(ret);

        if (used > offer) {
            snprintf(diag.first_diff, sizeof(diag.first_diff),
                     "consumed %zu from offer %zu", used, offer);
            CHECK(0);
        }
        if (produced > SHORT_OUT_CAP) {
            snprintf(diag.first_diff, sizeof(diag.first_diff),
                     "produced %zu > capacity %u", produced, SHORT_OUT_CAP);
            CHECK(0);
        }
        if (!guard_ok(&guard)) {
            snprintf(diag.first_diff, sizeof(diag.first_diff),
                     "output sentinel corrupted after call %zu", step);
            CHECK(0);
        }
        if (!is_error) {
            /*
             * A successful call reports the exact written prefix.  An error
             * return can leave *dstSizePtr at its entry value even though the
             * implementation touched the destination, so only check the fill
             * tail on successful calls; canaries are always checked.
             */
            for (i = produced; i < SHORT_OUT_CAP; ++i) {
                if (guard.out[i] != GUARD_FILL) {
                    snprintf(diag.first_diff, sizeof(diag.first_diff),
                             "output tail byte %zu overwritten", i);
                    CHECK(0);
                }
            }
        }

        output_before = result->out.len;
        if (produced != 0) {
            if (output_before + produced > seq->expected.len) {
                snprintf(diag.first_diff, sizeof(diag.first_diff),
                         "output overflow at cumulative %zu+%zu, model=%zu",
                         output_before, produced, seq->expected.len);
                CHECK(0);
            }
            if (!buf_reserve(&result->out, output_before + produced)) CHECK(0);
            memcpy(result->out.p + output_before, guard.out, produced);
            result->out.len = output_before + produced;
            for (i = 0; i < produced; ++i) {
                size_t at = output_before + i;
                if (result->out.p[at] != seq->expected.p[at]) {
                    snprintf(diag.first_diff, sizeof(diag.first_diff),
                             "output byte %zu: expected 0x%02x got 0x%02x",
                             at, seq->expected.p[at], result->out.p[at]);
                    CHECK(0);
                }
            }
        }

        pos += used;
        result->end_pos = pos;

        memset(&record, 0, sizeof(record));
        record.step = step;
        record.offer = offer;
        record.used = used;
        record.produced = produced;
        record.cum_out = result->out.len;
        record.cursor = pos;
        record.hint = is_error ? 0 : ret;
        record.skip_option = skip;
        record.is_error = is_error;
        record.error_code = error_code;

        if (is_error) {
            result->got_error = 1;
            result->error_code = error_code;
            record.completed_after = result->completed_frames;
            if (!run_result_add_step(result, &record)) CHECK(0);
            return 0;
        }

        if (ret == 0) {
            size_t expected_end;
            if (result->completed_frames >= (int)seq->frame_count) {
                snprintf(diag.first_diff, sizeof(diag.first_diff),
                         "zero return after all %zu frames", seq->frame_count);
                CHECK(0);
            }
            expected_end = seq->frames[result->completed_frames].stream_start +
                           seq->frames[result->completed_frames].raw.len;
            if (pos != expected_end) {
                snprintf(diag.first_diff, sizeof(diag.first_diff),
                         "zero return at pos %zu, frame %d ends %zu",
                         pos, result->completed_frames, expected_end);
                CHECK(0);
            }
            ++result->completed_frames;
            ++result->zero_returns;
        }
        record.completed_after = result->completed_frames;
        if (!run_result_add_step(result, &record)) CHECK(0);

        if (ret == 0 && pos == seq->stream.len) break;

        if (used == 0 && produced == 0) {
            result->incomplete = 1;
            result->exhaust_hint = ret;
            break;
        }
        ++step;
    }

    CHECK(!result->incomplete);
    CHECK(pos == seq->stream.len);
    CHECK(result->end_pos == seq->stream.len);
    CHECK(result->completed_frames == (int)seq->frame_count);
    CHECK(result->zero_returns == (int)seq->frame_count);
    return 0;
}

static int
decode_fresh(const Sequence *seq, const DecodeOptions *options,
             RunResult *result)
{
    LZ4F_dctx *dctx = NULL;
    size_t created = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    int rc;
    if (LZ4F_isError(created) || dctx == NULL) {
        fputs("MODEL_FAILURE could not create LZ4F_dctx\n", stderr);
        TASK_CHECK(0);
    }
    rc = decode_with_dctx(dctx, seq, options, result);
    LZ4F_freeDecompressionContext(dctx);
    return rc;
}

static int
verify_full_sequence(const Sequence *seq)
{
    RunResult result;
    int rc;
    run_result_init(&result);
    diag.sequence = seq;
    diag.run = &result;
    rc = decode_fresh(seq, NULL, &result);
    if (rc != 0) {
        run_result_free(&result);
        return 1;
    }
    CHECK(!result.got_error);
    CHECK(result.error_code == 0);
    CHECK(result.end_pos == seq->stream.len);
    CHECK(result.completed_frames == (int)seq->frame_count);
    CHECK(result.zero_returns == (int)seq->frame_count);
    CHECK(result.out.len == seq->expected.len);
    if (seq->expected.len)
        CHECK(memcmp(result.out.p, seq->expected.p, seq->expected.len) == 0);
    run_result_free(&result);
    diag.run = NULL;
    return 0;
}

static int
build_fixed_sequence(Sequence *seq, int chunk_mode)
{
    Buf empty_payload, short_payload, skip_payload;
    FrameModel empty_frame, short_frame, skip_frame;

    sequence_init(seq);
    seq->chunk_mode = chunk_mode;
    memset(&empty_payload, 0, sizeof(empty_payload));
    memset(&short_payload, 0, sizeof(short_payload));
    memset(&skip_payload, 0, sizeof(skip_payload));

    CHECK(payload_make(&short_payload, 333, 0, 7));
    CHECK(payload_make(&skip_payload, 29, 1, 9));
    CHECK(frame_make_normal(&empty_frame, &empty_payload,
                            LZ4F_blockLinked,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_contentChecksumEnabled));
    CHECK(frame_make_skippable(&skip_frame, &skip_payload, 3));
    CHECK(frame_make_normal(&short_frame, &short_payload,
                            LZ4F_blockIndependent,
                            LZ4F_noBlockChecksum,
                            LZ4F_contentChecksumEnabled));
    CHECK(sequence_add_frame(seq, &empty_frame));
    CHECK(sequence_add_frame(seq, &skip_frame));
    CHECK(sequence_add_frame(seq, &short_frame));
    if (chunk_mode == CHUNK_RANDOM)
        CHECK(sequence_make_random_plan(seq, 0x51A7U));

    buf_free(&short_payload);
    buf_free(&skip_payload);
    return 0;
}

static int
build_generated_sequence(Sequence *seq, int index)
{
    static const size_t small_sizes[] = {
        0, 1, 2, 7, 19, 64, 65, 200, 900, 4097
    };
    unsigned normal_count = 1U + rng_below(3U);
    unsigned j;
    int add_skippable = (index % 5) == 0;
    int add_bad_content = (index % 7) == 6;

    sequence_init(seq);
    seq->chunk_mode = index % 3;

    for (j = 0; j < normal_count; ++j) {
        Buf payload;
        FrameModel frame;
        size_t size;
        int mixed = 0;
        LZ4F_blockMode_t mode = ((index + (int)j) & 1) ?
            LZ4F_blockIndependent : LZ4F_blockLinked;
        LZ4F_blockChecksum_t block_crc = (rng_next() & 1U) ?
            LZ4F_blockChecksumEnabled : LZ4F_noBlockChecksum;

        memset(&payload, 0, sizeof(payload));
        if (seq->chunk_mode == CHUNK_BYTE) {
            size = small_sizes[rng_below(sizeof(small_sizes) /
                                         sizeof(small_sizes[0]))];
        } else {
            unsigned kind = rng_below(10U);
            if (kind < 2U) {
                size = 0;
            } else if (kind < 6U) {
                size = small_sizes[rng_below(
                    sizeof(small_sizes) / sizeof(small_sizes[0]))];
            } else {
                size = 66000U + rng_below(6000U);
                mixed = kind >= 8U;
            }
        }
        CHECK(payload_make(&payload, size, mixed,
                           (unsigned)(index * 131 + (int)j * 17)));
        CHECK(frame_make_normal(&frame, &payload, mode, block_crc,
                                LZ4F_contentChecksumEnabled));
        CHECK(sequence_add_frame(seq, &frame));
        buf_free(&payload);
    }

    if (add_skippable) {
        Buf skip_payload;
        FrameModel skip_frame;
        size_t skip_size = rng_below(97U);
        memset(&skip_payload, 0, sizeof(skip_payload));
        CHECK(payload_make(&skip_payload, skip_size, 1,
                           (unsigned)index + 101U));
        CHECK(frame_make_skippable(&skip_frame, &skip_payload,
                                   (unsigned)index));
        CHECK(sequence_add_frame(seq, &skip_frame));
        buf_free(&skip_payload);
    }

    if (add_bad_content) {
        Buf payload;
        FrameModel bad_frame;
        size_t size = 300U + rng_below(5000U);
        memset(&payload, 0, sizeof(payload));
        CHECK(payload_make(&payload, size, rng_next() & 1,
                           (unsigned)index + 211U));
        CHECK(frame_make_normal(&bad_frame, &payload,
                                (index & 1) ? LZ4F_blockIndependent :
                                              LZ4F_blockLinked,
                                (index & 2) ?
                                    LZ4F_blockChecksumEnabled :
                                    LZ4F_noBlockChecksum,
                                LZ4F_contentChecksumEnabled));
        CHECK(frame_flip_content_crc(&bad_frame));
        CHECK(sequence_add_frame(seq, &bad_frame));
        seq->expected_bad_frame = (int)seq->frame_count - 1;
        seq->expected_error = (int)LZ4F_ERROR_contentChecksum_invalid;
        buf_free(&payload);
    }

    if (seq->chunk_mode == CHUNK_RANDOM)
        CHECK(sequence_make_random_plan(seq, (uint32_t)(MODEL_SEED + index)));
    return 0;
}

static int
run_sequence_group(void)
{
    int i;
    int chunk;

    diag_reset();

    for (chunk = 0; chunk < 3; ++chunk) {
        Sequence fixed;
        diag.sequence_index = -1 - chunk;
        CHECK(build_fixed_sequence(&fixed, chunk) == 0);
        diag.sequence = &fixed;
        if (verify_full_sequence(&fixed) != 0) {
            sequence_free(&fixed);
            return 1;
        }
        sequence_free(&fixed);
        diag.sequence = NULL;
    }

    model_rng_state = MODEL_SEED;
    for (i = 0; i < GENERATED_SEQUENCE_COUNT; ++i) {
        Sequence seq;
        diag_reset();
        diag.sequence_index = i;
        CHECK(build_generated_sequence(&seq, i) == 0);
        diag.sequence = &seq;

        if (seq.expected_bad_frame >= 0) {
            RunResult result;
            int rc;
            run_result_init(&result);
            rc = decode_fresh(&seq, NULL, &result);
            if (rc != 0) {
                run_result_free(&result);
                sequence_free(&seq);
                return 1;
            }
            CHECK_INT_EQ(result.error_code, seq.expected_error);
            CHECK_INT_EQ(result.got_error, 1);
            CHECK_INT_EQ(result.completed_frames, seq.expected_bad_frame);
            CHECK_INT_EQ(result.zero_returns, seq.expected_bad_frame);
            CHECK(result.out.len <= seq.expected.len);
            run_result_free(&result);
        } else {
            if (verify_full_sequence(&seq) != 0) {
                sequence_free(&seq);
                return 1;
            }
        }

        sequence_free(&seq);
        diag.sequence = NULL;
    }
    return 0;
}

/* ---- integrity-focused fixed cases ------------------------------------- */

static int
integrity_content_skip_then_next_frame(void)
{
    Buf payload;
    FrameModel first, second;
    Sequence seq;
    DecodeOptions options;
    RunResult result;
    int rc;
    size_t first_zero_step = (size_t)-1;
    size_t i;

    diag_reset();
    diag.sequence_index = 1000;
    memset(&payload, 0, sizeof(payload));
    CHECK(payload_make(&payload, 20000U, 0, 31U));
    CHECK(frame_make_normal(&first, &payload, LZ4F_blockLinked,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_contentChecksumEnabled));
    CHECK(first.first_block_uncompressed == 0);
    CHECK(frame_flip_content_crc(&first));

    CHECK(frame_make_normal(&second, &payload, LZ4F_blockLinked,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_contentChecksumEnabled));
    CHECK(frame_flip_content_crc(&second));

    sequence_init(&seq);
    seq.chunk_mode = CHUNK_BYTE;
    CHECK(sequence_add_frame(&seq, &first));
    CHECK(sequence_add_frame(&seq, &second));
    diag_reset();
    diag.sequence_index = 1000;
    diag.sequence = &seq;

    options.skip_mode = SKIP_FIRST_ONLY;
    run_result_init(&result);
    rc = decode_fresh(&seq, &options, &result);
    if (rc != 0) {
        run_result_free(&result);
        sequence_free(&seq);
        buf_free(&payload);
        return 1;
    }

    CHECK_INT_EQ(result.error_code,
                 (int)LZ4F_ERROR_contentChecksum_invalid);
    CHECK_INT_EQ(result.got_error, 1);
    CHECK_INT_EQ(result.completed_frames, 1);
    CHECK_INT_EQ(result.zero_returns, 1);
    CHECK(result.end_pos >=
          seq.frames[0].stream_start + seq.frames[0].raw.len);
    CHECK(result.out.len >= payload.len);
    CHECK(memcmp(result.out.p, payload.p, payload.len) == 0);

    for (i = 0; i < result.step_count; ++i) {
        if (!result.steps[i].is_error &&
            result.steps[i].hint == 0 &&
            result.steps[i].completed_after == 1) {
            size_t first_end = seq.frames[0].stream_start +
                               seq.frames[0].raw.len;
            first_zero_step = i;
            CHECK(result.steps[i].cursor == first_end);
            CHECK(result.steps[i].cum_out == payload.len);
        }
    }
    CHECK(first_zero_step != (size_t)-1);

    run_result_free(&result);
    sequence_free(&seq);
    buf_free(&payload);
    return 0;
}

static int
integrity_one_block_path(int compressed_path, int skip)
{
    Buf payload;
    FrameModel frame;
    Sequence seq;
    DecodeOptions options;
    RunResult result;
    int rc;
    unsigned random_state = 0x1234ABCDU;
    size_t pi;

    diag_reset();
    diag.sequence_index = compressed_path ? 1101 : 1102;
    memset(&payload, 0, sizeof(payload));
    if (compressed_path) {
        CHECK(payload_make(&payload, 8192U, 0, 41U));
    } else {
        CHECK(payload_make(&payload, 8192U, 1, 43U));
        /* The raw-block regression needs a genuinely incompressible block. */
        for (pi = 0; pi < payload.len; ++pi) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            payload.p[pi] = (unsigned char)(random_state >> 11);
        }
    }
    CHECK(frame_make_normal(&frame, &payload, LZ4F_blockIndependent,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_noContentChecksum));
    diag.hex = &frame.raw;
    diag.hex_label = compressed_path ? "COMPRESSED_BLOCK_FRAME" :
                                       "RAW_BLOCK_FRAME";
    if (compressed_path) CHECK(frame.first_block_uncompressed == 0);
    else CHECK(frame.first_block_uncompressed == 1);
    CHECK(frame_flip_first_block_crc(&frame));

    sequence_init(&seq);
    seq.chunk_mode = CHUNK_FIELD;
    CHECK(sequence_add_frame(&seq, &frame));
    diag.hex = NULL;
    diag.sequence = &seq;

    options.skip_mode = skip ? SKIP_ALL : SKIP_NONE;
    run_result_init(&result);
    rc = decode_fresh(&seq, &options, &result);
    if (rc != 0) {
        run_result_free(&result);
        sequence_free(&seq);
        buf_free(&payload);
        return 1;
    }

    if (compressed_path || !skip) {
        CHECK_INT_EQ(result.error_code,
                     (int)LZ4F_ERROR_blockChecksum_invalid);
        CHECK_INT_EQ(result.got_error, 1);
    } else {
        CHECK_INT_EQ(result.got_error, 0);
        CHECK(result.end_pos == seq.stream.len);
        CHECK(result.completed_frames == 1);
        CHECK(result.out.len == payload.len);
        CHECK(memcmp(result.out.p, payload.p, payload.len) == 0);
    }

    run_result_free(&result);
    sequence_free(&seq);
    buf_free(&payload);
    return 0;
}

static int
integrity_default_content_error(void)
{
    Buf payload;
    FrameModel frame;
    Sequence seq;
    RunResult result;
    int rc;

    diag_reset();
    diag.sequence_index = 1103;
    memset(&payload, 0, sizeof(payload));
    CHECK(payload_make(&payload, 6000U, 0, 51U));
    CHECK(frame_make_normal(&frame, &payload, LZ4F_blockIndependent,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_contentChecksumEnabled));
    CHECK(frame.first_block_uncompressed == 0);
    CHECK(frame_flip_content_crc(&frame));

    sequence_init(&seq);
    seq.chunk_mode = CHUNK_RANDOM;
    CHECK(sequence_add_frame(&seq, &frame));
    CHECK(sequence_make_random_plan(&seq, 771U));
    diag_reset();
    diag.sequence_index = 1103;
    diag.sequence = &seq;

    run_result_init(&result);
    rc = decode_fresh(&seq, NULL, &result);
    if (rc != 0) {
        run_result_free(&result);
        sequence_free(&seq);
        buf_free(&payload);
        return 1;
    }
    CHECK_INT_EQ(result.error_code,
                 (int)LZ4F_ERROR_contentChecksum_invalid);
    CHECK_INT_EQ(result.got_error, 1);
    run_result_free(&result);
    sequence_free(&seq);
    buf_free(&payload);
    return 0;
}

static int
integrity_truncation_hint_is_not_detection(void)
{
    Buf payload;
    FrameModel frame;
    LZ4F_dctx *dctx = NULL;
    GuardBuffer guard;
    size_t cut;
    size_t in_size;
    size_t out_size;
    size_t ret;
    size_t created;

    diag_reset();
    diag.sequence_index = 1104;
    memset(&payload, 0, sizeof(payload));
    CHECK(payload_make(&payload, 70000U, 1, 61U));
    CHECK(frame_make_normal(&frame, &payload, LZ4F_blockLinked,
                            LZ4F_noBlockChecksum,
                            LZ4F_contentChecksumEnabled));
    diag_reset();
    diag.sequence_index = 1104;
    diag.hex = &frame.raw;
    diag.hex_label = "TRUNCATED_FRAME";

    created = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    CHECK(!LZ4F_isError(created) && dctx != NULL);
    cut = frame.header_size + 2U;
    CHECK(cut < frame.raw.len);
    guard_init(&guard);

    in_size = cut;
    out_size = SHORT_OUT_CAP;
    ret = LZ4F_decompress(dctx, guard.out, &out_size,
                          frame.raw.p, &in_size, NULL);
    CHECK(!LZ4F_isError(ret));
    CHECK(ret > 0);
    CHECK(in_size <= cut);
    CHECK(guard_ok(&guard));

    in_size = 0;
    out_size = SHORT_OUT_CAP;
    ret = LZ4F_decompress(dctx, guard.out, &out_size,
                          frame.raw.p + in_size, &in_size, NULL);
    CHECK(!LZ4F_isError(ret));
    CHECK(ret > 0);
    CHECK(guard_ok(&guard));

    LZ4F_freeDecompressionContext(dctx);
    frame_free(&frame);
    buf_free(&payload);
    diag.hex = NULL;
    return 0;
}

static int
run_integrity_group(void)
{
    if (integrity_content_skip_then_next_frame() != 0) return 1;
    if (integrity_one_block_path(1, 0) != 0) return 1;
    if (integrity_one_block_path(0, 0) != 0) return 1;
    if (integrity_one_block_path(1, 1) != 0) return 1;
    if (integrity_one_block_path(0, 1) != 0) return 1;
    if (integrity_default_content_error() != 0) return 1;
    if (integrity_truncation_hint_is_not_detection() != 0) return 1;
    return 0;
}

/* ---- recovery and context equivalence ---------------------------------- */

static int
build_recovery_sequence(Sequence *seq)
{
    Buf a, b, skip_payload;
    FrameModel fa, fb, skip;

    sequence_init(seq);
    seq->chunk_mode = CHUNK_RANDOM;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&skip_payload, 0, sizeof(skip_payload));
    CHECK(payload_make(&a, 5000U, 0, 71U));
    CHECK(payload_make(&skip_payload, 37U, 1, 73U));
    CHECK(payload_make(&b, 9000U, 1, 79U));

    CHECK(frame_make_normal(&fa, &a, LZ4F_blockLinked,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_contentChecksumEnabled));
    CHECK(frame_make_skippable(&skip, &skip_payload, 8));
    CHECK(frame_make_normal(&fb, &b, LZ4F_blockIndependent,
                            LZ4F_noBlockChecksum,
                            LZ4F_contentChecksumEnabled));
    CHECK(sequence_add_frame(seq, &fa));
    CHECK(sequence_add_frame(seq, &skip));
    CHECK(sequence_add_frame(seq, &fb));
    CHECK(sequence_make_random_plan(seq, 881U));

    buf_free(&a);
    buf_free(&b);
    buf_free(&skip_payload);
    return 0;
}

static int
make_bad_content_sequence(Sequence *seq, uint32_t payload_seed)
{
    Buf payload;
    FrameModel frame;

    memset(&payload, 0, sizeof(payload));
    CHECK(payload_make(&payload, 7000U, 0, payload_seed));
    CHECK(frame_make_normal(&frame, &payload, LZ4F_blockLinked,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_contentChecksumEnabled));
    CHECK(frame.first_block_uncompressed == 0);
    CHECK(frame_flip_content_crc(&frame));
    sequence_init(seq);
    seq->chunk_mode = CHUNK_RANDOM;
    CHECK(sequence_add_frame(seq, &frame));
    CHECK(sequence_make_random_plan(seq, payload_seed + 900U));
    buf_free(&payload);
    return 0;
}

static int
assert_same_run(const RunResult *reused, const RunResult *fresh,
                const Sequence *seq)
{
    size_t i;
    size_t compare_len;
    CHECK_INT_EQ(reused->step_count, fresh->step_count);
    for (i = 0; i < reused->step_count; ++i) {
        const StepRecord *a = &reused->steps[i];
        const StepRecord *b = &fresh->steps[i];
        CHECK_INT_EQ(a->offer, b->offer);
        CHECK_INT_EQ(a->used, b->used);
        CHECK_INT_EQ(a->produced, b->produced);
        CHECK_INT_EQ(a->cum_out, b->cum_out);
        CHECK_INT_EQ(a->cursor, b->cursor);
        CHECK_INT_EQ(a->hint, b->hint);
        CHECK_INT_EQ(a->is_error, b->is_error);
        CHECK_INT_EQ(a->error_code, b->error_code);
        CHECK_INT_EQ(a->completed_after, b->completed_after);
    }
    CHECK_INT_EQ(reused->end_pos, fresh->end_pos);
    CHECK_INT_EQ(reused->completed_frames, fresh->completed_frames);
    CHECK_INT_EQ(reused->zero_returns, fresh->zero_returns);
    CHECK_INT_EQ(reused->got_error, fresh->got_error);
    CHECK_INT_EQ(reused->error_code, fresh->error_code);
    CHECK_INT_EQ(reused->out.len, fresh->out.len);
    CHECK_INT_EQ(reused->out.len, seq->expected.len);
    compare_len = reused->out.len;
    if (compare_len && memcmp(reused->out.p, fresh->out.p, compare_len) != 0) {
        for (i = 0; i < compare_len; ++i) {
            if (reused->out.p[i] != fresh->out.p[i]) {
                snprintf(diag.first_diff, sizeof(diag.first_diff),
                         "reused/fresh output byte %zu: reused=0x%02x fresh=0x%02x",
                         i, reused->out.p[i], fresh->out.p[i]);
                break;
            }
        }
        CHECK(0);
    }
    if (seq->expected.len &&
        memcmp(reused->out.p, seq->expected.p, seq->expected.len) != 0) {
        for (i = 0; i < seq->expected.len; ++i) {
            if (reused->out.p[i] != seq->expected.p[i]) {
                snprintf(diag.first_diff, sizeof(diag.first_diff),
                         "model output byte %zu: expected=0x%02x actual=0x%02x",
                         i, seq->expected.p[i], reused->out.p[i]);
                break;
            }
        }
        CHECK(0);
    }
    return 0;
}

static int
feed_unfinished_with_skip(LZ4F_dctx *dctx, const FrameModel *frame)
{
    GuardBuffer guard;
    LZ4F_decompressOptions_t options;
    size_t cut = frame->header_size + 2U;
    size_t in_size = cut;
    size_t out_size = SHORT_OUT_CAP;
    size_t ret;

    CHECK(cut < frame->raw.len);
    guard_init(&guard);
    memset(&options, 0, sizeof(options));
    options.stableDst = 0;
    options.skipChecksums = 1;
    diag_switch(0, 1);
    ret = LZ4F_decompress(dctx, guard.out, &out_size,
                          frame->raw.p, &in_size, &options);
    CHECK(!LZ4F_isError(ret));
    CHECK(ret > 0);
    CHECK(in_size <= cut);
    CHECK(guard_ok(&guard));
    return 0;
}

static int
recovery_after_unfinished(void)
{
    Buf unfinished_payload;
    FrameModel unfinished;
    Sequence full, bad;
    RunResult reused_result, fresh_result, bad_result;
    LZ4F_dctx *dctx = NULL;
    size_t created;
    int rc;

    diag_reset();
    diag.sequence_index = 1201;
    memset(&unfinished_payload, 0, sizeof(unfinished_payload));
    CHECK(payload_make(&unfinished_payload, 70000U, 0, 91U));
    CHECK(frame_make_normal(&unfinished, &unfinished_payload,
                            LZ4F_blockLinked,
                            LZ4F_blockChecksumEnabled,
                            LZ4F_contentChecksumEnabled));

    created = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    CHECK(!LZ4F_isError(created) && dctx != NULL);
    diag_reset();
    diag.sequence_index = 1201;
    diag.hex = &unfinished.raw;
    diag.hex_label = "UNFINISHED_FRAME";
    CHECK(feed_unfinished_with_skip(dctx, &unfinished) == 0);

    LZ4F_resetDecompressionContext(dctx);

    CHECK(build_recovery_sequence(&full) == 0);
    diag.hex = NULL;
    diag.sequence = &full;
    run_result_init(&reused_result);
    rc = decode_with_dctx(dctx, &full, NULL, &reused_result);
    if (rc != 0) {
        LZ4F_freeDecompressionContext(dctx);
        return 1;
    }

    run_result_init(&fresh_result);
    CHECK(decode_fresh(&full, NULL, &fresh_result) == 0);
    CHECK(assert_same_run(&reused_result, &fresh_result, &full) == 0);

    CHECK(make_bad_content_sequence(&bad, 977U) == 0);
    diag.sequence = &bad;
    diag.run = &bad_result;
    run_result_init(&bad_result);
    rc = decode_with_dctx(dctx, &bad, NULL, &bad_result);
    if (rc != 0) {
        LZ4F_freeDecompressionContext(dctx);
        return 1;
    }
    CHECK_INT_EQ(bad_result.error_code,
                 (int)LZ4F_ERROR_contentChecksum_invalid);
    CHECK_INT_EQ(bad_result.got_error, 1);

    run_result_free(&bad_result);
    run_result_free(&reused_result);
    run_result_free(&fresh_result);
    sequence_free(&bad);
    sequence_free(&full);
    frame_free(&unfinished);
    buf_free(&unfinished_payload);
    LZ4F_freeDecompressionContext(dctx);
    return 0;
}

static int
recovery_after_checksum_failure(void)
{
    Sequence bad_first, full, bad_again;
    RunResult first_error, reused_result, fresh_result, again_result;
    LZ4F_dctx *dctx = NULL;
    size_t created;
    int rc;

    diag_reset();
    diag.sequence_index = 1202;
    created = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    CHECK(!LZ4F_isError(created) && dctx != NULL);

    CHECK(make_bad_content_sequence(&bad_first, 991U) == 0);
    diag_reset();
    diag.sequence_index = 1202;
    diag.sequence = &bad_first;
    run_result_init(&first_error);
    rc = decode_with_dctx(dctx, &bad_first, NULL, &first_error);
    if (rc != 0) {
        LZ4F_freeDecompressionContext(dctx);
        return 1;
    }
    CHECK_INT_EQ(first_error.error_code,
                 (int)LZ4F_ERROR_contentChecksum_invalid);
    CHECK_INT_EQ(first_error.got_error, 1);
    run_result_free(&first_error);
    sequence_free(&bad_first);

    LZ4F_resetDecompressionContext(dctx);

    CHECK(build_recovery_sequence(&full) == 0);
    diag.sequence = &full;
    run_result_init(&reused_result);
    rc = decode_with_dctx(dctx, &full, NULL, &reused_result);
    if (rc != 0) {
        LZ4F_freeDecompressionContext(dctx);
        return 1;
    }
    run_result_init(&fresh_result);
    CHECK(decode_fresh(&full, NULL, &fresh_result) == 0);
    CHECK(assert_same_run(&reused_result, &fresh_result, &full) == 0);

    CHECK(make_bad_content_sequence(&bad_again, 997U) == 0);
    diag.sequence = &bad_again;
    run_result_init(&again_result);
    rc = decode_with_dctx(dctx, &bad_again, NULL, &again_result);
    if (rc != 0) {
        LZ4F_freeDecompressionContext(dctx);
        return 1;
    }
    CHECK_INT_EQ(again_result.error_code,
                 (int)LZ4F_ERROR_contentChecksum_invalid);
    CHECK_INT_EQ(again_result.got_error, 1);

    run_result_free(&again_result);
    run_result_free(&reused_result);
    run_result_free(&fresh_result);
    sequence_free(&bad_again);
    sequence_free(&full);
    LZ4F_freeDecompressionContext(dctx);
    return 0;
}

static int
run_recovery_group(void)
{
    if (recovery_after_unfinished() != 0) return 1;
    if (recovery_after_checksum_failure() != 0) return 1;
    return 0;
}

const struct task_case task_cases[] = {
    { "sequence", "seed_1681_structured_sequences", run_sequence_group },
    { "integrity", "checksum_scope_and_error_codes", run_integrity_group },
    { "recovery", "reset_equivalence_and_recheck", run_recovery_group }
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
