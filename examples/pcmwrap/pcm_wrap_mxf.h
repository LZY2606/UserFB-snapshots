/*
 * Audio-only OP1a MXF partition, metadata and index writing
 *
 * Internal interface between the streaming PCM wrapper and the libMXF
 * partition/metadata/index primitives.
 */

#ifndef PCM_WRAP_MXF_H_
#define PCM_WRAP_MXF_H_


#include <stdint.h>

#include <mxf/mxf.h>

#include "pcm_frame.h"


#ifdef __cplusplus
extern "C"
{
#endif


#define PCM_WRAP_KAG_SIZE               512
#define PCM_WRAP_BODY_SID               1
#define PCM_WRAP_INDEX_SID              1
#define PCM_WRAP_MAX_UNITS_PER_BODY     1024


typedef struct PCMMxfMuxer PCMMxfMuxer;


/*
 * Create the muxer and write the header partition pack and the placeholder
 * (zero duration) header metadata graph. The file cursor ends at the start of
 * the first body partition's content area. Returns NULL on failure.
 */
PCMMxfMuxer *pcm_mxf_create(MXFFile *mxfFile, const PCMFrameConfig *config,
                            uint32_t maxEditUnitsPerBodyPartition);

/*
 * Write one completed BWFFrameWrapped audio edit unit, starting a new body
 * partition (KAG aligned) whenever the configured limit is reached. Records
 * the per-unit essence stream offset. Returns 0 on failure.
 */
int pcm_mxf_write_unit(PCMMxfMuxer *muxer, const uint8_t *unit, uint32_t byteLength);

/*
 * Fill final durations, rewrite the header metadata, close the partition
 * chain and write the footer partition, one VBE index table segment per body
 * partition and the random index pack. Returns 0 on failure.
 */
int pcm_mxf_finish(PCMMxfMuxer *muxer, uint64_t completedUnits);

/* Free all muxer resources. The MXFFile is not closed. */
void pcm_mxf_destroy(PCMMxfMuxer *muxer);

/* Number of body partitions opened so far. */
uint32_t pcm_mxf_num_bodies(const PCMMxfMuxer *muxer);


#ifdef __cplusplus
}
#endif


#endif
