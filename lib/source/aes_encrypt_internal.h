/* aes_encrypt_internal.h - shared bitslice block kernel */

#ifndef __TC_AES_ENCRYPT_INTERNAL_H__
#define __TC_AES_ENCRYPT_INTERNAL_H__

#include <tinycrypt/aes.h>

/*
 * Encrypt nblocks (1..4) consecutive 16-byte blocks from in into out using
 * the table-free bit-plane kernel.  out == in (complete overlap) is allowed.
 * Returns TC_CRYPTO_SUCCESS or TC_CRYPTO_FAIL on invalid arguments.
 *
 * This is internal to the library; applications should call tc_aes_encrypt,
 * tc_aes_encrypt4 or tc_ctr_mode.
 */
int tc_aes_encrypt_blocks(uint8_t *out, const uint8_t *in,
			  unsigned int nblocks, const TCAesKeySched_t s);

#endif /* __TC_AES_ENCRYPT_INTERNAL_H__ */
