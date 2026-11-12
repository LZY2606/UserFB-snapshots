/* See LICENSE file for copyright and license details. */

/*
 * Byte-oriented, streaming KT128 (KangarooTwelve, 128-bit security),
 * paraphrased from RFC 9861. See devcheck/kt128-contract.txt for the
 * exact byte conventions used here.
 *
 * Bounded-memory design: the whole context, including every working
 * buffer, lives in a single allocation well below 32 KiB. Neither the
 * message, the customisation string nor the chaining values are retained
 * in full; each piece chaining value is folded into the root sponge as
 * soon as the piece is complete.
 */
#include "common.h"

#include "libkeccak/k12.h"
#include "libkeccak/keccakp1600.h"


/** TurboSHAKE128 rate, in bytes (1344 bits). */
#define K12_RATE 168

/** KT128 piece size, in bytes. */
#define K12_PIECE 8192

/** Length of a piece chaining value. */
#define K12_CV_LEN 32

/* TurboSHAKE128 domain separation bytes. */
#define K12_DOMAIN_SINGLE 0x07 /* the sole (first) piece, direct output */
#define K12_DOMAIN_CV     0x0b /* a non-root piece, hashed to a CV */
#define K12_DOMAIN_ROOT   0x06 /* the tree root */


/**
 * A streaming TurboSHAKE128 sponge over Keccak-p[1600,12].
 *
 * Before finalisation `buf` holds un-absorbed input bytes; after
 * finalisation it holds the current 168-byte output block and `outpos`
 * tracks how much of it has already been squeezed.
 */
struct k12_sponge {
	uint64_t S[K12_RATE / 8 + 4]; /* 25 lanes, library storage order */
	unsigned char buf[K12_RATE];
	size_t fill;                   /* input bytes buffered, 0..167 */
	size_t outpos;                 /* output bytes consumed, 0..168 */
};

struct libkeccak_k12 {
	/* 0 while update/final accept input, 1 once final has succeeded. */
	int phase;

	/* Number of bytes of S = M||C||E(length(C)) consumed so far. */
	size_t total;

	/* Non-zero once the input crossed the single-piece boundary. */
	int tree;

	/* Root sponge: S0 in tree mode, all of S in single mode. */
	struct k12_sponge root;

	/* Reused scratch for computing piece chaining values. */
	struct k12_sponge cv;

	/* Chaining value of the most recently completed piece. */
	unsigned char last_cv[K12_CV_LEN];

	/* Number of completed non-root pieces (the eventual k). */
	size_t npieces;

	/* Carry buffer that aligns post-S0 bytes into 8192-byte pieces. */
	unsigned char tail[K12_PIECE];
	size_t tfill;
};


/**
 * Storage index of stream lane `i`: the library transposes lanes, so the
 * byte-ordered lane `i` lives at `(i % 5) * 5 + i / 5`. This must not be
 * guessed from the flat array order.
 */
static inline size_t
k12_lane_index(size_t i)
{
	return (i % 5) * 5 + i / 5;
}

static inline uint64_t
k12_load64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static inline void
k12_store64(unsigned char *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

/**
 * XOR one full rate block into the state and apply Keccak-p[1600,12].
 */
static void
k12_absorb_block(struct k12_sponge *z, const unsigned char *block)
{
	size_t i;
	for (i = 0; i < K12_RATE / 8; i++)
		z->S[k12_lane_index(i)] ^= k12_load64(block + 8 * i);
	libkeccak_keccakp1600_12rounds(z->S);
}

/**
 * Serialise the current state into the output block buffer.
 */
static void
k12_serialise_block(struct k12_sponge *z)
{
	size_t i;
	for (i = 0; i < K12_RATE / 8; i++)
		k12_store64(z->buf + 8 * i, z->S[k12_lane_index(i)]);
}

static void
k12_sponge_init(struct k12_sponge *z)
{
	memset(z, 0, sizeof(*z));
}

/**
 * Absorb arbitrary-length input, buffering partial rate blocks.
 */
static void
k12_sponge_absorb(struct k12_sponge *z, const unsigned char *data, size_t length)
{
	size_t take;

	if (z->fill) {
		take = length;
		if (take > K12_RATE - z->fill)
			take = K12_RATE - z->fill;
		memcpy(z->buf + z->fill, data, take);
		z->fill += take;
		data += take;
		length -= take;
		if (z->fill == K12_RATE) {
			k12_absorb_block(z, z->buf);
			z->fill = 0;
		}
	}

	while (length >= K12_RATE) {
		k12_absorb_block(z, data);
		data += K12_RATE;
		length -= K12_RATE;
	}

	if (length) {
		memcpy(z->buf, data, length);
		z->fill = length;
	}
}

/**
 * Apply the TurboSHAKE128 suffix: append D, zero-pad to a rate multiple,
 * XOR 0x80 into the final byte, then absorb the padded block. Afterwards
 * the first output block is already present in the state and is made
 * available without an extra permutation.
 */
static void
k12_sponge_finalise(struct k12_sponge *z, unsigned char domain)
{
	memset(z->buf + z->fill, 0, K12_RATE - z->fill);
	z->buf[z->fill] = domain;
	z->buf[K12_RATE - 1] ^= 0x80;
	k12_absorb_block(z, z->buf);
	z->fill = 0;
	z->outpos = 0;
	k12_serialise_block(z);
}

/**
 * Continue squeezing the single XOF stream, permuting only when a fresh
 * rate block is needed.
 */
static void
k12_sponge_squeeze(struct k12_sponge *z, unsigned char *out, size_t length)
{
	size_t take;

	while (length) {
		if (z->outpos == K12_RATE) {
			libkeccak_keccakp1600_12rounds(z->S);
			k12_serialise_block(z);
			z->outpos = 0;
		}

		take = length;
		if (take > K12_RATE - z->outpos)
			take = K12_RATE - z->outpos;
		memcpy(out, z->buf + z->outpos, take);
		out += take;
		length -= take;
		z->outpos += take;
	}
}

/**
 * Encode E(x): the minimal big-endian bytes of x followed by their
 * count. E(0) is a single zero byte (see the contract); the result has
 * at most 9 bytes.
 *
 * @return  The number of bytes written to `out`
 */
static size_t
k12_encode_e(unsigned char *out, size_t value)
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

/**
 * Hash the full 8192-byte piece in `tail` to a chaining value, fold that
 * CV into the root sponge, and advance the piece count.
 */
static void
k12_finish_piece(struct libkeccak_k12 *ctx)
{
	k12_sponge_init(&ctx->cv);
	k12_sponge_absorb(&ctx->cv, ctx->tail, K12_PIECE);
	k12_sponge_finalise(&ctx->cv, K12_DOMAIN_CV);
	k12_sponge_squeeze(&ctx->cv, ctx->last_cv, K12_CV_LEN);

	/* The root frame is S0 || 0x03 0^7 || CV1 || ... so each CV is
	 * appended in piece order, immediately, keeping only the latest. */
	k12_sponge_absorb(&ctx->root, ctx->last_cv, K12_CV_LEN);
	ctx->npieces += 1;
	ctx->tfill = 0;
}

/**
 * Feed one segment of the logical input S through the bounded machinery.
 */
static int
k12_feed(struct libkeccak_k12 *ctx, const unsigned char *data, size_t length)
{
	static const unsigned char root_header[8] = {
		0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	size_t take;

	while (length) {
		if (!ctx->tree) {
			/* The root sponge absorbs the first piece S0 directly. */
			take = length;
			if (take > K12_PIECE - ctx->total)
				take = K12_PIECE - ctx->total;
			k12_sponge_absorb(&ctx->root, data, take);
			ctx->total += take;
			data += take;
			length -= take;

			if (ctx->total < K12_PIECE)
				continue;

			/* Exactly S0 so far; tree mode is only committed by a
			 * later byte, which the next loop iteration handles. */
			if (!length)
				break;

			/* Commit to tree mode and emit the root header before
			 * any chaining value. */
			ctx->tree = 1;
			k12_sponge_absorb(&ctx->root, root_header, sizeof(root_header));
			continue;
		}

		/* Tree mode: gather post-S0 bytes into whole pieces. */
		take = length;
		if (take > K12_PIECE - ctx->tfill)
			take = K12_PIECE - ctx->tfill;
		memcpy(ctx->tail + ctx->tfill, data, take);
		ctx->tfill += take;
		ctx->total += take;
		data += take;
		length -= take;

		if (ctx->tfill == K12_PIECE)
			k12_finish_piece(ctx);
	}

	return 0;
}


struct libkeccak_k12 *
libkeccak_k12_create(void)
{
	struct libkeccak_k12 *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	k12_sponge_init(&ctx->root);
	k12_sponge_init(&ctx->cv);
	return ctx;
}

void
libkeccak_k12_destroy(struct libkeccak_k12 *ctx)
{
	free(ctx);
}

int
libkeccak_k12_update(struct libkeccak_k12 *ctx, const void *data, size_t length)
{
	if (!ctx || ctx->phase != 0)
		return -1;
	if (length && !data)
		return -1;
	if (!length)
		return 0;

	return k12_feed(ctx, data, length);
}

int
libkeccak_k12_final(struct libkeccak_k12 *ctx, const void *custom, size_t length)
{
	unsigned char encoded[9];
	size_t encoded_len;

	if (!ctx || ctx->phase != 0)
		return -1;
	if (length && !custom)
		return -1;

	/* The customisation string, then E(length(C)); even an empty
	 * customisation string contributes the one-byte E(0). */
	if (length && k12_feed(ctx, custom, length))
		return -1;
	encoded_len = k12_encode_e(encoded, length);
	if (k12_feed(ctx, encoded, encoded_len))
		return -1;

	if (!ctx->tree) {
		/* S fits a single piece: T(S, 0x07, L). */
		k12_sponge_finalise(&ctx->root, K12_DOMAIN_SINGLE);
	} else {
		/* Fold the trailing partial piece, if there is one. */
		if (ctx->tfill) {
			k12_sponge_init(&ctx->cv);
			k12_sponge_absorb(&ctx->cv, ctx->tail, ctx->tfill);
			k12_sponge_finalise(&ctx->cv, K12_DOMAIN_CV);
			k12_sponge_squeeze(&ctx->cv, ctx->last_cv, K12_CV_LEN);
			k12_sponge_absorb(&ctx->root, ctx->last_cv, K12_CV_LEN);
			ctx->npieces += 1;
			ctx->tfill = 0;
		}

		encoded_len = k12_encode_e(encoded, ctx->npieces);
		k12_sponge_absorb(&ctx->root, encoded, encoded_len);
		k12_sponge_absorb(&ctx->root, (const unsigned char *)"\xff\xff", 2);
		k12_sponge_finalise(&ctx->root, K12_DOMAIN_ROOT);
	}

	ctx->phase = 1;
	return 0;
}

int
libkeccak_k12_squeeze(struct libkeccak_k12 *ctx, void *out, size_t length)
{
	if (!ctx || ctx->phase != 1)
		return -1;
	if (length && !out)
		return -1;
	if (!length)
		return 0;

	k12_sponge_squeeze(&ctx->root, out, length);
	return 0;
}
