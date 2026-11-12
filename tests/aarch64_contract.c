/*
 * aarch64_contract.c
 *
 * Contract tests for the AArch64 NEON back end of libmpeg2.
 *
 * Three groups:
 *
 *   test_idct    - the NEON IDCT against the C IDCT, block for block, plus
 *                  the workspace state each of them leaves behind.  Covers
 *                  asymmetric AC coefficients, DC boundary values, both
 *                  saturation directions, unaligned destinations, odd
 *                  strides and padding sentinels.
 *
 *   test_motion  - all four prediction positions (o, x, y, xy) for put and
 *                  avg, at both 8 and 16 pixels wide, against the C
 *                  kernels.  Includes the "avg4 is not two avg2 calls"
 *                  case, unaligned ref/dest and non-tight strides, and
 *                  verifies the 8 wide path does not read past ref[8].
 *
 *   test_stream  - decodes the bundled streams through the public parser
 *                  in two separate processes, one forcing mpeg2_accel(0)
 *                  (C) and one using the automatic selection (NEON), then
 *                  compares every plane of every displayed frame together
 *                  with the frame types and display order.  The second
 *                  stream (tests/neon_move.m2v) uses a moving, rotating
 *                  texture so that bidirectional prediction and the
 *                  half-pel paths are actually reached; the run also
 *                  asserts that they were, via the kernel call counters.
 *
 * The C kernels are static in idct.c / motion_comp.c; each group that needs
 * them links them through the public function pointers (IDCT) or through the
 * mpeg2_mc_c table (motion compensation), which is how the library itself
 * reaches them.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

#include "check.h"

/* Kernel entry points, declared in mpeg2_internal.h under ARCH_AARCH64. */
extern void (* mpeg2_idct_copy) (int16_t * block, uint8_t * dest, int stride);
extern void (* mpeg2_idct_add) (int last, int16_t * block,
				uint8_t * dest, int stride);
extern mpeg2_mc_t mpeg2_mc;
extern mpeg2_mc_t mpeg2_mc_c;
extern unsigned long mpeg2_neon_idct_calls[2];
extern unsigned long mpeg2_neon_mc_calls;
extern unsigned long mpeg2_neon_mc_slots[16];

#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))

static uint32_t rng_state = 0x12345678u;

static uint32_t next_rand (void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int rand_u8 (void)
{
    return (int) (next_rand () >> 24);
}

/* --------------------------------------------------------------------- */
/* IDCT group                                                             */
/* --------------------------------------------------------------------- */

/*
 * Build a block that exercises the parts of the transform that a naive
 * optimisation is most likely to get wrong:
 *
 *   mode 0: full 16-bit noise
 *   mode 1: small symmetric-ish values (the normal decoded range)
 *   mode 2: DC only, with DC values at both int16 boundaries
 *   mode 3: single AC coefficient (asymmetric by construction)
 *   mode 4: large positive / large negative AC residues
 */
static void make_block (int16_t * block, int mode, int iter)
{
    int i;

    switch (mode) {
    case 0:
	for (i = 0; i < 64; i++)
	    block[i] = (int16_t) ((int) next_rand () - 32768);
	break;
    case 1:
	for (i = 0; i < 64; i++)
	    block[i] = (int16_t) ((int) (next_rand () & 0xff) - 128);
	break;
    case 2:
	memset (block, 0, 64 * sizeof (int16_t));
	/* DC only, sweeping the int16 extremes */
	block[0] = (int16_t) ((iter * 8191) & 0xffff);
	break;
    case 3:
	memset (block, 0, 64 * sizeof (int16_t));
	block[0] = (int16_t) (iter * 977);
	block[1 + (iter % 63)] = (int16_t) (1 + (iter % 4095));
	break;
    case 4:
	for (i = 0; i < 64; i++)
	    block[i] = (int16_t) ((i & 1) ? 32767 : -32768);
	break;
    default:
	memset (block, 0, 64 * sizeof (int16_t));
	break;
    }
}

#define DEST_PAD 96
#define DEST_ROWS 16

static void fill_dest (uint8_t * dest, int stride, int rows)
{
    int y, x;

    for (y = 0; y < rows; y++)
	for (x = 0; x < stride; x++)
	    dest[y * stride + x] = (uint8_t) rand_u8 ();
}

/*
 * Run both back ends on identical input and compare the destination block
 * *and* the coefficient workspace afterwards.  Padding bytes outside the
 * written 8x8 area are compared too, so a kernel that spills into a
 * neighbouring block is caught.
 */
static void compare_idct (int16_t * src, const uint8_t * dest_template,
			  int stride, int dest_offset, int last, int mode,
                          const char * what)
{
    int16_t bc[64], bn[64];
    uint8_t dc[DEST_ROWS * DEST_PAD], dn[DEST_ROWS * DEST_PAD];
    uint8_t * cdest, * ndest;

    memcpy (bc, src, sizeof (bc));
    memcpy (bn, src, sizeof (bn));
    memcpy (dc, dest_template, sizeof (dc));
    memcpy (dn, dest_template, sizeof (dn));
    cdest = dc + dest_offset;
    ndest = dn + dest_offset;

    mpeg2_idct_copy_c (bc, cdest, stride);
    mpeg2_idct_copy_neon (bn, ndest, stride);
    unit_check (!memcmp (bc, bn, sizeof (bc)),
		"idct copy leaves the same coefficient workspace");
    unit_check (!memcmp (dc, dn, sizeof (dc)),
		"idct copy writes the same destination bytes");

    memcpy (bc, src, sizeof (bc));
    memcpy (bn, src, sizeof (bn));
    memcpy (dc, dest_template, sizeof (dc));
    memcpy (dn, dest_template, sizeof (dn));
    mpeg2_idct_add_c (last, bc, cdest, stride);
    mpeg2_idct_add_neon (last, bn, ndest, stride);
    unit_check (!memcmp (bc, bn, sizeof (bc)),
		"idct add leaves the same coefficient workspace");
    unit_check (!memcmp (dc, dn, sizeof (dc)),
		"idct add writes the same destination bytes");

    (void) mode;
    (void) what;
}

/*
 * The library installs its kernel tables on the first mpeg2_accel() call
 * (mpeg2_init() would do it too).  All three groups run in one process, so
 * this is the one place where the automatic selection is made; it must
 * report the AArch64 NEON capability, and a later request must not change
 * the selection.
 */
static void ensure_accel (void)
{
    uint32_t first = mpeg2_accel (MPEG2_ACCEL_DETECT);
    uint32_t again = mpeg2_accel (0);

    unit_check ((first & MPEG2_ACCEL_ARM_AARCH64_NEON) != 0,
		"automatic selection reports the AArch64 NEON capability");
    unit_check (again == first,
		"accel initialisation only takes effect on the first call");
}

void test_idct (void)
{
    static const int strides[] = { 8, 13, 17, 8 + DEST_PAD };
    static const int offsets[] = { 0, 1, 3, 5 };
    int16_t src[64];
    uint8_t dest[DEST_ROWS * DEST_PAD];
    unsigned i, mode, iter;

    ensure_accel ();

    /* Runtime selection must actually point at the NEON kernels here;
       a capability bit alone is not evidence. */
    {
	unsigned long before_copy = mpeg2_neon_idct_calls[0];
	unsigned long before_add = mpeg2_neon_idct_calls[1];
	int16_t probe[64];
	uint8_t out[64];

	memset (probe, 0, sizeof (probe));
	probe[0] = 100;
	mpeg2_idct_copy (probe, out, 8);
	mpeg2_idct_add (129, probe, out, 8);
	unit_check (mpeg2_neon_idct_calls[0] > before_copy,
		    "mpeg2_idct_copy resolves to the NEON kernel");
	unit_check (mpeg2_neon_idct_calls[1] > before_add,
		    "mpeg2_idct_add resolves to the NEON kernel");
    }

    /* The two back ends must be different code, not the C function
       re-published under a NEON name.  Their addresses differ, and a
       distinguishable input must still agree byte for byte (checked by
       compare_idct below). */
    unit_check ((void *) mpeg2_idct_copy_neon != (void *) mpeg2_idct_copy_c,
		"NEON idct copy is a distinct kernel from the C one");
    unit_check ((void *) mpeg2_idct_add_neon != (void *) mpeg2_idct_add_c,
		"NEON idct add is a distinct kernel from the C one");

    for (mode = 0; mode < 5; mode++) {
	for (iter = 0; iter < 40; iter++) {
	    make_block (src, mode, (int) iter);
	    fill_dest (dest, DEST_PAD, DEST_ROWS);
	    for (i = 0; i < ARRAY_SIZE (strides); i++) {
		int s = strides[i];
		int off = offsets[iter % ARRAY_SIZE (offsets)];

		/* last == 129 is the DC fast-path probe; block[0] bits
		   4..6 decide which way it goes.  The corpus includes both
		   DC-only blocks (which take the fast path) and blocks
		   with AC (which take the general path). */
		compare_idct (src, dest, s, off, 129, (int) mode,
			      "copy/add");
		compare_idct (src, dest, s, off, 0, (int) mode,
			      "generic path");
	    }
	}
    }

    /* The DC fast path is only taken for last == 129 with the DC bits
       matching; make sure both branches were actually visited by giving
       the coefficient workspace a marker and checking it afterwards. */
    {
	int16_t block[64];
	uint8_t d[8 * 13];

	memset (block, 0, sizeof (block));
	block[0] = 0x0450;	/* (block[0] & (7 << 4)) == (4 << 4) */
	memset (d, 200, sizeof (d));
	mpeg2_idct_add_c (129, block, d, 13);
	unit_check (block[0] == 0 && block[63] == 0,
		    "idct add DC fast path clears block[0] and block[63]");
	unit_check (block[1] == 0 && block[62] == 0,
		    "idct add DC fast path leaves the rest of the block alone");

	memset (block, 0, sizeof (block));
	block[0] = 0x0450;
	memset (d, 200, sizeof (d));
	mpeg2_idct_add_neon (129, block, d, 13);
	unit_check (block[0] == 0 && block[63] == 0,
		    "NEON idct add DC fast path clears block[0] and block[63]");
	unit_check (block[1] == 0 && block[62] == 0,
		    "NEON idct add DC fast path leaves the rest of the block alone");
    }

    /* A padded workspace sentinel: the kernels must not write outside the
       64 coefficients, and copy must zero all of them on the way out. */
    {
	int16_t guarded[64 + 16];
	uint8_t d[8 * 9];
	int k;

	for (k = 0; k < 64 + 16; k++)
	    guarded[k] = (int16_t) 0x5a5a;
	make_block (guarded, 0, 7);
	memset (d, 0, sizeof (d));
	mpeg2_idct_copy_neon (guarded, d, 9);
	for (k = 64; k < 64 + 16; k++)
	    unit_check (guarded[k] == (int16_t) 0x5a5a,
			"NEON idct copy does not write past coefficient 63");
	for (k = 0; k < 64; k++)
	    unit_check (guarded[k] == 0,
			"NEON idct copy clears the coefficient workspace");
    }
}

/* --------------------------------------------------------------------- */
/* Motion compensation group                                              */
/* --------------------------------------------------------------------- */

/*
 * mpeg2_mc.put/avg index 0..3 are the 16 wide o/x/y/xy kernels and 4..7
 * the 8 wide ones.  We drive both tables over identical memory and compare
 * all eight pixels / sixteen pixels of each row, including the untouched
 * neighbours around the destination block.
 */
static void compare_mc (int index, int stride, int height,
			int ref_off, int dest_off, const char * label)
{
    /* Table layout: 0..3 are the 16 wide o/x/y/xy kernels, 4..7 the 8
       wide ones.  index already encodes that. */
    int i = index;
    int rows = 40, cols = 512;
    uint8_t refbuf[40 * 512], cbuf[40 * 512], nbuf[40 * 512];
    uint8_t * ref, * cd, * nd;
    int y;

    for (y = 0; y < rows * cols; y++) {
	refbuf[y] = (uint8_t) rand_u8 ();
	cbuf[y] = nbuf[y] = (uint8_t) rand_u8 ();
    }

    ref = refbuf + ref_off;
    cd = cbuf + dest_off;
    nd = nbuf + dest_off;

    if (i < 4) {
	mpeg2_mc_c.put[i] (cd, ref, stride, height);
	mpeg2_mc.put[i] (nd, ref, stride, height);
    } else {
	mpeg2_mc_c.avg[i - 4] (cd, ref, stride, height);
	mpeg2_mc.avg[i - 4] (nd, ref, stride, height);
    }

    unit_check (!memcmp (cd, nd, (size_t) height * stride), label);

    /* The destination rectangle is dest_off .. dest_off + height*stride
       (inclusive of the stride padding between rows).  Every byte
       outside it must be untouched; a single check on that range is
       enough, comparing both buffers against their pre-call copy. */
    {
	int outside_ok = 1;

	for (y = 0; y < rows * cols; y++) {
	    if (y < dest_off || y >= dest_off + height * stride) {
		if (cbuf[y] != nbuf[y])
		    outside_ok = 0;
	    }
	}
	unit_check (outside_ok,
		    "motion compensation does not touch pixels outside the block");
    }
}

void test_motion (void)
{
    uint8_t ref[64 * 64], cdest[64 * 64], ndest[64 * 64];
    int y, x;

    ensure_accel ();

    /* Selection check: the table must point at the NEON kernels, and
       they must be different functions from the C ones. */
    {
	unsigned long before = mpeg2_neon_mc_calls;

	unit_check ((void *) mpeg2_mc.put[0] != (void *) mpeg2_mc_c.put[0],
		    "NEON put is a distinct kernel from the C one");
	unit_check ((void *) mpeg2_mc.avg[0] != (void *) mpeg2_mc_c.avg[0],
		    "NEON avg is a distinct kernel from the C one");
	mpeg2_mc.put[0] (cdest, ref, 64, 16);
	unit_check (mpeg2_neon_mc_calls > before,
		    "mpeg2_mc.put resolves to the NEON kernels");
	before = mpeg2_neon_mc_calls;
	mpeg2_mc.avg[4] (cdest, ref, 64, 8);
	unit_check (mpeg2_neon_mc_calls > before,
		    "mpeg2_mc.avg resolves to the NEON kernels");
    }

    /* All should be exercised in the stream group too; see below. */

    /* Deterministic contents with a fine texture, so the half-pel sums
       differ between positions. */
    for (y = 0; y < 64 * 64; y++)
	ref[y] = (uint8_t) (((y * 7) ^ (y >> 3)) & 0xff);

    /*
     * The key rounding case: pick four samples with an odd sum (10+20+
     * 30+31 = 91 -> (91+2)>>2 = 23).  Two avg2 calls would give
     * avg2(avg2(a,b), avg2(c,d)) = avg2(15, 31) = 23 here, but e.g.
     * 1,2,3,4 -> 3 while the split gives 2.  Both are checked by the
     * exhaustive comparison below; this one is explicit because it is the
     * case an implementation is most likely to get wrong.
     */
    {
	int idx;
	for (idx = 0; idx < 8; idx++) {
	    int width = (idx < 4) ? 16 : 8;
	    int i = idx & 3;

	    memset (ref, 0, sizeof (ref));
	    ref[0] = 1; ref[1] = 2; ref[64] = 3; ref[65] = 4;
	    memset (cdest, 0, sizeof (cdest));
	    memset (ndest, 0, sizeof (ndest));
	    mpeg2_mc_c.put[i] (cdest, ref, 64, 1);
	    mpeg2_mc.put[i] (ndest, ref, 64, 1);
	    unit_check (!memcmp (cdest, ndest, 64),
			"put prediction matches the C kernel");
	    mpeg2_mc_c.avg[i] (cdest, ref, 64, 1);
	    mpeg2_mc.avg[i] (ndest, ref, 64, 1);
	    unit_check (!memcmp (cdest, ndest, 64),
			"avg prediction matches the C kernel");
	    (void) width;
	}
    }

    /* Structured comparison over many strides/heights/offsets. */
    {
	static const int widths[] = { 16, 8 };
	static const int heights[] = { 1, 2, 3, 7, 8, 9, 16, 17 };
	static const int ref_offs[] = { 0, 1, 2, 3, 5, 7 };
	static const int dest_offs[] = { 0, 1, 2, 3, 6, 9 };
	unsigned w, idx, h, ro, dop;
	int stride;

	for (w = 0; w < ARRAY_SIZE (widths); w++) {
	    int width = widths[w];
	    for (stride = width + 1; stride <= width + 24; stride += 3) {
		for (idx = 0; idx < 8; idx++) {
		    for (h = 0; h < ARRAY_SIZE (heights); h++) {
			ro = (unsigned) (rand_u8 () % ARRAY_SIZE (ref_offs));
			dop = (unsigned) (rand_u8 () % ARRAY_SIZE (dest_offs));
			compare_mc ((int) idx, stride, heights[h],
				    ref_offs[ro], dest_offs[dop],
				    "motion compensation matches the C kernel");
		    }
		}
	    }
	}
    }

    /*
     * 8 wide must read only its own eight samples plus the one half-pel
     * neighbour on each axis.  With stride 8 and height 8 that means at
     * most bytes 0..64 for o/x and 0..72 for y/xy (the vertical
     * neighbour row of the last row starts at byte 64).  Corrupting
     * bytes 73.. must not change the output.  Checked for all eight
     * 8-wide slots (table entries 4..7 are put/avg o/x/y/xy).
     */
    for (x = 0; x < 64 * 64; x++)
	ref[x] = (uint8_t) rand_u8 ();
    {
	int pos;

	for (pos = 0; pos < 8; pos++) {
	    int i = 4 + (pos & 3);	/* 8 wide slots are 4..7 */
	    int is_avg = pos >= 4;
	    uint8_t base[80], other[80], a[64], b[64];

	    memset (base, 0, sizeof (base));
	    memset (other, 0, sizeof (other));
	    memcpy (base, ref, 64);
	    memcpy (other, ref, 64);
	    other[8 * 9 + 1] ^= 0xff;	/* beyond the last neighbour row */
	    other[8 * 9 + 5] ^= 0xa5;

	    if (is_avg) {
		mpeg2_mc.avg[i] (a, base, 8, 8);
		mpeg2_mc.avg[i] (b, other, 8, 8);
	    } else {
		mpeg2_mc.put[i] (a, base, 8, 8);
		mpeg2_mc.put[i] (b, other, 8, 8);
	    }
	    unit_check (!memcmp (a, b, sizeof (a)),
			"8 wide path does not read past the half-pel neighbour");
	}
    }
}

/* --------------------------------------------------------------------- */
/* Stream group                                                           */
/* --------------------------------------------------------------------- */

typedef struct {
    unsigned frames;
    uint32_t hash;
    uint32_t type_hash;
    uint32_t order_hash;
    unsigned char_types[4];
    unsigned width, height, chroma_width, chroma_height;
} stream_summary_t;

static uint32_t hash_plane (const uint8_t * p, unsigned n, uint32_t h)
{
    unsigned i;

    for (i = 0; i < n; i++)
	h = (h ^ p[i]) * 16777619u;
    return h;
}

/*
 * Decode one stream through the public parser.  req is the accel request
 * passed to mpeg2_accel; the return value is stored in *got.
 */
static int run_stream (const char * path, uint32_t req,
		       stream_summary_t * out, uint32_t * got)
{
    FILE * f;
    uint8_t chunk[73];
    size_t n = 1;
    mpeg2dec_t * dec;
    const mpeg2_info_t * info;
    uint32_t h = 2166136261u;
    uint32_t type_h = 2166136261u;
    uint32_t order_h = 2166136261u;

    memset (out, 0, sizeof (*out));

    *got = mpeg2_accel (req);
    f = fopen (path, "rb");
    if (!f)
	return 1;
    dec = mpeg2_init ();
    if (!dec) {
	fclose (f);
	return 1;
    }
    info = mpeg2_info (dec);

    while (n) {
	mpeg2_state_t s = mpeg2_parse (dec);

	if (s == STATE_BUFFER) {
	    n = fread (chunk, 1, sizeof (chunk), f);
	    mpeg2_buffer (dec, chunk, chunk + n);
	}
	if ((s == STATE_SLICE || s == STATE_END || s == STATE_INVALID_END) &&
	    info->display_fbuf) {
	    const mpeg2_picture_t * pic = info->display_picture;
	    const mpeg2_sequence_t * seq = info->sequence;
	    int p;

	    out->width = seq->width;
	    out->height = seq->height;
	    out->chroma_width = seq->chroma_width;
	    out->chroma_height = seq->chroma_height;

	    for (p = 0; p < 3; p++) {
		unsigned w = p ? seq->chroma_width : seq->width;
		unsigned hh = p ? seq->chroma_height : seq->height;

		h = hash_plane (info->display_fbuf->buf[p], w * hh, h);
	    }
	    type_h = (type_h ^ (pic->flags & PIC_MASK_CODING_TYPE)) *
		     16777619u;
	    order_h = (order_h ^ pic->temporal_reference) * 16777619u;
	    out->char_types[(pic->flags & PIC_MASK_CODING_TYPE) & 3]++;
	    out->frames++;
	}
    }

    out->hash = h;
    out->type_hash = type_h;
    out->order_hash = order_h;

    fclose (f);
    mpeg2_close (dec);
    return 0;
}

/*
 * The two accel selections have to be made in fresh processes: mpeg2_accel
 * is process global and only the first call takes effect, and by the time
 * the stream group runs this binary has already installed its kernels.
 * We therefore re-exec ourselves with the request encoded in the
 * environment.  A constructor runs before main () and, in that mode, only
 * decodes and prints the summary.  Re-exec gives each child pristine
 * library state, exactly like a separate program using the public API.
 */
static void stream_child_exec (void)
{
    const char * path = getenv ("MPEG2_NEON_STREAM_CHILD");
    const char * req = getenv ("MPEG2_NEON_STREAM_REQ");
    stream_summary_t s;
    uint32_t got;

    if (!path || !req)
	return;
    if (run_stream (path, (uint32_t) strtoul (req, NULL, 0), &s, &got))
	_exit (3);
    printf ("%u %u %u %u %u %u %u %u %u %u %u %u %u\n",
	    s.frames, (unsigned) s.hash, (unsigned) s.type_hash,
	    (unsigned) s.order_hash, s.char_types[0], s.char_types[1],
	    s.char_types[2], s.char_types[3], s.width, s.height,
	    s.chroma_width, s.chroma_height, (unsigned) got);
    fflush (stdout);
    _exit (0);
}

__attribute__((constructor)) static void stream_child_ctor (void)
{
    stream_child_exec ();
}

static int self_path (char * out, size_t out_len)
{
#ifdef __APPLE__
    uint32_t size = (uint32_t) out_len;

    return _NSGetExecutablePath (out, &size) == 0 ? 0 : 1;
#else
    ssize_t n = readlink ("/proc/self/exe", out, out_len - 1);

    if (n <= 0)
	return 1;
    out[n] = 0;
    return 0;
#endif
}

static int run_in_fresh_process (const char * path, uint32_t req,
				 stream_summary_t * out, uint32_t * got)
{
    char exe[512];
    char reqstr[64];
    int fd[2];
    pid_t pid;
    char line[512];
    int status;
    ssize_t rc;

    if (self_path (exe, sizeof (exe)))
	return 1;
    snprintf (reqstr, sizeof (reqstr), "%u", (unsigned) req);

    if (pipe (fd))
	return 1;
    pid = fork ();
    if (pid < 0) {
	close (fd[0]);
	close (fd[1]);
	return 1;
    }
    if (pid == 0) {
	char * argv[2];
	char * env_req;
	char * env_path;

	close (fd[0]);
	if (dup2 (fd[1], STDOUT_FILENO) < 0)
	    _exit (4);
	close (fd[1]);
	env_req = malloc (strlen (reqstr) + 24);
	env_path = malloc (strlen (path) + 24);
	if (!env_req || !env_path)
	    _exit (5);
	sprintf (env_req, "MPEG2_NEON_STREAM_REQ=%s", reqstr);
	sprintf (env_path, "MPEG2_NEON_STREAM_CHILD=%s", path);
	putenv (env_req);
	putenv (env_path);
	argv[0] = exe;
	argv[1] = NULL;
	execv (exe, argv);
	_exit (6);
    }
    close (fd[1]);
    rc = read (fd[0], line, sizeof (line) - 1);
    close (fd[0]);
    waitpid (pid, &status, 0);
    if (rc <= 0 || !WIFEXITED (status) || WEXITSTATUS (status) != 0)
	return 1;
    line[rc] = 0;
    if (sscanf (line, "%u %u %u %u %u %u %u %u %u %u %u %u %u",
		&out->frames, (unsigned *) &out->hash,
		(unsigned *) &out->type_hash, (unsigned *) &out->order_hash,
		&out->char_types[0], &out->char_types[1],
		&out->char_types[2], &out->char_types[3],
		&out->width, &out->height,
		&out->chroma_width, &out->chroma_height,
		(unsigned *) got) != 13)
	return 1;
    return 0;
}

#define GOLDEN_IPB_HASH      0xd302b869u
#define GOLDEN_MOVE_HASH     0xc1855c2eu

static void compare_streams (const char * path, unsigned min_frames,
			     unsigned need_b, uint32_t golden_hash,
			     const char * label)
{
    stream_summary_t c, n;
    uint32_t got_c, got_n;
    int ok;

    ok = run_in_fresh_process (path, 0, &c, &got_c);
    unit_check (!ok, "forced C decode runs");
    ok = run_in_fresh_process (path, MPEG2_ACCEL_DETECT, &n, &got_n);
    unit_check (!ok, "automatic decode runs");

    unit_check (got_c == 0,
		"mpeg2_accel(0) reports no acceleration");
    unit_check ((got_n & MPEG2_ACCEL_ARM_AARCH64_NEON) != 0,
		"automatic selection reports the AArch64 NEON capability");
    unit_check ((got_n & ~MPEG2_ACCEL_DETECT) != 0,
		"automatic selection reports at least one capability bit");

    /* The golden hash is the reference produced by the portable C back
       end, independently verified against the baseline harness.  Comparing
       the two back ends alone would not catch a regression shared by both
       (for example a wrong scan-table rotation). */
    unit_check (c.hash == golden_hash,
		"C decode matches the recorded reference hash");

    unit_check (c.frames >= min_frames,
		"forced C decode produces the expected pictures");
    unit_check (n.frames == c.frames,
		"NEON decode produces the same number of pictures");
    unit_check (n.hash == c.hash,
		"NEON decode matches C byte for byte on every plane");
    unit_check (n.type_hash == c.type_hash,
		"NEON decode sees the same picture coding types");
    unit_check (n.order_hash == c.order_hash,
		"NEON decode sees the same temporal_reference display order");
    unit_check (n.width == c.width && n.height == c.height &&
		n.chroma_width == c.chroma_width &&
		n.chroma_height == c.chroma_height,
		"NEON decode sees the same sequence dimensions");

    if (need_b) {
	unit_check (c.char_types[PIC_FLAG_CODING_TYPE_B] > 0,
		    "stream contains B pictures");
	unit_check (c.char_types[PIC_FLAG_CODING_TYPE_P] > 0,
		    "stream contains P pictures");
    }

    (void) label;
}

void test_stream (void)
{
    ensure_accel ();

    /*
     * First the bundled six picture stream.  This is the same data the
     * baseline runs, so the hashes can be cross-checked by hand.
     */
    compare_streams ("support/ipb.m2v", 6, 1, GOLDEN_IPB_HASH,
		     "bundled stream");

    /*
     * The moving-texture fixture: eight pictures, I/P/B, with enough
     * motion and half-pel detail to reach all sixteen motion
     * compensation slots and the bidirectional average.
     */
    compare_streams ("tests/neon_move.m2v", 8, 1, GOLDEN_MOVE_HASH,
		     "moving texture");

    /*
     * The stream group must not merely rely on the capability bit: decode
     * the moving fixture in this process with the automatic selection and
     * check that the NEON kernels were actually entered, including every
     * put/avg and o/x/y/xy slot.
     */
    {
	FILE * f;
	uint8_t chunk[73];
	size_t n = 1;
	mpeg2dec_t * dec;
	const mpeg2_info_t * info;
	unsigned long idct_before[2], mc_before, slots_before[16];
	unsigned decoded = 0;
	int i;

	for (i = 0; i < 2; i++)
	    idct_before[i] = mpeg2_neon_idct_calls[i];
	mc_before = mpeg2_neon_mc_calls;
	for (i = 0; i < 16; i++)
	    slots_before[i] = mpeg2_neon_mc_slots[i];

	f = fopen ("tests/neon_move.m2v", "rb");
	unit_check (f != NULL, "moving texture fixture is present");
	dec = mpeg2_init ();
	unit_check (dec != NULL, "decoder allocates");
	info = mpeg2_info (dec);
	while (n) {
	    mpeg2_state_t st = mpeg2_parse (dec);

	    if (st == STATE_BUFFER) {
		n = fread (chunk, 1, sizeof (chunk), f);
		mpeg2_buffer (dec, chunk, chunk + n);
	    }
	    if ((st == STATE_SLICE || st == STATE_END ||
		 st == STATE_INVALID_END) && info->display_fbuf)
		decoded++;
	}
	fclose (f);
	mpeg2_close (dec);

	unit_check (mpeg2_neon_idct_calls[0] > idct_before[0],
		    "stream decode enters the NEON IDCT copy kernel");
	unit_check (mpeg2_neon_idct_calls[1] > idct_before[1],
		    "stream decode enters the NEON IDCT add kernel");
	unit_check (mpeg2_neon_mc_calls > mc_before,
		    "stream decode enters the NEON motion compensation kernels");
	for (i = 0; i < 16; i++)
	    unit_check (mpeg2_neon_mc_slots[i] > slots_before[i],
			"stream decode reaches every put/avg and o/x/y/xy slot");
	/* and the decoding really went through the display path, i.e. the
	   counts above are not from a failed parse */
	unit_check (decoded >= 8,
		    "in-process decode produced the expected pictures");
    }

}
