/*
 * tests/slice_concealment.c
 *
 * Whole-slice loss concealment tests. No main(): task_support/driver.c drives
 * the task_cases table below.
 *
 * Two kinds of inputs are built entirely in C:
 *  - the checked-in fixture elementary stream (task_support/fixture.h) with
 *    whole slices removed, plus a separate list of the removed slice rows
 *  - small hand-built progressive 4:2:0 MPEG-2 elementary streams for the
 *    long skip run, cross-row slice and non-zero motion/residual cases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mpeg2.h"
#include "check.h"
#include "fixture.h"

#define FW 64			/* fixture coded width */
#define FH 48			/* fixture coded height */
#define FCW 32
#define FCH 24
#define MBW 4			/* fixture macroblock grid: 4 columns */
#define MBH 3			/* and 3 rows, one slice per row */
#define NMBS (MBW * MBH)
#define NFRAMES 9

/* ----------------------------------------------------------------------- */
/* Small bitstream writer for the synthetic elementary streams.            */
/* ----------------------------------------------------------------------- */

typedef struct {
    uint8_t * d;
    size_t n, cap;
} bitbuf_t;

static void bb (bitbuf_t * b, uint32_t v, int n)
{
    int i;

    for (i = n - 1; i >= 0; i--) {
	if ((b->n & 7) == 0) {
	    if (b->n / 8 >= b->cap)
		return;
	    b->d[b->n / 8] = 0;
	}
	if ((v >> i) & 1)
	    b->d[b->n / 8] |= (uint8_t) (1u << (7 - (b->n & 7)));
	b->n++;
    }
}

static void balign (bitbuf_t * b)
{
    while (b->n & 7)
	bb (b, 0, 1);
}

static void bstart (bitbuf_t * b, uint8_t code)
{
    balign (b);
    b->d[b->n / 8] = 0;
    b->d[b->n / 8 + 1] = 0;
    b->d[b->n / 8 + 2] = 1;
    b->d[b->n / 8 + 3] = code;
    b->n += 32;
}

static void g_seq (bitbuf_t * b, int w, int h)
{
    bstart (b, 0xb3);
    bb (b, w, 12); bb (b, h, 12); bb (b, 1, 4); bb (b, 3, 4);
    bb (b, 0x3ffff, 18); bb (b, 1, 1); bb (b, 112, 10);
    bb (b, 0, 1); bb (b, 0, 1); bb (b, 0, 1);

    bstart (b, 0xb5);			/* sequence extension */
    bb (b, 1, 4); bb (b, 0x48, 8); bb (b, 1, 1); bb (b, 1, 2);
    bb (b, 0, 2); bb (b, 0, 2); bb (b, 0, 12); bb (b, 1, 1);
    bb (b, 0, 8); bb (b, 0, 1); bb (b, 0, 2); bb (b, 0, 5);

    bstart (b, 0xb8);			/* GOP, marker bits set */
    bb (b, 0x00080040u, 32);
}

static void g_pic (bitbuf_t * b, int tempref, int type)
{
    bstart (b, 0x00);
    bb (b, tempref, 10); bb (b, type, 3); bb (b, 0xffff, 16);

    bstart (b, 0xb5);			/* picture coding extension */
    bb (b, 8, 4);
    bb (b, 1, 4); bb (b, 1, 4); bb (b, 1, 4); bb (b, 1, 4);
    bb (b, 0, 2); bb (b, 3, 2); bb (b, 0, 1); bb (b, 1, 1);
    bb (b, 0, 1); bb (b, 0, 1); bb (b, 0, 1); bb (b, 0, 1);
    bb (b, 0, 1); bb (b, 1, 1); bb (b, 1, 1); bb (b, 0, 1);
}

static void g_slice (bitbuf_t * b, int id)
{
    bstart (b, (uint8_t) id);
    bb (b, 8, 5);			/* quantiser_scale_code */
    bb (b, 0, 1);			/* no extra_bit_slice */
}

static void g_eob (bitbuf_t * b)
{
    bb (b, 2, 2);			/* B14 end-of-block */
}

/* intra luma block, DC-only, given signed dc difference */
static void g_luma_dc (bitbuf_t * b, int diff)
{
    if (diff == 0) {
	bb (b, 0b100, 3);		/* size 0 */
    } else {
	int ad = diff < 0 ? -diff : diff;
	int size = 1;
	while ((1 << size) <= ad)
	    size++;
	bb (b, size == 1 ? 0b00 : 0b01, size == 1 ? 2 : 2); /* size 1 code */
	bb (b, diff > 0 ? 1u : 0u, 1);
    }
    g_eob (b);
}

static void g_chroma_dc0 (bitbuf_t * b)
{
    bb (b, 0b00, 2);			/* chroma size 0 */
    g_eob (b);
}

/* one intra macroblock; caller already emitted the address increment */
static void g_intra_mb (bitbuf_t * b, int first_dc)
{
    bb (b, 1, 1);			/* intra macroblock type */
    g_luma_dc (b, first_dc);
    g_luma_dc (b, 0);
    g_luma_dc (b, 0);
    g_luma_dc (b, 0);
    g_chroma_dc0 (b);
    g_chroma_dc0 (b);
}

/* ----------------------------------------------------------------------- */
/* Fixture stream start-code bookkeeping and whole-slice cutting.          */
/* ----------------------------------------------------------------------- */

typedef struct {
    size_t start;
    int code;
} start_code_t;

static start_code_t starts[64];
static int n_starts;
static int pic_of_start[64];		/* picture index (decode order) */

static void index_fixture (void)
{
    size_t i;
    int pic = -1;

    n_starts = 0;
    for (i = 0; i + 3 < sizeof (task_video); i++)
	if (task_video[i] == 0 && task_video[i + 1] == 0 &&
	    task_video[i + 2] == 1) {
	    starts[n_starts].start = i;
	    starts[n_starts].code = task_video[i + 3];
	    if (task_video[i + 3] == 0x00)
		pic++;
	    pic_of_start[n_starts] = pic;
	    n_starts++;
	}
}

/*
 * cut[pic][slice_row]: slice_row 0..2 corresponds to slice start codes
 * 0x01..0x03. A separate, explicit removal list kept alongside the stream.
 */
static size_t build_cut_stream (uint8_t * out, const int cut[NFRAMES][MBH])
{
    size_t n = 0;
    int s;

    if (n_starts == 0)
	index_fixture ();
    for (s = 0; s < n_starts; s++) {
	size_t end = (s + 1 < n_starts) ? starts[s + 1].start :
		     sizeof (task_video);
	int code = starts[s].code;
	int pic = pic_of_start[s];
	int drop = 0;

	if (code >= 1 && code <= 0xaf && pic >= 0 && pic < NFRAMES)
	    drop = cut[pic][code - 1];
	if (!drop) {
	    memcpy (out + n, task_video + starts[s].start,
		    end - starts[s].start);
	    n += end - starts[s].start;
	}
    }
    return n;
}

/* picture type and temporal reference, indexed by decode-order picture */
static const int pic_type[NFRAMES] = { 1, 2, 3, 3, 2, 3, 3, 2, 3 };
static const int pic_temporal[NFRAMES] = { 0, 3, 1, 2, 6, 4, 5, 8, 7 };

/* ----------------------------------------------------------------------- */
/* Decode driver and captured display records.                             */
/* ----------------------------------------------------------------------- */

typedef struct {
    uint8_t y[FH * FW];
    uint8_t u[FCH * FCW];
    uint8_t v[FCH * FCW];
    uint8_t map[NMBS];
    unsigned int concealed;
    int type;
    unsigned int temporal;
    uint32_t tag;
    int have;
} frame_rec_t;

typedef struct {
    uint8_t y[FH * FW];
    uint8_t u[FCH * FCW];
    uint8_t v[FCH * FCW];
} plane_img_t;

/* Feed one already-built byte stream, optionally tagging each picture. */
static int decode_stream (mpeg2dec_t * d, const uint8_t * buf, size_t len,
			  frame_rec_t * recs, int max_recs, int use_tags,
			  const uint32_t * tag_by_decode_pic)
{
    const mpeg2_info_t * info = mpeg2_info (d);
    size_t off = 0;
    int next_pic = 0;
    int ndisp = 0;
    int steps;

    for (steps = 0; steps < 1000000; steps++) {
	mpeg2_state_t st = mpeg2_parse (d);

	if (st == STATE_BUFFER) {
	    if (off >= len)
		break;
	    {
		size_t k = 37;
		if (k > len - off)
		    k = len - off;
		mpeg2_buffer (d, (uint8_t *) buf + off,
			      (uint8_t *) buf + off + k);
		off += k;
	    }
	} else if (st == STATE_PICTURE && use_tags &&
		   next_pic < NFRAMES) {
	    mpeg2_tag_picture (d, tag_by_decode_pic[next_pic],
			       tag_by_decode_pic[next_pic]);
	    next_pic++;
	} else if (st == STATE_INVALID || st == STATE_INVALID_END) {
	    return -1;
	} else if ((st == STATE_SLICE || st == STATE_END) &&
		   info->display_fbuf) {
	    int x, yy;

	    if (ndisp >= max_recs)
		return -2;
	    for (yy = 0; yy < FH; yy++)
		for (x = 0; x < FW; x++)
		    recs[ndisp].y[yy * FW + x] =
			info->display_fbuf->buf[0][yy * FW + x];
	    for (yy = 0; yy < FCH; yy++)
		for (x = 0; x < FCW; x++) {
		    recs[ndisp].u[yy * FCW + x] =
			info->display_fbuf->buf[1][yy * FCW + x];
		    recs[ndisp].v[yy * FCW + x] =
			info->display_fbuf->buf[2][yy * FCW + x];
		}
	    recs[ndisp].concealed = info->display_concealed_mbs;
	    recs[ndisp].type =
		info->display_picture->flags & PIC_MASK_CODING_TYPE;
	    recs[ndisp].temporal =
		info->display_picture->temporal_reference;
	    recs[ndisp].tag =
		(info->display_picture->flags & PIC_FLAG_TAGS) ?
		info->display_picture->tag : 0;
	    if (info->display_concealment)
		memcpy (recs[ndisp].map, info->display_concealment, NMBS);
	    else
		memset (recs[ndisp].map, 0, NMBS);
	    recs[ndisp].have = 1;
	    ndisp++;
	}
    }
    return ndisp;
}

/*
 * Snapshot the cut stream's own completed pictures in decode order from
 * current_fbuf. Because concealment runs before this snapshot, every picture
 * is already complete. It is the ground truth for rebuilt macroblocks and the
 * actual (post-concealment) reference content.
 */
static int decode_cut_refs (const int cut[NFRAMES][MBH],
			    plane_img_t out[NFRAMES])
{
    static uint8_t stream[sizeof (task_video) + 16];
    mpeg2dec_t * d;
    const mpeg2_info_t * info;
    size_t off = 0, len;
    int npic = 0, x, yy, steps;

    len = build_cut_stream (stream, cut);
    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    info = mpeg2_info (d);
    for (steps = 0; steps < 1000000; steps++) {
	mpeg2_state_t st = mpeg2_parse (d);

	if (st == STATE_BUFFER) {
	    size_t k = 37;
	    if (off >= len)
		break;
	    if (k > len - off)
		k = len - off;
	    mpeg2_buffer (d, stream + off, stream + off + k);
	    off += k;
	} else if (st == STATE_INVALID || st == STATE_INVALID_END) {
	    TASK_CHECK (0);
	} else if (st == STATE_SLICE && info->current_fbuf &&
		   npic < NFRAMES) {
	    for (yy = 0; yy < FH; yy++)
		for (x = 0; x < FW; x++)
		    out[npic].y[yy * FW + x] =
			info->current_fbuf->buf[0][yy * FW + x];
	    for (yy = 0; yy < FCH; yy++)
		for (x = 0; x < FCW; x++) {
		    out[npic].u[yy * FCW + x] =
			info->current_fbuf->buf[1][yy * FCW + x];
		    out[npic].v[yy * FCW + x] =
			info->current_fbuf->buf[2][yy * FCW + x];
		}
	    npic++;
	}
    }
    mpeg2_close (d);
    return npic;
}

static int decode_cut (int enabled, const int cut[NFRAMES][MBH],
		       frame_rec_t * recs, int use_tags,
		       const uint32_t * tags)
{
    static uint8_t stream[sizeof (task_video) + 16];
    mpeg2dec_t * d;
    size_t len;
    int n;

    len = build_cut_stream (stream, cut);
    mpeg2_accel (0);
    d = mpeg2_init ();
    if (enabled)
	TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    n = decode_stream (d, stream, len, recs, NFRAMES + 2, use_tags, tags);
    mpeg2_close (d);
    return n;
}

static void no_cut (int cut[NFRAMES][MBH])
{
    memset (cut, 0, NFRAMES * MBH * sizeof (int));
}

static int frame_mb_row (int mb)
{
    return mb / MBW;
}

/* ----------------------------------------------------------------------- */
/* coverage group                                                           */
/* ----------------------------------------------------------------------- */

static int case_first_row (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t rec[NFRAMES + 2];
    int mb, n;

    no_cut (cut);
    cut[0][0] = 1;			/* remove the first slice of the I */
    n = decode_cut (1, cut, rec, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    TASK_CHECK (rec[0].concealed == MBW);
    for (mb = 0; mb < NMBS; mb++)
	TASK_CHECK (rec[0].map[mb] == (frame_mb_row (mb) == 0));
    return 0;
}

static int case_mid_row (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t rec[NFRAMES + 2];
    int mb, n;

    no_cut (cut);
    cut[0][1] = 1;
    n = decode_cut (1, cut, rec, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    TASK_CHECK (rec[0].concealed == MBW);
    for (mb = 0; mb < NMBS; mb++)
	TASK_CHECK (rec[0].map[mb] == (frame_mb_row (mb) == 1));
    return 0;
}

static int case_last_row (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t rec[NFRAMES + 2];
    int mb, n;

    no_cut (cut);
    cut[0][2] = 1;
    n = decode_cut (1, cut, rec, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    TASK_CHECK (rec[0].concealed == MBW);
    for (mb = 0; mb < NMBS; mb++)
	TASK_CHECK (rec[0].map[mb] == (frame_mb_row (mb) == 2));
    return 0;
}

static int case_multi_nonadjacent (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t rec[NFRAMES + 2];
    int n;

    no_cut (cut);
    /* separate, non-adjacent gaps in different pictures (decode order) */
    cut[0][0] = 1;
    cut[2][2] = 1;
    cut[4][1] = 1;
    cut[7][0] = 1; cut[7][2] = 1;	/* two non-adjacent rows same pic */
    n = decode_cut (1, cut, rec, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    /* records are in display order, indexed by temporal reference */
    TASK_CHECK (rec[pic_temporal[0]].concealed == MBW);
    TASK_CHECK (rec[pic_temporal[2]].concealed == MBW);
    TASK_CHECK (rec[pic_temporal[4]].concealed == MBW);
    TASK_CHECK (rec[pic_temporal[7]].concealed == 2 * MBW);
    /* a picture with no loss reports nothing */
    TASK_CHECK (rec[pic_temporal[1]].concealed == 0);
    return 0;
}

static int case_no_loss_all_zero (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t rec[NFRAMES + 2];
    int f, mb, n;

    no_cut (cut);
    n = decode_cut (1, cut, rec, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    for (f = 0; f < NFRAMES; f++) {
	TASK_CHECK (rec[f].concealed == 0);
	for (mb = 0; mb < NMBS; mb++)
	    TASK_CHECK (rec[f].map[mb] == 0);
    }
    return 0;
}

/* synthetic 32x32 stream: a long MBA skip run inside one cross-row slice */
static int case_long_skip_run (void)
{
    static uint8_t mem[1 << 16];
    bitbuf_t b;
    mpeg2dec_t * d;
    const mpeg2_info_t * info;
    size_t off, len;
    int mb;

    b.d = mem; b.n = 0; b.cap = sizeof (mem);
    g_seq (&b, 32, 32);
    /* I picture: single slice spanning both macroblock rows */
    g_pic (&b, 0, 1);
    g_slice (&b, 1);
    for (mb = 0; mb < 4; mb++) {
	bb (&b, 1, 1);		/* mba increment, including the row crossing */
	g_intra_mb (&b, 0);
    }
    /* P picture: explicit zero-MV MC macroblock then a long skip run */
    g_pic (&b, 1, 2);
    g_slice (&b, 1);
    bb (&b, 0b100, 3);		/* MC-only forward */
    bb (&b, 1, 1); bb (&b, 1, 1);	/* zero motion x and y */
    for (mb = 1; mb < 4; mb++)
	bb (&b, 1, 1);		/* skipped macroblocks are still rebuilt */
    bstart (&b, 0xb7);
    len = b.n / 8;

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    info = mpeg2_info (d);
    off = 0;
    for (;;) {
	mpeg2_state_t st = mpeg2_parse (d);
	if (st == STATE_BUFFER) {
	    if (off >= len)
		break;
	    mpeg2_buffer (d, mem + off, mem + len);
	    off = len;
	} else if (st == STATE_INVALID || st == STATE_INVALID_END) {
	    TASK_CHECK (0);
	    break;
	} else if ((st == STATE_SLICE || st == STATE_END) &&
		   info->display_fbuf) {
	    TASK_CHECK (info->display_concealed_mbs == 0);
	    for (mb = 0; mb < 4; mb++)
		TASK_CHECK (info->display_concealment[mb] == 0);
	}
    }
    mpeg2_close (d);
    return 0;
}

/* synthetic cross-row I slice with one row removed: only that row concealed */
static int case_cross_row_slice (void)
{
    static uint8_t mem[1 << 16];
    bitbuf_t b;
    mpeg2dec_t * d;
    const mpeg2_info_t * info;
    size_t off, len;
    int done = 0, mb;

    b.d = mem; b.n = 0; b.cap = sizeof (mem);
    g_seq (&b, 32, 32);
    g_pic (&b, 0, 1);
    /* only slice 1 (first macroblock row) is present; row 1 wholly missing */
    g_slice (&b, 1);
    bb (&b, 1, 1); g_intra_mb (&b, 1);
    bb (&b, 1, 1); g_intra_mb (&b, 0);
    bstart (&b, 0xb7);
    len = b.n / 8;

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    info = mpeg2_info (d);
    off = 0;
    for (;;) {
	mpeg2_state_t st = mpeg2_parse (d);
	if (st == STATE_BUFFER) {
	    if (off >= len)
		break;
	    mpeg2_buffer (d, mem + off, mem + len);
	    off = len;
	} else if (st == STATE_INVALID || st == STATE_INVALID_END) {
	    TASK_CHECK (0);
	    break;
	} else if ((st == STATE_SLICE || st == STATE_END) &&
		   info->display_fbuf) {
	    TASK_CHECK (info->display_concealed_mbs == 2);
	    for (mb = 0; mb < 4; mb++)
		TASK_CHECK (info->display_concealment[mb] == (mb >= 2));
	    done = 1;
	}
    }
    TASK_CHECK (done);
    mpeg2_close (d);
    return 0;
}

/* synthetic stream with non-zero motion and a coded residual: parses, and
 * normal rebuilt macroblocks are never reported as gaps */
static int case_motion_residual (void)
{
    static uint8_t mem[1 << 16];
    bitbuf_t b;
    mpeg2dec_t * d;
    const mpeg2_info_t * info;
    size_t off, len;
    int mb;

    b.d = mem; b.n = 0; b.cap = sizeof (mem);
    g_seq (&b, 32, 32);
    g_pic (&b, 0, 1);
    g_slice (&b, 1);
    for (mb = 0; mb < 4; mb++) {
	bb (&b, 1, 1);
	g_intra_mb (&b, mb == 0);
    }
    g_pic (&b, 1, 2);
    g_slice (&b, 1);
    bb (&b, 0b100, 3);			/* forward MC */
    bb (&b, 0b00010, 5);		/* non-zero mv x */
    bb (&b, 0b00010, 5);		/* non-zero mv y */
    bb (&b, 0b11, 2);			/* cbp: all six blocks coded */
    /* six DC-only residual blocks then EOB */
    for (mb = 0; mb < 6; mb++) {
	bb (&b, 0b111, 3);
	g_eob (&b);
    }
    for (mb = 1; mb < 4; mb++)
	bb (&b, 1, 1);
    bstart (&b, 0xb7);
    len = b.n / 8;

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    info = mpeg2_info (d);
    off = 0;
    for (;;) {
	mpeg2_state_t st = mpeg2_parse (d);
	if (st == STATE_BUFFER) {
	    if (off >= len)
		break;
	    mpeg2_buffer (d, mem + off, mem + len);
	    off = len;
	} else if (st == STATE_INVALID || st == STATE_INVALID_END) {
	    TASK_CHECK (0);
	    break;
	} else if ((st == STATE_SLICE || st == STATE_END) &&
		   info->display_fbuf &&
		   (info->display_picture->flags & PIC_MASK_CODING_TYPE) == 2) {
	    TASK_CHECK (info->display_concealed_mbs == 0);
	}
    }
    mpeg2_close (d);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* pixels group                                                             */
/* ----------------------------------------------------------------------- */

/* decode-order reference picture indices (-1 = none) for the fixture */
static const int fwd_ref[NFRAMES] = { -1, 0, 0, 0, 1, 1, 1, 4, 4 };
static const int bwd_ref[NFRAMES] = { -1, -1, 1, 1, -1, 4, 4, -1, 7 };

static void mb_xy (int mb, int * mx, int * my)
{
    *mx = mb % MBW;
    *my = mb / MBW;
}

static void fill_neutral_img (plane_img_t * im, int mb)
{
    int mx, my, x, y;

    mb_xy (mb, &mx, &my);
    for (y = 0; y < 16; y++)
	for (x = 0; x < 16; x++)
	    im->y[(my * 16 + y) * FW + mx * 16 + x] = 16;
    for (y = 0; y < 8; y++)
	for (x = 0; x < 8; x++) {
	    im->u[(my * 8 + y) * FCW + mx * 8 + x] = 128;
	    im->v[(my * 8 + y) * FCW + mx * 8 + x] = 128;
	}
}

static void copy_mb_img (plane_img_t * dst, const plane_img_t * src, int mb)
{
    int mx, my, y;

    mb_xy (mb, &mx, &my);
    for (y = 0; y < 16; y++)
	memcpy (dst->y + (my * 16 + y) * FW + mx * 16,
		src->y + (my * 16 + y) * FW + mx * 16, 16);
    for (y = 0; y < 8; y++) {
	memcpy (dst->u + (my * 8 + y) * FCW + mx * 8,
		src->u + (my * 8 + y) * FCW + mx * 8, 8);
	memcpy (dst->v + (my * 8 + y) * FCW + mx * 8,
		src->v + (my * 8 + y) * FCW + mx * 8, 8);
    }
}

static void avg_mb_img (plane_img_t * dst, const plane_img_t * a,
			const plane_img_t * b, int mb)
{
    int mx, my, x, y;

    mb_xy (mb, &mx, &my);
    for (y = 0; y < 16; y++)
	for (x = 0; x < 16; x++) {
	    int i = (my * 16 + y) * FW + mx * 16 + x;
	    dst->y[i] = (a->y[i] + b->y[i] + 1) >> 1;
	}
    for (y = 0; y < 8; y++)
	for (x = 0; x < 8; x++) {
	    int i = (my * 8 + y) * FCW + mx * 8 + x;
	    dst->u[i] = (a->u[i] + b->u[i] + 1) >> 1;
	    dst->v[i] = (a->v[i] + b->v[i] + 1) >> 1;
	}
}

/*
 * Build the expected contents in decode order. "actual" is the cut stream's
 * own completed pictures, so every reference it points at is already fully
 * rebuilt and concealed. Rebuilt macroblocks are taken verbatim from that
 * picture; concealed macroblocks are independently re-derived from the
 * explicit rules against the already-validated reference pictures:
 *   I / no reference -> neutral; P -> forward copy;
 *   B with both refs -> per-byte average, otherwise the single available ref.
 * This is the recursive "verified concealment result feeds the next picture"
 * model, without guessing any motion vector.
 */
static void build_expected (const int cut[NFRAMES][MBH],
			    const plane_img_t actual[NFRAMES],
			    plane_img_t out[NFRAMES],
			    int concealed[NFRAMES][NMBS])
{
    int p, mb;

    for (p = 0; p < NFRAMES; p++) {
	int type = pic_type[p];

	for (mb = 0; mb < NMBS; mb++) {
	    int row = frame_mb_row (mb);
	    int is_gap = cut[p][row];

	    concealed[p][mb] = is_gap;
	    if (!is_gap) {
		/* present slice: equal to the cut stream's real content */
		copy_mb_img (&out[p], &actual[p], mb);
	    } else if (type == 1 ||
		       (fwd_ref[p] < 0 && bwd_ref[p] < 0)) {
		fill_neutral_img (&out[p], mb);
	    } else if (type == 2) {
		copy_mb_img (&out[p], &actual[fwd_ref[p]], mb);
	    } else {
		int f = fwd_ref[p], b = bwd_ref[p];

		if (f >= 0 && b >= 0)
		    avg_mb_img (&out[p], &actual[f], &actual[b], mb);
		else if (f >= 0)
		    copy_mb_img (&out[p], &actual[f], mb);
		else
		    copy_mb_img (&out[p], &actual[b], mb);
	    }
	}
    }
}

static int compare_concealed_mbs (const frame_rec_t * got,
				  const plane_img_t * want,
				  const int concealed[NMBS])
{
    int mb, x, y, mx, my;

    for (mb = 0; mb < NMBS; mb++) {
	if (!concealed[mb])
	    continue;
	mb_xy (mb, &mx, &my);
	for (y = 0; y < 16; y++)
	    for (x = 0; x < 16; x++) {
		int i = (my * 16 + y) * FW + mx * 16 + x;
		TASK_CHECK (got->y[i] == want->y[i]);
	    }
	for (y = 0; y < 8; y++)
	    for (x = 0; x < 8; x++) {
		int i = (my * 8 + y) * FCW + mx * 8 + x;
		TASK_CHECK (got->u[i] == want->u[i]);
		TASK_CHECK (got->v[i] == want->v[i]);
	    }
    }
    return 0;
}

static int px_i_neutral (void)
{
    int cut[NFRAMES][MBH];
    plane_img_t clean[NFRAMES], expect[NFRAMES];
    frame_rec_t got[NFRAMES + 2];
    int concealed[NFRAMES][NMBS];
    int mb, mx, my, x, y, n;

    static const int rows[3] = { 0, 1, 2 };
    int r;

    no_cut (cut);
    TASK_CHECK (decode_cut_refs (cut, clean) == NFRAMES);
    for (r = 0; r < 3; r++) {
	no_cut (cut);
	cut[0][rows[r]] = 1;
	n = decode_cut (1, cut, got, 0, NULL);
	TASK_CHECK (n == NFRAMES);
	memset (expect, 0, sizeof (expect));
	memset (concealed, 0, sizeof (concealed));
	build_expected (cut, clean, expect, concealed);
	/* display frame 0 is the I picture */
	TASK_CHECK (got[0].type == 1);
	compare_concealed_mbs (&got[0], &expect[0], concealed[0]);
	/* the rebuilt macroblocks of the I picture match the clean decode */
	for (mb = 0; mb < NMBS; mb++) {
	    if (concealed[0][mb])
		continue;
	    mb_xy (mb, &mx, &my);
	    for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++) {
		    int i = (my * 16 + y) * FW + mx * 16 + x;
		    TASK_CHECK (got[0].y[i] == clean[0].y[i]);
		}
	}
    }
    return 0;
}

static int px_p_forward (void)
{
    int cut[NFRAMES][MBH];
    plane_img_t clean[NFRAMES], expect[NFRAMES];
    frame_rec_t got[NFRAMES + 2];
    int concealed[NFRAMES][NMBS];

    no_cut (cut);
    cut[1][1] = 1;			/* middle row of the first P picture */
    TASK_CHECK (decode_cut_refs (cut, clean) == NFRAMES);
    TASK_CHECK (decode_cut (1, cut, got, 0, NULL) == NFRAMES);
    memset (expect, 0, sizeof (expect));
    memset (concealed, 0, sizeof (concealed));
    build_expected (cut, clean, expect, concealed);
    /* that P is displayed after the I; locate it by temporal reference 3 */
    {
	int f;
	for (f = 0; f < NFRAMES; f++)
	    if (got[f].temporal == 3) {
		TASK_CHECK (got[f].type == 2);
		compare_concealed_mbs (&got[f], &expect[1], concealed[1]);
		return 0;
	    }
    }
    TASK_CHECK (0);
    return 0;
}

static int px_b_average (void)
{
    int cut[NFRAMES][MBH];
    plane_img_t clean[NFRAMES], expect[NFRAMES];
    frame_rec_t got[NFRAMES + 2];
    int concealed[NFRAMES][NMBS];

    no_cut (cut);
    cut[2][2] = 1;			/* last row of the first B picture */
    TASK_CHECK (decode_cut_refs (cut, clean) == NFRAMES);
    TASK_CHECK (decode_cut (1, cut, got, 0, NULL) == NFRAMES);
    memset (expect, 0, sizeof (expect));
    memset (concealed, 0, sizeof (concealed));
    build_expected (cut, clean, expect, concealed);
    {
	int f;
	for (f = 0; f < NFRAMES; f++)
	    if (got[f].temporal == 1) {	/* first B, temporal ref 1 */
		TASK_CHECK (got[f].type == 3);
		compare_concealed_mbs (&got[f], &expect[2], concealed[2]);
		return 0;
	    }
    }
    TASK_CHECK (0);
    return 0;
}

/*
 * Synthetic reference test. A complete I picture is followed by a complete P
 * picture whose first macroblock explicitly zero-MV copies the forward
 * reference and whose remaining macroblocks are normal MBA skip copies. The
 * whole P picture is rebuilt (no concealed MB) and its first macroblock is
 * pixel-identical to the completed I reference it reuses. The stronger
 * "a concealed reference is finished before another picture uses it" chain is
 * covered by px_p_forward_copy and px_recursion on the byte-exact fixture
 * stream.
 */
static int px_reference_completed_before_use (void)
{
    static uint8_t mem[1 << 16];
    bitbuf_t b;
    mpeg2dec_t * d;
    const mpeg2_info_t * info;
    size_t off, len;
    int saw_i = 0, saw_p = 0, mb;
    uint8_t i_mb0[16 * 16];

    b.d = mem; b.n = 0; b.cap = sizeof (mem);
    g_seq (&b, 32, 32);
    /* complete I picture: one cross-row slice, four macroblocks */
    g_pic (&b, 0, 1);
    g_slice (&b, 1);
    for (mb = 0; mb < 4; mb++) {
	bb (&b, 1, 1);
	g_intra_mb (&b, mb == 0);
    }
    /* complete P picture: explicit zero-MV forward MB then MBA skip MBs */
    g_pic (&b, 1, 2);
    g_slice (&b, 1);
    bb (&b, 0b100, 3);			/* first MB: MC-only forward */
    bb (&b, 1, 1); bb (&b, 1, 1);	/* zero motion x and y */
    for (mb = 1; mb < 4; mb++)
	bb (&b, 1, 1);			/* skipped MBs are rebuilt too */
    bstart (&b, 0xb7);
    len = b.n / 8;

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    info = mpeg2_info (d);
    off = 0;
    for (;;) {
	mpeg2_state_t st = mpeg2_parse (d);
	if (st == STATE_BUFFER) {
	    if (off >= len)
		break;
	    mpeg2_buffer (d, mem + off, mem + len);
	    off = len;
	} else if (st == STATE_INVALID || st == STATE_INVALID_END) {
	    TASK_CHECK (0);
	    break;
	} else if ((st == STATE_SLICE || st == STATE_END) &&
		   info->display_fbuf) {
	    int type = info->display_picture->flags & PIC_MASK_CODING_TYPE;
	    if (type == 1) {
		int i;
		saw_i = 1;
		TASK_CHECK (info->display_concealed_mbs == 0);
		for (i = 0; i < 16 * 16; i++)
		    i_mb0[i] = info->display_fbuf->buf[0][i];
	    } else if (type == 2) {
		int i;
		saw_p = 1;
		/* the whole P picture was rebuilt, none concealed */
		TASK_CHECK (info->display_concealed_mbs == 0);
		TASK_CHECK (info->display_concealment[0] == 0);
		/* its first MB zero-MV copied the completed I reference */
		for (i = 0; i < 16 * 16; i++)
		    TASK_CHECK (info->display_fbuf->buf[0][i] == i_mb0[i]);
	    }
	}
    }
    TASK_CHECK (saw_i && saw_p);
    mpeg2_close (d);
    return 0;
}

/* multi-gap recursion across I, P and B in the fixture stream */
static int px_recursion (void)
{
    int cut[NFRAMES][MBH];
    plane_img_t clean[NFRAMES], expect[NFRAMES];
    frame_rec_t got[NFRAMES + 2];
    int concealed[NFRAMES][NMBS];
    int f, p;

    no_cut (cut);
    cut[0][0] = 1;			/* I first row */
    cut[1][2] = 1;			/* P last row */
    cut[2][1] = 1;			/* B middle row */
    cut[4][0] = 1;			/* later P first row */
    TASK_CHECK (decode_cut_refs (cut, clean) == NFRAMES);
    TASK_CHECK (decode_cut (1, cut, got, 0, NULL) == NFRAMES);
    memset (expect, 0, sizeof (expect));
    memset (concealed, 0, sizeof (concealed));
    build_expected (cut, clean, expect, concealed);

    for (p = 0; p < NFRAMES; p++) {
	int any = 0;
	for (int mb = 0; mb < NMBS; mb++)
	    any |= concealed[p][mb];
	if (!any)
	    continue;
	for (f = 0; f < NFRAMES; f++)
	    if (got[f].temporal == (unsigned) pic_temporal[p]) {
		TASK_CHECK (got[f].concealed != 0);
		compare_concealed_mbs (&got[f], &expect[p], concealed[p]);
		break;
	    }
    }
    return 0;
}

/* ----------------------------------------------------------------------- */
/* reorder group                                                            */
/* ----------------------------------------------------------------------- */

/* B pictures are displayed immediately while I/P are delayed: the map must
 * follow display identity, not decode order. Temporal references are unique
 * in the fixture and match display order 0..8. */
static int ro_b_reorder_map (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t got[NFRAMES + 2];
    int f, n;

    no_cut (cut);
    cut[2][2] = 1;			/* gap in the first B (decode pic 2) */
    cut[3][0] = 1;			/* gap in the second B (decode pic 3) */
    n = decode_cut (1, cut, got, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    for (f = 0; f < NFRAMES; f++)
	TASK_CHECK ((int) got[f].temporal == f);
    /* temporal 1 and 2 are the two B pictures; only their rows are marked */
    for (f = 0; f < NFRAMES; f++) {
	int mb;
	if (f == 1) {
	    TASK_CHECK (got[f].type == 3);
	    TASK_CHECK (got[f].concealed == MBW);
	    for (mb = 0; mb < NMBS; mb++)
		TASK_CHECK (got[f].map[mb] == (frame_mb_row (mb) == 2));
	} else if (f == 2) {
	    TASK_CHECK (got[f].type == 3);
	    TASK_CHECK (got[f].concealed == MBW);
	    for (mb = 0; mb < NMBS; mb++)
		TASK_CHECK (got[f].map[mb] == (frame_mb_row (mb) == 0));
	} else {
	    TASK_CHECK (got[f].concealed == 0);
	}
    }
    return 0;
}

/*
 * Tags follow each displayed picture through the reordering. The library
 * binds a tag supplied at a picture header to the following picture (its
 * documented one-picture pipeline), so the first decoded picture carries no
 * tag and every later decode-order picture p carries tag p-1. The point under
 * test is that the tag travels with the same picture identity as its map,
 * across the I/P/B display reordering.
 */
static int ro_tags (void)
{
    static uint32_t tags[NFRAMES];
    int cut[NFRAMES][MBH];
    frame_rec_t got[NFRAMES + 2];
    int p, f, n;

    for (p = 0; p < NFRAMES; p++)
	tags[p] = 1000u + p;		/* tag keyed by decode-order picture */
    no_cut (cut);
    cut[1][1] = 1;
    n = decode_cut (1, cut, got, 1, tags);
    TASK_CHECK (n == NFRAMES);
    for (f = 0; f < NFRAMES; f++) {
	int decode_pic = -1;
	for (p = 0; p < NFRAMES; p++)
	    if (pic_temporal[p] == (int) got[f].temporal)
		decode_pic = p;
	TASK_CHECK (decode_pic >= 0);
	if (decode_pic == 0)
	    TASK_CHECK (got[f].tag == 0);	/* pipeline-filling picture */
	else
	    TASK_CHECK (got[f].tag == tags[decode_pic - 1]);
    }
    return 0;
}

/* the final reference pictures are drained at sequence end with their maps */
static int ro_end_drain (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t got[NFRAMES + 2];
    int n;

    no_cut (cut);
    cut[7][1] = 1;			/* last P picture (decode pic 7) */
    n = decode_cut (1, cut, got, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    /* last displayed frame is temporal 8, the drained final P */
    TASK_CHECK (got[NFRAMES - 1].temporal == 8);
    TASK_CHECK (got[NFRAMES - 1].type == 2);
    TASK_CHECK (got[NFRAMES - 1].concealed == MBW);
    return 0;
}

/* repeated slot rotation must never mix two pictures' maps */
static int ro_slot_reuse (void)
{
    int cut[NFRAMES][MBH];
    frame_rec_t got[NFRAMES + 2];
    int f, n;

    no_cut (cut);
    /* gaps spread so every frame buffer slot is reused while marked */
    cut[0][0] = 1; cut[1][0] = 1; cut[2][0] = 1; cut[4][2] = 1;
    cut[5][2] = 1; cut[7][1] = 1;
    n = decode_cut (1, cut, got, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    for (f = 0; f < NFRAMES; f++) {
	unsigned int counted = 0;
	int mb;
	for (mb = 0; mb < NMBS; mb++) {
	    TASK_CHECK (got[f].map[mb] == 0 || got[f].map[mb] == 1);
	    counted += got[f].map[mb];
	}
	TASK_CHECK (counted == got[f].concealed);
    }
    return 0;
}

/* ----------------------------------------------------------------------- */
/* lifecycle group                                                          */
/* ----------------------------------------------------------------------- */

/* minimal always-success mpeg2_convert_t used only to test mutual exclusion */
static int lc_stub_convert (int stage, void * id,
			    const mpeg2_sequence_t * sequence, int stride,
			    uint32_t accel, void * arg,
			    mpeg2_convert_init_t * result)
{
    (void) stage; (void) id; (void) sequence; (void) stride;
    (void) accel; (void) arg;
    if (result) {
	result->id_size = 0;
	result->buf_size[0] = result->buf_size[1] = result->buf_size[2] = 0;
	result->start = NULL;
	result->copy = NULL;
    }
    return 0;
}

static int lc_api_validation (void)
{
    mpeg2dec_t * d;
    static uint8_t dummy[8];

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 2) == -1);
    TASK_CHECK (mpeg2_slice_concealment (d, -1) == -1);
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);	/* still open */
    mpeg2_buffer (d, dummy, dummy + sizeof (dummy));
    TASK_CHECK (mpeg2_slice_concealment (d, 0) == -1);	/* window closed */
    mpeg2_close (d);
    return 0;
}

static int lc_convert_exclusion (void)
{
    mpeg2dec_t * d;

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    TASK_CHECK (mpeg2_convert (d, lc_stub_convert, NULL) == -1);
    mpeg2_close (d);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_convert (d, lc_stub_convert, NULL) == 0);
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == -1);
    mpeg2_close (d);
    return 0;
}

static int lc_reset_keeps_switch_clears_state (void)
{
    static uint8_t stream[sizeof (task_video) + 16];
    int cut[NFRAMES][MBH];
    frame_rec_t got[NFRAMES + 2];
    mpeg2dec_t * d;
    int n;

    no_cut (cut);
    cut[0][1] = 1;
    memcpy (stream, task_video, sizeof (task_video));

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    n = decode_stream (d, stream, build_cut_stream (stream, cut),
		       got, NFRAMES + 2, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    TASK_CHECK (got[0].concealed == MBW);

    mpeg2_reset (d, 0);			/* switch retained, state cleared */
    TASK_CHECK (mpeg2_slice_concealment (d, 7) == -1);	/* bad value */
    /* a brand new stream decodes and produces a fresh, correct map */
    n = decode_stream (d, stream, build_cut_stream (stream, cut),
		       got, NFRAMES + 2, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    TASK_CHECK (got[0].concealed == MBW);

    mpeg2_reset (d, 1);			/* full reset */
    n = decode_stream (d, stream, build_cut_stream (stream, cut),
		       got, NFRAMES + 2, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    TASK_CHECK (got[0].concealed == MBW);
    mpeg2_close (d);
    return 0;
}

static int lc_two_instances (void)
{
    static uint8_t stream[sizeof (task_video) + 16];
    int cut[NFRAMES][MBH];
    frame_rec_t a[NFRAMES + 2], b[NFRAMES + 2];
    mpeg2dec_t * da, * db;
    size_t len;
    int na, nb;

    no_cut (cut);
    cut[0][2] = 1;
    len = build_cut_stream (stream, cut);

    mpeg2_accel (0);
    da = mpeg2_init ();
    db = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (da, 1) == 0);
    /* db deliberately left in default (disabled) mode */
    na = decode_stream (da, stream, len, a, NFRAMES + 2, 0, NULL);
    nb = decode_stream (db, stream, len, b, NFRAMES + 2, 0, NULL);
    TASK_CHECK (na == NFRAMES && nb == NFRAMES);
    TASK_CHECK (a[0].concealed == MBW);
    TASK_CHECK (b[0].concealed == 0);
    TASK_CHECK (b[0].map[0] == 0);
    mpeg2_close (da);
    mpeg2_close (db);
    return 0;
}

/* with no lost slice, enabled and disabled modes decode byte-identically */
static int lc_enabled_matches_disabled (void)
{
    frame_rec_t on[NFRAMES + 2], off[NFRAMES + 2];
    mpeg2dec_t * da, * db;
    int non, noff, f, i;

    mpeg2_accel (0);
    da = mpeg2_init ();
    db = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (da, 1) == 0);
    non = decode_stream (da, task_video, sizeof (task_video), on,
			 NFRAMES + 2, 0, NULL);
    noff = decode_stream (db, task_video, sizeof (task_video), off,
			  NFRAMES + 2, 0, NULL);
    TASK_CHECK (non == NFRAMES && noff == NFRAMES);
    for (f = 0; f < NFRAMES; f++) {
	TASK_CHECK (on[f].concealed == 0 && off[f].concealed == 0);
	for (i = 0; i < FH * FW; i++)
	    TASK_CHECK (on[f].y[i] == off[f].y[i]);
	for (i = 0; i < FCH * FCW; i++) {
	    TASK_CHECK (on[f].u[i] == off[f].u[i]);
	    TASK_CHECK (on[f].v[i] == off[f].v[i]);
	}
    }
    mpeg2_close (da);
    mpeg2_close (db);
    return 0;
}

static int lc_disabled_nine_frames (void)
{
    frame_rec_t got[NFRAMES + 2];
    mpeg2dec_t * d;
    int n, f, types[5] = { 0, 0, 0, 0, 0 };    mpeg2_accel (0);
    d = mpeg2_init ();			/* concealment stays off */
    n = decode_stream (d, task_video, sizeof (task_video), got,
		       NFRAMES + 2, 0, NULL);
    TASK_CHECK (n == NFRAMES);
    for (f = 0; f < NFRAMES; f++) {
	TASK_CHECK (got[f].concealed == 0);
	TASK_CHECK (got[f].type >= 1 && got[f].type <= 3);
	types[got[f].type]++;
    }
    TASK_CHECK (types[1] == 1 && types[2] == 3 && types[3] == 5);
    mpeg2_close (d);
    return 0;
}

static int lc_no_map_without_display (void)
{
    static uint8_t stream[sizeof (task_video) + 16];
    int cut[NFRAMES][MBH];
    mpeg2dec_t * d;
    const mpeg2_info_t * info;
    size_t off = 0, len;
    int saw_non_display_null = 0, displays = 0;

    no_cut (cut);
    cut[0][0] = 1;
    len = build_cut_stream (stream, cut);

    mpeg2_accel (0);
    d = mpeg2_init ();
    TASK_CHECK (mpeg2_slice_concealment (d, 1) == 0);
    info = mpeg2_info (d);
    for (;;) {
	mpeg2_state_t st = mpeg2_parse (d);
	if (st == STATE_BUFFER) {
	    if (off >= len)
		break;
	    {
		size_t k = 37;
		if (k > len - off)
		    k = len - off;
		mpeg2_buffer (d, stream + off, stream + off + k);
		off += k;
	    }
	} else if (st == STATE_INVALID || st == STATE_INVALID_END) {
	    TASK_CHECK (0);
	} else if ((st == STATE_SLICE || st == STATE_END) &&
		   info->display_fbuf) {
	    TASK_CHECK (info->display_concealment != NULL);
	    displays++;
	} else {
	    TASK_CHECK (info->display_concealment == NULL);
	    TASK_CHECK (info->display_concealed_mbs == 0);
	    saw_non_display_null++;
	}
    }
    TASK_CHECK (displays == NFRAMES);
    TASK_CHECK (saw_non_display_null > 0);
    mpeg2_close (d);
    return 0;
}

const struct task_case task_cases[] = {
    { "coverage", "first_row", case_first_row },
    { "coverage", "middle_row", case_mid_row },
    { "coverage", "last_row", case_last_row },
    { "coverage", "multi_nonadjacent", case_multi_nonadjacent },
    { "coverage", "no_loss_all_zero", case_no_loss_all_zero },
    { "coverage", "long_skip_run", case_long_skip_run },
    { "coverage", "cross_row_slice", case_cross_row_slice },
    { "coverage", "motion_residual", case_motion_residual },

    { "pixels", "i_neutral", px_i_neutral },
    { "pixels", "p_forward_copy", px_p_forward },
    { "pixels", "b_average", px_b_average },
    { "pixels", "reference_completed_before_use",
      px_reference_completed_before_use },
    { "pixels", "recursion", px_recursion },

    { "reorder", "b_reorder_map", ro_b_reorder_map },
    { "reorder", "tags", ro_tags },
    { "reorder", "end_drain", ro_end_drain },
    { "reorder", "slot_reuse", ro_slot_reuse },

    { "lifecycle", "api_validation", lc_api_validation },
    { "lifecycle", "convert_exclusion", lc_convert_exclusion },
    { "lifecycle", "reset_keeps_switch", lc_reset_keeps_switch_clears_state },
    { "lifecycle", "two_instances", lc_two_instances },
    { "lifecycle", "enabled_matches_disabled", lc_enabled_matches_disabled },
    { "lifecycle", "disabled_nine_frames", lc_disabled_nine_frames },
    { "lifecycle", "no_map_without_display", lc_no_map_without_display },
};
const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
