/*
Incremental (pausable, macroblock-stepping) H.261 encoding interface.

This interface sits on top of the classic whole-frame entry points
(h261_write_picture_header / h261_encode_gobs / h261_bitstream_flush).
Those entry points keep working unchanged; the incremental interface is
an addition, not a replacement.  For the same input planes, picture
parameters and rate-control configuration it produces, bit for bit, the
same byte stream, the same reconstructed planes and the same rate-control
(VBR/QUANT) decisions as the whole-frame path, no matter how stepping
and pulling are interleaved.

Overview
--------
A h261_incremental_t instance is bound to an already initialized
h261_context_t (source format, frame planes, macroblock cache, pixelop
and transform functions and rate control set up as usual).  The caller
fills the context's s_current_frame Y/Cb/Cr planes exactly as for the
whole-frame path, starts a picture, then alternates macroblock-budgeted
steps with byte pulls until the picture completes, and finally ends the
sequence.

Contract
--------
- h261inc_begin_picture() copies the picture parameters; the caller must
  keep the input planes valid and unmodified until the picture completes.
  Starting while a picture is in progress, while unconsumed output is
  still buffered, or after the sequence was ended fails (non-zero return)
  and leaves the instance state unchanged.
- h261inc_step() commits at most i_mb_budget macroblocks; a budget of
  zero commits nothing.  Each macroblock passes through the regular
  encode/reconstruct path exactly once; nothing is pre-encoded and no
  GOB is replayed.  All coding state that matters across a pause
  (skipped-block address increments, quantiser inheritance, motion
  prediction, row-start resets) lives in the instance/context and
  survives any interleaving of steps and pulls, including pauses on the
  33rd-macroblock / next-GOB-header boundary.  The step result reports
  how many macroblocks were actually committed, whether output bytes
  need to be pulled, and whether the picture finished.  When unconsumed
  output occupies the bounded staging buffer a step may commit fewer
  macroblocks than requested, or none at all.
- h261inc_pull() copies complete bytes, in encoding order, into the
  caller's buffer and consumes them; the caller's buffer may be reused
  immediately.  A capacity of zero consumes nothing.  Bytes already
  delivered are never delivered again.  Bits that do not yet fill a byte
  are kept across pulls, GOB boundaries and picture boundaries without
  padding.
- The reference-plane update and the VBR rate-control update happen
  exactly once per picture, at the commit of its last macroblock; they
  never depend on how many or how large the pulls were.
- h261inc_end() is only legal when no picture is in progress.  It pads
  out at most one final byte, exactly like h261_bitstream_flush().
  Ending twice or pulling after the buffer ran dry produces no further
  data.  After h261inc_end(), h261inc_begin_picture() always fails.
- Instances are independent of each other.  Concurrency on a single
  instance and cancellation of a picture in progress are not supported.

Memory bound
------------
While an instance exists, the context's bitstream writer is swapped for
a bounded staging buffer of H261INC_STORAGE_CAPACITY bytes (the old
writer's buffer is released and re-created on destroy).  Together with
the small instance struct this is the complete compressed storage of the
incremental path: no hidden whole-frame compressed copy exists anywhere.
When the staging buffer cannot hold another macroblock's bits, stepping
pauses (commits zero) until bytes are pulled; no codeword is ever lost.
A single macroblock is always encoded straight into this bounded
staging.  The peak amount of buffered-but-unpulled bytes is observable
through h261inc_peak().

Observability
-------------
h261inc_committed() reports the real macroblock cursor of the current
picture (not a replay of step return values), h261inc_pending() the
bytes waiting right now, h261inc_capacity() the staging capacity and
h261inc_peak() the high-water mark of pending bytes.
*/

#ifndef __H261_INCREMENTAL_H__
#define __H261_INCREMENTAL_H__

#include "h261_decl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded compressed staging, in bytes.  Must stay <= 8192. */
#define H261INC_STORAGE_CAPACITY 4096

typedef struct h261_incremental_s h261_incremental_t;

/* Result of one h261inc_step() call. */
typedef struct {
	Int32 i_committed;       /* macroblocks committed by this call */
	Int32 i_output_pending;  /* 1 while unpulled bytes wait in staging */
	Int32 i_picture_done;    /* 1 when no picture is in progress */
} h261inc_step_t;

/* Bind a new instance to an initialized context.  NULL on failure. */
h261_incremental_t *h261inc_create( h261_context_t *ps_ctx );

/* Detach and free the instance; restores the context's original
   bitstream writer so the whole-frame path keeps working. */
Void h261inc_destroy( h261_incremental_t *ps_inc );

/* Copy the picture parameters and start a picture.
   Returns 0 on success, -1 (state unchanged) when a picture is in
   progress, when unconsumed output is still buffered, or after end. */
Int32 h261inc_begin_picture( h261_incremental_t *ps_inc, const h261_picture_parameters_t *ps_parameters );

/* Commit up to i_mb_budget macroblocks.  Returns 0 on success and fills
   ps_out (may be NULL), -1 when no picture is in progress. */
Int32 h261inc_step( h261_incremental_t *ps_inc, Int32 i_mb_budget, h261inc_step_t *ps_out );

/* Move up to ui_capacity complete bytes into pui8_dst, in encoding
   order.  Returns the number of bytes actually delivered. */
UInt32 h261inc_pull( h261_incremental_t *ps_inc, UInt8 *pui8_dst, UInt32 ui_capacity );

/* End the sequence, padding out at most one final byte.  Returns 0 on
   success (repeated calls are harmless no-ops), -1 while a picture is
   in progress. */
Int32 h261inc_end( h261_incremental_t *ps_inc );

/* Observability. */
Int32  h261inc_committed( const h261_incremental_t *ps_inc ); /* MBs committed, current picture */
UInt32 h261inc_pending( const h261_incremental_t *ps_inc );   /* unpulled bytes right now */
UInt32 h261inc_capacity( const h261_incremental_t *ps_inc );  /* staging capacity in bytes */
UInt32 h261inc_peak( const h261_incremental_t *ps_inc );      /* pending high-water mark */
Int32  h261inc_busy( const h261_incremental_t *ps_inc );      /* picture in progress */
Int32  h261inc_ended( const h261_incremental_t *ps_inc );     /* sequence ended */

#ifdef __cplusplus
}
#endif

#endif
