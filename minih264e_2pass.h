#ifndef MINIH264E_2PASS_H
#define MINIH264E_2PASS_H

/*
    Two-pass bitrate planning for minih264.

    Pass 1 encodes a clip with the normal encoder and records, for every
    frame, an identity fingerprint plus a complexity/overhead pair (C, H)
    measured from the production bit-writing path:

        s(q)  = 2^((q-26)/6)
        C     = sum over macroblocks of bits_mb * s(qp_mb)
        H     = AU_bits - sum(bits_mb)

    where bits_mb are the VCL syntax bits submitted by that macroblock and
    qp_mb is the QP that macroblock actually used.  Pass 2 is a separate
    process which reads the statistics file, plans a single QP per frame
    against a whole-clip byte target, encodes the same I420 frames, and
    feeds actual costs back into per-class (I/P) factors.

    New-mode restrictions: one spatial layer, one thread, no long-term
    references, no denoise, no VBV stuffing or frame dropping, fixed frame
    size/framerate/GOP, input frames are not reordered.

    Everything here is additive: the classic library calls and command line
    are untouched, and installing the statistics hook does not alter the
    produced bitstream by a single byte.
*/

#include <stddef.h>
#include "minih264e.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H264E_2PASS_OK                  0
#define H264E_2PASS_ERR_ARGUMENT        1
#define H264E_2PASS_ERR_OPEN            2
#define H264E_2PASS_ERR_IO              3
#define H264E_2PASS_ERR_FORMAT          4   /* malformed text / bad numbers   */
#define H264E_2PASS_ERR_MISSING         5   /* fewer records than frames      */
#define H264E_2PASS_ERR_EXTRA           6   /* more records than frames       */
#define H264E_2PASS_ERR_ORDER           7   /* non-consecutive record index   */
#define H264E_2PASS_ERR_IDENTITY        8   /* frame identity/type mismatch   */
#define H264E_2PASS_ERR_STATE           9   /* API used out of order          */

#define H264E_2PASS_FILE_VERSION        1
#define H264E_2PASS_FILE_MAGIC          "minih264e_2pass"
#define H264E_2PASS_FNV_OFFSET          14695981039346656037ULL
#define H264E_2PASS_FNV_PRIME           1099511628211ULL

/** Per-macroblock measurement, valid while the owning frame stats is valid. */
typedef struct
{
    int qp;                 /* QP used to encode this macroblock              */
    unsigned bits;          /* VCL syntax bits submitted by this macroblock   */
} H264E_2pass_mb_t;

/**
*   Public statistics view of the last measured access unit.
*
*   This structure and stats->mb are borrowed: they stay valid until the next
*   writer call (begin/end/abort/close) and must not be freed by the caller.
*   C and H use the definitions at the top of this file.
*/
typedef struct
{
    int input_index;                        /* consecutive input frame number */
    int is_idr;                             /* final IDR (1) / P (0) class     */
    int width, height;                      /* luma dimensions                */
    int mb_count;                           /* number of macroblocks          */
    unsigned long long fnv1a;               /*FNV-1a-64 of valid I420 pixels  */
    double c;                               /* sum(bits_mb * s(qp_mb))        */
    double h;                               /* AU_bits - sum(bits_mb)         */
    const H264E_2pass_mb_t *mb;             /* per-macroblock rows            */
} H264E_2pass_frame_stats_t;

/** One on-disk pass-1 record. */
typedef struct
{
    int index;                              /* consecutive input frame number */
    int is_idr;
    int width, height;
    unsigned long long fnv1a;
    double c, h;
} H264E_2pass_record_t;

/*****************************************************************************/
/*  Measurement helpers                                                      */
/*****************************************************************************/

/** s(q) = 2^((q-26)/6). */
double H264E_2pass_qscale(int qp);

/** FNV-1a-64 over the valid I420 pixels of a frame, row order Y/U/V, no
    stride padding.  Returns the offset basis for invalid arguments. */
unsigned long long H264E_2pass_fnv1a_i420(
    const H264E_io_yuv_t *frame, int width, int height);

/** Length in bits of the unsigned Exp-Golomb code for val (test helper). */
unsigned H264E_2pass_ue_bits(unsigned val);

/** Return 1 if the Annex-B access unit contains an IDR slice, 0 for a P
    slice, -1 if no slice NAL was found. */
int H264E_2pass_au_is_idr(const unsigned char *data, int size);

/*****************************************************************************/
/*  Pass 1 writer                                                            */
/*****************************************************************************/

typedef struct H264E_2pass_writer H264E_2pass_writer_t;

/** Open a pass-1 writer.  Existing file is truncated. */
int H264E_2pass_writer_open(
    H264E_2pass_writer_t **writer, const char *path,
    int width, int height, int gop);

/** Fill a hook to install with H264E_set_mb_stats_hook().  The returned hook
    may be copied; its token points at the writer. */
int H264E_2pass_writer_hook(
    H264E_2pass_writer_t *writer, H264E_mb_stats_hook_t *hook);

/** Prepare for the next frame.  Computes the identity fingerprint from the
    valid pixels of frame.  Must be followed by end() or abort(). */
int H264E_2pass_writer_begin(
    H264E_2pass_writer_t *writer, int input_index,
    const H264E_io_yuv_t *frame);

/** Finish the current frame: au_bytes is the complete Annex-B size returned
    by H264E_encode, is_idr the class observed in the stream.  On success the
    record is appended and stats (optional) describes the frame. */
int H264E_2pass_writer_end(
    H264E_2pass_writer_t *writer, int au_bytes, int is_idr,
    H264E_2pass_frame_stats_t *stats);

/** Discard the current frame (for example after an encode failure). */
int H264E_2pass_writer_abort(H264E_2pass_writer_t *writer);

/** Flush all records to disk and destroy the writer.  frames receives the
    number of records written.  The file is only complete after this call. */
int H264E_2pass_writer_close(H264E_2pass_writer_t *writer, int *frames);

/** Destroy without writing (safe on NULL). */
void H264E_2pass_writer_destroy(H264E_2pass_writer_t *writer);

/*****************************************************************************/
/*  Statistics file reader                                                   */
/*****************************************************************************/

typedef struct H264E_2pass_file H264E_2pass_file_t;

/** Open and fully validate a statistics file. */
int H264E_2pass_file_open(const char *path, H264E_2pass_file_t **file);
int H264E_2pass_file_frames(const H264E_2pass_file_t *file);
int H264E_2pass_file_width(const H264E_2pass_file_t *file);
int H264E_2pass_file_height(const H264E_2pass_file_t *file);
int H264E_2pass_file_gop(const H264E_2pass_file_t *file);
const H264E_2pass_record_t *H264E_2pass_file_record(
    const H264E_2pass_file_t *file, int index);
void H264E_2pass_file_close(H264E_2pass_file_t *file);

/*****************************************************************************/
/*  Pass 2 planner                                                           */
/*****************************************************************************/

typedef struct H264E_2pass_planner H264E_2pass_planner_t;

/** What next() proposes for the current record. */
typedef struct
{
    int record_index;
    int q;                      /* chosen integer QP                          */
    int saturated;              /* 1 if even qp_max cannot meet the budget    */
    double remaining_bytes;     /* target minus committed actual bytes        */
    double predicted_bits;      /* predicted bits of all unprocessed records  */
    double frame_predicted_bits;/* predicted H + a*C/s(q) for this record     */
    double a_i, a_p;            /* feedback factors used for the prediction   */
} H264E_2pass_plan_t;

/** Post-commit diagnostics.  Predicted and actual costs are kept apart. */
typedef struct
{
    int record_index;
    int q;
    int saturated;
    double predicted_bits;      /* total predicted bits at q                  */
    double frame_predicted_bits;
    long actual_bits;           /* actual AU bytes * 8, never the plan value  */
    double remaining_bytes;     /* remaining target after this commit         */
    double a_i, a_p;            /* feedback factors after the update          */
    int committed;
} H264E_2pass_diag_t;

/** Open a planner over count records.  qp_min/qp_max must satisfy
    10 <= qp_min <= qp_max <= 51.  target_bytes must be positive. */
int H264E_2pass_planner_open(
    H264E_2pass_planner_t **planner,
    const H264E_2pass_record_t *records, int count,
    double target_bytes, int qp_min, int qp_max);

/** Propose a QP for the current (not yet committed) record.  Repeated calls
    without a commit are idempotent. */
int H264E_2pass_planner_next(
    H264E_2pass_planner_t *planner, H264E_2pass_plan_t *plan);

/** Account a successfully produced access unit and advance the cursor. */
int H264E_2pass_planner_commit(
    H264E_2pass_planner_t *planner, long actual_au_bytes);

/** Abandon the pending plan: cursor and remaining target are unchanged. */
int H264E_2pass_planner_abort(H264E_2pass_planner_t *planner);

/** 1 when every record has been committed. */
int H264E_2pass_planner_done(const H264E_2pass_planner_t *planner);

/** Total record count, and how many still have to be committed. */
int H264E_2pass_planner_frames(const H264E_2pass_planner_t *planner);
int H264E_2pass_planner_remaining(const H264E_2pass_planner_t *planner);

/** Last committed diagnostics. */
int H264E_2pass_planner_diag(
    const H264E_2pass_planner_t *planner, H264E_2pass_diag_t *diag);

void H264E_2pass_planner_close(H264E_2pass_planner_t *planner);

/** Human-readable error name (never NULL). */
const char *H264E_2pass_strerror(int error);

#ifdef __cplusplus
}
#endif

#endif /* MINIH264E_2PASS_H */
