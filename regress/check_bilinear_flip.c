// Regression checks for signed (mirroring) dimensions in
// olivec_sprite_copy_bilinear(). Everything runs on small in-memory textures:
// no windows, no image libraries.
//
// Covered: the four flip directions, clipping on all four edges (rendered
// first and cropped afterwards, so clipping never rescales), sentinel-padded
// strides on both source and destination, olivec_subcanvas() views, single
// pixel axes (1xN, Nx1, 1x1), empty sources / zero sizes / fully clipped
// rectangles, and positive-size compatibility with the original filter.
#include <stdio.h>
#include <stdint.h>

#define OLIVEC_IMPLEMENTATION
#include "olive.c"

#define SENTINEL 0x12345678u
#define SRC_PAD  0xDEADBEEFu

static int checks = 0;
static int failures = 0;

static void expect_rgba(const char *what, uint32_t got, int x, int y,
                        uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    ++checks;
    if (OLIVEC_RED(got)   != r || OLIVEC_GREEN(got) != g ||
        OLIVEC_BLUE(got)  != b || OLIVEC_ALPHA(got) != a) {
        ++failures;
        printf("FAIL %s at (%d,%d): expected rgba(%u,%u,%u,%u), got rgba(%u,%u,%u,%u)\n",
               what, x, y, r, g, b, a,
               OLIVEC_RED(got), OLIVEC_GREEN(got),
               OLIVEC_BLUE(got), OLIVEC_ALPHA(got));
    }
}

static void expect_u32(const char *what, uint32_t got, uint32_t want, int idx)
{
    ++checks;
    if (got != want) {
        ++failures;
        printf("FAIL %s at [%d]: got 0x%08x, want 0x%08x\n",
               what, idx, got, want);
    }
}

static void fill_sentinel(uint32_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i) buf[i] = SENTINEL;
}

// uint32_t operands keep the alpha shift free of signed-overflow UB; the
// produced value matches the library's RGBA8 layout byte for byte.
static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Explicit bilinear output of the 2x2 RGBA source below, rendered at 4x4.
// Integer truncation and edge clamping are those of olivec_pixel_bilinear().
static const uint32_t EXP_R[4][4] = {
    {  0,   0,  50, 100},
    {  0,   0,  50, 100},
    {100, 100, 138, 177},
    {200, 200, 227, 255},
};
static const uint32_t EXP_G[4][4] = {
    { 0,  0,  5, 10},
    { 0,  0,  5, 10},
    {15, 15, 22, 30},
    {30, 30, 40, 50},
};
static const uint32_t EXP_B[4][4] = {
    { 0,  0, 10, 20},
    { 0,  0, 10, 20},
    {20, 20, 30, 40},
    {40, 40, 50, 60},
};

static Olivec_Canvas make_rgba_source(uint32_t *buf)
{
    // 2x2 source with stride 3: the third column is padding that must never
    // participate in sampling.
    buf[0] = rgba(  0,  0,  0, 255);
    buf[1] = rgba(100, 10, 20, 255);
    buf[2] = SRC_PAD;
    buf[3] = rgba(200, 30, 40, 255);
    buf[4] = rgba(255, 50, 60, 255);
    buf[5] = SRC_PAD;
    return olivec_canvas(buf, 2, 2, 3);
}

static Olivec_Canvas make_red_source(uint32_t *buf)
{
    // Same red-only source as regress/baseline_bilinear.c.
    buf[0] = rgba(  0, 0, 0, 255);
    buf[1] = rgba(100, 0, 0, 255);
    buf[2] = SRC_PAD;
    buf[3] = rgba(200, 0, 0, 255);
    buf[4] = rgba(255, 0, 0, 255);
    buf[5] = SRC_PAD;
    return olivec_canvas(buf, 2, 2, 3);
}

static void check_positive_compat(Olivec_Canvas src)
{
    // Destination with stride larger than width and a spare bottom row: the
    // padding and everything outside the 4x4 rect must stay untouched.
    uint32_t dst[35];
    fill_sentinel(dst, sizeof(dst)/sizeof(dst[0]));
    Olivec_Canvas oc = olivec_canvas(dst, 5, 5, 7);
    olivec_sprite_copy_bilinear(oc, 0, 0, 4, 4, src);

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            expect_rgba("positive", dst[y*7 + x], x, y,
                        EXP_R[y][x], EXP_G[y][x], EXP_B[y][x], 255);
        }
    }
    // Destination row padding and the spare row/column are not rewritten.
    for (int y = 0; y < 5; ++y) {
        for (int x = (y < 4 ? 4 : 0); x < 7; ++x) {
            expect_u32("positive dst padding", dst[y*7 + x], SENTINEL, y*7 + x);
        }
    }
}

static void check_four_directions(Olivec_Canvas src)
{
    struct { const char *name; int x, y, w, h; } cases[4] = {
        {"pos",   0, 0,  4,  4},
        {"hflip", 3, 0, -4,  4},
        {"vflip", 0, 3,  4, -4},
        {"flip",  3, 3, -4, -4},
    };

    for (int c = 0; c < 4; ++c) {
        uint32_t dst[16];
        fill_sentinel(dst, 16);
        Olivec_Canvas oc = olivec_canvas(dst, 4, 4, 4);
        olivec_sprite_copy_bilinear(oc, cases[c].x, cases[c].y,
                                    cases[c].w, cases[c].h, src);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                int sx = (cases[c].w < 0) ? 3 - x : x;
                int sy = (cases[c].h < 0) ? 3 - y : y;
                expect_rgba(cases[c].name, dst[y*4 + x], x, y,
                            EXP_R[sy][sx], EXP_G[sy][sx],
                            EXP_B[sy][sx], 255);
            }
        }
    }
}

// Full 4x4 reference render, used to validate clipped draws: clipping may
// only drop destination pixels, it must never rescale the visible part.
static void render_reference(Olivec_Canvas src, uint32_t ref[16])
{
    fill_sentinel(ref, 16);
    Olivec_Canvas oc = olivec_canvas(ref, 4, 4, 4);
    olivec_sprite_copy_bilinear(oc, 0, 0, 4, 4, src);
}

static void check_clipped(const char *name, Olivec_Canvas src,
                          int x, int y, int w, int h,
                          size_t cw, size_t ch, size_t stride,
                          int xa, int ya)
{
    uint32_t ref[16];
    render_reference(src, ref);

    uint32_t dst[32];
    fill_sentinel(dst, sizeof(dst)/sizeof(dst[0]));
    Olivec_Canvas oc = olivec_canvas(dst, cw, ch, stride);
    olivec_sprite_copy_bilinear(oc, x, y, w, h, src);

    int aw = OLIVEC_ABS(int, w);
    int ah = OLIVEC_ABS(int, h);
    for (size_t ty = 0; ty < ch; ++ty) {
        for (size_t tx = 0; tx < cw; ++tx) {
            // Logical index along the signed direction from the anchor. Only
            // 0..aw-1 / 0..ah-1 belong to the rectangle: coverage is one-sided
            // relative to the anchor, not symmetric.
            int lx = OLIVEC_SIGN(int, w)*((int) tx - xa);
            int ly = OLIVEC_SIGN(int, h)*((int) ty - ya);
            uint32_t want = SENTINEL;
            if (0 <= lx && lx < aw && 0 <= ly && ly < ah) {
                want = ref[ly*4 + lx];
            }
            expect_u32(name, dst[ty*stride + tx], want, (int)(ty*stride + tx));
        }
        for (size_t px = cw; px < stride; ++px) {
            expect_u32("clipped dst stride pad",
                       dst[ty*stride + px], SENTINEL, (int)(ty*stride + px));
        }
    }
}

static void check_clipping(Olivec_Canvas red_src)
{
    // Explicit matrix from the task description (red channel of the 2x2 source
    // scaled to 4x4):
    static const uint32_t RED_POS[4][4] = {
        {  0,   0,  50, 100},
        {  0,   0,  50, 100},
        {100, 100, 138, 177},
        {200, 200, 227, 255},
    };
    uint32_t ref[16];
    render_reference(red_src, ref);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            expect_rgba("red reference", ref[y*4 + x], x, y,
                        RED_POS[y][x], 0, 0, 255);
        }
    }

    // The task's anchor example: (x=2, y=0, w=-4, h=4) onto a width-3 target.
    // Logical column 0 lands at target x=2; the off-canvas logical column 3
    // must not rescale the rest. First red row is exactly [50, 0, 0].
    check_clipped("neg-right-anchor", red_src, 2, 0, -4, 4, 3, 4, 5, 2, 0);
    {
        uint32_t dst[20];
        fill_sentinel(dst, sizeof(dst)/sizeof(dst[0]));
        Olivec_Canvas oc = olivec_canvas(dst, 3, 4, 5);
        olivec_sprite_copy_bilinear(oc, 2, 0, -4, 4, red_src);
        expect_u32("task row0 col0 red",
                   OLIVEC_RED(dst[0*5 + 0]), 50, 0);
        expect_u32("task row0 col1 red",
                   OLIVEC_RED(dst[0*5 + 1]), 0, 1);
        expect_u32("task row0 col2 red",
                   OLIVEC_RED(dst[0*5 + 2]), 0, 2);
    }

    // All four edges, positive direction.
    check_clipped("left-cut",   red_src, -2, 0, 4, 4, 3, 4, 5, -2, 0);
    check_clipped("right-cut",  red_src,  2, 0, 4, 4, 3, 4, 5,  2, 0);
    check_clipped("top-cut",    red_src, 0, -2, 4, 4, 4, 3, 6, 0, -2);
    check_clipped("bottom-cut", red_src,  0, 2, 4, 4, 4, 3, 6,  0, 2);

    // Explicit left-cut constants: target row 0 keeps logical columns 2,3.
    {
        uint32_t dst[20]; // 4 rows * stride 5
        fill_sentinel(dst, 20);
        Olivec_Canvas oc = olivec_canvas(dst, 3, 4, 5);
        olivec_sprite_copy_bilinear(oc, -2, 0, 4, 4, red_src);
        expect_u32("left-cut r0c0", OLIVEC_RED(dst[0]), 50, 0);
        expect_u32("left-cut r0c1", OLIVEC_RED(dst[1]), 100, 1);
        expect_u32("left-cut r0c2 untouched", dst[2], SENTINEL, 2);
    }

    // Both axes flipped and clipped at once.
    check_clipped("flip-cut", red_src, 2, 2, -4, -4, 3, 3, 5, 2, 2);
}

static void check_empty_and_zero(Olivec_Canvas src)
{
    uint32_t dst[16];

    // w == 0 draws nothing.
    fill_sentinel(dst, 16);
    olivec_sprite_copy_bilinear(olivec_canvas(dst, 4, 4, 4), 0, 0, 0, 4, src);
    for (int i = 0; i < 16; ++i) expect_u32("zero width", dst[i], SENTINEL, i);

    // h == 0 draws nothing.
    fill_sentinel(dst, 16);
    olivec_sprite_copy_bilinear(olivec_canvas(dst, 4, 4, 4), 0, 0, 4, 0, src);
    for (int i = 0; i < 16; ++i) expect_u32("zero height", dst[i], SENTINEL, i);

    // Fully clipped rectangles (positive and negative) draw nothing.
    fill_sentinel(dst, 16);
    olivec_sprite_copy_bilinear(olivec_canvas(dst, 4, 4, 4), 10, 10, 4, 4, src);
    for (int i = 0; i < 16; ++i) expect_u32("fully clipped pos", dst[i], SENTINEL, i);

    fill_sentinel(dst, 16);
    olivec_sprite_copy_bilinear(olivec_canvas(dst, 4, 4, 4), -10, 0, -4, 4, src);
    for (int i = 0; i < 16; ++i) expect_u32("fully clipped neg", dst[i], SENTINEL, i);

    // Empty source (zero width/height) draws nothing.
    uint32_t empty_buf[4] = {SENTINEL, SENTINEL, SENTINEL, SENTINEL};
    Olivec_Canvas empty = olivec_canvas(empty_buf, 0, 0, 2);
    fill_sentinel(dst, 16);
    olivec_sprite_copy_bilinear(olivec_canvas(dst, 4, 4, 4), 0, 0, 4, 4, empty);
    for (int i = 0; i < 16; ++i) expect_u32("empty source", dst[i], SENTINEL, i);
    for (int i = 0; i < 4; ++i) expect_u32("empty source buf", empty_buf[i], SENTINEL, i);
}

static void check_single_pixel_axes(void)
{
    // 1x1 source scaled up: every destination pixel equals the source pixel.
    {
        uint32_t src_buf[1] = {rgba(77, 88, 99, 255)};
        Olivec_Canvas src = olivec_canvas(src_buf, 1, 1, 1);
        uint32_t dst[8];
        int sizes[4][2] = {{3, 2}, {-3, 2}, {3, -2}, {-3, -2}};
        for (int c = 0; c < 4; ++c) {
            fill_sentinel(dst, 8);
            Olivec_Canvas oc = olivec_canvas(dst, 3, 2, 4);
            int x = sizes[c][0] < 0 ? 2 : 0;
            int y = sizes[c][1] < 0 ? 1 : 0;
            olivec_sprite_copy_bilinear(oc, x, y, sizes[c][0], sizes[c][1], src);
            for (int yy = 0; yy < 2; ++yy) {
                for (int xx = 0; xx < 3; ++xx) {
                    expect_rgba("1x1 scaled", dst[yy*4 + xx], xx, yy, 77, 88, 99, 255);
                }
                expect_u32("1x1 stride pad", dst[yy*4 + 3], SENTINEL, yy*4 + 3);
            }
        }
    }

    // Nx1 source (single row) scaled to 2x4: only source columns 0 and 1 are
    // reached, every destination row is identical.
    {
        uint32_t src_buf[3] = {
            rgba(10, 20, 30, 255),
            rgba(40, 50, 60, 255),
            rgba(70, 80, 90, 255),
        };
        Olivec_Canvas src = olivec_canvas(src_buf, 3, 1, 3);
        uint32_t dst[8];
        fill_sentinel(dst, 8);
        olivec_sprite_copy_bilinear(olivec_canvas(dst, 2, 4, 2), 0, 0, 2, 4, src);
        for (int yy = 0; yy < 4; ++yy) {
            expect_rgba("Nx1 col0", dst[yy*2 + 0], 0, yy, 10, 20, 30, 255);
            expect_rgba("Nx1 col1", dst[yy*2 + 1], 1, yy, 40, 50, 60, 255);
        }
    }

    // 1xN source (single column) scaled to 4x2: only source rows 0 and 1 are
    // reached, every destination column is identical.
    {
        uint32_t src_buf[3] = {
            rgba(1, 2, 3, 255),
            rgba(4, 5, 6, 255),
            rgba(7, 8, 9, 255),
        };
        Olivec_Canvas src = olivec_canvas(src_buf, 1, 3, 1);
        uint32_t dst[8];
        fill_sentinel(dst, 8);
        olivec_sprite_copy_bilinear(olivec_canvas(dst, 4, 2, 4), 0, 0, 4, 2, src);
        for (int xx = 0; xx < 4; ++xx) {
            expect_rgba("1xN row0", dst[0*4 + xx], xx, 0, 1, 2, 3, 255);
            expect_rgba("1xN row1", dst[1*4 + xx], xx, 1, 4, 5, 6, 255);
        }
    }

    // Smallest signed rectangles (the frames rendered right after an
    // interactive size crosses zero): +/-1 must hit the kernel with a nonzero
    // divisor and keep the anchor pixel equal to the zero-th source sample.
    {
        uint32_t src_buf[4] = {
            rgba(11, 22, 33, 255),
            rgba(44, 55, 66, 255),
            rgba(77, 88, 99, 255),
            rgba(111, 122, 133, 255),
        };
        Olivec_Canvas src = olivec_canvas(src_buf, 2, 2, 2);
        uint32_t dst[9];
        struct { int x, y, w, h; } tiny[4] = {
            {1, 1,  1,  1},
            {1, 1, -1,  1},
            {1, 1,  1, -1},
            {1, 1, -1, -1},
        };
        for (int c = 0; c < 4; ++c) {
            fill_sentinel(dst, 9);
            Olivec_Canvas oc = olivec_canvas(dst, 3, 3, 3);
            olivec_sprite_copy_bilinear(oc, tiny[c].x, tiny[c].y,
                                        tiny[c].w, tiny[c].h, src);
            // The anchor pixel always samples the source's zero-th column/row.
            expect_rgba("tiny anchor", dst[1*3 + 1], 1, 1, 11, 22, 33, 255);
            for (int i = 0; i < 9; ++i) {
                if (i != 1*3 + 1) expect_u32("tiny untouched", dst[i], SENTINEL, i);
            }
        }
    }
}

static void check_subcanvas(Olivec_Canvas compact_src)
{
    // Source living inside a padded backing, reached via olivec_subcanvas().
    uint32_t sb[25];
    fill_sentinel(sb, 25);
    sb[1*5 + 1] = rgba(  0,  0,  0, 255);
    sb[1*5 + 2] = rgba(100, 10, 20, 255);
    sb[2*5 + 1] = rgba(200, 30, 40, 255);
    sb[2*5 + 2] = rgba(255, 50, 60, 255);
    Olivec_Canvas sparent = olivec_canvas(sb, 4, 4, 5);
    Olivec_Canvas ssub = olivec_subcanvas(sparent, 1, 1, 2, 2);
    expect_u32("subcanvas source width", ssub.width, 2, 0);
    expect_u32("subcanvas source stride", ssub.stride, 5, 0);

    uint32_t ref[16], got[16];
    render_reference(compact_src, ref);
    fill_sentinel(got, 16);
    olivec_sprite_copy_bilinear(olivec_canvas(got, 4, 4, 4), 0, 0, 4, 4, ssub);
    for (int i = 0; i < 16; ++i) expect_u32("subcanvas source render", got[i], ref[i], i);
    for (int i = 0; i < 25; ++i) {
        int x = (int)(i % 5), y = (int)(i / 5);
        int inside = (x >= 1 && x <= 2 && y >= 1 && y <= 2);
        if (!inside) expect_u32("source backing untouched", sb[i], SENTINEL, i);
    }

    // Destination as a non-compact subcanvas view.
    uint32_t db[64];
    fill_sentinel(db, 64);
    Olivec_Canvas dparent = olivec_canvas(db, 8, 8, 8);
    Olivec_Canvas dsub = olivec_subcanvas(dparent, 2, 1, 4, 4);
    olivec_sprite_copy_bilinear(dsub, 0, 0, 4, 4, compact_src);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            expect_u32("subcanvas dst pixel",
                       db[(y + 1)*8 + (x + 2)], ref[y*4 + x], y*4 + x);
        }
    }
    for (int i = 0; i < 64; ++i) {
        int x = i % 8, y = i / 8;
        int inside = (x >= 2 && x <= 5 && y >= 1 && y <= 4);
        if (!inside) expect_u32("dst backing untouched", db[i], SENTINEL, i);
    }

    // Flipped draw into the same kind of view.
    fill_sentinel(db, 64);
    dsub = olivec_subcanvas(dparent, 2, 1, 4, 4);
    olivec_sprite_copy_bilinear(dsub, 3, 3, -4, -4, compact_src);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            expect_u32("subcanvas dst flipped",
                       db[(y + 1)*8 + (x + 2)],
                       ref[(3 - y)*4 + (3 - x)], y*4 + x);
        }
    }
}

int main(void)
{
    uint32_t rgba_buf[6];
    Olivec_Canvas rgba_src = make_rgba_source(rgba_buf);
    uint32_t red_buf[6];
    Olivec_Canvas red_src = make_red_source(red_buf);

    check_positive_compat(rgba_src);
    check_four_directions(rgba_src);
    check_clipping(red_src);
    check_empty_and_zero(rgba_src);
    check_single_pixel_axes();
    check_subcanvas(rgba_src);

    // Source stride padding must never be sampled or overwritten.
    expect_u32("rgba src pad 0", rgba_buf[2], SRC_PAD, 2);
    expect_u32("rgba src pad 1", rgba_buf[5], SRC_PAD, 5);
    expect_u32("red src pad 0", red_buf[2], SRC_PAD, 2);
    expect_u32("red src pad 1", red_buf[5], SRC_PAD, 5);

    if (failures != 0) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d bilinear flip/clipping/stride checks\n", checks);
    return 0;
}
