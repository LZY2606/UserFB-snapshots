/* See LICENSE file for copyright and license details. */

/**
 * Internal Keccak-p[1600,12] entry point shared by the byte-oriented
 * KT128 implementation. The regular SHA-3/SHAKE state machine keeps
 * using the 24-round Keccak-f[1600] path in digest.c.
 */
#ifndef LIBKECCAK_KECCAKP1600_H
#define LIBKECCAK_KECCAKP1600_H 1

#include <stdint.h>

/**
 * Apply Keccak-p[1600,12], i.e. the LAST twelve rounds of
 * Keccak-f[1600], to the 25 64-bit lanes.
 *
 * @param  lanes  The 25 state lanes in the library's storage order,
 *                i.e. stream lane `i` is stored at `(i % 5) * 5 + i / 5`
 */
void libkeccak_keccakp1600_12rounds(uint64_t *lanes);

#endif
