/*
 * tests/entropy_reference.c
 *
 * Independent regression reference for the TTA1 two-level Rice entropy
 * codec.
 *
 * The reference bitstream, Rice adaptation and CRC below are written
 * from the format rules directly.  They never call the production
 * put/get routines, the bit_mask/bit_shift tables or the production
 * CRC helpers.  libtta.c is included only so the tests can observe the
 * production encoder/decoder entropy state (TTA_adapt) and drive the
 * internal put/get entry points on hand-built byte streams; the
 * production coding loop is not reused as the reference.
 *
 * Public entry points used by devcheck/target_main.c:
 *
 *   int case_thresholds(void)
 *   int case_fifo(void)
 *   int case_frames(void)
 *
 * Each returns the number of positive assertions actually executed.
 * Any divergence between reference and production prints a line
 * starting with "ENTROPY_MISMATCH " and exits non-zero (first mismatch
 * wins); a crash, empty output or compile failure can never masquerade
 * as a detection.
 */

#include "libtta.c"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_checks;

static void mismatch(const char *fmt, ...) {
	va_list ap;
	fputs("ENTROPY_MISMATCH ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	exit(1);
}

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) mismatch(__VA_ARGS__); \
		g_checks++; \
	} while (0)

/* ------------------------------------------------------------------ */
/* Independent LSB-first bitstream                                    */
/* ------------------------------------------------------------------ */

#define REF_MAX_BYTES (512u * 1024u)

typedef struct {
	TTAuint8 buf[REF_MAX_BYTES];
	size_t nbits;
} ref_bs;

static void ref_bs_init(ref_bs *bs) {
	memset(bs->buf, 0, sizeof(bs->buf));
	bs->nbits = 0;
}

/* Write the low n bits of value LSB-first, one bit at a time. */
static void ref_put_bits(ref_bs *bs, TTAuint32 value, unsigned n) {
	unsigned i;
	for (i = 0; i < n; i++) {
		size_t byte = bs->nbits >> 3;
		if (byte >= REF_MAX_BYTES) mismatch("reference stream overflow");
		bs->buf[byte] |=
			(TTAuint8)(((value >> i) & 1u) << (bs->nbits & 7));
		bs->nbits++;
	}
}

static unsigned ref_get_bit(ref_bs *bs) {
	size_t byte = bs->nbits >> 3;
	unsigned bit;
	if (byte >= REF_MAX_BYTES) mismatch("reference stream underrun");
	bit = (bs->buf[byte] >> (bs->nbits & 7)) & 1u;
	bs->nbits++;
	return bit;
}

static TTAuint32 ref_get_bits(ref_bs *bs, unsigned n) {
	TTAuint32 value = 0;
	unsigned i;
	for (i = 0; i < n; i++)
		value |= ref_get_bit(bs) << i;
	return value;
}

static size_t ref_bs_bytes(const ref_bs *bs) {
	return (bs->nbits + 7) >> 3;
}

/* Compare two arbitrary bit spans bit by bit. */
static int ref_span_equal(const ref_bs *a, size_t ab,
	const ref_bs *b, size_t bb, size_t nbits) {
	size_t i;
	for (i = 0; i < nbits; i++) {
		unsigned x = (a->buf[(ab + i) >> 3] >> ((ab + i) & 7)) & 1u;
		unsigned y = (b->buf[(bb + i) >> 3] >> ((bb + i) & 7)) & 1u;
		if (x != y) return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Independent CRC-32 (IEEE polynomial, table-free, bitwise)          */
/* ------------------------------------------------------------------ */

static TTAuint32 ref_crc32(const TTAuint8 *data, size_t len) {
	TTAuint32 crc = 0xffffffffu;
	size_t i;
	int bit;
	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return crc ^ 0xffffffffu;
}

static TTAuint32 ref_rd_u16(const TTAuint8 *p) {
	return (TTAuint32)p[0] | ((TTAuint32)p[1] << 8);
}

static TTAuint32 ref_rd_u32(const TTAuint8 *p) {
	return (TTAuint32)p[0] | ((TTAuint32)p[1] << 8) |
		((TTAuint32)p[2] << 16) | ((TTAuint32)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Independent two-level Rice state machine                           */
/*                                                                    */
/* Rules, expressed without any production helper:                    */
/*   residual -> unsigned:  x > 0 -> 2x-1,  x <= 0 -> -2x             */
/*   level 0 when u < 2^k0 with the pre-update k0; otherwise the      */
/*   level-0 base is subtracted and the remainder is expressed with   */
/*   level-1 unary/remainder using the pre-update k1.                 */
/*   sum0 tracks the full unsigned value; sum1 tracks the post-base   */
/*   level-1 quantity and is only touched on level-1 symbols.  Each   */
/*   update first subtracts one sixteenth of the old sum.  A sum      */
/*   exactly equal to an adaptation threshold leaves k unchanged.     */
/* ------------------------------------------------------------------ */

typedef struct {
	unsigned k0;
	unsigned k1;
	TTAuint32 sum0;
	TTAuint32 sum1;
} ref_rice;

/* Adaptation threshold for parameter k.  The format shifts a 32-bit
 * accumulator, so 2^(k+4) saturates at the sign bit: derived here from
 * the 32-bit arithmetic rule rather than any production table. */
static TTAuint32 ref_thresh(unsigned k) {
	if (k >= 27) return 0x80000000u;	/* 2^31, the saturation value */
	return 1u << (k + 4);
}

/* 2^k with the same 32-bit shift saturation. */
static TTAuint32 ref_pow2(unsigned k) {
	if (k >= 31) return 0x80000000u;
	return 1u << k;
}

#define REF_THRESH(k) ref_thresh(k)

static void ref_rice_reset(ref_rice *r) {
	r->k0 = r->k1 = 10;
	r->sum0 = r->sum1 = REF_THRESH(10); /* 1 << 14 */
}

/* Equality with a threshold: no move. Only strict crossing changes k. */
static void ref_adapt(unsigned *k, TTAuint32 sum) {
	if (*k > 0 && sum < REF_THRESH(*k))
		(*k)--;
	else if (sum > REF_THRESH(*k + 1))
		(*k)++;
}

static TTAuint32 ref_map_enc(TTAint32 x) {
	return (x > 0) ? (TTAuint32)((x << 1) - 1)
	               : (TTAuint32)((-x) << 1);
}

static TTAint32 ref_map_dec(TTAuint32 u) {
	return (u & 1u) ? (TTAint32)((u + 1) >> 1)
	                : -(TTAint32)(u >> 1);
}

typedef struct {
	size_t start;		/* bit offset where the symbol starts */
	unsigned nbits;		/* bits consumed by this symbol */
	int level;		/* 0 or 1 */
	unsigned unary;		/* unary count written/read */
	unsigned rem_k;		/* remainder width */
	TTAuint32 unsigned_value; /* full unsigned value u */
	TTAint32 residual;	/* signed residual */
	unsigned pre_k0, pre_k1, post_k0, post_k1;
	TTAuint32 pre_sum0, pre_sum1, post_sum0, post_sum1;
} ref_trace;

static void ref_snap_pre(ref_rice *r, ref_trace *t) {
	t->pre_k0 = r->k0;
	t->pre_k1 = r->k1;
	t->pre_sum0 = r->sum0;
	t->pre_sum1 = r->sum1;
}

static void ref_snap_post(ref_rice *r, ref_trace *t) {
	t->post_k0 = r->k0;
	t->post_k1 = r->k1;
	t->post_sum0 = r->sum0;
	t->post_sum1 = r->sum1;
}

/* Reference encode of one signed residual. */
static void ref_rice_encode(ref_rice *r, TTAint32 residual,
	ref_bs *bs, ref_trace *t) {
	TTAuint32 u = ref_map_enc(residual);
	TTAuint32 base, v;
	unsigned unary, rk;

	memset(t, 0, sizeof(*t));
	t->start = bs->nbits;
	ref_snap_pre(r, t);

	/* sum0 always tracks the full unsigned value; k0 adapts first,
	 * but this symbol is coded with the pre-update k0. */
	r->sum0 += u - (r->sum0 >> 4);
	ref_adapt(&r->k0, r->sum0);

	base = ref_pow2(t->pre_k0);
	if (u < base) {
		t->level = 0;
		unary = 0;
		rk = t->pre_k0;
		v = u;
	} else {
		t->level = 1;
		v = u - base;
		r->sum1 += v - (r->sum1 >> 4);
		ref_adapt(&r->k1, r->sum1);
		unary = 1u + (v >> t->pre_k1);
		rk = t->pre_k1;
	}

	/* unary: runs of 1 bits terminated by a 0 bit.  The terminator is
	 * always emitted (level-0 symbols carry only that zero), matching
	 * the production do/while loop which runs at least once.  Runs
	 * longer than 23 are split into 23-one slices before the final
	 * (shorter) one-run plus terminator. */
	while (unary > 23) {
		ref_put_bits(bs, (1u << 23) - 1u, 23);
		unary -= 23;
	}
	ref_put_bits(bs, (1u << unary) - 1u, unary); /* unary ones (0..23) */
	ref_put_bits(bs, 0u, 1);      /* terminating zero */
	/* binary remainder, LSB-first */
	if (rk) {
		if (rk >= 32) mismatch("remainder width %u exceeds 32-bit cache", rk);
		ref_put_bits(bs, v & ((1u << rk) - 1u), rk);
	}

	t->unary = t->level ? (1u + (v >> t->pre_k1)) : 0u;
	t->rem_k = rk;
	t->unsigned_value = u;
	t->residual = residual;
	t->nbits = (unsigned)(bs->nbits - t->start);
	ref_snap_post(r, t);
}

/* Reference decode of one symbol from the independent bitstream. */
static TTAint32 ref_rice_decode(ref_bs *bs, ref_rice *r, ref_trace *t) {
	TTAuint32 ones = 0, value, full;
	unsigned k;

	memset(t, 0, sizeof(*t));
	t->start = bs->nbits;
	ref_snap_pre(r, t);

	/* unary: count 1 bits up to the first 0.  Reading bit by bit
	 * crosses 0xff byte and 23-bit slice boundaries naturally. */
	while (ref_get_bit(bs))
		ones++;

	if (ones) {
		t->level = 1;
		k = t->pre_k1;
		value = ones - 1;
	} else {
		t->level = 0;
		k = t->pre_k0;
		value = 0;
	}

	if (k) {
		if (k >= 32) mismatch("remainder width %u exceeds 32-bit cache", k);
		value = (value << k) + ref_get_bits(bs, k);
	}

	if (t->level) {
		r->sum1 += value - (r->sum1 >> 4);
		ref_adapt(&r->k1, r->sum1);
		full = value + ref_pow2(t->pre_k0);
	} else {
		full = value;
	}

	r->sum0 += full - (r->sum0 >> 4);
	ref_adapt(&r->k0, r->sum0);

	t->unary = ones;
	t->rem_k = k;
	t->unsigned_value = full;
	t->residual = ref_map_dec(full);
	t->nbits = (unsigned)(bs->nbits - t->start);
	ref_snap_post(r, t);
	return t->residual;
}

static void ref_check_trace(const char *dir, unsigned sym,
	const ref_trace *want, const ref_trace *got,
	const ref_bs *want_bs, const ref_bs *got_bs) {
	CHECK(want->nbits == got->nbits,
		"sym=%u dir=%s nbits ref=%u got=%u",
		sym, dir, want->nbits, got->nbits);
	CHECK(ref_span_equal(want_bs, want->start, got_bs, got->start,
		want->nbits),
		"sym=%u dir=%s bit string divergence over %u bits",
		sym, dir, want->nbits);
	CHECK(want->level == got->level,
		"sym=%u dir=%s level ref=%d got=%d",
		sym, dir, want->level, got->level);
	CHECK(want->unary == got->unary,
		"sym=%u dir=%s unary ref=%u got=%u",
		sym, dir, want->unary, got->unary);
	CHECK(want->rem_k == got->rem_k,
		"sym=%u dir=%s rem_k ref=%u got=%u",
		sym, dir, want->rem_k, got->rem_k);
	CHECK(want->unsigned_value == got->unsigned_value,
		"sym=%u dir=%s unsigned ref=%u got=%u",
		sym, dir, want->unsigned_value, got->unsigned_value);
	CHECK(want->residual == got->residual,
		"sym=%u dir=%s residual ref=%d got=%d",
		sym, dir, want->residual, got->residual);
	CHECK(want->pre_k0 == got->pre_k0 && want->pre_k1 == got->pre_k1,
		"sym=%u dir=%s pre-k ref=(%u,%u) got=(%u,%u)",
		sym, dir, want->pre_k0, want->pre_k1,
		got->pre_k0, got->pre_k1);
	CHECK(want->post_k0 == got->post_k0 &&
		want->post_k1 == got->post_k1,
		"sym=%u dir=%s post-k ref=(%u,%u) got=(%u,%u)",
		sym, dir, want->post_k0, want->post_k1,
		got->post_k0, got->post_k1);
	CHECK(want->pre_sum0 == got->pre_sum0 &&
		want->pre_sum1 == got->pre_sum1,
		"sym=%u dir=%s pre-sum ref=(%u,%u) got=(%u,%u)",
		sym, dir, want->pre_sum0, want->pre_sum1,
		got->pre_sum0, got->pre_sum1);
	CHECK(want->post_sum0 == got->post_sum0 &&
		want->post_sum1 == got->post_sum1,
		"sym=%u dir=%s post-sum ref=(%u,%u) got=(%u,%u)",
		sym, dir, want->post_sum0, want->post_sum1,
		got->post_sum0, got->post_sum1);
}

/* ------------------------------------------------------------------ */
/* Memory IO: the callback lives at the start of the container so the */
/* production callbacks can recover their context from the cb pointer */
/* ------------------------------------------------------------------ */

#define MEMIO_CAP REF_MAX_BYTES

typedef struct mem_io {
	TTA_io_callback cb;	/* must be first */
	TTAuint8 data[MEMIO_CAP];
	size_t pos;
	size_t len;
	size_t chunk_size[256];
	unsigned chunks;
} mem_io;

static TTAint32 mem_read(TTA_io_callback *cb, TTAuint8 *p, TTAuint32 n) {
	mem_io *io = (mem_io *)cb;
	size_t z = io->len - io->pos;
	if (z > n) z = n;
	memset(p, 0, n);
	if (z) memcpy(p, io->data + io->pos, z);
	io->pos += z;
	return (TTAint32)z;
}

static TTAint32 mem_write(TTA_io_callback *cb, TTAuint8 *p, TTAuint32 n) {
	mem_io *io = (mem_io *)cb;
	assert(io->pos + n <= MEMIO_CAP);
	memcpy(io->data + io->pos, p, n);
	io->pos += n;
	if (io->pos > io->len) io->len = io->pos;
	if (io->chunks < 256) io->chunk_size[io->chunks++] = n;
	return (TTAint32)n;
}

static TTAint64 mem_seek(TTA_io_callback *cb, TTAint64 off) {
	mem_io *io = (mem_io *)cb;
	assert(off >= 0 && (size_t)off < MEMIO_CAP);
	io->pos = (size_t)off;
	return off;
}

static mem_io *mem_io_new(void) {
	mem_io *io = (mem_io *)calloc(1, sizeof(mem_io));
	assert(io);
	io->cb.read = mem_read;
	io->cb.write = mem_write;
	io->cb.seek = mem_seek;
	return io;
}

/* ------------------------------------------------------------------ */
/* Production entropy plumbing on raw (container-free) byte streams   */
/* ------------------------------------------------------------------ */

static void prod_encoder_open(mem_io *io) {
	enc_fifo_io = &io->cb;
	writer_start();
	writer_reset();
	encoder_last = encoder;
	rice_init(&encoder[0].rice, 10, 10);
	encoder[0].prev = 0;
}

/* Drain the production bit cache to whole bytes without appending the
 * frame CRC that flush_bit_cache() would add. */
static void prod_encoder_drain_entropy(void) {
	while (enc_fifo_bcount) {
		write_byte(enc_fifo_bcache);
		enc_fifo_bcache >>= 8;
		enc_fifo_bcount = (enc_fifo_bcount > 8)
			? (enc_fifo_bcount - 8) : 0;
	}
}

static void prod_decoder_open(mem_io *io) {
	dec_fifo_io = &io->cb;
	reader_start();
	reader_reset();
	decoder_last = decoder;
	rice_init(&decoder[0].rice, 10, 10);
	decoder[0].prev = 0;
}

/* ================================================================== */
/* Case 1: adaptation thresholds and both cross directions            */
/* ================================================================== */

typedef struct {
	int level1;
	int k0_zero;
	int k_change;
	int equal_threshold;
	int adjacent;
} coverage;

static void ref_scan_coverage(const TTAint32 *seq, unsigned count,
	coverage *cov) {
	ref_rice r;
	ref_bs bs;
	ref_trace t;
	unsigned i;
	ref_rice_reset(&r);
	ref_bs_init(&bs);
	for (i = 0; i < count; i++) {
		ref_rice_encode(&r, seq[i], &bs, &t);
		if (t.level == 1) cov->level1 = 1;
		if (t.pre_k0 == 0 || t.post_k0 == 0 ||
			t.pre_k1 == 0 || t.post_k1 == 0)
			cov->k0_zero = 1;
		if (t.pre_k0 != t.post_k0 || t.pre_k1 != t.post_k1)
			cov->k_change = 1;
		if (t.post_sum0 == REF_THRESH(t.post_k0) ||
			t.post_sum0 == REF_THRESH(t.post_k0 + 1) ||
			t.post_sum1 == REF_THRESH(t.post_k1) ||
			t.post_sum1 == REF_THRESH(t.post_k1 + 1))
			cov->equal_threshold = 1;
		if (t.post_sum0 + 1 == REF_THRESH(t.post_k0) ||
			t.post_sum0 == REF_THRESH(t.post_k0) + 1 ||
			t.post_sum0 + 1 == REF_THRESH(t.post_k0 + 1) ||
			t.post_sum0 == REF_THRESH(t.post_k0 + 1) + 1)
			cov->adjacent = 1;
	}
}

static void cross_check_sequence(const char *tag,
	const TTAint32 *seq, unsigned count) {
	ref_rice ref_enc_st, parser;
	ref_bs ref_enc_bs, parse_bs;
	ref_trace *enc_tr, *parse_tr, *dec_tr;
	unsigned i;

	enc_tr = (ref_trace *)calloc(count, sizeof(ref_trace));
	parse_tr = (ref_trace *)calloc(count, sizeof(ref_trace));
	dec_tr = (ref_trace *)calloc(count, sizeof(ref_trace));
	assert(enc_tr && parse_tr && dec_tr);

	ref_bs_init(&ref_enc_bs);
	ref_rice_reset(&ref_enc_st);
	for (i = 0; i < count; i++)
		ref_rice_encode(&ref_enc_st, seq[i], &ref_enc_bs, &enc_tr[i]);

	/* ---- direction B: production encode, independent parse ---- */
	{
		mem_io *io = mem_io_new();
		prod_encoder_open(io);
		for (i = 0; i < count; i++) {
			CHECK(encoder[0].rice.k0 == enc_tr[i].pre_k0 &&
				encoder[0].rice.k1 == enc_tr[i].pre_k1 &&
				encoder[0].rice.sum0 == enc_tr[i].pre_sum0 &&
				encoder[0].rice.sum1 == enc_tr[i].pre_sum1,
				"seq=%s sym=%u dir=prod-enc pre-state divergence",
				tag, i);
			tta_encoder_put_value(&encoder[0].rice, seq[i]);
			CHECK(encoder[0].rice.k0 == enc_tr[i].post_k0 &&
				encoder[0].rice.k1 == enc_tr[i].post_k1 &&
				encoder[0].rice.sum0 == enc_tr[i].post_sum0 &&
				encoder[0].rice.sum1 == enc_tr[i].post_sum1,
				"seq=%s sym=%u dir=prod-enc post-state divergence",
				tag, i);
		}
		prod_encoder_drain_entropy();
		writer_done();

		CHECK(io->len == ref_bs_bytes(&ref_enc_bs),
			"seq=%s dir=prod-enc byte length ref=%zu got=%zu",
			tag, ref_bs_bytes(&ref_enc_bs), io->len);
		CHECK(memcmp(io->data, ref_enc_bs.buf, io->len) == 0,
			"seq=%s dir=prod-enc output bytes differ", tag);

		memset(parse_bs.buf, 0, sizeof(parse_bs.buf));
		memcpy(parse_bs.buf, io->data, io->len);
		parse_bs.nbits = 0;
		ref_rice_reset(&parser);
		for (i = 0; i < count; i++) {
			ref_rice_decode(&parse_bs, &parser, &parse_tr[i]);
			ref_check_trace("prod-enc", i,
				&enc_tr[i], &parse_tr[i],
				&ref_enc_bs, &parse_bs);
		}
		CHECK(parse_bs.nbits == ref_enc_bs.nbits,
			"seq=%s dir=prod-enc consumed bits ref=%zu got=%zu",
			tag, ref_enc_bs.nbits, parse_bs.nbits);
		free(io);
	}

	/* ---- direction A: independent encode, production decode ---- */
	{
		mem_io *io = mem_io_new();
		io->len = ref_bs_bytes(&ref_enc_bs);
		memcpy(io->data, ref_enc_bs.buf, io->len);
		prod_decoder_open(io);
		ref_rice_reset(&parser);
		ref_bs_init(&parse_bs);
		memcpy(parse_bs.buf, ref_enc_bs.buf, io->len);
		for (i = 0; i < count; i++) {
			TTAint32 got = tta_decoder_get_value(&decoder[0].rice);
			ref_rice_decode(&parse_bs, &parser, &dec_tr[i]);
			CHECK(got == seq[i],
				"seq=%s sym=%u dir=prod-dec residual ref=%d got=%d",
				tag, i, seq[i], got);
			CHECK(decoder[0].rice.k0 == dec_tr[i].post_k0 &&
				decoder[0].rice.k1 == dec_tr[i].post_k1 &&
				decoder[0].rice.sum0 == dec_tr[i].post_sum0 &&
				decoder[0].rice.sum1 == dec_tr[i].post_sum1,
				"seq=%s sym=%u dir=prod-dec post-state divergence",
				tag, i);
			ref_check_trace("prod-dec", i,
				&enc_tr[i], &dec_tr[i],
				&ref_enc_bs, &parse_bs);
		}
		CHECK(parse_bs.nbits == ref_enc_bs.nbits,
			"seq=%s dir=prod-dec consumed bits ref=%zu got=%zu",
			tag, ref_enc_bs.nbits, parse_bs.nbits);
		free(io);
	}

	free(enc_tr);
	free(parse_tr);
	free(dec_tr);
}

int case_thresholds(void) {
	int start = g_checks;
	coverage cov;
	memset(&cov, 0, sizeof(cov));
	unsigned total_symbols = 0;

	/* sequence 1: zeros. sum0 decays geometrically so k0 walks down
	 * to its floor at zero, where it must stay without underflow. */
	{
		enum { N1 = 192 };
		TTAint32 *seq = (TTAint32 *)calloc(N1, sizeof(TTAint32));
		ref_rice r;
		ref_bs bs;
		ref_trace t;
		unsigned i;
		assert(seq);
		ref_rice_reset(&r);
		ref_bs_init(&bs);
		for (i = 0; i < N1; i++) {
			ref_rice_encode(&r, 0, &bs, &t);
			CHECK(r.k0 == t.post_k0, "zero seq k0 bookkeeping");
		}
		CHECK(r.k0 == 0, "zero sequence never reached k0=0");
		ref_scan_coverage(seq, N1, &cov);
		cross_check_sequence("zeros", seq, N1);
		total_symbols += N1;
		free(seq);
	}

	/* sequence 2: strictly sign-alternating residuals, constructed to
	 * hit the adaptation events deliberately: exact equality with the
	 * upper/lower thresholds (no parameter move) and the adjacent
	 * values that cross them (k moves).  When the unsigned value needed
	 * for a pending event has the wrong parity for this position's
	 * sign, the event stays pending for the opposite-sign position and
	 * a small alternating residual is emitted instead. */
	{
		enum { N2 = 256 };
		TTAint32 *seq = (TTAint32 *)calloc(N2, sizeof(TTAint32));
		/* event want queue, cycled until every class is observed */
		enum { EV_EQ_UPPER, EV_OVER, EV_EQ_LOWER, EV_UNDER, EV_COUNT };
		int want = EV_EQ_UPPER;
		int event_seen[EV_COUNT] = {0};
		ref_rice sim;
		ref_bs scratch;
		ref_trace t;
		unsigned n = 0, i;
		ref_rice_reset(&sim);
		ref_bs_init(&scratch);

		for (i = 0; i < N2; i++) {
			int negative = i & 1;
			TTAuint32 target = 0, u;
			int feasible = 0;

			switch (want) {
			case EV_EQ_UPPER:
				target = REF_THRESH(sim.k0 + 1);
				break;
			case EV_OVER:
				target = REF_THRESH(sim.k0 + 1) + 1;
				break;
			case EV_EQ_LOWER:
				target = REF_THRESH(sim.k0);
				break;
			default: /* EV_UNDER */
				target = REF_THRESH(sim.k0) - 1;
				break;
			}

			/* decay zeros cannot share an alternating sign slot, so
			 * only reach targets attainable with one unsigned input */
			if (target + (sim.sum0 >> 4) >= sim.sum0) {
				u = target - sim.sum0 + (sim.sum0 >> 4);
				/* a positive residual maps to an odd unsigned value,
				 * a negative one to an even value */
				unsigned want_odd = negative ? 0u : 1u;
				if ((u & 1u) == want_odd)
					feasible = 1;
			}

			if (!feasible) {
				/* keep the strict alternation with a small residual */
				seq[n++] = negative ? -1 : 1;
				ref_rice_encode(&sim, seq[n - 1], &scratch, &t);
				continue;
			}

			seq[n++] = negative ? -(TTAint32)(u >> 1)
			                    : (TTAint32)((u >> 1) + 1);
			ref_rice_encode(&sim, seq[n - 1], &scratch, &t);

			switch (want) {
			case EV_EQ_UPPER:
				if (sim.sum0 == REF_THRESH(sim.k0 + 1))
					event_seen[EV_EQ_UPPER] = 1;
				break;
			case EV_OVER:
				if (t.pre_k0 + 1 == t.post_k0 &&
					sim.sum0 == REF_THRESH(t.pre_k0 + 1) + 1)
					event_seen[EV_OVER] = 1;
				break;
			case EV_EQ_LOWER:
				if (sim.sum0 == REF_THRESH(sim.k0))
					event_seen[EV_EQ_LOWER] = 1;
				break;
			default:
				if (sim.k0 > 0 && t.pre_k0 == t.post_k0 + 1 &&
					sim.sum0 == REF_THRESH(t.pre_k0) - 1)
					event_seen[EV_UNDER] = 1;
				break;
			}
			want = (want + 1) % EV_COUNT;
		}

		for (i = 0; i < EV_COUNT; i++)
			CHECK(event_seen[i],
				"alternating sequence missed threshold event class %d", i);
		/* strict sign alternation of the finished sequence */
		for (i = 1; i < n; i++)
			CHECK((seq[i] > 0) != (seq[i - 1] > 0),
				"alternating sequence sign broken at %u", i);
		CHECK(n >= 200, "threshold sequence too short: %u", n);
		ref_scan_coverage(seq, n, &cov);
		cross_check_sequence("thresholds", seq, n);
		total_symbols += n;
		free(seq);
	}

	/* sequence 3: low-amplitude decay followed by a sudden high
	 * amplitude burst, forcing level 0 -> level 1 switches, k=0 work
	 * and unary runs spanning multiple 23-bit slices. */
	{
		enum { N3 = 240 };
		TTAint32 *seq = (TTAint32 *)calloc(N3, sizeof(TTAint32));
		ref_rice r;
		ref_bs bs;
		ref_trace t;
		unsigned i, n = 0;
		int long_unary = 0;
		ref_rice_reset(&r);
		ref_bs_init(&bs);

		for (i = 0; i < 140; i++)
			seq[n++] = (i % 3 == 0) ? 0 : (TTAint32)((i % 7) - 3);
		for (i = 0; i < 100; i++) {
			TTAint32 big = (TTAint32)(1500000u + i * 71000u);
			seq[n++] = (i & 1) ? big : -big;
		}
		for (i = 0; i < n; i++) {
			ref_rice_encode(&r, seq[i], &bs, &t);
			if (t.level == 1 && t.unary > 23) long_unary = 1;
		}
		CHECK(long_unary, "burst sequence never exceeded 23 unary bits");
		ref_scan_coverage(seq, n, &cov);
		cross_check_sequence("burst", seq, n);
		total_symbols += n;
		free(seq);
	}

	CHECK(cov.level1, "no sequence exercised level 1");
	CHECK(cov.k0_zero, "no sequence exercised k = 0");
	CHECK(cov.k_change, "no symbol used different old vs new k");
	CHECK(cov.equal_threshold, "no symbol landed exactly on a threshold");
	CHECK(cov.adjacent, "no symbol used a value adjacent to a threshold");

	printf("TARGET_STATS case=thresholds symbols=%u level1=%d "
		"k_floor=%d k_changes=%d equal_threshold=%d adjacent=%d\n",
		total_symbols, cov.level1, cov.k0_zero, cov.k_change,
		cov.equal_threshold, cov.adjacent);

	return g_checks - start;
}

/* ================================================================== */
/* Case 2: 23-bit unary slices, 8-bit flushes and the 5120-byte FIFO  */
/* ================================================================== */

/* Reference FIFO: whole bytes only, flushed through the callback in
 * chunks of TTA_FIFO_BUFFER_SIZE, mirroring the production writer. */
typedef struct {
	mem_io *sink;
	TTAuint8 cache[TTA_FIFO_BUFFER_SIZE];
	size_t cache_bytes;
	size_t total_bytes;
} ref_fifo;

static void ref_fifo_byte(ref_fifo *f, TTAuint8 byte) {
	f->cache[f->cache_bytes++] = byte;
	if (f->cache_bytes == TTA_FIFO_BUFFER_SIZE) {
		assert(mem_write(&f->sink->cb, f->cache,
			(TTAuint32)f->cache_bytes) == (TTAint32)f->cache_bytes);
		f->cache_bytes = 0;
	}
	f->total_bytes++;
}

static void ref_fifo_flush(ref_fifo *f) {
	if (f->cache_bytes) {
		assert(mem_write(&f->sink->cb, f->cache,
			(TTAuint32)f->cache_bytes) == (TTAint32)f->cache_bytes);
		f->cache_bytes = 0;
	}
}

int case_fifo(void) {
	int start = g_checks;
	enum { N_MAX = 8000 };
	TTAint32 *seq = (TTAint32 *)calloc(N_MAX, sizeof(TTAint32));
	ref_rice r;
	ref_bs ref_plain;
	ref_trace t;
	unsigned i, n = 0;
	int straddle = 0;
	unsigned long_sym = UINT_MAX;
	const size_t cut_bits = (size_t)TTA_FIFO_BUFFER_SIZE * 8;
	mem_io *ref_sink, *prod_sink;
	ref_fifo f;

	assert(seq);
	ref_rice_reset(&r);
	ref_bs_init(&ref_plain);

	/* phase A: large alternating residuals grow the bitstream to
	 * within 200 bits of the first 5120-byte FIFO cut. */
	for (i = 0; i < N_MAX; i++) {
		TTAint32 v = (TTAint32)(500000u +
			(i * 2654435761u) % 9000000u);
		seq[n] = (i & 1) ? v : -v;
		ref_rice_encode(&r, seq[n], &ref_plain, &t);
		n++;
		if (ref_plain.nbits + 200 >= cut_bits)
			break;
	}
	CHECK(ref_plain.nbits < cut_bits,
		"phase A overshot the FIFO cut at %zu bits", ref_plain.nbits);

	/* phase B: force one level-1 symbol whose unary run (well over 23
	 * bits) is constructed to straddle the cut.  With pre-update k1,
	 * unary = 1 + (v >> k1); the run must fit in a uint32 at k1. */
	{
		TTAuint32 need_unary =
			(TTAuint32)(cut_bits - ref_plain.nbits) + 32u;
		TTAuint32 max_unary = (0xffffffffu >> r.k1) + 1u;
		TTAuint32 v;
		TTAuint64 u64;
		CHECK(need_unary > 23, "forced unary %u not above 23",
			need_unary);
		CHECK(need_unary <= max_unary,
			"forced unary %u exceeds k1=%u limit %u",
			need_unary, r.k1, max_unary);
		v = (need_unary - 1u) << r.k1;	/* zero remainder */
		u64 = (TTAuint64)ref_pow2(r.k0) + v;
		CHECK(u64 <= 0xffffffffu, "forced unsigned value overflow");
		CHECK(n < N_MAX, "FIFO sequence capacity exhausted");
		seq[n] = ref_map_dec((TTAuint32)u64);
		ref_rice_encode(&r, seq[n], &ref_plain, &t);
		CHECK(t.level == 1 && t.unary > 23,
			"forced symbol level=%d unary=%u", t.level, t.unary);
		long_sym = n;
		CHECK(t.start < cut_bits && t.start + t.nbits > cut_bits,
			"forced symbol start=%zu len=%u misses cut=%zu",
			t.start, t.nbits, cut_bits);
		straddle = 1;
		n++;
	}

	/* phase C: ordinary symbols so the stream continues well past the
	 * cut and ends on a natural tail. */
	for (i = 0; i < 400 && n < N_MAX; i++) {
		TTAint32 v = (TTAint32)(1000u + (i * 48611u) % 200000u);
		seq[n] = (i & 1) ? v : -v;
		ref_rice_encode(&r, seq[n], &ref_plain, &t);
		n++;
	}

	CHECK(straddle,
		"no >23-unary symbol straddled a FIFO boundary in %u symbols", n);
	CHECK(long_sym != UINT_MAX, "no symbol with unary > 23");

	/* independent side: stream whole bytes through reference FIFO */
	ref_sink = mem_io_new();
	memset(&f, 0, sizeof(f));
	f.sink = ref_sink;
	{
		size_t bit, nbytes = ref_bs_bytes(&ref_plain);
		for (bit = 0; bit < nbytes; bit++)
			ref_fifo_byte(&f, ref_plain.buf[bit]);
		ref_fifo_flush(&f);
	}
	/* zero padding of the sub-byte tail */
	if (ref_plain.nbits & 7) {
		TTAuint8 last = ref_plain.buf[ref_bs_bytes(&ref_plain) - 1];
		TTAuint8 used = (TTAuint8)((1u << (ref_plain.nbits & 7)) - 1u);
		CHECK((last & (TTAuint8)~used) == 0,
			"non-zero tail padding 0x%02x for %zu valid bits",
			last, ref_plain.nbits & 7);
	}
	CHECK(f.total_bytes == ref_bs_bytes(&ref_plain),
		"ref FIFO byte accounting error");
	CHECK(ref_sink->len > TTA_FIFO_BUFFER_SIZE,
		"stream %zu bytes never crossed the %d-byte FIFO cut",
		ref_sink->len, TTA_FIFO_BUFFER_SIZE);
	CHECK(ref_sink->chunks >= 2, "expected >= 2 FIFO chunks, got %u",
		ref_sink->chunks);
	for (i = 0; i + 1 < ref_sink->chunks; i++)
		CHECK(ref_sink->chunk_size[i] == TTA_FIFO_BUFFER_SIZE,
			"ref FIFO chunk %u size %zu != %d",
			i, ref_sink->chunk_size[i], TTA_FIFO_BUFFER_SIZE);
	CHECK(ref_sink->chunk_size[ref_sink->chunks - 1] ==
		ref_sink->len % TTA_FIFO_BUFFER_SIZE,
		"ref final FIFO chunk size mismatch");

	/* production side: real encoder, same sequence */
	prod_sink = mem_io_new();
	prod_encoder_open(prod_sink);
	{
		ref_rice pr;
		ref_bs pbs;
		ref_trace pt;
		ref_rice_reset(&pr);
		ref_bs_init(&pbs);
		for (i = 0; i < n; i++) {
			unsigned bi;
			ref_rice_encode(&pr, seq[i], &pbs, &pt);
			tta_encoder_put_value(&encoder[0].rice, seq[i]);
			/* exact bookkeeping: flushed bytes plus the bits still in
			 * the cache must equal the reference bit count at every
			 * symbol boundary */
			CHECK((size_t)enc_fifo_count * 8u + enc_fifo_bcount
				== pbs.nbits,
				"sym=%u bit accounting ref=%zu got=%u+%u",
				i, pbs.nbits, enc_fifo_count * 8u, enc_fifo_bcount);
			/* every residual cache bit must match the reference tail */
			for (bi = 0; bi < enc_fifo_bcount; bi++) {
				size_t pos = pbs.nbits - enc_fifo_bcount + bi;
				unsigned ref_bit =
					(pbs.buf[pos >> 3] >> (pos & 7)) & 1u;
				unsigned got_bit =
					(enc_fifo_bcache >> bi) & 1u;
				CHECK(ref_bit == got_bit,
					"sym=%u cache bit %u differs", i, bi);
			}
		}
	}
	prod_encoder_drain_entropy();
	writer_done();

	CHECK(prod_sink->len == ref_sink->len,
		"FIFO length production=%zu reference=%zu",
		prod_sink->len, ref_sink->len);
	CHECK(prod_sink->chunks == ref_sink->chunks,
		"FIFO chunk count production=%u reference=%u",
		prod_sink->chunks, ref_sink->chunks);
	for (i = 0; i < ref_sink->chunks; i++)
		CHECK(prod_sink->chunk_size[i] == ref_sink->chunk_size[i],
			"FIFO chunk %u size production=%zu reference=%zu",
			i, prod_sink->chunk_size[i], ref_sink->chunk_size[i]);
	CHECK(memcmp(prod_sink->data, ref_sink->data, ref_sink->len) == 0,
		"FIFO payload differs between production and reference");

	/* independent parse across the FIFO cut */
	{
		ref_bs parse;
		ref_rice parser;
		ref_trace pt;
		memset(parse.buf, 0, sizeof(parse.buf));
		memcpy(parse.buf, ref_sink->data, ref_sink->len);
		parse.nbits = 0;
		ref_rice_reset(&parser);
		for (i = 0; i < n; i++) {
			TTAint32 got = ref_rice_decode(&parse, &parser, &pt);
			CHECK(got == seq[i],
				"ref parse sym=%u residual ref=%d got=%d",
				i, seq[i], got);
		}
		CHECK(parse.nbits == ref_plain.nbits,
			"ref parse consumed %zu of %zu bits",
			parse.nbits, ref_plain.nbits);
	}

	/* production decode across the same cut */
	{
		mem_io *in = mem_io_new();
		in->len = ref_sink->len;
		memcpy(in->data, ref_sink->data, ref_sink->len);
		prod_decoder_open(in);
		for (i = 0; i < n; i++) {
			TTAint32 got = tta_decoder_get_value(&decoder[0].rice);
			CHECK(got == seq[i],
				"prod decode sym=%u residual ref=%d got=%d",
				i, seq[i], got);
		}
		free(in);
	}

	/* tail valid-bit sweep: short frames ending on each reachable
	 * padding residue; check zero padding, independent CRC and the
	 * exact consumed position through the public frame entry. */
	{
		unsigned try_len;
		int residues_seen[8] = {0};
		int distinct = 0;
		for (try_len = 1; try_len <= 64; try_len++) {
			mem_io *frame = mem_io_new();
			ref_bs fbs, parse;
			ref_rice fr, pr;
			ref_trace ft, ptt;
			size_t entropy_bits, entropy_bytes;
			TTAuint32 want_crc, got_crc;
			unsigned pad, j;

			prod_encoder_open(frame);
			ref_bs_init(&fbs);
			ref_rice_reset(&fr);
			for (j = 0; j < try_len; j++) {
				TTAint32 v = (TTAint32)(1 +
					(j * 7919 + try_len * 31) % 5000);
				v = (j & 1) ? v : -v;
				ref_rice_encode(&fr, v, &fbs, &ft);
				tta_encoder_put_value(&encoder[0].rice, v);
			}
			flush_bit_cache();	/* pad byte + CRC32 */
			writer_done();

			entropy_bits = fbs.nbits;
			entropy_bytes = (entropy_bits + 7) >> 3;
			pad = (unsigned)((8 - (entropy_bits & 7)) & 7);
			residues_seen[entropy_bits & 7] = 1;

			CHECK(frame->len == entropy_bytes + 4,
				"tail sweep len=%u frame bytes %zu != %zu+4",
				try_len, frame->len, entropy_bytes);
			if (pad) {
				TTAuint8 used =
					(TTAuint8)((1u << (entropy_bits & 7)) - 1u);
				CHECK((frame->data[entropy_bytes - 1] &
					(TTAuint8)~used) == 0,
					"tail sweep len=%u padding not zero", try_len);
			}
			want_crc = ref_crc32(frame->data, entropy_bytes);
			got_crc = ref_rd_u32(frame->data + entropy_bytes);
			CHECK(got_crc == want_crc,
				"tail sweep len=%u CRC ref=%08x got=%08x",
				try_len, want_crc, got_crc);

			/* independent parse stops exactly at the entropy edge */
			memset(parse.buf, 0, sizeof(parse.buf));
			memcpy(parse.buf, frame->data, entropy_bytes);
			parse.nbits = 0;
			ref_rice_reset(&pr);
			for (j = 0; j < try_len; j++)
				ref_rice_decode(&parse, &pr, &ptt);
			CHECK(parse.nbits == entropy_bits,
				"tail sweep len=%u consumed bits %zu != %zu",
				try_len, parse.nbits, entropy_bits);

			/* production getter must recover every residual and end in
			 * exactly the same adapted state as the independent parser;
			 * the byte reader may legitimately prefetch past the
			 * entropy edge into its bit cache, so the exact consumed
			 * position is established by the independent parse above
			 * (parse.nbits == entropy_bits) and by state equality here.
			 * The full process_frame/CRC entry is exercised on real
			 * frames in case_frames. */
			{
				mem_io *in = mem_io_new();
				in->len = frame->len;
				memcpy(in->data, frame->data, frame->len);
				prod_decoder_open(in);
				for (j = 0; j < try_len; j++) {
					TTAint32 got =
						tta_decoder_get_value(&decoder[0].rice);
					TTAint32 want = (TTAint32)(1 +
						(j * 7919 + try_len * 31) % 5000);
					want = (j & 1) ? want : -want;
					CHECK(got == want,
						"tail sweep len=%u sym=%u ref=%d got=%d",
						try_len, j, want, got);
				}
				CHECK(decoder[0].rice.k0 == pr.k0 &&
					decoder[0].rice.k1 == pr.k1 &&
					decoder[0].rice.sum0 == pr.sum0 &&
					decoder[0].rice.sum1 == pr.sum1,
					"tail sweep len=%u final adapted state differs",
					try_len);
				free(in);
			}
			free(frame);
		}
		for (i = 0; i < 8; i++) distinct += residues_seen[i];
		CHECK(distinct >= 7,
			"tail sweep only covered %d bit residues", distinct);
	}

	printf("TARGET_STATS case=fifo symbols=%u fifo_cut=%d chunks=%u "
		"long_unary_sym=%u stream_bytes=%zu\n",
		n, TTA_FIFO_BUFFER_SIZE, ref_sink->chunks,
		long_sym, ref_sink->len);

	free(seq);
	free(ref_sink);
	free(prod_sink);
	return g_checks - start;
}

/* ================================================================== */
/* Case 3: public 16/24-bit, 1..6 channel frame entries, with an      */
/* independent parse of header, seek table, frame lengths and CRCs    */
/* ================================================================== */

#define CASE3_SPS 245u
#define CASE3_FLEN 256u		/* MUL_FRAME_TIME(245) */
#define CASE3_TAIL 37u
#define CASE3_SAMPLES (2u * CASE3_FLEN + CASE3_TAIL)
#define CASE3_FRAMES 3u

static TTAint32 pcm_value(unsigned i, unsigned ch, unsigned bps) {
	TTAuint32 span = (bps == 16) ? 60000u : 14000000u;
	TTAuint32 mix = (i * 1103515245u + ch * 2654435761u +
		bps * 40503u) % span;
	TTAint32 v = (TTAint32)mix - (TTAint32)(span / 2);
	/* channel-specific component guarantees L != R */
	v += (TTAint32)((ch + 1) * 13) * (TTAint32)((i % 97) - 48);
	if (bps == 16) {
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
	} else {
		if (v > 8388607) v = 8388607;
		if (v < -8388608) v = -8388608;
	}
	return v;
}

static void pack_sample(TTAuint8 *p, TTAint32 v, unsigned depth) {
	TTAuint32 u = (TTAuint32)v;
	p[0] = (TTAuint8)u;
	p[1] = (TTAuint8)(u >> 8);
	if (depth == 3) p[2] = (TTAuint8)(u >> 16);
}

int case_frames(void) {
	int start = g_checks;
	unsigned bps, nch;
	unsigned total_frames = 0;
	size_t total_symbols = 0;
	size_t total_fifo_flushes = 0;
	int stereo_diff_checked = 0;

	for (bps = 16; bps <= 24; bps += 8) {
		for (nch = 1; nch <= 6; nch++) {
			unsigned depth = bps / 8;
			size_t valid_bytes =
				(size_t)CASE3_SAMPLES * nch * depth;
			/* spare tail: the existing stream contract reads one
			 * sample past the buffer; keep the traditional extra
			 * writable space for the 24-bit path as well. */
			size_t cap = valid_bytes + (size_t)nch * depth + 16;
			TTAuint8 *input = (TTAuint8 *)calloc(1, cap);
			TTAuint8 *output = (TTAuint8 *)calloc(1, cap);
			mem_io *io = mem_io_new();
			TTA_info info, got_info;
			unsigned s, c;
			size_t off;
			assert(input && output);

			for (s = 0; s < CASE3_SAMPLES; s++)
				for (c = 0; c < nch; c++)
					pack_sample(
						input + ((size_t)s * nch + c) * depth,
						pcm_value(s, c, bps), depth);
			if (nch >= 2) {
				int diff = 0;
				for (s = 0; s < CASE3_SAMPLES; s++)
					if (pcm_value(s, 0, bps) != pcm_value(s, 1, bps))
						diff = 1;
				CHECK(diff, "bps=%u nch=%u channels 0/1 identical",
					bps, nch);
				stereo_diff_checked = 1;
			}

			/* public encode entry */
			tta_encoder_new(&io->cb);
			info.format = 1; info.nch = nch; info.bps = bps;
			info.sps = CASE3_SPS; info.samples = CASE3_SAMPLES;
			CHECK(tta_encoder_init_set_info(&info, 0) == 0,
				"bps=%u nch=%u encoder init failed", bps, nch);
			CHECK(tta_encoder_process_stream(input,
				(TTAuint32)valid_bytes, 0) == 0,
				"bps=%u nch=%u encode failed", bps, nch);
			CHECK(tta_encoder_finalize() == 0,
				"bps=%u nch=%u finalize failed", bps, nch);
			tta_encoder_done();

			/* ---- independent container parse ---- */
			CHECK(io->len > 22u + (CASE3_FRAMES + 1) * 4,
				"container too small");
			CHECK(memcmp(io->data, "TTA1", 4) == 0,
				"bad TTA1 signature");
			CHECK(ref_rd_u16(io->data + 4) == 1, "bad format field");
			CHECK(ref_rd_u16(io->data + 6) == nch, "bad nch field");
			CHECK(ref_rd_u16(io->data + 8) == bps, "bad bps field");
			CHECK(ref_rd_u32(io->data + 10) == CASE3_SPS,
				"bad sps field");
			CHECK(ref_rd_u32(io->data + 14) == CASE3_SAMPLES,
				"bad samples field");
			CHECK(ref_crc32(io->data, 18) == ref_rd_u32(io->data + 18),
				"bps=%u nch=%u header CRC mismatch", bps, nch);

			{
				size_t seek_off = 22;
				size_t data_off =
					22 + (size_t)(CASE3_FRAMES + 1) * 4;
				TTAuint32 f;

				CHECK(ref_crc32(io->data + seek_off,
					(size_t)CASE3_FRAMES * 4) ==
					ref_rd_u32(io->data + seek_off +
						(size_t)CASE3_FRAMES * 4),
					"bps=%u nch=%u seek table CRC mismatch", bps, nch);

				off = data_off;
				for (f = 0; f < CASE3_FRAMES; f++) {
					TTAuint32 flen =
						ref_rd_u32(io->data + seek_off + f * 4);
					unsigned expect_samples =
						(f + 1 < CASE3_FRAMES) ?
						CASE3_FLEN : CASE3_TAIL;
					ref_bs fbs;
					ref_rice st[MAX_NCH];
					ref_trace tr;
					unsigned z, ch2;
					size_t entropy_bits;

					CHECK(flen > 4, "frame %u too short", f);
					CHECK(off + flen <= io->len,
						"frame %u runs past container end", f);
					CHECK(ref_crc32(io->data + off, flen - 4) ==
						ref_rd_u32(io->data + off + flen - 4),
						"bps=%u nch=%u frame %u CRC mismatch",
						bps, nch, f);

					/* independent entropy parse; Rice state is rebuilt
					 * per channel for every frame, never carried over
					 * from the previous frame or another channel. */
					memset(&fbs, 0, sizeof(fbs));
					memcpy(fbs.buf, io->data + off, flen - 4);
					fbs.nbits = 0;
					for (ch2 = 0; ch2 < nch; ch2++)
						ref_rice_reset(&st[ch2]);
					for (z = 0; z < expect_samples; z++)
						for (ch2 = 0; ch2 < nch; ch2++) {
							ref_rice_decode(&fbs, &st[ch2], &tr);
							total_symbols++;
						}
					entropy_bits = (size_t)(flen - 4) * 8;
					CHECK(fbs.nbits <= entropy_bits &&
						entropy_bits - fbs.nbits < 8,
						"bps=%u nch=%u frame %u consumed %zu of %zu bits",
						bps, nch, f, fbs.nbits, entropy_bits);
					/* the sub-byte residue is byte-alignment padding and
					 * must be all zero */
					while (fbs.nbits < entropy_bits)
						CHECK(ref_get_bit(&fbs) == 0,
							"bps=%u nch=%u frame %u non-zero padding bit",
							bps, nch, f);
					CHECK(fbs.nbits == entropy_bits,
						"bps=%u nch=%u frame %u padding accounting",
						bps, nch, f);
					off += flen;
				}
				CHECK(off == io->len,
					"bps=%u nch=%u container has %zu trailing bytes",
					bps, nch, io->len - off);
				total_frames += CASE3_FRAMES;
				total_fifo_flushes += io->chunks;
			}

			/* public decode entry: sample-exact PCM recovery, driven
			 * one frame at a time so per-frame state rebuild can be
			 * observed. */
			io->pos = 0;
			tta_decoder_new(&io->cb);
			CHECK(tta_decoder_init_get_info(&got_info) == 0,
				"bps=%u nch=%u decoder init failed", bps, nch);
			CHECK(got_info.nch == nch && got_info.bps == bps &&
				got_info.samples == CASE3_SAMPLES,
				"bps=%u nch=%u decoded info mismatch", bps, nch);
			{
				int got_total = 0;
				size_t got_bytes = 0;
				while ((unsigned)got_total < CASE3_SAMPLES) {
					TTAuint32 budget =
						(TTAuint32)(cap - got_bytes);
					int n = tta_decoder_process_stream(
						output + got_bytes, budget, 0);
					if (n <= 0)
						mismatch("bps=%u nch=%u decoder stalled at %d",
							bps, nch, got_total);
					got_total += n;
					got_bytes += (size_t)n * nch * depth;

					/* after each frame boundary the library must have
					 * rebuilt fresh Rice state for every channel. */
					if ((unsigned)got_total < CASE3_SAMPLES) {
						for (c = 0; c < nch; c++) {
							CHECK(decoder[c].rice.k0 == 10 &&
								decoder[c].rice.k1 == 10 &&
								decoder[c].rice.sum0 ==
									REF_THRESH(10) &&
								decoder[c].rice.sum1 ==
									REF_THRESH(10),
								"bps=%u nch=%u ch=%u frame state "
								"not rebuilt", bps, nch, c);
						}
					}
				}
				CHECK((unsigned)got_total == CASE3_SAMPLES,
					"bps=%u nch=%u decoded %d samples",
					bps, nch, got_total);
				/* only the valid sample bytes are compared; spare
				 * tail space is outside the contract. */
				CHECK(memcmp(input, output, valid_bytes) == 0,
					"bps=%u nch=%u PCM differs over %zu valid bytes",
					bps, nch, valid_bytes);
			}
			tta_decoder_done();

			free(input);
			free(output);
			free(io);
		}
	}

	CHECK(stereo_diff_checked, "no stereo configuration checked");
	CHECK(total_frames == 12u * CASE3_FRAMES,
		"frame count %u != %u", total_frames, 12u * CASE3_FRAMES);

	printf("TARGET_STATS case=frames combos=12 frames=%u symbols=%zu "
		"fifo_flush_calls=%zu samples_per_combo=%u\n",
		total_frames, total_symbols, total_fifo_flushes,
		CASE3_SAMPLES);

	return g_checks - start;
}
