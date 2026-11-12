/* ctr_mode.c - TinyCrypt CTR mode implementation */

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

#include <tinycrypt/constants.h>
#include <tinycrypt/ctr_mode.h>
#include <tinycrypt/utils.h>

#include "aes_encrypt_internal.h"

/* XOR len bytes of keystream into out.  len <= 16. */
static void xor_stream(uint8_t *out, const uint8_t *in,
		       const uint8_t *stream, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; ++i) {
		out[i] = stream[i] ^ in[i];
	}
}

int tc_ctr_mode(uint8_t *out, unsigned int outlen, const uint8_t *in,
		unsigned int inlen, uint8_t *ctr, const TCAesKeySched_t sched)
{
	uint8_t nonce[TC_AES_BLOCK_SIZE];
	uint8_t stream[4 * TC_AES_BLOCK_SIZE];
	uint8_t ctrblock[4 * TC_AES_BLOCK_SIZE];
	unsigned int block_num;
	unsigned int offset;
	unsigned int i;

	/* input sanity check: */
	if (out == (uint8_t *) 0 ||
	    in == (uint8_t *) 0 ||
	    ctr == (uint8_t *) 0 ||
	    sched == (TCAesKeySched_t) 0 ||
	    inlen == 0 ||
	    outlen == 0 ||
	    outlen != inlen) {
		return TC_CRYPTO_FAIL;
	}

	/* copy the ctr to the nonce */
	(void)_copy(nonce, sizeof(nonce), ctr, sizeof(nonce));

	/* select the last 4 bytes of the nonce to be incremented */
	block_num = ((unsigned int)nonce[12] << 24) |
		    ((unsigned int)nonce[13] << 16) |
		    ((unsigned int)nonce[14] << 8) | (unsigned int)nonce[15];

	/*
	 * Process the input in batches of up to four blocks.  Every full batch
	 * goes through the public four-block entry; the last partial batch goes
	 * through the same table-free kernel with only the needed lanes active.
	 * out == in is supported; other overlaps are not.
	 */
	for (offset = 0; offset < inlen; offset += 4 * TC_AES_BLOCK_SIZE) {
		unsigned int remain = inlen - offset;
		unsigned int nblocks;
		unsigned int k;

		if (remain >= 4 * TC_AES_BLOCK_SIZE) {
			nblocks = 4;
		} else {
			nblocks = (remain + TC_AES_BLOCK_SIZE - 1) /
				  TC_AES_BLOCK_SIZE;
		}

		for (k = 0; k < nblocks; ++k) {
			(void)_copy(ctrblock + TC_AES_BLOCK_SIZE * k,
				    TC_AES_BLOCK_SIZE, nonce, TC_AES_BLOCK_SIZE);
			block_num++;
			nonce[12] = (uint8_t)(block_num >> 24);
			nonce[13] = (uint8_t)(block_num >> 16);
			nonce[14] = (uint8_t)(block_num >> 8);
			nonce[15] = (uint8_t)(block_num);
		}

		if (nblocks == 4) {
			if (tc_aes_encrypt4(stream, ctrblock, sched) ==
			    TC_CRYPTO_FAIL) {
				return TC_CRYPTO_FAIL;
			}
		} else {
			if (tc_aes_encrypt_blocks(stream, ctrblock, nblocks,
						  sched) == TC_CRYPTO_FAIL) {
				return TC_CRYPTO_FAIL;
			}
		}

		for (i = 0; i < nblocks; ++i) {
			unsigned int base = i * TC_AES_BLOCK_SIZE;
			unsigned int n = TC_AES_BLOCK_SIZE;

			if (base + n > remain) {
				n = remain - base;
			}
			xor_stream(out + offset + base, in + offset + base,
				   stream + base, n);
		}
	}
	/* update the counter */
	ctr[12] = nonce[12]; ctr[13] = nonce[13];
	ctr[14] = nonce[14]; ctr[15] = nonce[15];

	_set(stream, TC_ZERO_BYTE, sizeof(stream));
	_set(ctrblock, TC_ZERO_BYTE, sizeof(ctrblock));
	_set(nonce, TC_ZERO_BYTE, sizeof(nonce));

	return TC_CRYPTO_SUCCESS;
}
