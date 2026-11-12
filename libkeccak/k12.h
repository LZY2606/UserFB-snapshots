/* See LICENSE file for copyright and license details. */

/**
 * KT128: the 128-bit-security KangarooTwelve extendable-output function
 * from RFC 9861, in a byte-oriented, streaming form.
 *
 * The intended use is local content fingerprinting of records that are
 * too long to assemble before hashing. KT256, keyed/MAC modes and SIMD
 * are deliberately not included.
 */
#ifndef LIBKECCAK_K12_H
#define LIBKECCAK_K12_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque KT128 hashing context. Its storage and all working buffers are
 * bounded (the whole context, including every dynamic buffer it uses,
 * stays within 32 KiB) and independent between contexts: no mutable state
 * is shared. The context never retains a caller pointer past the call
 * that supplied it.
 */
struct libkeccak_k12;

/**
 * Allocate a fresh KT128 context in its input-accepting phase.
 *
 * @return  A new context, or NULL on allocation failure
 */
struct libkeccak_k12 *libkeccak_k12_create(void);

/**
 * Release a KT128 context and all of its storage.
 *
 * @param  ctx  The context, may be NULL (then this is a no-op)
 */
void libkeccak_k12_destroy(struct libkeccak_k12 *ctx);

/**
 * Absorb the next chunk of the message.
 *
 * Chunk boundaries do not affect the result, zero-length updates are
 * allowed, and NUL bytes are ordinary data. Only valid before
 * libkeccak_k12_final(3) has succeeded. The buffer is only borrowed for
 * the duration of this call.
 *
 * @param   ctx     The context
 * @param   data    Message bytes; NULL is accepted only when length is 0
 * @param   length  Number of bytes to absorb
 * @return          0 on success, -1 on error (wrong phase, or NULL data
 *                  with a non-zero length)
 */
int libkeccak_k12_update(struct libkeccak_k12 *ctx, const void *data, size_t length);

/**
 * Absorb the customisation string and finish the input.
 *
 * The tree-vs-single decision is made over the message, the customisation
 * string and the encoding of the customisation length together, so an
 * empty customisation string still contributes its (one-byte) encoding.
 * This succeeds at most once; afterwards update fails and only squeeze
 * is accepted. The buffer is only borrowed for this call.
 *
 * @param   ctx     The context
 * @param   custom  Customisation bytes; NULL is accepted only when length
 *                  is 0
 * @param   length  Number of customisation bytes
 * @return          0 on success, -1 on error (wrong phase, or NULL custom
 *                  with a non-zero length)
 */
int libkeccak_k12_final(struct libkeccak_k12 *ctx, const void *custom, size_t length);

/**
 * Read more bytes from the single KT128 XOF stream.
 *
 * Only valid after a successful final. Repeated calls concatenate into
 * one continuous output stream; a zero-length read advances nothing and
 * succeeds. On failure the output buffer is not written.
 *
 * @param   ctx     The context
 * @param   out     Destination buffer; NULL is accepted only when length
 *                  is 0
 * @param   length  Number of output bytes
 * @return          0 on success, -1 on error (called before final, or NULL
 *                  out with a non-zero length)
 */
int libkeccak_k12_squeeze(struct libkeccak_k12 *ctx, void *out, size_t length);

#ifdef __cplusplus
}
#endif

#endif
