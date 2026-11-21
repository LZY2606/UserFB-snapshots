/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2026             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: multi-stream Ogg page multiplexer

 ********************************************************************/
#ifndef _OGG_MUX_H
#define _OGG_MUX_H

#include <stddef.h>
#include <ogg/ogg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An ogg_mux_t combines several already-framed logical Ogg bitstreams
   into a single physical stream.  Pages are handed in unchanged; the
   multiplexer never rewrites, re-frames or decodes them.  They must
   come from <ogg/ogg.h> framing (valid BOS, CRC, page numbering and
   EOS), though the mux itself only relies on the page header accessors.

   All functions take the instance pointer first and return an
   ogg_mux_result_t code.  An instance is created with
   ogg_mux_create() and released with ogg_mux_destroy().

   Thread safety is not provided; a single instance must be driven by
   one thread at a time. */

typedef struct ogg_mux_state ogg_mux_t;

/* Return codes.  Every public call reports one of these. */
typedef enum {
  OGG_MUX_OK              = 0,  /* success */
  OGG_MUX_AGAIN           = 1,  /* no safe page yet; try again later */
  OGG_MUX_CAPACITY        = 2,  /* page limit reached; free space and retry */
  OGG_MUX_NEED_BOS        = 3,  /* submit_page: expecting this track's BOS */
  OGG_MUX_NEED_EOS        = 4,  /* reserved; end_input reports INCOMPLETE */
  OGG_MUX_BAD_SERIAL      = 5,  /* unknown serial number */
  OGG_MUX_BAD_PAGE        = 6,  /* malformed page, wrong page number, ... */
  OGG_MUX_BAD_GRANULE     = 7,  /* negative shift of an anchor, EOS granule */
  OGG_MUX_FRONTIER        = 8,  /* would violate a committed watermark */
  OGG_MUX_INCOMPLETE      = 9,  /* end_input while a track lacks EOS */
  OGG_MUX_INVALID         = 10, /* invalid argument to a call */
  OGG_MUX_NOMEM           = 11  /* allocation failed */
} ogg_mux_result_t;

/* Create an instance with fixed, immutable track parameters.

   serials[i] must be unique across all tracks (duplicates are
   rejected).  num/den are positive, rational per-track time bases; the
   scheduling time of a page is granule * num / den, compared exactly
   with integer arithmetic (192-bit cross products) and no floating
   point.  granulepos and num/den may be any non-negative value up to
   2^63-1.

   limit is the maximum number of bytes of page header plus body that
   the mux may hold at once (across all tracks and the ready queue).
   It must be positive.

   The returned instance owns no input pages: every accepted page is
   copied.  Returns NULL on invalid arguments or allocation failure. */
ogg_mux_t *ogg_mux_create(int track_count, const int *serials,
                          const ogg_int64_t *num, const ogg_int64_t *den,
                          size_t limit);

/* Free the instance and all queued page copies. */
void ogg_mux_destroy(ogg_mux_t *mux);

/* Submit one complete physical page (header+body) for the track whose
   serial it carries.  On success the mux keeps its own copy of the
   page; the caller's buffer may be reused immediately.  On failure
   nothing is queued and the mux state (including watermarks and the
   anchor sequence) is unchanged.

   A track's pages must arrive in order: the first from that track must
   be its BOS page with page number 0, followed by consecutive page
   numbers ending in an EOS page.  Non-BOS pages carrying granule -1
   are grouped with the next page of the same track that has a
   non-negative granulepos (the group anchor); a track may not have two
   open groups.  Returns OGG_MUX_CAPACITY if the byte limit would be
   exceeded; the page is not consumed and may be resubmitted unchanged
   after ogg_mux_set_limit() raises the limit.  */
ogg_mux_result_t ogg_mux_submit_page(ogg_mux_t *mux, const ogg_page *page);

/* Advance the watermark of a track to ticks.  Scheduling time is
   granule * num / den, a rational measured in ticks; the watermark is
   an integer time in those same tick units.  After a successful call,
   every non-BOS page subsequently submitted for that track must
   schedule strictly later than ticks.  The watermark is monotonic:
   lowering it is rejected with OGG_MUX_INVALID.  It only constrains
   future submissions; pages already queued stay queued.  Setting the
   watermark for a track that has accepted EOS is a no-op (such a track
   behaves as an infinite watermark). */
ogg_mux_result_t ogg_mux_advance_frontier(ogg_mux_t *mux, int serial,
                                          ogg_int64_t ticks);

/* Change the byte limit.  Raising it is always allowed.  Lowering it
   below the currently held bytes is rejected with OGG_MUX_CAPACITY and
   leaves the existing limit in place. */
ogg_mux_result_t ogg_mux_set_limit(ogg_mux_t *mux, size_t limit);

/* Fetch the next page that is safe to deliver.

   All BOS pages are delivered first, in registration order, once every
   track's BOS has been submitted.  After that, non-BOS groups are
   delivered in increasing scheduling time, ties broken by registration
   order, provided every track that has not yet accepted EOS has
   committed a watermark at least as large as the candidate group's
   time and no track is holding an unanchored group.  Returns OGG_MUX_OK
   and fills *page (pointing at mux-owned storage) on success, or
   OGG_MUX_AGAIN when no page is safe yet.

   The returned page is borrowed: it stays valid only until the next
   call that changes this instance (submit_page, advance_frontier,
   set_limit, end_input, destroy) or the next fetch. */
ogg_mux_result_t ogg_mux_next_page(ogg_mux_t *mux, ogg_page *page);

/* Signal that no more pages or watermarks will be submitted.  Returns
   OGG_MUX_INCOMPLETE if any track has not yet accepted EOS; otherwise
   OGG_MUX_OK.  After end_input, submit_page and advance_frontier are
   rejected with OGG_MUX_INVALID. */
ogg_mux_result_t ogg_mux_end_input(ogg_mux_t *mux);

/* Convenience accessors for diagnostics. */
int ogg_mux_track_count(const ogg_mux_t *mux);
size_t ogg_mux_bytes(const ogg_mux_t *mux);

#ifdef __cplusplus
}
#endif

#endif /* _OGG_MUX_H */
