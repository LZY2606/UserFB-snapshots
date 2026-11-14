#ifndef FUZZY_EDIT_H
#define FUZZY_EDIT_H

/*
 * Editable in-memory buffer with an incrementally maintained ssdeep
 * (context-triggered piecewise hash) summary.
 *
 * A fuzzy_edit object owns a private copy of up to FUZZY_EDIT_MAX_LENGTH
 * bytes.  fuzzy_edit_splice applies an in-place replacement (delete plus
 * insert at one offset) and only the inserted bytes, a bounded prefix and
 * suffix of the original data around the seam, and a logarithmically
 * bounded number of summary nodes are reprocessed.  fuzzy_edit_digest
 * reproduces byte-for-byte the signature that the streaming interface
 * (fuzzy_new / fuzzy_update / fuzzy_digest) produces for the same full
 * buffer and the same flags; it performs no byte I/O and does not alter
 * any state.
 *
 * This header only adds an API.  The existing streaming interface in
 * fuzzy.h and the ssdeep command line are unchanged.
 */

#include "fuzzy.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest buffer a fuzzy_edit object can hold (8 MiB). */
#define FUZZY_EDIT_MAX_LENGTH ((size_t)8 * 1024u * 1024u)

/**
 * Opaque editable buffer.  Its layout is private to fuzzy_edit.c; callers
 * only ever hold pointers.
 */
struct fuzzy_edit;

/**
 * @brief Construct an editable buffer.
 *
 * The @p data buffer is copied; the caller may release it as soon as the
 * constructor returns.  The internal copy never contains NUL-terminated
 * strings: zero and high bytes are ordinary data bytes.
 *
 * @param data   Initial bytes, or NULL when @p length is zero.
 * @param length Number of bytes, at most FUZZY_EDIT_MAX_LENGTH.
 * @return the constructed object or NULL on failure (bad arguments,
 *         length overflow, or out of memory).
 */
extern /*@only@*/ /*@null@*/ struct fuzzy_edit *
fuzzy_edit_new(const unsigned char *data, size_t length);

/**
 * @brief Dispose an editable buffer.
 * @param self Object to dispose, or NULL (in which case this is a no-op).
 */
extern void fuzzy_edit_free(/*@only@*/ struct fuzzy_edit *self);

/**
 * @brief Return the current number of bytes held in the buffer.
 * @param self A valid fuzzy_edit object.
 */
extern size_t fuzzy_edit_size(const struct fuzzy_edit *self);

/**
 * @brief Replace the byte range [offset, offset+removed) with @p inserted
 *        bytes taken from @p insert.
 *
 * Offsets are interpreted against the buffer as it exists before the
 * edit.  Insertion at the very end (offset == current length, removed == 0)
 * and deletion down to an empty buffer are allowed.  On failure the
 * buffer and every previously readable digest are left untouched.
 *
 * @param self     A valid fuzzy_edit object.
 * @param offset   Start of the replaced range.
 * @param removed  Number of existing bytes to remove.
 * @param insert   Replacement bytes, or NULL when @p inserted is zero.
 * @param inserted Number of replacement bytes.
 * @return 0 on success, -1 on failure (out of range, length overflow,
 *         NULL with a non-zero length, resulting buffer larger than
 *         FUZZY_EDIT_MAX_LENGTH, or out of memory).
 */
extern int fuzzy_edit_splice(struct fuzzy_edit *self,
			     size_t offset, size_t removed,
			     const unsigned char *insert, size_t inserted);

/**
 * @brief Produce the ssdeep signature of the current buffer.
 *
 * The output is identical, byte for byte, to running a fresh fuzzy_new,
 * feeding the complete current buffer through fuzzy_update and calling
 * fuzzy_digest with the same flags.  This call does not modify the
 * object: repeated calls and later splices are unaffected.  On failure
 * @p result is not written.
 *
 * @param self   A valid fuzzy_edit object.
 * @param result Output buffer of at least FUZZY_MAX_RESULT bytes.
 * @param flags  0, FUZZY_FLAG_ELIMSEQ, FUZZY_FLAG_NOTRUNC or their bitwise
 *               combination.  Unknown flag bits cause failure.
 * @return 0 on success, -1 on failure.
 */
extern int fuzzy_edit_digest(struct fuzzy_edit *self,
			     /*@out@*/ char *result, unsigned int flags);

/**
 * @brief Report cumulative work counters since construction.
 *
 * raw_bytes counts the number of times an original (or freshly inserted)
 * data byte is read for summary computation; a byte read more than once
 * is counted each time, while one load that updates several hash states
 * counts once.  node_visits counts every access to a summary tree node for
 * location, digest computation, splitting, merging, rebalancing or
 * reclamation; repeated visits are counted repeatedly.  Repeated digest
 * queries add nothing to either counter.  The slow streaming reference
 * used by tests lives outside the library and is not counted here.
 *
 * Either output pointer may be NULL.
 *
 * @param self       A valid fuzzy_edit object.
 * @param raw_bytes  Receives the cumulative raw byte count.
 * @param node_visits Receives the cumulative node visit count.
 */
extern void fuzzy_edit_stats(const struct fuzzy_edit *self,
			     /*@null@*/ uint64_t *raw_bytes,
			     /*@null@*/ uint64_t *node_visits);

#ifdef __cplusplus
}
#endif

#endif
