/*
 * Regression tests for the ReadOrder duplicate-checking state machine.
 *
 * Every sequence is built through the public streaming entry points
 * (ass_process_codec_private / ass_process_chunk / ass_set_check_readorder /
 * ass_prune_events / ass_flush_events / ass_configure_prune /
 * ass_render_frame).  Failures report the test stage, the event ReadOrder
 * and the first field that differs from the expectation.
 *
 * Covered:
 *   1. a truncated/malformed packet does not occupy its ReadOrder and a
 *      complete retry of the same ReadOrder is accepted;
 *   2. duplicate checking toggled off/on while events with the same
 *      ReadOrder coexist, including pruning between the toggles;
 *   2b. re-enabling the check without any pruning keeps the coexisting
 *      ReadOrder occupied (a same-id packet is still rejected);
 *   3. partial pruning of events sharing a ReadOrder;
 *   4. the strict Start+Duration < deadline boundary (equality is kept),
 *      with events not stored in time order;
 *   5. ass_flush_events() frees every ReadOrder while distinct ids in the
 *      next batch are still deduplicated;
 *   6. manual pruning and render-triggered automatic pruning leave the
 *      same track state and the same rendered vector bitmaps;
 *   7. the linear-scan fallback for negative/too large ReadOrder values,
 *      and recovery back to the cached path;
 *   8. Layer and event order are preserved by prune compaction.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

static int failures;

#define FAIL(...) do { \
        fprintf(stderr, "FAIL " __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } while (0)

static void silent_msg(int level, const char *fmt, va_list args, void *data)
{
    (void) level;
    (void) fmt;
    (void) args;
    (void) data;
}

static ASS_Track *new_track(ASS_Library *lib)
{
    ASS_Track *t = ass_new_track(lib);
    assert(t);
    static const char header[] =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 320\n"
        "PlayResY: 240\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    ass_process_codec_private(t, header, (int) strlen(header));
    return t;
}

static void packet(ASS_Track *t, const char *p, long long tc, long long dur)
{
    ass_process_chunk(t, p, (int) strlen(p), tc, dur);
}

static void pkt(ASS_Track *t, const char *p)
{
    packet(t, p, 100, 100);
}

// Check the fields a streaming packet can actually set, in a fixed order
// so the reported field is the first one that differs.
static void expect_event(const char *stage, ASS_Track *t, int idx,
                         int readorder, int layer, int style,
                         long long start, long long duration,
                         const char *text)
{
    if (idx < 0 || idx >= t->n_events) {
        FAIL("stage=%s ReadOrder=%d: event index %d out of range (n_events=%d)",
             stage, readorder, idx, t->n_events);
        return;
    }
    const ASS_Event *e = &t->events[idx];
    const char *field;
    if (e->ReadOrder != readorder)            field = "ReadOrder";
    else if (e->Layer != layer)               field = "Layer";
    else if (e->Style != style)               field = "Style";
    else if (e->Start != start)               field = "Start";
    else if (e->Duration != duration)         field = "Duration";
    else if (!e->Text || strcmp(e->Text, text)) field = "Text";
    else                                      field = NULL;
    if (field)
        FAIL("stage=%s ReadOrder=%d event[%d]: first differing field=%s",
             stage, readorder, idx, field);
}

static void expect_count(const char *stage, ASS_Track *t, int want)
{
    if (t->n_events != want)
        FAIL("stage=%s n_events: got %d, want %d", stage, t->n_events, want);
}

/* 1. malformed packet must not reserve its ReadOrder */
static void test_malformed_retry(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    pkt(t, "10,0,Default,,0,0,0,,good");
    expect_count("malformed:seed", t, 1);
    expect_event("malformed:seed", t, 0, 10, 0, 0, 100, 100, "good");

    // ordinary duplicate while checking is enabled
    pkt(t, "10,0,Default,,0,0,0,,duplicate");
    expect_count("malformed:duplicate", t, 1);
    expect_event("malformed:duplicate", t, 0, 10, 0, 0, 100, 100, "good");

    // truncated packet: ReadOrder 11 must not be reserved
    pkt(t, "11");
    expect_count("malformed:truncated", t, 1);

    // ...so the complete retry is accepted
    pkt(t, "11,0,Default,,0,0,0,,valid");
    expect_count("malformed:retry", t, 2);
    expect_event("malformed:retry", t, 1, 11, 0, 0, 100, 100, "valid");
    expect_event("malformed:retry", t, 0, 10, 0, 0, 100, 100, "good");

    // and afterwards 11 dedupes normally
    pkt(t, "11,0,Default,,0,0,0,,again");
    expect_count("malformed:retry-duplicate", t, 2);
    expect_event("malformed:retry-duplicate", t, 1, 11, 0, 0, 100, 100, "valid");

    ass_free_track(t);
}

/* 2. toggling the check around pruning, with same-ReadOrder coexistence */
static void test_toggle_and_prune(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    pkt(t, "12,0,Default,,0,0,0,,old");
    pkt(t, "12,0,Default,,0,0,0,,duplicate");
    expect_count("toggle:seed", t, 1);
    expect_event("toggle:seed", t, 0, 12, 0, 0, 100, 100, "old");

    // checking off: a second event with ReadOrder 12 may coexist
    ass_set_check_readorder(t, 0);
    pkt(t, "12,0,Default,,0,0,0,,second");
    expect_count("toggle:off-insert", t, 2);
    expect_event("toggle:off-insert", t, 1, 12, 0, 0, 100, 100, "second");

    // pruning while the check is off must still free the ReadOrder
    ass_prune_events(t, 201);
    expect_count("toggle:pruned", t, 0);

    // checking back on: the removed ReadOrder is reusable again
    ass_set_check_readorder(t, 1);
    pkt(t, "12,0,Default,,0,0,0,,new");
    expect_count("toggle:reuse", t, 1);
    expect_event("toggle:reuse", t, 0, 12, 0, 0, 100, 100, "new");
    pkt(t, "12,0,Default,,0,0,0,,new-duplicate");
    expect_count("toggle:reuse-duplicate", t, 1);
    expect_event("toggle:reuse-duplicate", t, 0, 12, 0, 0, 100, 100, "new");

    ass_free_track(t);
}

/* 2b. same-ReadOrder events survive re-enabling the check without a prune */
static void test_reenable_without_prune(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    ass_set_check_readorder(t, 0);
    pkt(t, "20,0,Default,,0,0,0,,first");
    pkt(t, "20,0,Default,,0,0,0,,second");
    pkt(t, "21,0,Default,,0,0,0,,other");
    expect_count("reenable:seed", t, 3);

    // nothing removed: ReadOrder 20 is still in use (twice), so a new
    // packet with it must be rejected while a fresh id is accepted
    ass_set_check_readorder(t, 1);
    pkt(t, "20,0,Default,,0,0,0,,rejected");
    expect_count("reenable:same-id-rejected", t, 3);
    pkt(t, "22,0,Default,,0,0,0,,accepted");
    expect_count("reenable:fresh-id", t, 4);
    expect_event("reenable:fresh-id", t, 3, 22, 0, 0, 100, 100, "accepted");

    ass_free_track(t);
}

/* 3. pruning only one of several events sharing a ReadOrder */
static void test_partial_prune_same_readorder(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    ass_set_check_readorder(t, 0);
    packet(t, "30,0,Default,,0,0,0,,early", 100, 100); // ends 200
    packet(t, "30,0,Default,,0,0,0,,late",  300, 100); // ends 400
    expect_count("partial:seed", t, 2);

    // only "early" is discardable; ReadOrder 30 must stay occupied
    ass_prune_events(t, 250);
    expect_count("partial:first-prune", t, 1);
    expect_event("partial:first-prune", t, 0, 30, 0, 0, 300, 100, "late");

    // re-enabling the check cannot make the surviving id reusable
    ass_set_check_readorder(t, 1);
    pkt(t, "30,0,Default,,0,0,0,,rejected");
    expect_count("partial:still-duplicate", t, 1);
    expect_event("partial:still-duplicate", t, 0, 30, 0, 0, 300, 100, "late");

    // once the last event with that id is gone, it becomes reusable
    ass_prune_events(t, 450);
    expect_count("partial:second-prune", t, 0);
    pkt(t, "30,0,Default,,0,0,0,,reborn");
    expect_count("partial:reborn", t, 1);
    expect_event("partial:reborn", t, 0, 30, 0, 0, 100, 100, "reborn");

    ass_free_track(t);
}

/* 4. deadline boundary with out-of-time-order events */
static void test_deadline_boundary(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    packet(t, "40,0,Default,,0,0,0,,later",   500, 100); // ends 600, inserted first
    packet(t, "41,0,Default,,0,0,0,,earlier", 100, 100); // ends 200, inserted second
    expect_count("boundary:seed", t, 2);

    // end == deadline is not strictly smaller: nothing is pruned
    ass_prune_events(t, 200);
    expect_count("boundary:equal-kept", t, 2);

    // one ms later the earlier event goes, the later one survives and is
    // compacted to index 0 without any field change
    ass_prune_events(t, 201);
    expect_count("boundary:strict-cut", t, 1);
    expect_event("boundary:strict-cut", t, 0, 40, 0, 0, 500, 100, "later");

    ass_free_track(t);
}

/* 5. flush frees all ReadOrders but dedup still works afterwards */
static void test_flush(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    pkt(t, "50,0,Default,,0,0,0,,a");
    pkt(t, "51,0,Default,,0,0,0,,b");
    pkt(t, "52,0,Default,,0,0,0,,c");
    pkt(t, "50,0,Default,,0,0,0,,a-dup");
    expect_count("flush:seed", t, 3);

    ass_flush_events(t);
    expect_count("flush:emptied", t, 0);

    // every id, including flushed ones, is reusable
    pkt(t, "50,0,Default,,0,0,0,,a-again");
    expect_count("flush:reuse", t, 1);
    expect_event("flush:reuse", t, 0, 50, 0, 0, 100, 100, "a-again");

    // distinct ids of the new batch are still deduplicated
    pkt(t, "53,0,Default,,0,0,0,,d");
    expect_count("flush:new-id", t, 2);
    pkt(t, "53,0,Default,,0,0,0,,d-dup");
    expect_count("flush:new-id-dup", t, 2);

    ass_free_track(t);
}

/* deterministic snapshot of a rendered ASS_Image list */
typedef struct {
    int w, h, stride, dst_x, dst_y, type;
    uint32_t color;
    unsigned char *bitmap; // exactly stride * (h - 1) + w bytes
    size_t bitmap_size;
} ImageRecord;

typedef struct {
    ImageRecord *recs;
    int n;
} ImageSnapshot;

static void free_snapshot(ImageSnapshot *snap)
{
    for (int i = 0; i < snap->n; i++)
        free(snap->recs[i].bitmap);
    free(snap->recs);
    snap->recs = NULL;
    snap->n = 0;
}

static int take_snapshot(const char *stage, ASS_Image *img, ImageSnapshot *out)
{
    out->recs = NULL;
    out->n = 0;

    int n = 0;
    for (ASS_Image *cur = img; cur; cur = cur->next)
        n++;
    out->recs = calloc((size_t) n, sizeof(*out->recs));
    if (n && !out->recs) {
        FAIL("stage=%s: out of memory", stage);
        return -1;
    }
    out->n = n;

    int i = 0;
    for (ASS_Image *cur = img; cur; cur = cur->next, i++) {
        ImageRecord *r = &out->recs[i];
        r->w = cur->w;
        r->h = cur->h;
        r->stride = cur->stride;
        r->dst_x = cur->dst_x;
        r->dst_y = cur->dst_y;
        r->color = cur->color;
        r->type = cur->type;
        // only guaranteed-initialized bytes: last row is not padded
        r->bitmap_size = (size_t) cur->stride * (cur->h - 1) + cur->w;
        r->bitmap = malloc(r->bitmap_size);
        if (!r->bitmap) {
            FAIL("stage=%s: out of memory", stage);
            return -1;
        }
        memcpy(r->bitmap, cur->bitmap, r->bitmap_size);
    }
    return 0;
}

static void compare_snapshots(const char *stage,
                              const ImageSnapshot *got,
                              const ImageSnapshot *ref)
{
    if (got->n != ref->n) {
        FAIL("stage=%s: image count: got %d, want %d", stage, got->n, ref->n);
        return;
    }
    for (int i = 0; i < ref->n; i++) {
        const ImageRecord *g = &got->recs[i];
        const ImageRecord *r = &ref->recs[i];
        const char *field;
        if (g->w != r->w)                 field = "w";
        else if (g->h != r->h)            field = "h";
        else if (g->stride != r->stride)  field = "stride";
        else if (g->dst_x != r->dst_x)    field = "dst_x";
        else if (g->dst_y != r->dst_y)    field = "dst_y";
        else if (g->color != r->color)    field = "color";
        else if (g->type != r->type)      field = "type";
        else if (g->bitmap_size != r->bitmap_size ||
                 memcmp(g->bitmap, r->bitmap, r->bitmap_size))
                                          field = "bitmap";
        else
            field = NULL;
        if (field) {
            FAIL("stage=%s: image[%d] first differing field=%s", stage, i, field);
            return;
        }
    }
}

static int snapshot_has_visible_pixels(const ImageSnapshot *snap)
{
    for (int i = 0; i < snap->n; i++) {
        const ImageRecord *r = &snap->recs[i];
        if (r->w <= 0 || r->h <= 0)
            continue;
        for (size_t k = 0; k < r->bitmap_size; k++)
            if (r->bitmap[k])
                return 1;
    }
    return 0;
}

static void add_bundled_font(ASS_Library *lib)
{
    FILE *f = fopen("harness/SourceSans3-Regular.ttf", "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *font = malloc((size_t) n);
    assert(font && fread(font, 1, (size_t) n, f) == (size_t) n);
    fclose(f);
    ass_add_font(lib, "SourceSans3-Regular.ttf", font, (int) n);
    free(font);
}

static ASS_Renderer *make_renderer(ASS_Library *lib)
{
    ASS_Renderer *r = ass_renderer_init(lib);
    assert(r);
    ass_set_frame_size(r, 320, 240);
    ass_set_fonts(r, NULL, "Source Sans 3", ASS_FONTPROVIDER_NONE, NULL, 0);
    return r;
}

/* vector drawing events, p1 rectangles so no system font is required */
static const char draw_red[] =
    "60,0,Default,,0,0,0,,{\\an7\\pos(10,10)\\1c&H0000FF&\\p1}m 0 0 l 30 0 30 20 0 20";
static const char draw_green[] =
    "61,0,Default,,0,0,0,,{\\an7\\pos(40,40)\\1c&H00FF00&\\p1}m 0 0 l 30 0 30 20 0 20";
static const char draw_blue[] =
    "62,0,Default,,0,0,0,,{\\an7\\pos(70,70)\\1c&HFF0000&\\p1}m 0 0 l 30 0 30 20 0 20";

/* 6. automatic (render) pruning matches manual pruning, bitmaps included */
static void test_manual_vs_auto_prune(ASS_Library *lib)
{
    ASS_Track *manual = new_track(lib);
    ASS_Track *auto_p = new_track(lib);
    ASS_Renderer *ra = NULL, *rm = NULL;

    // ends: 200, 500, 800; inserted in ReadOrder order
    packet(manual, draw_red,   100, 100);
    packet(manual, draw_green, 300, 200);
    packet(manual, draw_blue,  600, 200);
    packet(auto_p, draw_red,   100, 100);
    packet(auto_p, draw_green, 300, 200);
    packet(auto_p, draw_blue,  600, 200);

    // same deadline (201) reached two different ways
    ass_prune_events(manual, 201);
    ass_configure_prune(auto_p, 0);
    ra = make_renderer(lib);
    // no event is active at now=201, so the list may legitimately be empty;
    // this render still has to perform the automatic prune
    (void) ass_render_frame(ra, auto_p, 201, NULL);

    // track state must be identical
    expect_count("prune-cmp:auto", auto_p, 2);
    expect_count("prune-cmp:manual", manual, 2);
    for (int i = 0; i < 2; i++) {
        const char *stage = i == 0 ? "prune-cmp:green" : "prune-cmp:blue";
        int ro = i == 0 ? 61 : 62;
        long long start = i == 0 ? 300 : 600;
        long long dur = i == 0 ? 200 : 200;
        expect_event(stage, auto_p, i, ro, 0, 0, start, dur,
                     auto_p->events[i].Text);
        expect_event(stage, manual, i, ro, 0, 0, start, dur,
                     auto_p->events[i].Text);
    }

    rm = make_renderer(lib);

    // at now=400 only the green rectangle is active on both tracks
    ImageSnapshot ref400, got400;
    ASS_Image *img = ass_render_frame(rm, manual, 400, NULL);
    if (take_snapshot("render:400:manual", img, &ref400))
        goto out;
    img = ass_render_frame(ra, auto_p, 400, NULL);
    if (take_snapshot("render:400:auto", img, &got400)) {
        free_snapshot(&ref400);
        goto out;
    }
    if (!snapshot_has_visible_pixels(&ref400))
        FAIL("stage=render:400:manual: vector render produced no visible pixels");
    compare_snapshots("render:400", &got400, &ref400);
    free_snapshot(&ref400);
    free_snapshot(&got400);

    // keep the manual track's prune state in lockstep for the next frame
    ass_prune_events(manual, 700);

    // at now=700 only the blue rectangle is active
    ImageSnapshot ref700, got700;
    img = ass_render_frame(rm, manual, 700, NULL);
    if (take_snapshot("render:700:manual", img, &ref700))
        goto out;
    img = ass_render_frame(ra, auto_p, 700, NULL);
    if (take_snapshot("render:700:auto", img, &got700)) {
        free_snapshot(&ref700);
        goto out;
    }
    if (!snapshot_has_visible_pixels(&ref700))
        FAIL("stage=render:700:manual: vector render produced no visible pixels");
    compare_snapshots("render:700", &got700, &ref700);
    expect_count("prune-cmp:auto:700", auto_p, 1);
    expect_count("prune-cmp:manual:700", manual, 1);
    free_snapshot(&ref700);
    free_snapshot(&got700);

out:
    ass_renderer_done(rm);
    ass_renderer_done(ra);
    ass_free_track(manual);
    ass_free_track(auto_p);
}

/* 7. linear-scan fallback for unrepresentable ReadOrder values */
static void test_fallback_path(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    pkt(t, "-5,0,Default,,0,0,0,,negative");
    expect_count("fallback:negative", t, 1);
    pkt(t, "-5,0,Default,,0,0,0,,negative-dup");
    expect_count("fallback:negative-dup", t, 1);
    expect_event("fallback:negative-dup", t, 0, -5, 0, 0, 100, 100, "negative");

    // an id above the cache limit forces the linear path as well
    pkt(t, "99999999,0,Default,,0,0,0,,huge");
    expect_count("fallback:huge", t, 2);
    pkt(t, "99999999,0,Default,,0,0,0,,huge-dup");
    expect_count("fallback:huge-dup", t, 2);

    // ordinary packets must not build a partial cache that would let the
    // unrepresentable id's duplicates slip through
    pkt(t, "7,0,Default,,0,0,0,,ordinary");
    expect_count("fallback:ordinary", t, 3);
    pkt(t, "99999999,0,Default,,0,0,0,,huge-dup-again");
    expect_count("fallback:huge-dup-still-rejected", t, 3);
    pkt(t, "7,0,Default,,0,0,0,,ordinary-dup");
    expect_count("fallback:ordinary-dup", t, 3);

    // pruning empties the track; the next ordinary packet rebuilds the
    // cache and normal deduplication resumes
    ass_prune_events(t, 201);
    expect_count("fallback:pruned", t, 0);
    pkt(t, "5,0,Default,,0,0,0,,normal");
    expect_count("fallback:recovered", t, 1);
    expect_event("fallback:recovered", t, 0, 5, 0, 0, 100, 100, "normal");
    pkt(t, "5,0,Default,,0,0,0,,normal-dup");
    expect_count("fallback:recovered-dup", t, 1);
    expect_event("fallback:recovered-dup", t, 0, 5, 0, 0, 100, 100, "normal");

    ass_free_track(t);
}

/* 8. prune compaction keeps Layer and relative event order */
static void test_layer_and_order(ASS_Library *lib)
{
    ASS_Track *t = new_track(lib);

    packet(t, "70,2,Default,,0,0,0,,L2", 100, 300); // ends 400
    packet(t, "71,0,Default,,0,0,0,,L0", 100, 50);  // ends 150
    packet(t, "72,1,Default,,0,0,0,,L1", 100, 300); // ends 400
    expect_count("order:seed", t, 3);

    ass_prune_events(t, 200);
    expect_count("order:pruned", t, 2);
    expect_event("order:pruned", t, 0, 70, 2, 0, 100, 300, "L2");
    expect_event("order:pruned", t, 1, 72, 1, 0, 100, 300, "L1");

    ass_free_track(t);
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    assert(lib);
    ass_set_message_cb(lib, silent_msg, NULL);
    add_bundled_font(lib);

    test_malformed_retry(lib);
    test_toggle_and_prune(lib);
    test_reenable_without_prune(lib);
    test_partial_prune_same_readorder(lib);
    test_deadline_boundary(lib);
    test_flush(lib);
    test_manual_vs_auto_prune(lib);
    test_fallback_path(lib);
    test_layer_and_order(lib);

    ass_library_done(lib);

    if (failures) {
        fprintf(stderr, "READORDER_LIFECYCLE_FAILURES=%d\n", failures);
        return 1;
    }
    puts("READORDER_LIFECYCLE_OK");
    return 0;
}
