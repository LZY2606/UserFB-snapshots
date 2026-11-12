/* See LICENSE file for copyright and license details. */
#include "common.h"

/* Pulls in the shared 64-bit round core and the rc64 constants. */
#include "1600.c"

#include "libkeccak/keccakp1600.h"


/**
 * Apply Keccak-p[1600,12], the last twelve rounds of Keccak-f[1600],
 * to a bare 25-lane state. The round implementation is the library's
 * existing one; only the starting index into the round-constant table
 * differs from the 24-round SHA-3/SHAKE path.
 *
 * @param  lanes  The 25 state lanes in library storage order
 */
void
libkeccak_keccakp1600_12rounds(uint64_t *lanes)
{
	int i;
	for (i = 12; i < 24; i++)
		libkeccak_f_round64_lanes(lanes, rc64[i]);
}
