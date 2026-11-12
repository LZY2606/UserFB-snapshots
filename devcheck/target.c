/*
 * devcheck/target.c - KT128 conformance and lifecycle checks.
 *
 * The library is checked against an independent, self-contained reference
 * written straight from devcheck/kt128-contract.txt: it has its own
 * Keccak-p[1600,12], materialises the whole S = M||C||E(length(C)), and
 * assembles the tree root itself, so tree-mode results are not vouched
 * for by two copies of the same streaming code.
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libkeccak.h"
#include "libkeccak/k12.h"
#include "kt128-vectors.h"


/* ------------------------------------------------------------------ */
/* Independent reference implementation                               */
/* ------------------------------------------------------------------ */

static const uint64_t ref_rc[12] = {
	UINT64_C(0x000000008000808B), UINT64_C(0x800000000000008B),
	UINT64_C(0x8000000000008089), UINT64_C(0x8000000000008003),
	UINT64_C(0x8000000000008002), UINT64_C(0x8000000000000080),
	UINT64_C(0x000000000000800A), UINT64_C(0x800000008000000A),
	UINT64_C(0x8000000080008081), UINT64_C(0x8000000000008080),
	UINT64_C(0x0000000080000001), UINT64_C(0x8000000080008008)
};

/* rho offsets, indexed [x][y] in the standard Keccak coordinates. */
static const int ref_rho[5][5] = {
	{ 0, 36,  3, 41, 18},
	{ 1, 44, 10, 45,  2},
	{62,  6, 43, 15, 61},
	{28, 55, 25, 21, 56},
	{27, 20, 39,  8, 14}
};

static uint64_t
ref_rotl(uint64_t x, int n)
{
	return n ? ((x << n) | (x >> (64 - n))) : x;
}

/* Keccak-p[1600,12] on lanes stored at A[x + 5*y] (natural coordinates). */
static void
ref_keccakp(uint64_t A[25])
{
	int round, x, y;

	for (round = 0; round < 12; round++) {
		uint64_t C[5], D[5], B[25];

		for (x = 0; x < 5; x++)
			C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
		for (x = 0; x < 5; x++)
			D[x] = C[(x + 4) % 5] ^ ref_rotl(C[(x + 1) % 5], 1);
		for (y = 0; y < 5; y++)
			for (x = 0; x < 5; x++)
				B[y + 5 * ((2 * x + 3 * y) % 5)] =
				    ref_rotl(A[x + 5 * y] ^ D[x], ref_rho[x][y]);
		for (y = 0; y < 5; y++)
			for (x = 0; x < 5; x++)
				A[x + 5 * y] = B[x + 5 * y] ^
				    (~B[((x + 1) % 5) + 5 * y] & B[((x + 2) % 5) + 5 * y]);
		A[0] ^= ref_rc[round];
	}
}

static uint64_t
ref_load64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static void
ref_store64(unsigned char *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

/* Whole-buffer TurboSHAKE128. */
static void
ref_turbo(const unsigned char *X, size_t len, unsigned char domain,
          unsigned char *out, size_t outlen)
{
	uint64_t A[25];
	unsigned char block[168];
	size_t i, lane;

	memset(A, 0, sizeof(A));

	while (len >= 168) {
		for (lane = 0; lane < 21; lane++)
			A[(lane % 5) + 5 * (lane / 5)] ^= ref_load64(X + 8 * lane);
		ref_keccakp(A);
		X += 168;
		len -= 168;
	}

	memset(block, 0, sizeof(block));
	memcpy(block, X, len);
	block[len] = domain;
	block[167] ^= 0x80;
	for (lane = 0; lane < 21; lane++)
		A[(lane % 5) + 5 * (lane / 5)] ^= ref_load64(block + 8 * lane);
	ref_keccakp(A);

	while (outlen) {
		size_t take = outlen < 168 ? outlen : 168;
		for (i = 0; i < 21; i++)
			ref_store64(block + 8 * i, A[(i % 5) + 5 * (i / 5)]);
		memcpy(out, block, take);
		out += take;
		outlen -= take;
		if (outlen)
			ref_keccakp(A);
	}
}

/* E(x): minimal big-endian bytes then their count; E(0) is one zero byte. */
static size_t
ref_encode_e(unsigned char *out, size_t value)
{
	unsigned char be[8];
	size_t n = 0, i;

	if (value == 0) {
		out[0] = 0;
		return 1;
	}
	while (value) {
		be[n++] = (unsigned char)value;
		value >>= 8;
	}
	for (i = 0; i < n; i++)
		out[i] = be[n - 1 - i];
	out[n] = (unsigned char)n;
	return n + 1;
}

/* Independent KT128 over fully materialised inputs. */
static void
ref_k12(const unsigned char *M, size_t mlen,
        const unsigned char *C, size_t clen,
        unsigned char *out, size_t outlen)
{
	unsigned char encoded[9];
	unsigned char *S, *F;
	size_t elen, slen, k, fp, i;

	elen = ref_encode_e(encoded, clen);
	slen = mlen + clen + elen;
	S = malloc(slen ? slen : 1);
	memcpy(S, M, mlen);
	memcpy(S + mlen, C, clen);
	memcpy(S + mlen + clen, encoded, elen);

	if (slen <= 8192) {
		ref_turbo(S, slen, 0x07, out, outlen);
		free(S);
		return;
	}

	k = (slen - 1) / 8192; /* pieces after S0 */
	F = malloc(8192 + 8 + k * 32 + 9 + 2);
	memcpy(F, S, 8192);
	fp = 8192;
	F[fp++] = 0x03;
	for (i = 0; i < 7; i++)
		F[fp++] = 0x00;

	for (i = 1; i <= k; i++) {
		size_t off = i * 8192;
		size_t n = slen - off;
		if (n > 8192)
			n = 8192;
		ref_turbo(S + off, n, 0x0b, F + fp, 32);
		fp += 32;
	}

	fp += ref_encode_e(F + fp, k);
	F[fp++] = 0xff;
	F[fp++] = 0xff;
	ref_turbo(F, fp, 0x06, out, outlen);

	free(F);
	free(S);
}


/* ------------------------------------------------------------------ */
/* Test helpers                                                        */
/* ------------------------------------------------------------------ */

static void
fill_pattern(unsigned char *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(i % 251);
}

/* Output buffer with guard bytes on both sides: the usable region is
 * base+1 .. base+n, with sentinels at base[0] and base[n+2]. */
static unsigned char *
guard_alloc(size_t n, unsigned char **base_out)
{
	unsigned char *base = malloc(n + 3);
	base[0] = 0xA5;
	base[n + 2] = 0x5A;
	*base_out = base;
	return base + 1;
}

static int
guard_intact(const unsigned char *p, size_t n)
{
	return p[-1] == 0xA5 && p[n + 1] == 0x5A;
}

/*
 * Drive the streaming library with the given message split schedule
 * (chunk sizes that must sum to mlen; NULL means one update).
 */
static void
run_library(const unsigned char *m, size_t mlen,
            const unsigned char *c, size_t clen,
            const size_t *splits, size_t nsplits,
            unsigned char *out, size_t outlen)
{
	struct libkeccak_k12 *ctx = libkeccak_k12_create();
	size_t i, off = 0;

	CHECK_CHECK(ctx != NULL);

	if (!splits) {
		CHECK_CHECK(libkeccak_k12_update(ctx, m, mlen) == 0);
	} else {
		for (i = 0; i < nsplits; i++) {
			CHECK_CHECK(libkeccak_k12_update(ctx, m + off, splits[i]) == 0);
			off += splits[i];
		}
		CHECK_CHECK(off == mlen);
	}

	CHECK_CHECK(libkeccak_k12_final(ctx, c, clen) == 0);
	CHECK_CHECK(libkeccak_k12_squeeze(ctx, out, outlen) == 0);
	libkeccak_k12_destroy(ctx);
}

static void
check_matches_reference(const unsigned char *m, size_t mlen,
                        const unsigned char *c, size_t clen,
                        const size_t *splits, size_t nsplits,
                        size_t outlen)
{
	unsigned char *base_a, *base_b;
	unsigned char *a = guard_alloc(outlen, &base_a);
	unsigned char *b = guard_alloc(outlen, &base_b);

	run_library(m, mlen, c, clen, splits, nsplits, a, outlen);
	ref_k12(m, mlen, c, clen, b, outlen);

	CHECK_CHECK(memcmp(a, b, outlen) == 0);
	CHECK_CHECK(guard_intact(a, outlen));
	CHECK_CHECK(guard_intact(b, outlen));
	free(base_a);
	free(base_b);
}

static void
make_splits(size_t total, size_t chunk, size_t *splits, size_t *nsplits)
{
	size_t n = 0, left = total;
	while (left > chunk) {
		splits[n++] = chunk;
		left -= chunk;
	}
	if (left)
		splits[n++] = left;
	*nsplits = n;
}


/* ------------------------------------------------------------------ */
/* Group: vectors                                                      */
/* ------------------------------------------------------------------ */

static void
group_vectors(void)
{
	unsigned char want[32], got_base_guard[35];
	unsigned char *got = got_base_guard + 1;
	size_t i, j;
	size_t splits[64], nsplits;

	got_base_guard[0] = 0xA5;
	got_base_guard[34] = 0x5A;

	for (i = 0; i < sizeof(k12_vectors) / sizeof(k12_vectors[0]); i++) {
		const struct k12_vector *v = &k12_vectors[i];
		unsigned char *m = malloc(v->message_len ? v->message_len : 1);
		unsigned char *c = malloc(v->custom_len ? v->custom_len : 1);

		fill_pattern(m, v->message_len);
		fill_pattern(c, v->custom_len);
		libkeccak_unhex(want, v->hex32);

		/* One-shot update. */
		run_library(m, v->message_len, c, v->custom_len, NULL, 0, got, 32);
		CHECK_CHECK(memcmp(got, want, 32) == 0);
		CHECK_CHECK(guard_intact(got, 32));

		/* Split updates, including a leading zero-length update. */
		make_splits(v->message_len, 4096, splits, &nsplits);
		{
			struct libkeccak_k12 *ctx = libkeccak_k12_create();
			size_t off = 0;
			CHECK_CHECK(ctx != NULL);
			CHECK_CHECK(libkeccak_k12_update(ctx, NULL, 0) == 0);
			for (j = 0; j < nsplits; j++) {
				CHECK_CHECK(libkeccak_k12_update(ctx, m + off, splits[j]) == 0);
				off += splits[j];
			}
			CHECK_CHECK(off == v->message_len);
			CHECK_CHECK(libkeccak_k12_final(ctx, c, v->custom_len) == 0);
			CHECK_CHECK(libkeccak_k12_squeeze(ctx, got, 32) == 0);
			CHECK_CHECK(memcmp(got, want, 32) == 0);
			libkeccak_k12_destroy(ctx);
		}

		free(m);
		free(c);
	}

	/* Empty input, 10032 output bytes: check the documented tail. */
	{
		const size_t L = 10032;
		unsigned char *base, *out = guard_alloc(L, &base);
		unsigned char tail_want[32];
		struct libkeccak_k12 *ctx = libkeccak_k12_create();
		CHECK_CHECK(ctx != NULL);
		CHECK_CHECK(libkeccak_k12_final(ctx, NULL, 0) == 0);
		CHECK_CHECK(libkeccak_k12_squeeze(ctx, out, L) == 0);
		libkeccak_k12_destroy(ctx);
		libkeccak_unhex(tail_want, k12_empty_10032_tail32);
		CHECK_CHECK(memcmp(out + L - 32, tail_want, 32) == 0);

		/* And the whole stream against the independent reference. */
		{
			unsigned char *rbase, *ref = guard_alloc(L, &rbase);
			ref_k12(NULL, 0, NULL, 0, ref, L);
			CHECK_CHECK(memcmp(out, ref, L) == 0);
			CHECK_CHECK(guard_intact(ref, L));
			free(rbase);
		}
		CHECK_CHECK(guard_intact(out, L));
		free(base);
	}
}


/* ------------------------------------------------------------------ */
/* Group: boundary (custom-length encoding counts toward the limit)   */
/* ------------------------------------------------------------------ */

struct boundary_case {
	size_t mlen, clen;
	int want_tree;
};

/* Number of bytes E(value) occupies: minimal big-endian bytes plus the
 * one-byte count, with E(0) being a single zero byte. */
static size_t
e_length(size_t value)
{
	size_t n = 0;
	if (value == 0)
		return 1;
	while (value) {
		n++;
		value >>= 8;
	}
	return n + 1;
}

static void
group_boundary(void)
{
	/* E(1) is 01 01 (two bytes), E(256) is 01 00 02 (three bytes).
	 * The limit is tested over S = M || C || E(length(C)), so the
	 * customisation-length encoding always counts. */
	static const struct boundary_case cases[] = {
		{ 8191,    0, 0}, /* S = 8191 + 0 + 1 = 8192: single */
		{ 8192,    0, 1}, /* S = 8192 + 0 + 1 = 8193: tree   */
		{ 8189,    1, 0}, /* S = 8189 + 1 + 2 = 8192: single */
		{ 8190,    1, 1}, /* S = 8190 + 1 + 2 = 8193: tree   */
		{ 7933,  256, 0}, /* S = 7933 + 256 + 3 = 8192: single */
		{ 7934,  256, 1}, /* S = 7934 + 256 + 3 = 8193: tree   */
		{ 8192, 8189, 1}, /* published vector, S = 16384 */
		{ 8192, 8190, 1}  /* published vector, S = 16385 */
	};
	size_t i;
	unsigned char prev_single[32], prev_tree[32];
	int have_single = 0, have_tree = 0;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct boundary_case *bc = &cases[i];
		unsigned char *m = malloc(bc->mlen ? bc->mlen : 1);
		unsigned char *c = malloc(bc->clen ? bc->clen : 1);
		size_t splits[8], nsplits;
		unsigned char *ba, *bb, *base_a, *base_b;
		unsigned char one_shot[32];
		size_t slen = bc->mlen + bc->clen + e_length(bc->clen);

		/* Pin the table's mode flag to the actual S length. */
		CHECK_CHECK(bc->want_tree == (slen > 8192));

		fill_pattern(m, bc->mlen);
		fill_pattern(c, bc->clen);

		/* Unsplit. */
		ba = guard_alloc(32, &base_a);
		bb = guard_alloc(32, &base_b);
		run_library(m, bc->mlen, c, bc->clen, NULL, 0, ba, 32);
		ref_k12(m, bc->mlen, c, bc->clen, bb, 32);
		CHECK_CHECK(memcmp(ba, bb, 32) == 0);
		memcpy(one_shot, ba, 32);
		CHECK_CHECK(guard_intact(ba, 32));
		CHECK_CHECK(guard_intact(bb, 32));
		free(base_a);
		free(base_b);

		/* Split inside the message as well. */
		make_splits(bc->mlen, 4097, splits, &nsplits);
		check_matches_reference(m, bc->mlen, c, bc->clen, splits, nsplits, 32);

		/* Neighbouring single-piece and tree cases must not share
		 * an ending marker: their digests have to differ. */
		if (bc->want_tree) {
			memcpy(prev_tree, one_shot, 32);
			if (have_single)
				CHECK_CHECK(memcmp(prev_single, prev_tree, 32) != 0);
			have_tree = 1;
		} else {
			memcpy(prev_single, one_shot, 32);
			if (have_tree)
				CHECK_CHECK(memcmp(prev_single, prev_tree, 32) != 0);
			have_single = 1;
		}

		free(m);
		free(c);
	}
}


/* ------------------------------------------------------------------ */
/* Group: streaming (split independence, >255 pieces)                 */
/* ------------------------------------------------------------------ */

static void
group_streaming(void)
{
	static const size_t sizes[] = {
		0, 1, 17, 167, 168, 169, 8191, 8192, 8193, 10000, 20000
	};
	size_t i;
	size_t *splits = malloc(20000 * sizeof(size_t));
	size_t nsplits;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t mlen = sizes[i];
		size_t clen = (mlen % 97) + 1; /* always a non-empty custom */
		unsigned char *m = malloc(mlen ? mlen : 1);
		unsigned char *c = malloc(clen);

		fill_pattern(m, mlen);
		fill_pattern(c, clen);

		/* One shot. */
		check_matches_reference(m, mlen, c, clen, NULL, 0, 48);
		/* Byte at a time. */
		if (mlen) {
			make_splits(mlen, 1, splits, &nsplits);
			check_matches_reference(m, mlen, c, clen, splits, nsplits, 48);
		}
		/* Rate-sized and odd-sized chunks, with zero-length updates. */
		make_splits(mlen, 168, splits, &nsplits);
		{
			struct libkeccak_k12 *ctx = libkeccak_k12_create();
			unsigned char *base, *out = guard_alloc(48, &base);
			unsigned char *rbase, *ref = guard_alloc(48, &rbase);
			size_t off = 0, j;
			CHECK_CHECK(ctx != NULL);
			CHECK_CHECK(libkeccak_k12_update(ctx, NULL, 0) == 0);
			for (j = 0; j < nsplits; j++) {
				if ((j & 1) == 0)
					CHECK_CHECK(libkeccak_k12_update(ctx, NULL, 0) == 0);
				CHECK_CHECK(libkeccak_k12_update(ctx, m + off, splits[j]) == 0);
				off += splits[j];
			}
			CHECK_CHECK(off == mlen);
			CHECK_CHECK(libkeccak_k12_final(ctx, c, clen) == 0);
			CHECK_CHECK(libkeccak_k12_squeeze(ctx, out, 48) == 0);
			ref_k12(m, mlen, c, clen, ref, 48);
			CHECK_CHECK(memcmp(out, ref, 48) == 0);
			CHECK_CHECK(guard_intact(out, 48));
			CHECK_CHECK(guard_intact(ref, 48));
			libkeccak_k12_destroy(ctx);
			free(base);
			free(rbase);
		}
		/* Piece-boundary-straddling chunk size. */
		make_splits(mlen, 8191, splits, &nsplits);
		check_matches_reference(m, mlen, c, clen, splits, nsplits, 48);

		free(m);
		free(c);
	}
	free(splits);

	/* More than 255 pieces: k = 256, so E(k) is the multi-byte
	 * 01 00 02. The reference assembles the root independently. */
	{
		const size_t mlen = 2100000; /* S = mlen + 1 > 256 * 8192 */
		unsigned char *m = malloc(mlen);
		size_t big_splits[64], nbig;
		CHECK_CHECK(mlen + 1 > 256UL * 8192);
		fill_pattern(m, mlen);
		make_splits(mlen, 65537, big_splits, &nbig);
		check_matches_reference(m, mlen, NULL, 0, big_splits, nbig, 64);
		free(m);
	}
}


/* ------------------------------------------------------------------ */
/* Group: xof (one continuous stream, read in pieces)                 */
/* ------------------------------------------------------------------ */

static void
group_xof(void)
{
	const size_t mlen = 5000, total = 1000;
	unsigned char *m = malloc(mlen);
	unsigned char *base_full, *full = guard_alloc(total, &base_full);
	unsigned char *base_ref, *ref = guard_alloc(total, &base_ref);
	struct libkeccak_k12 *ctx;
	size_t off = 0;
	static const size_t reads[] = {1, 167, 1, 168};
	size_t i, rest;

	fill_pattern(m, mlen);
	ref_k12(m, mlen, NULL, 0, ref, total);

	/* 1, 167, 1, 168 then the remainder. */
	ctx = libkeccak_k12_create();
	CHECK_CHECK(ctx != NULL);
	CHECK_CHECK(libkeccak_k12_update(ctx, m, mlen) == 0);
	CHECK_CHECK(libkeccak_k12_final(ctx, NULL, 0) == 0);
	for (i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
		CHECK_CHECK(libkeccak_k12_squeeze(ctx, full + off, reads[i]) == 0);
		off += reads[i];
	}
	rest = total - off;
	CHECK_CHECK(libkeccak_k12_squeeze(ctx, full + off, rest) == 0);
	CHECK_CHECK(memcmp(full, ref, total) == 0);
	CHECK_CHECK(guard_intact(full, total));
	libkeccak_k12_destroy(ctx);
	free(base_full);

	/* Zero-length reads do not advance the stream. */
	{
		unsigned char *b2, *piece = guard_alloc(total, &b2);
		static const size_t zreads[] = {0, 10, 0, 158, 0, 333, 0, 499};
		size_t p = 0;
		ctx = libkeccak_k12_create();
		CHECK_CHECK(ctx != NULL);
		CHECK_CHECK(libkeccak_k12_update(ctx, m, mlen) == 0);
		CHECK_CHECK(libkeccak_k12_final(ctx, NULL, 0) == 0);
		for (i = 0; i < sizeof(zreads) / sizeof(zreads[0]); i++) {
			CHECK_CHECK(libkeccak_k12_squeeze(ctx, piece + p, zreads[i]) == 0);
			p += zreads[i];
		}
		CHECK_CHECK(p == total);
		CHECK_CHECK(memcmp(piece, ref, total) == 0);
		CHECK_CHECK(guard_intact(piece, total));
		libkeccak_k12_destroy(ctx);
		free(b2);
	}

	/* Squeeze before final fails and writes nothing. */
	{
		unsigned char *b3, *out = guard_alloc(32, &b3);
		memset(out, 0x77, 32);
		ctx = libkeccak_k12_create();
		CHECK_CHECK(ctx != NULL);
		CHECK_CHECK(libkeccak_k12_update(ctx, m, 100) == 0);
		CHECK_CHECK(libkeccak_k12_squeeze(ctx, out, 32) == -1);
		for (i = 0; i < 32; i++)
			CHECK_CHECK(out[i] == 0x77);
		CHECK_CHECK(guard_intact(out, 32));

		/* The failed call leaves the context usable. */
		CHECK_CHECK(libkeccak_k12_final(ctx, NULL, 0) == 0);
		CHECK_CHECK(libkeccak_k12_squeeze(ctx, out, 32) == 0);
		{
			unsigned char *b4, *want = guard_alloc(32, &b4);
			unsigned char mm[100];
			fill_pattern(mm, 100);
			ref_k12(mm, 100, NULL, 0, want, 32);
			CHECK_CHECK(memcmp(out, want, 32) == 0);
			free(b4);
		}
		libkeccak_k12_destroy(ctx);
		free(b3);
	}

	/* Empty message and empty customisation string. */
	check_matches_reference(NULL, 0, NULL, 0, NULL, 0, 200);

	free(base_ref);
	free(m);
}


/* ------------------------------------------------------------------ */
/* Group: lifecycle (phases, ownership, interleaved contexts)          */
/* ------------------------------------------------------------------ */

static void
group_lifecycle(void)
{
	struct libkeccak_k12 *a, *b;
	unsigned char msg[300], custom[16];
	unsigned char *ba, *bb, *bc, *base_a, *base_b, *base_c;
	size_t i;

	fill_pattern(msg, sizeof(msg));
	fill_pattern(custom, sizeof(custom));

	/* destroy(NULL) is safe. */
	libkeccak_k12_destroy(NULL);

	/* NULL pointers are only accepted with zero length. */
	a = libkeccak_k12_create();
	CHECK_CHECK(a != NULL);
	CHECK_CHECK(libkeccak_k12_update(a, NULL, 0) == 0);
	CHECK_CHECK(libkeccak_k12_update(a, NULL, 1) == -1);
	CHECK_CHECK(libkeccak_k12_final(a, NULL, 0) == 0);
	CHECK_CHECK(libkeccak_k12_squeeze(a, NULL, 0) == 0);
	CHECK_CHECK(libkeccak_k12_squeeze(a, NULL, 1) == -1);

	/* Wrong-phase calls fail and do not corrupt later legal reads. */
	{
		unsigned char *bg, *got = guard_alloc(32, &bg);
		unsigned char *bw, *want = guard_alloc(32, &bw);
		CHECK_CHECK(libkeccak_k12_update(a, msg, 1) == -1);
		CHECK_CHECK(libkeccak_k12_final(a, NULL, 0) == -1);
		CHECK_CHECK(libkeccak_k12_squeeze(a, got, 32) == 0);
		ref_k12(NULL, 0, NULL, 0, want, 32);
		CHECK_CHECK(memcmp(got, want, 32) == 0);
		CHECK_CHECK(guard_intact(got, 32));
		free(bg);
		free(bw);
	}
	libkeccak_k12_destroy(a);

	/* Input ownership: buffers may be reused or overwritten after the
	 * call returns. */
	{
		unsigned char *bg, *got = guard_alloc(32, &bg);
		unsigned char *bw, *want = guard_alloc(32, &bw);
		unsigned char scratch[300], cscratch[16];
		memcpy(scratch, msg, sizeof(scratch));
		memcpy(cscratch, custom, sizeof(cscratch));

		a = libkeccak_k12_create();
		CHECK_CHECK(a != NULL);
		CHECK_CHECK(libkeccak_k12_update(a, scratch, sizeof(scratch)) == 0);
		memset(scratch, 0xEE, sizeof(scratch));
		CHECK_CHECK(libkeccak_k12_final(a, cscratch, sizeof(cscratch)) == 0);
		memset(cscratch, 0xDD, sizeof(cscratch));
		CHECK_CHECK(libkeccak_k12_squeeze(a, got, 32) == 0);
		ref_k12(msg, sizeof(msg), custom, sizeof(custom), want, 32);
		CHECK_CHECK(memcmp(got, want, 32) == 0);
		CHECK_CHECK(guard_intact(got, 32));
		libkeccak_k12_destroy(a);
		free(bg);
		free(bw);
	}

	/* Two contexts interleaved, including interleaved squeezing. */
	a = libkeccak_k12_create();
	b = libkeccak_k12_create();
	CHECK_CHECK(a != NULL && b != NULL);
	CHECK_CHECK(libkeccak_k12_update(a, msg, 123) == 0);
	CHECK_CHECK(libkeccak_k12_update(b, msg, 200) == 0);
	CHECK_CHECK(libkeccak_k12_update(a, msg + 123, sizeof(msg) - 123) == 0);
	CHECK_CHECK(libkeccak_k12_update(b, msg + 200, sizeof(msg) - 200) == 0);
	CHECK_CHECK(libkeccak_k12_final(a, NULL, 0) == 0);
	CHECK_CHECK(libkeccak_k12_final(b, custom, sizeof(custom)) == 0);

	ba = guard_alloc(40, &base_a);
	bb = guard_alloc(40, &base_b);
	bc = guard_alloc(40, &base_c);
	for (i = 0; i < 40; i++) {
		CHECK_CHECK(libkeccak_k12_squeeze(a, ba + i, 1) == 0);
		CHECK_CHECK(libkeccak_k12_squeeze(b, bb + i, 1) == 0);
	}
	ref_k12(msg, sizeof(msg), NULL, 0, bc, 40);
	CHECK_CHECK(memcmp(ba, bc, 40) == 0);
	ref_k12(msg, sizeof(msg), custom, sizeof(custom), bc, 40);
	CHECK_CHECK(memcmp(bb, bc, 40) == 0);
	CHECK_CHECK(guard_intact(ba, 40));
	CHECK_CHECK(guard_intact(bb, 40));
	CHECK_CHECK(guard_intact(bc, 40));
	libkeccak_k12_destroy(a);
	libkeccak_k12_destroy(b);
	free(base_a);
	free(base_b);
	free(base_c);

	/* NUL bytes are ordinary data. */
	{
		unsigned char zeros[64];
		memset(zeros, 0, sizeof(zeros));
		check_matches_reference(zeros, sizeof(zeros), zeros, 7, NULL, 0, 32);
	}
}


/* ------------------------------------------------------------------ */

void
check_case(const char *group)
{
	if (!strcmp(group, "vectors"))
		group_vectors();
	else if (!strcmp(group, "boundary"))
		group_boundary();
	else if (!strcmp(group, "streaming"))
		group_streaming();
	else if (!strcmp(group, "xof"))
		group_xof();
	else if (!strcmp(group, "lifecycle"))
		group_lifecycle();
	else
		CHECK_CHECK(0);
}
