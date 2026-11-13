// Contract tests for crypto_eddsa_check_batch().
//
// All keys and signatures are generated inside this file from public,
// fixed seeds: nothing secret is stored, and the fixed coefficients are
// only good for reproducible test runs.  Production callers must draw
// fresh coefficients from a secure random source, after the whole batch
// is fixed.

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "monocypher.h"
#include "check.h"

// Test hooks from monocypher.c (CRYPTO_EDDSA_BATCH_TEST builds only)
void   crypto_eddsa_batch_test_reset(void);
size_t crypto_eddsa_batch_test_doubles(void);

// Deterministic test key pair from a public fixed seed
static void make_key(uint8_t sk[64], uint8_t pk[32], unsigned id)
{
	uint8_t seed[32];
	for (unsigned i = 0; i < 32; i++) {
		seed[i] = (uint8_t)(id * 41 + i * 7 + 3);
	}
	crypto_eddsa_key_pair(sk, pk, seed);
}

// Fixed, reproducible, non-zero 128-bit little-endian coefficient
static void make_coeff(uint8_t c[16], unsigned id)
{
	for (unsigned i = 0; i < 16; i++) {
		c[i] = (uint8_t)(id * 29 + i * 13 + 1);
	}
	c[0] |= 1; // never zero
}

static void make_msg(uint8_t *msg, size_t size, unsigned id)
{
	for (size_t i = 0; i < size; i++) {
		msg[i] = (uint8_t)(id * 11 + i * 5 + 1);
	}
}

// x += 1 (32-byte little-endian)
static void le_add1(uint8_t x[32])
{
	unsigned carry = 1;
	for (unsigned i = 0; i < 32; i++) {
		carry += x[i];
		x[i]   = (uint8_t)carry;
		carry >>= 8;
	}
}

// x -= 1 (32-byte little-endian), x must not be zero
static void le_sub1(uint8_t x[32])
{
	int borrow = 1;
	for (unsigned i = 0; i < 32; i++) {
		int d = (int)x[i] - borrow;
		if (d < 0) { d += 256; borrow = 1; } else { borrow = 0; }
		x[i] = (uint8_t)d;
	}
}

// The prime order of Curve25519, and that order minus one
static const uint8_t L[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
	0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};
static const uint8_t L_MINUS_1[32] = {
	0xec, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
	0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

// Valid batches, cross-checked against independent single checks
static void test_valid(void)
{
	enum { N = 8 };
	static uint8_t sk [N][64];
	static uint8_t pk [N][32];
	static uint8_t sig[N][64];
	static uint8_t msg[N][37];
	crypto_eddsa_batch_item items[N];
	uint8_t coeffs[N][16];

	for (unsigned i = 0; i < N; i++) {
		make_key(sk[i], pk[i], i + 1);
		make_msg(msg[i], sizeof msg[i], i + 1);
		crypto_eddsa_sign(sig[i], sk[i], msg[i], sizeof msg[i]);
		items[i].signature    = sig[i];
		items[i].public_key   = pk[i];
		items[i].message      = msg[i];
		items[i].message_size = sizeof msg[i];
		make_coeff(coeffs[i], i + 1);
	}
	// Empty message: a NULL message pointer is allowed with size 0
	crypto_eddsa_sign(sig[3], sk[3], 0, 0);
	items[3].message      = 0;
	items[3].message_size = 0;
	// Duplicated items are allowed: each occurrence is its own term
	items[5] = items[1];

	// Independent per-signature cross-check of the small batch
	for (unsigned i = 0; i < N; i++) {
		UNIT_CHECK(crypto_eddsa_check(items[i].signature,
		                              items[i].public_key,
		                              items[i].message,
		                              items[i].message_size) == 0);
	}

	size_t needed = crypto_eddsa_batch_workspace_size(N);
	UNIT_CHECK(needed != 0);
	UNIT_CHECK(needed != SIZE_MAX);
	uint8_t *ws = (uint8_t*)malloc(needed);
	UNIT_CHECK(ws != 0);

	// Alternating batch and single checks
	UNIT_CHECK(crypto_eddsa_check_batch(items, N, coeffs, ws, needed) == 0);
	UNIT_CHECK(crypto_eddsa_check(sig[0], pk[0], msg[0], sizeof msg[0]) == 0);
	UNIT_CHECK(crypto_eddsa_check_batch(items, N, coeffs, ws, needed) == 0);
	UNIT_CHECK(crypto_eddsa_check(sig[3], pk[3], 0, 0) == 0);
	UNIT_CHECK(crypto_eddsa_check_batch(items, N, coeffs, ws, needed) == 0);

	// Batch of one
	UNIT_CHECK(crypto_eddsa_check_batch(items, 1, coeffs, ws, needed) == 0);

	// Corrupting one message breaks the batch, restoring it heals it
	msg[2][0] ^= 1;
	UNIT_CHECK(crypto_eddsa_check_batch(items, N, coeffs, ws, needed) == -1);
	msg[2][0] ^= 1;
	UNIT_CHECK(crypto_eddsa_check_batch(items, N, coeffs, ws, needed) == 0);

	free(ws);
}

// Weighted aggregation: opposite residuals cancel only with equal weights
static void test_equations(void)
{
	uint8_t sk[64], pk[32], sig[64];
	uint8_t msg[23];
	make_key(sk, pk, 99);
	make_msg(msg, sizeof msg, 99);
	crypto_eddsa_sign(sig, sk, msg, sizeof msg);
	UNIT_CHECK(crypto_eddsa_check(sig, pk, msg, sizeof msg) == 0);

	// Two forgeries with opposite residuals: same R, public key, and
	// message (hence the same h), but S+ = S+1 and S- = S-1.
	// Their residuals S B - h A - R are exactly +B and -B.
	uint8_t sig_plus [64];
	uint8_t sig_minus[64];
	static const uint8_t zero[32] = {0};
	memcpy(sig_plus , sig, 64);
	memcpy(sig_minus, sig, 64);
	UNIT_CHECK(memcmp(sig + 32, zero, 32) != 0); // S-1 does not wrap
	le_add1(sig_plus  + 32);
	le_sub1(sig_minus + 32);
	UNIT_CHECK(memcmp(sig_plus + 32, L, 32) != 0); // S+1 stays below L

	// Both are forgeries on their own
	UNIT_CHECK(crypto_eddsa_check(sig_plus , pk, msg, sizeof msg) == -1);
	UNIT_CHECK(crypto_eddsa_check(sig_minus, pk, msg, sizeof msg) == -1);

	crypto_eddsa_batch_item items[2];
	items[0].signature    = sig_plus;
	items[0].public_key   = pk;
	items[0].message      = msg;
	items[0].message_size = sizeof msg;
	items[1].signature    = sig_minus;
	items[1].public_key   = pk;
	items[1].message      = msg;
	items[1].message_size = sizeof msg;

	size_t needed = crypto_eddsa_batch_workspace_size(2);
	uint8_t *ws = (uint8_t*)malloc(needed);
	UNIT_CHECK(ws != 0);

	// Equal weights: the opposite residuals cancel, like an unweighted
	// sum would.  The aggregate equation holds even though both
	// signatures are forgeries.
	uint8_t c_eq[2][16] = {{1}, {1}};
	UNIT_CHECK(crypto_eddsa_check_batch(items, 2, c_eq, ws, needed) == 0);

	// Different weights: the residuals no longer cancel.
	uint8_t c_12[2][16] = {{1}, {2}};
	uint8_t c_21[2][16] = {{2}, {1}};
	UNIT_CHECK(crypto_eddsa_check_batch(items, 2, c_12, ws, needed) == -1);
	UNIT_CHECK(crypto_eddsa_check_batch(items, 2, c_21, ws, needed) == -1);

	// Each forgery alone fails, whatever the coefficient
	uint8_t c_one[1][16] = {{7}};
	UNIT_CHECK(crypto_eddsa_check_batch(items    , 1, c_one, ws, needed) == -1);
	UNIT_CHECK(crypto_eddsa_check_batch(items + 1, 1, c_one, ws, needed) == -1);

	// A batch mixing a genuine and a forged signature fails
	crypto_eddsa_batch_item mixed[2];
	mixed[0].signature    = sig;
	mixed[0].public_key   = pk;
	mixed[0].message      = msg;
	mixed[0].message_size = sizeof msg;
	mixed[1]              = items[0];
	UNIT_CHECK(crypto_eddsa_check_batch(mixed, 2, c_eq, ws, needed) == -1);

	free(ws);
}

// Low-order points, non-canonical encodings, and scalar boundaries
static void test_encodings(void)
{
	// Test key from a public fixed seed
	uint8_t sk[64], pk[32];
	make_key(sk, pk, 7);
	// Secret scalar of that throwaway key (public building blocks only)
	uint8_t a64[64];
	uint8_t a  [32];
	crypto_blake2b(a64, 64, sk, 32);
	crypto_eddsa_trim_scalar(a, a64);

	uint8_t msg[19];
	make_msg(msg, sizeof msg, 7);

	static const uint8_t identity[32] = {1}; // canonical identity
	static const uint8_t identity_nc[32] = { // y = p+1: non-canonical identity
		0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
	};
	static const uint8_t order_8[32] = { // canonical point of order 8
		0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0,
		0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
		0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39,
		0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05,
	};
	static const uint8_t base_B[32] = { // base point
		0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	};
	static const uint8_t minus_B[32] = { // -B: same y, flipped x sign
		0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0xe6,
	};
	static const uint8_t not_curve[32] = { // off the curve
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	};

	// h = hash(R || A || message) % L over the *raw* encodings,
	// then S = h*a % L.  When R (or A) is low-order, the equation
	// only holds after the cofactor clearing.
	uint8_t sig[4][64];
	crypto_eddsa_batch_item good[4];
	uint8_t coeffs[4][16];
	for (unsigned i = 0; i < 4; i++) {
		make_coeff(coeffs[i], i + 50);
		good[i].signature    = sig[i];
		good[i].message      = msg;
		good[i].message_size = sizeof msg;
	}
	{
		uint8_t hash[64], h[32], zero[32] = {0};
		crypto_blake2b_ctx ctx;

		// R is a point of order 8 (low-order R is allowed)
		memcpy(sig[0], order_8, 32);
		crypto_blake2b_init  (&ctx, 64);
		crypto_blake2b_update(&ctx, sig[0], 32);
		crypto_blake2b_update(&ctx, pk    , 32);
		crypto_blake2b_update(&ctx, msg   , sizeof msg);
		crypto_blake2b_final (&ctx, hash);
		crypto_eddsa_reduce(h, hash);
		crypto_eddsa_mul_add(sig[0] + 32, h, a, zero);
		good[0].public_key = pk;

		// R is the identity, in non-canonical encoding.  The hash
		// covers the raw non-canonical bytes, so a check that
		// re-encoded R before hashing would fail here.
		memcpy(sig[1], identity_nc, 32);
		crypto_blake2b_init  (&ctx, 64);
		crypto_blake2b_update(&ctx, sig[1], 32);
		crypto_blake2b_update(&ctx, pk    , 32);
		crypto_blake2b_update(&ctx, msg   , sizeof msg);
		crypto_blake2b_final (&ctx, hash);
		crypto_eddsa_reduce(h, hash);
		crypto_eddsa_mul_add(sig[1] + 32, h, a, zero);
		good[1].public_key = pk;

		// A is the identity in non-canonical encoding (low-order,
		// non-canonical A is allowed), R = B, S = 1
		memcpy(sig[2], base_B, 32);
		memset(sig[2] + 32, 0, 32);
		sig[2][32] = 1;
		good[2].public_key = identity_nc;

		// S = L-1, the largest allowed scalar.  A is the identity,
		// R = -B, so (L-1)B - h*0 - (-B) = 0.
		memcpy(sig[3]     , minus_B  , 32);
		memcpy(sig[3] + 32, L_MINUS_1, 32);
		good[3].public_key = identity;
	}

	// Every crafted item is accepted on its own...
	for (unsigned i = 0; i < 4; i++) {
		UNIT_CHECK(crypto_eddsa_check(good[i].signature,
		                              good[i].public_key,
		                              good[i].message,
		                              good[i].message_size) == 0);
	}
	// ...and the batch accepts them, low-order components included
	size_t needed = crypto_eddsa_batch_workspace_size(5); // room to spare
	uint8_t *ws = (uint8_t*)malloc(needed);
	UNIT_CHECK(ws != 0);
	UNIT_CHECK(crypto_eddsa_check_batch(good, 4, coeffs, ws, needed) == 0);

	// The S = L-1 item alone makes the base point scalar exactly L-1,
	// whose top bit (252) exercises the top, partially used window
	uint8_t c_top[1][16] = {{1}};
	UNIT_CHECK(crypto_eddsa_check_batch(good + 3, 1, c_top, ws, needed) == 0);

	// S = L is out of range and must be rejected
	uint8_t bad_sig[64];
	memcpy(bad_sig     , minus_B, 32);
	memcpy(bad_sig + 32, L      , 32);
	crypto_eddsa_batch_item bad = { bad_sig, identity, msg, sizeof msg };
	uint8_t c_one[1][16] = {{3}};
	UNIT_CHECK(crypto_eddsa_check(bad_sig, identity, msg, sizeof msg) == -1);
	UNIT_CHECK(crypto_eddsa_check_batch(&bad, 1, c_one, ws, needed) == -1);

	// R not on the curve must be rejected
	memcpy(bad_sig, not_curve, 32);
	memset(bad_sig + 32, 0, 32);
	bad_sig[32] = 1;
	UNIT_CHECK(crypto_eddsa_check(bad_sig, identity, msg, sizeof msg) == -1);
	UNIT_CHECK(crypto_eddsa_check_batch(&bad, 1, c_one, ws, needed) == -1);

	// A not on the curve must be rejected
	uint8_t ok_sig[64];
	memcpy(ok_sig, base_B, 32);
	memset(ok_sig + 32, 0, 32);
	ok_sig[32] = 1;
	bad.signature  = ok_sig;
	bad.public_key = not_curve;
	UNIT_CHECK(crypto_eddsa_check(ok_sig, not_curve, msg, sizeof msg) == -1);
	UNIT_CHECK(crypto_eddsa_check_batch(&bad, 1, c_one, ws, needed) == -1);

	// A single bad item poisons an otherwise valid batch
	crypto_eddsa_batch_item mixed[5];
	for (unsigned i = 0; i < 4; i++) {
		mixed[i] = good[i];
	}
	uint8_t c5[5][16];
	for (unsigned i = 0; i < 5; i++) {
		make_coeff(c5[i], i + 70);
	}
	bad.signature  = ok_sig;
	bad.public_key = not_curve;
	mixed[4] = bad;
	UNIT_CHECK(crypto_eddsa_check_batch(mixed, 5, c5, ws, needed) == -1);

	free(ws);
	crypto_wipe(a, 32);
}

// Workspace sizing, argument validation, and memory boundaries
static void test_workspace(void)
{
	// Size queries: empty batch, overflow, monotonic growth
	UNIT_CHECK(crypto_eddsa_batch_workspace_size(0) == 0);
	UNIT_CHECK(crypto_eddsa_batch_workspace_size(1) != 0);
	UNIT_CHECK(crypto_eddsa_batch_workspace_size(1) != SIZE_MAX);
	UNIT_CHECK(crypto_eddsa_batch_workspace_size(2) >
	           crypto_eddsa_batch_workspace_size(1));
	UNIT_CHECK(crypto_eddsa_batch_workspace_size(SIZE_MAX)     == SIZE_MAX);
	UNIT_CHECK(crypto_eddsa_batch_workspace_size(SIZE_MAX / 2) == SIZE_MAX);

	// Empty batch: valid, all pointers may be NULL
	UNIT_CHECK(crypto_eddsa_check_batch(0, 0, 0, 0, 0) == 0);

	// One valid item with an empty (NULL) message
	uint8_t sk[64], pk[32], sig[64];
	make_key(sk, pk, 42);
	crypto_eddsa_sign(sig, sk, 0, 0);
	crypto_eddsa_batch_item item;
	item.signature    = sig;
	item.public_key   = pk;
	item.message      = 0;
	item.message_size = 0;
	uint8_t coeff[1][16] = {{1}};
	size_t needed = crypto_eddsa_batch_workspace_size(1);

	uint8_t *ws = (uint8_t*)malloc(needed);
	UNIT_CHECK(ws != 0);

	// NULL pointers are rejected on non-empty batches
	UNIT_CHECK(crypto_eddsa_check_batch(0    , 1, coeff, ws, needed) == -2);
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, 0    , ws, needed) == -2);
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, coeff, 0, needed) == -2);
	// Insufficient workspace is rejected, exactly enough is accepted
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, coeff, ws, 0) == -2);
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, coeff, ws, needed - 1) == -2);
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, coeff, ws, needed) == 0);

	// NULL fields inside the item are rejected
	crypto_eddsa_batch_item bad = item;
	bad.signature = 0;
	UNIT_CHECK(crypto_eddsa_check_batch(&bad, 1, coeff, ws, needed) == -2);
	bad = item;
	bad.public_key = 0;
	UNIT_CHECK(crypto_eddsa_check_batch(&bad, 1, coeff, ws, needed) == -2);
	bad = item;
	bad.message      = 0;
	bad.message_size = 5; // non-empty message may not be NULL
	UNIT_CHECK(crypto_eddsa_check_batch(&bad, 1, coeff, ws, needed) == -2);

	// Zero coefficients are rejected, high non-zero ones are fine
	uint8_t zero_coeff[1][16] = {{0}};
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, zero_coeff, ws, needed) == -2);
	uint8_t high_coeff[1][16] = {{0}};
	high_coeff[0][15] = 0x80;
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, high_coeff, ws, needed) == 0);

	free(ws);

	// Sentinels: the batch must not write outside the workspace
	const size_t guard = 32;
	uint8_t *buf = (uint8_t*)malloc(needed + 2*guard);
	UNIT_CHECK(buf != 0);
	memset(buf, 0xA5, needed + 2*guard);
	uint8_t *ws2 = buf + guard;
	UNIT_CHECK(crypto_eddsa_check_batch(&item, 1, coeff, ws2, needed) == 0);
	for (size_t i = 0; i < guard; i++) {
		UNIT_CHECK(buf[i] == 0xA5);
		UNIT_CHECK(buf[guard + needed + i] == 0xA5);
	}
	free(buf);
}

// The shared multi-scalar path: bounded point doublings, counted directly
static void test_work(void)
{
	enum { N = 64 };
	static uint8_t sk [N][64];
	static uint8_t pk [N][32];
	static uint8_t sig[N][64];
	static uint8_t msg[N][29];
	static crypto_eddsa_batch_item items[N];
	static uint8_t coeffs[N][16];

	for (unsigned i = 0; i < N; i++) {
		make_key(sk[i], pk[i], i + 100);
		make_msg(msg[i], sizeof msg[i], i + 100);
		crypto_eddsa_sign(sig[i], sk[i], msg[i], sizeof msg[i]);
		items[i].signature    = sig[i];
		items[i].public_key   = pk[i];
		items[i].message      = msg[i];
		items[i].message_size = sizeof msg[i];
		make_coeff(coeffs[i], i + 100);
	}

	size_t needed = crypto_eddsa_batch_workspace_size(N);
	UNIT_CHECK(needed != SIZE_MAX);
	uint8_t *ws = (uint8_t*)malloc(needed);
	UNIT_CHECK(ws != 0);

	// 64 valid signatures
	crypto_eddsa_batch_test_reset();
	UNIT_CHECK(crypto_eddsa_batch_test_doubles() == 0);
	UNIT_CHECK(crypto_eddsa_check_batch(items, N, coeffs, ws, needed) == 0);
	size_t d64 = crypto_eddsa_batch_test_doubles();
	UNIT_CHECK(d64 > 0);    // the shared multi-scalar path really ran
	UNIT_CHECK(d64 <= 260); // cofactor-clearing doublings included

	// 16 valid signatures
	crypto_eddsa_batch_test_reset();
	UNIT_CHECK(crypto_eddsa_check_batch(items, 16, coeffs, ws, needed) == 0);
	size_t d16 = crypto_eddsa_batch_test_doubles();
	UNIT_CHECK(d16 > 0);
	UNIT_CHECK(d16 <= 260);

	// Doublings are shared by the whole batch: checking 4 times as
	// many signatures does not take more of them.  (Per-signature
	// checks would need hundreds of doublings each.)
	UNIT_CHECK(d16 == d64);

	free(ws);
}

void register_contract(void)
{
	unit_add("valid",     test_valid);
	unit_add("equations", test_equations);
	unit_add("encodings", test_encodings);
	unit_add("workspace", test_workspace);
	unit_add("work",      test_work);
}
