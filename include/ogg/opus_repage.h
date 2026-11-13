/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Ogg CONTAINER SOURCE CODE IS (C) COPYRIGHT 1994-2026         *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: streaming re-pager for single-track Ogg Opus recordings

 The Opus re-pager accepts the pages of one complete, already
 synchronized Ogg Opus stream (a single logical bitstream, one audio
 track, no chaining) and re-emits it page by page with a hard upper
 bound on the total byte size of every emitted page.  Audio packet
 payloads are copied verbatim; only page boundaries, page sequence
 numbers, granule positions and CRCs are regenerated.  Neither the
 whole recording nor a whole packet is ever buffered, so memory use
 stays bounded regardless of packet sizes, comment length or stream
 duration.

 This interface is independent of the generic ogg_stream_state
 framing API; nothing here changes existing libogg behavior.

 ********************************************************************/
#ifndef _OGG_OPUS_REPAGE_H
#define _OGG_OPUS_REPAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <ogg/ogg.h>

/* Opaque re-pager instance.  All state is per-instance; independent
   instances may be interleaved freely. */
typedef struct ogg_opus_repage ogg_opus_repage;

/* Optional allocator hook.  Every heap allocation of an instance goes
   through these callbacks, and the sum of simultaneously outstanding
   bytes never exceeds max_page_bytes + 80000.  When NULL is passed to
   ogg_opus_repage_new, malloc/free are used.  When non-NULL, both
   function pointers must be set. */
typedef struct {
  void *user;
  void *(*alloc)(void *user, size_t bytes);
  void (*release)(void *user, void *ptr);
} ogg_opus_repage_allocator;

/* Output callback.  Receives one complete, checksummed page at a
   time.  The page borrows instance storage and is only valid until
   the callback returns.  Return 0 to accept the page; any nonzero
   value aborts the stream (the instance enters its terminal error
   state). */
typedef int (*ogg_opus_repage_emit)(void *user, const ogg_page *page);

/* Create a re-pager.  max_page_bytes is the hard cap on the total
   size (header + segment table + body) of every emitted page and must
   be within [512, 65307].  Returns NULL on invalid arguments or
   allocation failure. */
extern ogg_opus_repage *ogg_opus_repage_new(
    size_t max_page_bytes,
    const ogg_opus_repage_allocator *allocator,
    ogg_opus_repage_emit emit,
    void *emit_user);

/* Submit one input page.  Pages must arrive in order: a single
   serial, page sequence starting at 0 with no gaps, correct
   continuation flags, a 19 byte version 1 mapping family 0 OpusHead
   alone on the BOS page, a complete OpusTags header ending at a page
   boundary, then audio packets, terminated by an EOS page.  Pages
   that are complete under the fixed re-paging rule are handed to the
   emit callback before this call returns; the input page is not
   referenced after return.  Returns 0 on success, -1 on failure
   (malformed sequence, inconsistent audio clock, unsupported headers,
   callback or allocation failure).  After a failure the instance is
   terminal and all further calls return -1. */
extern int ogg_opus_repage_pagein(ogg_opus_repage *repage,
                                  const ogg_page *page);

/* Signal end of input.  Succeeds (0) only if an EOS page was seen and
   no packet is left unfinished; otherwise returns -1 and the instance
   becomes terminal. */
extern int ogg_opus_repage_finish(ogg_opus_repage *repage);

/* Release all resources.  Safe to call with NULL and safe to call on
   an instance in the terminal error state. */
extern void ogg_opus_repage_free(ogg_opus_repage *repage);

#ifdef __cplusplus
}
#endif

#endif  /* _OGG_OPUS_REPAGE_H */
