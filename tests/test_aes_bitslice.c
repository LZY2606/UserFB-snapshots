/* test_aes_bitslice.c - Tests for the TinyCrypt table-free AES kernel */

/*
 *  Copyright (C) 2017 by Intel Corporation, All Rights Reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *    - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *    - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 *    - Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * DESCRIPTION
 *
 * This module tests:
 *
 * - aes_vectors: FIPS-197 key schedule/encryption vectors and SP800-38A CTR.
 * - aes_lanes:   an independent scalar AES reference used to check four
 *                distinct blocks, all 256 values at every byte position,
 *                single-block vs. batch ordering, in-place and unaligned
 *                buffers.
 * - aes_ctr:     CTR lengths 1, 15, 16, 17, 63, 64, 65, 127, 128, 129,
 *                low-word carry, sentinels, and proof that full batches use
 *                the four-block entry point.
 *
 * The reference below is deliberately written in a different style from the
 * library kernel (byte-oriented scalar code with an sbox table used only for
 * test values).  It is not the implementation under test.
 */

#include <tinycrypt/aes.h>
#include <tinycrypt/constants.h>
#include <tinycrypt/ctr_mode.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "../lib/source/aes_encrypt_internal.h"

/*
 * Test-only instrumentation for the CTR batching proof.  The counters and
 * the instrumented copy of the library's CTR loop live at the bottom of this
 * file; these prototypes must be visible to the checks above.
 */
static unsigned int encrypt4_calls;
static unsigned int smalls_calls;

int tc_aes_encrypt4_probe(uint8_t *out, const uint8_t *in,
			  const TCAesKeySched_t s);
int tc_aes_encrypt_blocks_probe(uint8_t *out, const uint8_t *in,
				unsigned int nblocks, const TCAesKeySched_t s);
int tc_ctr_mode_probe(uint8_t *out, unsigned int outlen, const uint8_t *in,
		      unsigned int inlen, uint8_t *ctr,
		      const TCAesKeySched_t sched);

/* ---------------------------------------------------------------------- */
/* Independent scalar reference                                          */
/* ---------------------------------------------------------------------- */

static const uint8_t ref_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
	0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
	0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
	0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
	0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
	0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
	0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
	0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
	0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
	0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
	0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
	0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t ref_rcon[11] = {
	0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static uint8_t ref_xtime(uint8_t a)
{
	return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

/* Reference key expansion: 44 words, big-endian byte order per word. */
static void ref_key_expand(uint32_t words[44], const uint8_t key[16])
{
	unsigned int i;

	for (i = 0; i < 4; ++i) {
		words[i] = ((uint32_t)key[4 * i] << 24) |
			   ((uint32_t)key[4 * i + 1] << 16) |
			   ((uint32_t)key[4 * i + 2] << 8) |
			   key[4 * i + 3];
	}
	for (i = 4; i < 44; ++i) {
		uint32_t t = words[i - 1];

		if ((i % 4) == 0) {
			uint32_t r = (t << 8) | (t >> 24);

			t = ((uint32_t)ref_sbox[r >> 24] << 24) |
			    ((uint32_t)ref_sbox[(r >> 16) & 0xff] << 16) |
			    ((uint32_t)ref_sbox[(r >> 8) & 0xff] << 8) |
			    ref_sbox[r & 0xff];
			t ^= (uint32_t)ref_rcon[i / 4] << 24;
		}
		words[i] = words[i - 4] ^ t;
	}
}

/*
 * Reference one-block encryption.  The state is kept as 16 named bytes in
 * column-major order (state[4*c+r]); this is independent of the library's
 * plane representation.
 */
static void ref_encrypt_block(uint8_t out[16], const uint8_t in[16],
			      const uint8_t key[16])
{
	uint32_t w[44];
	/* s[row][col] in FIPS-197 order; the wire byte is in[4*col+row]. */
	uint8_t s[4][4];
	unsigned int c;
	unsigned int r;
	unsigned int round;

	ref_key_expand(w, key);
	for (c = 0; c < 4; ++c) {
		for (r = 0; r < 4; ++r) {
			/* word c byte r, big-endian */
			s[r][c] = in[4 * c + r] ^ (uint8_t)(w[c] >> (24 - 8 * r));
		}
	}
	for (round = 1; round <= 10; ++round) {
		uint8_t t[4][4];

		for (r = 0; r < 4; ++r) {
			for (c = 0; c < 4; ++c) {
				t[r][c] = ref_sbox[s[r][c]];
			}
		}
		/* ShiftRows: row r shifts left by r columns. */
		for (r = 0; r < 4; ++r) {
			for (c = 0; c < 4; ++c) {
				s[r][c] = t[r][(c + r) % 4];
			}
		}
		if (round != 10) {
			for (c = 0; c < 4; ++c) {
				uint8_t a0 = s[0][c];
				uint8_t a1 = s[1][c];
				uint8_t a2 = s[2][c];
				uint8_t a3 = s[3][c];
				uint8_t x0 = ref_xtime(a0);
				uint8_t x1 = ref_xtime(a1);
				uint8_t x2 = ref_xtime(a2);
				uint8_t x3 = ref_xtime(a3);

				s[0][c] = x0 ^ x1 ^ a1 ^ a2 ^ a3;
				s[1][c] = a0 ^ x1 ^ x2 ^ a2 ^ a3;
				s[2][c] = a0 ^ a1 ^ x2 ^ x3 ^ a3;
				s[3][c] = x0 ^ a0 ^ a1 ^ a2 ^ x3;
			}
		}
		for (c = 0; c < 4; ++c) {
			for (r = 0; r < 4; ++r) {
				s[r][c] ^= (uint8_t)(w[4 * round + c] >>
						     (24 - 8 * r));
			}
		}
	}
	for (c = 0; c < 4; ++c) {
		for (r = 0; r < 4; ++r) {
			out[4 * c + r] = s[r][c];
		}
	}
}

/* Continuous-chain RNG for test inputs (not used by the library). */
static uint32_t rng_state;

static uint32_t rng_next(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state;
}

/* ---------------------------------------------------------------------- */
/* aes_vectors                                                            */
/* ---------------------------------------------------------------------- */

static void check_known_vectors(void)
{
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	static const uint32_t expected[44] = {
		0x2b7e1516u, 0x28aed2a6u, 0xabf71588u, 0x09cf4f3cu,
		0xa0fafe17u, 0x88542cb1u, 0x23a33939u, 0x2a6c7605u,
		0xf2c295f2u, 0x7a96b943u, 0x5935807au, 0x7359f67fu,
		0x3d80477du, 0x4716fe3eu, 0x1e237e44u, 0x6d7a883bu,
		0xef44a541u, 0xa8525b7fu, 0xb671253bu, 0xdb0bad00u,
		0xd4d1c6f8u, 0x7c839d87u, 0xcaf2b8bcu, 0x11f915bcu,
		0x6d88a37au, 0x110b3efdu, 0xdbf98641u, 0xca0093fdu,
		0x4e54f70eu, 0x5f5fc9f3u, 0x84a64fb2u, 0x4ea6dc4fu,
		0xead27321u, 0xb58dbad2u, 0x312bf560u, 0x7f8d292fu,
		0xac7766f3u, 0x19fadc21u, 0x28d12941u, 0x575c006eu,
		0xd014f9a8u, 0xc9ee2589u, 0xe13f0cc8u, 0xb6630ca6u
	};
	struct tc_aes_key_sched_struct s;
	unsigned int i;
	int ok = 1;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	for (i = 0; i < 44; ++i) {
		if (s.words[i] != expected[i]) {
			ok = 0;
		}
	}
	UNIT_CHECK(ok);

	{
		static const uint8_t pt[16] = {
			0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
			0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34
		};
		static const uint8_t want[16] = {
			0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
			0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32
		};
		uint8_t out[16];

		UNIT_CHECK(tc_aes_encrypt(out, pt, &s) == TC_CRYPTO_SUCCESS);
		UNIT_CHECK(memcmp(out, want, 16) == 0);
	}
	{
		/* FIPS-197 Appendix C.1 key. */
		static const uint8_t k1[16] = {
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
		};
		static const uint8_t pt[16] = {
			0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
			0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
		};
		static const uint8_t want[16] = {
			0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
			0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a
		};
		struct tc_aes_key_sched_struct ks;
		uint8_t out[16];

		UNIT_CHECK(tc_aes128_set_encrypt_key(&ks, k1) ==
			   TC_CRYPTO_SUCCESS);
		UNIT_CHECK(tc_aes_encrypt(out, pt, &ks) == TC_CRYPTO_SUCCESS);
		UNIT_CHECK(memcmp(out, want, 16) == 0);
	}
	{
		static const uint8_t k2[16] = { 0 };
		static const uint8_t p2[16] = { 0 };
		static const uint8_t want[16] = {
			0x66, 0xe9, 0x4b, 0xd4, 0xef, 0x8a, 0x2c, 0x3b,
			0x88, 0x4c, 0xfa, 0x59, 0xca, 0x34, 0x2b, 0x2e
		};
		struct tc_aes_key_sched_struct z;
		uint8_t out[16];

		UNIT_CHECK(tc_aes128_set_encrypt_key(&z, k2) == TC_CRYPTO_SUCCESS);
		UNIT_CHECK(tc_aes_encrypt(out, p2, &z) == TC_CRYPTO_SUCCESS);
		UNIT_CHECK(memcmp(out, want, 16) == 0);
	}
}

static void check_ctr_vectors(void)
{
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	static const uint8_t ctr0[16] = {
		0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
		0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
	};
	static const uint8_t pt[64] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
		0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
		0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
		0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
		0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
		0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
		0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
		0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
	};
	static const uint8_t ct[64] = {
		0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26,
		0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce,
		0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff,
		0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff,
		0x5a, 0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e,
		0x5b, 0x4f, 0x09, 0x02, 0x0d, 0xb0, 0x3e, 0xab,
		0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1,
		0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee
	};
	struct tc_aes_key_sched_struct s;
	uint8_t ctr[16];
	uint8_t out[64];
	uint8_t dec[64];

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	(void)memcpy(ctr, ctr0, 16);
	UNIT_CHECK(tc_ctr_mode(out, sizeof(out), pt, sizeof(pt), ctr, &s) ==
		   TC_CRYPTO_SUCCESS);
	UNIT_CHECK(memcmp(out, ct, sizeof(ct)) == 0);
	/* First 12 bytes of the counter must be untouched. */
	UNIT_CHECK(memcmp(ctr, ctr0, 12) == 0);
	/* 64 bytes consume four counters: 0xfcfdfeff + 4 = 0xfcfdff03. */
	UNIT_CHECK(ctr[12] == 0xfc && ctr[13] == 0xfd &&
		   ctr[14] == 0xff && ctr[15] == 0x03);

	(void)memcpy(ctr, ctr0, 16);
	UNIT_CHECK(tc_ctr_mode(dec, sizeof(dec), ct, sizeof(ct), ctr, &s) ==
		   TC_CRYPTO_SUCCESS);
	UNIT_CHECK(memcmp(dec, pt, sizeof(pt)) == 0);
}

void aes_vectors(void)
{
	check_known_vectors();
	check_ctr_vectors();
}

/* ---------------------------------------------------------------------- */
/* aes_lanes                                                              */
/* ---------------------------------------------------------------------- */

/* Four distinct blocks with distinct first bytes. */
static void build_blocks(uint8_t blocks[4][16])
{
	unsigned int i;
	unsigned int j;

	for (i = 0; i < 4; ++i) {
		for (j = 0; j < 16; ++j) {
			blocks[i][j] = (uint8_t)((i * 0x41u) ^ (j * 0x1du) ^
						 (uint8_t)rng_next());
		}
	}
	blocks[0][0] = 0x00;
	blocks[1][0] = 0x19;
	blocks[2][0] = 0x80;
	blocks[3][0] = 0xff;
}

static void check_four_distinct_blocks(void)
{
	uint8_t key[16];
	uint8_t blocks[4][16];
	uint8_t flat[64];
	uint8_t out[64];
	struct tc_aes_key_sched_struct s;
	uint8_t reference[4][16];
	unsigned int i;

	rng_state = 0x12345678u;
	for (i = 0; i < 16; ++i) {
		key[i] = (uint8_t)rng_next();
	}
	build_blocks(blocks);
	for (i = 0; i < 64; ++i) {
		flat[i] = blocks[i / 16][i % 16];
	}

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes_encrypt4(out, flat, &s) == TC_CRYPTO_SUCCESS);

	for (i = 0; i < 4; ++i) {
		ref_encrypt_block(reference[i], blocks[i], key);
		UNIT_CHECK(memcmp(out + 16 * i, reference[i], 16) == 0);
	}
}

/* For every byte position, run all 256 byte values through lane 0..3. */
static void check_each_position_all_values(void)
{
	static const uint8_t key[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
	};
	struct tc_aes_key_sched_struct s;
	unsigned int pos;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	for (pos = 0; pos < 16; ++pos) {
		unsigned int v;

		for (v = 0; v < 256; v += 4) {
			uint8_t flat[64];
			uint8_t out[64];
			uint8_t blocks[4][16];
			unsigned int lane;
			unsigned int j;
			int ok = 1;

			for (lane = 0; lane < 4; ++lane) {

				for (j = 0; j < 16; ++j) {
					blocks[lane][j] = (uint8_t)
						(0xa5u ^ (lane * 0x33u) ^ j);
				}
				blocks[lane][pos] = (uint8_t)(v + lane);
			}
			for (lane = 0; lane < 4; ++lane) {
				for (j = 0; j < 16; ++j) {
					flat[16 * lane + j] = blocks[lane][j];
				}
			}
			UNIT_CHECK(tc_aes_encrypt4(out, flat, &s) ==
				   TC_CRYPTO_SUCCESS);
			for (lane = 0; lane < 4; ++lane) {
				uint8_t ref[16];

				ref_encrypt_block(ref, blocks[lane], key);
				if (out[16 * lane + pos] != ref[pos]) {
					ok = 0;
				}
			}
			UNIT_CHECK(ok);
		}
	}
}

/* Single block, batch prefix and order must agree. */
static void check_single_vs_batch_order(void)
{
	uint8_t key[16];
	uint8_t blocks[4][16];
	uint8_t flat[64];
	uint8_t single[16];
	uint8_t batch[64];
	struct tc_aes_key_sched_struct s;
	unsigned int i;
	unsigned int lane;

	rng_state = 0x0badf00du;
	for (i = 0; i < 16; ++i) {
		key[i] = (uint8_t)rng_next();
	}
	build_blocks(blocks);
	for (i = 0; i < 64; ++i) {
		flat[i] = blocks[i / 16][i % 16];
	}
	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes_encrypt4(batch, flat, &s) == TC_CRYPTO_SUCCESS);
	for (lane = 0; lane < 4; ++lane) {
		uint8_t ref[16];

		ref_encrypt_block(ref, blocks[lane], key);
		UNIT_CHECK(tc_aes_encrypt(single, blocks[lane], &s) ==
			   TC_CRYPTO_SUCCESS);
		UNIT_CHECK(memcmp(single, ref, 16) == 0);
		UNIT_CHECK(memcmp(single, batch + 16 * lane, 16) == 0);
	}
}

/* In-place: full overlap must equal out-of-place output. */
static void check_in_place(void)
{
	uint8_t key[16];
	uint8_t flat[64];
	uint8_t expected[64];
	struct tc_aes_key_sched_struct s;
	unsigned int i;

	rng_state = 0x5eed5eedu;
	for (i = 0; i < 16; ++i) {
		key[i] = (uint8_t)rng_next();
	}
	for (i = 0; i < 64; ++i) {
		flat[i] = (uint8_t)rng_next();
	}
	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes_encrypt4(expected, flat, &s) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes_encrypt4(flat, flat, &s) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(memcmp(flat, expected, 64) == 0);

	/* Single block in place. */
	{
		uint8_t blk[16];
		uint8_t want[16];
		uint8_t ref[16];

		(void)memcpy(blk, expected, 16);
		UNIT_CHECK(tc_aes_encrypt(want, blk, &s) == TC_CRYPTO_SUCCESS);
		ref_encrypt_block(ref, expected, key);
		UNIT_CHECK(memcmp(want, ref, 16) == 0);
		UNIT_CHECK(tc_aes_encrypt(blk, blk, &s) == TC_CRYPTO_SUCCESS);
		UNIT_CHECK(memcmp(blk, ref, 16) == 0);
	}
}

/* Unaligned input and output buffers must give the same result. */
static void check_unaligned(void)
{
	uint8_t key[16];
	uint8_t expected[64];
	uint8_t storage[64 + 32];
	struct tc_aes_key_sched_struct s;
	unsigned int i;
	unsigned int off;

	rng_state = 0xabcdef01u;
	for (i = 0; i < 16; ++i) {
		key[i] = (uint8_t)rng_next();
	}
	for (i = 0; i < 64; ++i) {
		storage[16 + i] = (uint8_t)rng_next();
	}
	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes_encrypt4(expected, storage + 16, &s) ==
		   TC_CRYPTO_SUCCESS);

	for (off = 1; off <= 15; ++off) {
		uint8_t local[64 + 32];
		uint8_t out[64 + 32];

		(void)memcpy(local + off, storage + 16, 64);
		UNIT_CHECK(tc_aes_encrypt4(out + off, local + off, &s) ==
			   TC_CRYPTO_SUCCESS);
		UNIT_CHECK(memcmp(out + off, expected, 64) == 0);
		/* Single block, unaligned. */
		{
			uint8_t ref[16];
			uint8_t one[16 + 16];

			(void)memcpy(one + off, storage + 16, 16);
			UNIT_CHECK(tc_aes_encrypt(out + off, one + off, &s) ==
				   TC_CRYPTO_SUCCESS);
			ref_encrypt_block(ref, storage + 16, key);
			UNIT_CHECK(memcmp(out + off, ref, 16) == 0);
		}
	}
}

/* A different key must produce a different batch result. */
static void check_keys_differ(void)
{
	uint8_t key[16];
	uint8_t key2[16];
	uint8_t flat[64];
	uint8_t a[64];
	uint8_t b[64];
	struct tc_aes_key_sched_struct s;
	unsigned int i;

	rng_state = 0x11223344u;
	for (i = 0; i < 16; ++i) {
		key[i] = (uint8_t)rng_next();
		key2[i] = key[i];
	}
	key2[0] ^= 0x01;
	for (i = 0; i < 64; ++i) {
		flat[i] = (uint8_t)rng_next();
	}
	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes_encrypt4(a, flat, &s) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key2) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_aes_encrypt4(b, flat, &s) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(memcmp(a, b, 64) != 0);
}

void aes_lanes(void)
{
	check_four_distinct_blocks();
	check_each_position_all_values();
	check_single_vs_batch_order();
	check_in_place();
	check_unaligned();
	check_keys_differ();
}

/* ---------------------------------------------------------------------- */
/* aes_ctr                                                                */
/* ---------------------------------------------------------------------- */

/*
 * One-block keystream helpers used to build the expected CTR output
 * independently of the library's CTR loop.
 */
static void ctr_stream_lane(uint8_t ks[16], const uint8_t ctr[16],
			    const uint8_t key[16])
{
	ref_encrypt_block(ks, ctr, key);
}

/* Reference CTR: one block at a time, byte-by-byte XOR. */
static void ref_ctr(uint8_t *out, unsigned int len, const uint8_t *in,
		    uint8_t ctr[16], const uint8_t key[16])
{
	unsigned int off;

	for (off = 0; off < len; off += 16) {
		uint8_t ks[16];
		unsigned int n = 16;
		unsigned int i;

		if (off + 16 > len) {
			n = len - off;
		}
		ctr_stream_lane(ks, ctr, key);
		for (i = 0; i < n; ++i) {
			out[off + i] = in[off + i] ^ ks[i];
		}
		/* increment low 32 bits big-endian */
		{
			unsigned int b = ((unsigned int)ctr[12] << 24) |
					 ((unsigned int)ctr[13] << 16) |
					 ((unsigned int)ctr[14] << 8) |
					 (unsigned int)ctr[15];

			b++;
			ctr[12] = (uint8_t)(b >> 24);
			ctr[13] = (uint8_t)(b >> 16);
			ctr[14] = (uint8_t)(b >> 8);
			ctr[15] = (uint8_t)b;
		}
	}
}

static void check_ctr_lengths(void)
{
	static const unsigned int lens[] = {
		1, 15, 16, 17, 63, 64, 65, 127, 128, 129
	};
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	static const uint8_t ctr0[16] = {
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	struct tc_aes_key_sched_struct s;
	unsigned int li;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	for (li = 0; li < sizeof(lens) / sizeof(lens[0]); ++li) {
		unsigned int len = lens[li];
		uint8_t in[192];
		uint8_t out[192];
		uint8_t want[192];
		uint8_t ctr[16];
		uint8_t refctr[16];
		unsigned int i;
		int ok = 1;

		rng_state = 0x1000u + len;
		for (i = 0; i < len; ++i) {
			in[i] = (uint8_t)rng_next();
		}
		(void)memcpy(ctr, ctr0, 16);
		(void)memcpy(refctr, ctr0, 16);
		UNIT_CHECK(tc_ctr_mode(out, len, in, len, ctr, &s) ==
			   TC_CRYPTO_SUCCESS);
		ref_ctr(want, len, in, refctr, key);
		for (i = 0; i < len; ++i) {
			if (out[i] != want[i]) {
				ok = 0;
			}
		}
		UNIT_CHECK(ok);
		UNIT_CHECK(memcmp(ctr, refctr, 16) == 0);
	}
}

static void check_ctr_carry(void)
{
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	/* Low word starts at 0x000000ff; 65 bytes consume four counters. */
	static const uint8_t ctr0[16] = {
		0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0xff
	};
	struct tc_aes_key_sched_struct s;
	uint8_t in[129];
	uint8_t out[129];
	uint8_t want[129];
	uint8_t ctr[16];
	uint8_t refctr[16];
	unsigned int i;
	int ok = 1;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	for (i = 0; i < sizeof(in); ++i) {
		in[i] = (uint8_t)(i * 7u + 3u);
	}
	(void)memcpy(ctr, ctr0, 16);
	(void)memcpy(refctr, ctr0, 16);
	UNIT_CHECK(tc_ctr_mode(out, sizeof(out), in, sizeof(in), ctr, &s) ==
		   TC_CRYPTO_SUCCESS);
	ref_ctr(want, sizeof(want), in, refctr, key);
	for (i = 0; i < sizeof(out); ++i) {
		if (out[i] != want[i]) {
			ok = 0;
		}
	}
	UNIT_CHECK(ok);
	UNIT_CHECK(memcmp(ctr, refctr, 16) == 0);
	/* Low word 0xff + 9 blocks = 0x108, big-endian in bytes 12..15. */
	UNIT_CHECK(ctr[12] == 0x00 && ctr[13] == 0x00 &&
		   ctr[14] == 0x01 && ctr[15] == 0x08);
	/* First 12 bytes (nonce) unchanged. */
	UNIT_CHECK(memcmp(ctr, ctr0, 12) == 0);
}

static void check_ctr_sentinels(void)
{
	static const uint8_t key[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
	};
	struct tc_aes_key_sched_struct s;
	uint8_t in[80];
	uint8_t out[80 + 32];
	uint8_t ctr[16];
	uint8_t before[16];
	unsigned int i;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	for (i = 0; i < sizeof(in); ++i) {
		in[i] = (uint8_t)(i + 1u);
	}
	(void)memcpy(ctr, "\x00\x11\x22\x33\x44\x55\x66\x77"
			   "\x88\x99\xaa\xbb\xcc\xdd\xee\xff", 16);
	(void)memcpy(before, ctr, 16);

	for (i = 0; i < 80 + 32; ++i) {
		out[i] = 0xee;
	}
	/* 17 bytes: one full 16-byte block plus one partial byte. */
	UNIT_CHECK(tc_ctr_mode(out + 16, 17, in, 17, ctr, &s) ==
		   TC_CRYPTO_SUCCESS);
	/* Output region is exactly 17 bytes; sentinels intact. */
	UNIT_CHECK(out[15] == 0xee);
	UNIT_CHECK(out[16 + 17] == 0xee);
	{
		uint8_t want[17];
		uint8_t refctr[16];
		int ok = 1;

		(void)memcpy(refctr, before, 16);
		ref_ctr(want, 17, in, refctr, key);
		for (i = 0; i < 17; ++i) {
			if (out[16 + i] != want[i]) {
				ok = 0;
			}
		}
		UNIT_CHECK(ok);
		UNIT_CHECK(memcmp(ctr, refctr, 16) == 0);
	}
}

/*
 * Proof that full batches go through the four-block entry.
 *
 * The test translation unit compiles the library's ctr_mode.c a second time
 * under private macro names (see the bottom of this file), with
 * tc_aes_encrypt4 renamed to tc_aes_encrypt4_probe and defined here as a
 * counting forwarder to the real kernel.  ctr_mode_probe therefore runs the
 * real batch loop, so the call counter observes exactly which entry points
 * that loop chooses.
 *
 * For 129 bytes the loop runs two complete 64-byte batches plus a 1-block
 * tail: the counter must be 2, and each tail/small batch must reach the
 * shared kernel.  For 64 bytes the counter must be exactly 1: neither 0 (old
 * block-at-a-time path) nor 4 (one-per-block) is acceptable.
 */
static void check_ctr_uses_four_block_kernel(void)
{
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	struct tc_aes_key_sched_struct s;
	uint8_t in[129];
	uint8_t out[129];
	uint8_t out2[129];
	uint8_t ctr[16];
	uint8_t ctr2[16];
	uint8_t base[16];
	uint8_t counters[64];
	uint8_t ks[64];
	unsigned int i;
	unsigned int block;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	rng_state = 0x42424242u;
	for (i = 0; i < sizeof(in); ++i) {
		in[i] = (uint8_t)rng_next();
	}
	for (i = 0; i < 16; ++i) {
		base[i] = (uint8_t)rng_next();
	}

	/* 129 bytes: two full four-block batches and a one-block tail. */
	(void)memcpy(ctr, base, 16);
	encrypt4_calls = 0;
	smalls_calls = 0;
	UNIT_CHECK(tc_ctr_mode_probe(out, sizeof(out), in, sizeof(in), ctr, &s)
		   == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(encrypt4_calls == 2);
	UNIT_CHECK(smalls_calls == 1);

	/* The real public entry gives the same bytes and the same final ctr. */
	(void)memcpy(ctr2, base, 16);
	UNIT_CHECK(tc_ctr_mode(out2, sizeof(out2), in, sizeof(in), ctr2, &s) ==
		   TC_CRYPTO_SUCCESS);
	UNIT_CHECK(memcmp(out, out2, sizeof(out)) == 0);
	UNIT_CHECK(memcmp(ctr, ctr2, 16) == 0);

	/*
	 * The first 64 bytes must equal tc_aes_encrypt4 applied to the four
	 * consecutive counter blocks, byte for byte.
	 */
	(void)memcpy(counters, base, 16);
	{
		unsigned int low = ((unsigned int)base[12] << 24) |
				   ((unsigned int)base[13] << 16) |
				   ((unsigned int)base[14] << 8) |
				   (unsigned int)base[15];

		for (block = 1; block < 4; ++block) {
			uint8_t *p = counters + 16 * block;
			unsigned int b = low + block;

			(void)memcpy(p, base, 16);
			p[12] = (uint8_t)(b >> 24);
			p[13] = (uint8_t)(b >> 16);
			p[14] = (uint8_t)(b >> 8);
			p[15] = (uint8_t)b;
		}
	}
	UNIT_CHECK(tc_aes_encrypt4(ks, counters, &s) == TC_CRYPTO_SUCCESS);
	for (i = 0; i < 64; ++i) {
		UNIT_CHECK(out[i] == (in[i] ^ ks[i]));
	}

	/* Exactly 64 bytes: one call, not four and not zero. */
	(void)memcpy(ctr, base, 16);
	encrypt4_calls = 0;
	smalls_calls = 0;
	UNIT_CHECK(tc_ctr_mode_probe(out, 64, in, 64, ctr, &s) ==
		   TC_CRYPTO_SUCCESS);
	UNIT_CHECK(encrypt4_calls == 1);
	UNIT_CHECK(smalls_calls == 0);

	/* 1..17 bytes: no full batch, but the safe kernel is still used. */
	(void)memcpy(ctr, base, 16);
	encrypt4_calls = 0;
	smalls_calls = 0;
	UNIT_CHECK(tc_ctr_mode_probe(out, 17, in, 17, ctr, &s) ==
		   TC_CRYPTO_SUCCESS);
	UNIT_CHECK(encrypt4_calls == 0);
	UNIT_CHECK(smalls_calls == 1);

	/* 129 bytes consume ceil(129/16) = 9 counter values. */
	(void)memcpy(ctr, base, 16);
	UNIT_CHECK(tc_ctr_mode_probe(out, sizeof(out), in, sizeof(in), ctr, &s)
		   == TC_CRYPTO_SUCCESS);
	{
		uint8_t exp[16];
		unsigned int low = ((unsigned int)base[12] << 24) |
				   ((unsigned int)base[13] << 16) |
				   ((unsigned int)base[14] << 8) |
				   (unsigned int)base[15];
		unsigned int next = low + 9;

		(void)memcpy(exp, base, 16);
		exp[12] = (uint8_t)(next >> 24);
		exp[13] = (uint8_t)(next >> 16);
		exp[14] = (uint8_t)(next >> 8);
		exp[15] = (uint8_t)next;
		UNIT_CHECK(memcmp(ctr, exp, 16) == 0);
	}
}

/* In-place CTR (out == in), across the interesting lengths. */
static void check_ctr_in_place(void)
{
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	static const unsigned int lens[] = { 1, 15, 16, 17, 63, 64, 65, 129 };
	struct tc_aes_key_sched_struct s;
	unsigned int li;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	for (li = 0; li < sizeof(lens) / sizeof(lens[0]); ++li) {
		unsigned int len = lens[li];
		uint8_t plain[129];
		uint8_t buf[129];
		uint8_t out[129];
		uint8_t ctr[16];
		uint8_t refctr[16];
		uint8_t c0[16] = {
			0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
			0x90, 0xa0, 0xb0, 0xc0, 0x00, 0x00, 0x7f, 0xfe
		};
		unsigned int i;
		uint8_t want[129];
		int ok = 1;

		rng_state = 0x77770000u + len;
		for (i = 0; i < len; ++i) {
			plain[i] = (uint8_t)rng_next();
			buf[i] = plain[i];
		}
		(void)memcpy(ctr, c0, 16);
		(void)memcpy(refctr, c0, 16);
		UNIT_CHECK(tc_ctr_mode(out, len, plain, len, ctr, &s) ==
			   TC_CRYPTO_SUCCESS);
		ref_ctr(want, len, plain, refctr, key);
		/* In-place output must equal the out-of-place output; reset ctr. */
		(void)memcpy(ctr, c0, 16);
		UNIT_CHECK(tc_ctr_mode(buf, len, buf, len, ctr, &s) ==
			   TC_CRYPTO_SUCCESS);
		for (i = 0; i < len; ++i) {
			if (buf[i] != want[i] || out[i] != want[i]) {
				ok = 0;
			}
		}
		UNIT_CHECK(ok);
	}
}

/* Unaligned CTR buffers: copy in/out at an offset and compare. */
static void check_ctr_unaligned(void)
{
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	struct tc_aes_key_sched_struct s;
	uint8_t src[129];
	uint8_t want[129];
	uint8_t ctr[16];
	uint8_t c0[16] = {
		0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
		0x22, 0x33, 0x44, 0x55, 0x00, 0x01, 0x00, 0x01
	};
	unsigned int off;
	unsigned int i;

	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	rng_state = 0x99990000u;
	for (i = 0; i < sizeof(src); ++i) {
		src[i] = (uint8_t)rng_next();
	}
	(void)memcpy(ctr, c0, 16);
	UNIT_CHECK(tc_ctr_mode(want, sizeof(want), src, sizeof(src), ctr, &s) ==
		   TC_CRYPTO_SUCCESS);

	for (off = 1; off <= 15; off += 7) {
		uint8_t inbuf[129 + 16];
		uint8_t outbuf[129 + 16];
		uint8_t tctr[16];
		uint8_t *p = inbuf + off;
		uint8_t *q = outbuf + off;
		unsigned int j;
		int ok = 1;

		(void)memcpy(tctr, c0, 16);
		for (j = 0; j < sizeof(src); ++j) {
			p[j] = src[j];
			q[j] = 0x5c;
		}
		UNIT_CHECK(tc_ctr_mode(q, sizeof(src), p, sizeof(src), tctr, &s)
			   == TC_CRYPTO_SUCCESS);
		for (j = 0; j < sizeof(want); ++j) {
			if (q[j] != want[j]) {
				ok = 0;
			}
		}
		UNIT_CHECK(ok);
	}
}

/* Reject invalid arguments and leave output untouched. */
static void check_ctr_errors(void)
{
	static const uint8_t key[16] = { 0 };
	struct tc_aes_key_sched_struct s;
	uint8_t in[16];
	uint8_t out[16];
	uint8_t ctr[16];

	(void)memset(in, 0, sizeof(in));
	(void)memset(out, 0x5a, sizeof(out));
	(void)memset(ctr, 0, sizeof(ctr));
	UNIT_CHECK(tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_SUCCESS);
	UNIT_CHECK(tc_ctr_mode((uint8_t *)0, 16, in, 16, ctr, &s) ==
		   TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_ctr_mode(out, 16, (uint8_t *)0, 16, ctr, &s) ==
		   TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_ctr_mode(out, 16, in, 16, (uint8_t *)0, &s) ==
		   TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_ctr_mode(out, 16, in, 16, ctr, (TCAesKeySched_t)0) ==
		   TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_ctr_mode(out, 16, in, 0, ctr, &s) == TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_ctr_mode(out, 0, in, 0, ctr, &s) == TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_ctr_mode(out, 8, in, 16, ctr, &s) == TC_CRYPTO_FAIL);
	{
		unsigned int i;
		int ok = 1;

		for (i = 0; i < sizeof(out); ++i) {
			if (out[i] != 0x5a) {
				ok = 0;
			}
		}
		UNIT_CHECK(ok);
	}
	UNIT_CHECK(tc_aes_encrypt4((uint8_t *)0, in, &s) == TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_aes_encrypt4(out, (uint8_t *)0, &s) == TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_aes_encrypt4(out, in, (TCAesKeySched_t)0) ==
		   TC_CRYPTO_FAIL);
	UNIT_CHECK(tc_aes_encrypt((uint8_t *)0, in, &s) == TC_CRYPTO_FAIL);
}

void aes_ctr(void)
{
	check_ctr_lengths();
	check_ctr_carry();
	check_ctr_sentinels();
	check_ctr_in_place();
	check_ctr_unaligned();
	check_ctr_uses_four_block_kernel();
	check_ctr_errors();
}

/*
 * Instrumented copy of the library's CTR loop.
 *
 * Compile ../lib/source/ctr_mode.c a second time under private names.  The
 * macro substitutions are applied before the file's own includes, so the
 * narrowed declarations line up, and the include expands in this translation
 * unit exactly as in the library build.  tc_ctr_mode_probe is therefore the
 * real batching control flow, not a reimplementation.
 */
#define tc_ctr_mode tc_ctr_mode_probe
#define tc_aes_encrypt4 tc_aes_encrypt4_probe
#define tc_aes_encrypt_blocks tc_aes_encrypt_blocks_probe
#include "../lib/source/ctr_mode.c"
#undef tc_ctr_mode
#undef tc_aes_encrypt4
#undef tc_aes_encrypt_blocks

int tc_aes_encrypt4_probe(uint8_t *out, const uint8_t *in,
			  const TCAesKeySched_t s)
{
	++encrypt4_calls;
	return tc_aes_encrypt_blocks(out, in, 4, s);
}

int tc_aes_encrypt_blocks_probe(uint8_t *out, const uint8_t *in,
				unsigned int nblocks, const TCAesKeySched_t s)
{
	++smalls_calls;
	return tc_aes_encrypt_blocks(out, in, nblocks, s);
}
