/*
 * bwf.h - shared definitions for the BWF volume joiner.
 *
 * The timeline pass (bwf_timeline.c) owns input validation, the overlap
 * audit and the streaming render.  The metadata pass (bwf_metadata.c) owns
 * cue / LIST-adtl / smpl validation, ID remapping and output metadata.
 * bwf_join.c only provides main(); the orchestrator it calls lives in the
 * timeline module so both the tool and the test binary can link it.
 */
#ifndef BWF_JOIN_BWF_H
#define BWF_JOIN_BWF_H

#include "dr_wav.h"
#include <stddef.h>
#include <stdio.h>

/* Fixed audio working block: memory never grows with recording length. */
#define BWF_BLOCK_FRAMES   4096u
#define BWF_ERR_SIZE       320

typedef struct
{
    const char *path;
    drwav wav;
    int opened;

    drwav_uint64 timeReference;   /* bext.timeReference, absolute frame position */
    drwav_uint64 frameCount;      /* frames in this volume */
    drwav_uint64 declaredDataSize; /* size written in the data chunk header */
    drwav_uint32 dataChunkCount;  /* data chunks seen while walking the RIFF */
    int hasWavl;                  /* a wavl chunk is present and therefore rejected */

    /* Pointers into wav.pMetadata; NULL when the chunk is absent. */
    drwav_metadata *bext;
    drwav_metadata *cue;
    drwav_metadata *smpl;

    /* Local cue ID namespace and its remap into the output namespace. */
    drwav_uint32 *localIds;
    drwav_uint32 *newIds;
    drwav_uint32 idCount;
} bwf_input;

typedef struct
{
    drwav_uint32 sampleRate;
    drwav_uint32 channels;
    drwav_uint32 bitsPerSample;
    drwav_uint32 bytesPerFrame;

    drwav_uint64 origin;         /* earliest timeReference */
    drwav_uint64 end;            /* latest (timeReference + frameCount) */
    drwav_uint64 totalFrames;    /* end - origin */

    size_t count;
    bwf_input *inputs;

    /* Merged output metadata and the storage it references. */
    drwav_metadata *outMeta;
    drwav_uint32 outMetaCount;
    drwav_cue_point *outCue;
    drwav_smpl_loop *outLoops;
    drwav_uint32 outLoopCount;
    drwav_uint8 *outSmplSpecific;
    drwav_uint32 outSmplSpecificSize;
} bwf_join;

/* Overflow-checked arithmetic. Any failure is a hard rejection. */
static inline int bwf_add_u64(drwav_uint64 a, drwav_uint64 b, drwav_uint64 *out)
{
    if (b > (drwav_uint64)(-1) - a) return 0;
    *out = a + b;
    return 1;
}

/* timeline module */
int bwf_timeline_open(bwf_input *in, const char *path, char *err, size_t errSize);
void bwf_timeline_close(bwf_input *in);
int bwf_timeline_check(bwf_join *job, char *err, size_t errSize);
int bwf_timeline_render(bwf_join *job, FILE *out, char *err, size_t errSize);
int bwf_join_run(int argc, char **argv, char *err, size_t errSize);

/* metadata module */
int bwf_metadata_validate(bwf_input *in, char *err, size_t errSize);
int bwf_metadata_build(bwf_join *job, char *err, size_t errSize);
void bwf_metadata_free(bwf_join *job);

#endif
