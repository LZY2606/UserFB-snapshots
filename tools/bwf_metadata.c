/*
 * bwf_metadata.c - cue / LIST-adtl / smpl validation and merging.
 *
 * Each input owns an independent cue ID namespace.  New output IDs are
 * assigned from 1 in command-line order and then cue-table order; adtl
 * text and smpl loops are rewritten to the new IDs.  All positions are
 * translated by the distance between the volume's origin and the output
 * origin, never through floating point seconds.
 */
#include "bwf.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define BWF_FAIL(...) do { snprintf(err, errSize, __VA_ARGS__); return 0; } while (0)

static int bwf_is_data_fourcc(const drwav_uint8 id[4])
{
    return memcmp(id, "data", 4) == 0;
}

/* Find a local cue ID; returns the table index or -1. */
static int bwf_find_local(const bwf_input *in, drwav_uint32 id)
{
    drwav_uint32 i;
    for (i = 0; i < in->idCount; ++i) {
        if (in->localIds[i] == id) return (int)i;
    }
    return -1;
}

int bwf_metadata_validate(bwf_input *in, char *err, size_t errSize)
{
    drwav_uint32 i, k;
    drwav_uint64 frameCount = in->frameCount;

    if (in->cue != NULL) {
        drwav_cue *cue = &in->cue->data.cue;

        if (cue->cuePointCount == 0)
            BWF_FAIL("%s: empty cue chunk", in->path);

        in->localIds = (drwav_uint32 *)malloc(sizeof(drwav_uint32) * cue->cuePointCount);
        in->newIds = (drwav_uint32 *)malloc(sizeof(drwav_uint32) * cue->cuePointCount);
        if (in->localIds == NULL || in->newIds == NULL)
            BWF_FAIL("out of memory");
        in->idCount = cue->cuePointCount;

        for (i = 0; i < cue->cuePointCount; ++i) {
            drwav_cue_point *cp = &cue->pCuePoints[i];

            if (!bwf_is_data_fourcc(cp->dataChunkId) || cp->chunkStart != 0 || cp->blockStart != 0)
                BWF_FAIL("%s: cue point %u does not target the data chunk (id=%" PRIu32 ")",
                         in->path, (unsigned)i, cp->id);
            if ((drwav_uint64)cp->sampleOffset >= frameCount)
                BWF_FAIL("%s: cue point %" PRIu32 " is outside the volume's frames",
                         in->path, cp->id);
            for (k = 0; k < i; ++k) {
                if (in->localIds[k] == cp->id)
                    BWF_FAIL("%s: duplicate local cue ID %" PRIu32, in->path, cp->id);
            }
            in->localIds[i] = cp->id;
        }
    }

    /* LIST/adtl labels, notes and labelled regions. */
    for (i = 0; i < in->wav.metadataCount; ++i) {
        drwav_metadata *md = &in->wav.pMetadata[i];

        if (md->type == drwav_metadata_type_list_label ||
            md->type == drwav_metadata_type_list_note) {
            if (bwf_find_local(in, md->data.labelOrNote.cuePointId) < 0)
                BWF_FAIL("%s: adtl text references unknown cue ID %" PRIu32,
                         in->path, md->data.labelOrNote.cuePointId);
        } else if (md->type == drwav_metadata_type_list_labelled_cue_region) {
            drwav_list_labelled_cue_region *ltxt = &md->data.labelledCueRegion;
            int idx;
            drwav_uint64 start, end;

            idx = bwf_find_local(in, ltxt->cuePointId);
            if (idx < 0)
                BWF_FAIL("%s: ltxt references unknown cue ID %" PRIu32,
                         in->path, ltxt->cuePointId);
            if (ltxt->sampleLength == 0)
                BWF_FAIL("%s: ltxt for cue ID %" PRIu32 " has zero length",
                         in->path, ltxt->cuePointId);

            start = in->cue->data.cue.pCuePoints[idx].sampleOffset;
            end = start + (drwav_uint64)ltxt->sampleLength;   /* u32 + u32 cannot overflow u64 */
            if (end > frameCount)
                BWF_FAIL("%s: ltxt region for cue ID %" PRIu32 " extends past the volume",
                         in->path, ltxt->cuePointId);
        }
    }

    if (in->smpl != NULL) {
        drwav_smpl *smpl = &in->smpl->data.smpl;

        for (i = 0; i < smpl->sampleLoopCount; ++i) {
            drwav_smpl_loop *loop = &smpl->pLoops[i];
            if (bwf_find_local(in, loop->cuePointId) < 0)
                BWF_FAIL("%s: smpl loop references unknown cue ID %" PRIu32,
                         in->path, loop->cuePointId);
            if (loop->firstSampleOffset > loop->lastSampleOffset ||
                (drwav_uint64)loop->lastSampleOffset >= frameCount)
                BWF_FAIL("%s: smpl loop %u is outside the volume's frames",
                         in->path, (unsigned)i);
        }
    }

    return 1;
}

static int bwf_smpl_nonloop_equal(const drwav_smpl *a, const drwav_smpl *b)
{
    return a->manufacturerId == b->manufacturerId &&
           a->productId == b->productId &&
           a->samplePeriodNanoseconds == b->samplePeriodNanoseconds &&
           a->midiUnityNote == b->midiUnityNote &&
           a->midiPitchFraction == b->midiPitchFraction &&
           a->smpteFormat == b->smpteFormat &&
           a->smpteOffset == b->smpteOffset &&
           a->samplerSpecificDataSizeInBytes == b->samplerSpecificDataSizeInBytes &&
           (a->samplerSpecificDataSizeInBytes == 0 ||
            memcmp(a->pSamplerSpecificData, b->pSamplerSpecificData,
                   a->samplerSpecificDataSizeInBytes) == 0);
}

/* Shifted u32 offset; rejects anything that cannot be represented in the output. */
static int bwf_shift_u32(drwav_uint32 value, drwav_uint64 delta, drwav_uint32 *out)
{
    drwav_uint64 shifted = (drwav_uint64)value + delta;
    if (shifted > (drwav_uint64)0xFFFFFFFFu) return 0;
    *out = (drwav_uint32)shifted;
    return 1;
}

void bwf_metadata_free(bwf_join *job)
{
    free(job->outMeta);
    free(job->outCue);
    free(job->outLoops);
    free(job->outSmplSpecific);
    job->outMeta = NULL;
    job->outCue = NULL;
    job->outLoops = NULL;
    job->outSmplSpecific = NULL;
    job->outMetaCount = 0;
}

int bwf_metadata_build(bwf_join *job, char *err, size_t errSize)
{
    size_t n = job->count, i, mi;
    drwav_uint32 nextId = 1, totalCues = 0, totalAdtl = 0, totalLoops = 0;
    drwav_uint32 smplSpecificSize = 0;
    drwav_metadata *firstSmpl = NULL;
    drwav_uint32 metaCount, cursor;
    drwav_uint64 delta;

    /* First pass: count output objects and assign the new cue IDs. */
    for (i = 0; i < n; ++i) {
        bwf_input *in = &job->inputs[i];
        drwav_uint32 k;

        if ((drwav_uint64)totalCues + in->idCount > (drwav_uint64)0xFFFFFFFFu)
            BWF_FAIL("too many cue points for a RIFF output");
        for (k = 0; k < in->idCount; ++k)
            in->newIds[k] = nextId++;
        totalCues += in->idCount;

        for (mi = 0; mi < in->wav.metadataCount; ++mi) {
            drwav_metadata_type t = in->wav.pMetadata[mi].type;
            if (t == drwav_metadata_type_list_label ||
                t == drwav_metadata_type_list_note ||
                t == drwav_metadata_type_list_labelled_cue_region) {
                if (totalAdtl == (drwav_uint32)0xFFFFFFFFu)
                    BWF_FAIL("too many adtl entries for a RIFF output");
                ++totalAdtl;
            }
        }

        if (in->smpl != NULL) {
            drwav_smpl *s = &in->smpl->data.smpl;
            if (firstSmpl != NULL &&
                !bwf_smpl_nonloop_equal(&firstSmpl->data.smpl, s))
                BWF_FAIL("%s: smpl non-loop parameters differ from %s",
                         in->path, job->inputs[0].path);
            if (firstSmpl == NULL) {
                firstSmpl = in->smpl;
                smplSpecificSize = s->samplerSpecificDataSizeInBytes;
            }
            if ((drwav_uint64)totalLoops + s->sampleLoopCount > (drwav_uint64)0xFFFFFFFFu)
                BWF_FAIL("too many smpl loops for a RIFF output");
            totalLoops += s->sampleLoopCount;
        }
    }

    metaCount = 1u /* bext */ + (totalCues ? 1u : 0u) + totalAdtl + (firstSmpl ? 1u : 0u);
    if ((drwav_uint64)1 + (totalCues ? 1u : 0u) + (drwav_uint64)totalAdtl +
        (firstSmpl ? 1u : 0u) > (drwav_uint64)0xFFFFFFFFu)
        BWF_FAIL("too many metadata objects for a RIFF output");

    job->outMeta = (drwav_metadata *)calloc(metaCount ? metaCount : 1, sizeof(drwav_metadata));
    job->outCue = totalCues ? (drwav_cue_point *)malloc(sizeof(drwav_cue_point) * totalCues) : NULL;
    job->outLoops = totalLoops ? (drwav_smpl_loop *)malloc(sizeof(drwav_smpl_loop) * totalLoops) : NULL;
    job->outSmplSpecific = smplSpecificSize ? (drwav_uint8 *)malloc(smplSpecificSize) : NULL;
    if (job->outMeta == NULL ||
        (totalCues && job->outCue == NULL) ||
        (totalLoops && job->outLoops == NULL) ||
        (smplSpecificSize && job->outSmplSpecific == NULL)) {
        bwf_metadata_free(job);
        BWF_FAIL("out of memory");
    }
    job->outMetaCount = metaCount;
    job->outLoopCount = totalLoops;
    job->outSmplSpecificSize = smplSpecificSize;
    if (smplSpecificSize)
        memcpy(job->outSmplSpecific, firstSmpl->data.smpl.pSamplerSpecificData, smplSpecificSize);

    cursor = 0;

    /* bext: first input's fields, time reference moved to the earliest start. */
    job->outMeta[cursor] = *job->inputs[0].bext;
    job->outMeta[cursor].data.bext.timeReference = job->origin;
    /* dr_wav's writer dereferences these unconditionally; the reader yields NULL for empty fields. */
    if (job->outMeta[cursor].data.bext.pDescription == NULL)
        job->outMeta[cursor].data.bext.pDescription = "";
    if (job->outMeta[cursor].data.bext.pOriginatorName == NULL)
        job->outMeta[cursor].data.bext.pOriginatorName = "";
    if (job->outMeta[cursor].data.bext.pOriginatorReference == NULL)
        job->outMeta[cursor].data.bext.pOriginatorReference = "";
    ++cursor;

    /* Merged cue chunk. */
    if (totalCues) {
        drwav_uint32 out = 0;
        job->outMeta[cursor].type = drwav_metadata_type_cue;
        job->outMeta[cursor].data.cue.cuePointCount = totalCues;
        job->outMeta[cursor].data.cue.pCuePoints = job->outCue;
        ++cursor;

        for (i = 0; i < n; ++i) {
            bwf_input *in = &job->inputs[i];
            drwav_uint32 k;

            delta = in->timeReference - job->origin;
            for (k = 0; k < in->idCount; ++k) {
                drwav_cue_point *src = &in->cue->data.cue.pCuePoints[k];
                drwav_cue_point *dst = &job->outCue[out++];

                dst->id = in->newIds[k];
                dst->playOrderPosition = 0;
                memcpy(dst->dataChunkId, "data", 4);
                dst->chunkStart = 0;
                dst->blockStart = 0;
                if (!bwf_shift_u32(src->sampleOffset, delta, &dst->sampleOffset))
                    BWF_FAIL("%s: shifted cue offset does not fit in 32 bits", in->path);
            }
        }
    }

    /* adtl text, in command order; references point at the remapped cue IDs. */
    for (i = 0; i < n; ++i) {
        bwf_input *in = &job->inputs[i];
        for (mi = 0; mi < in->wav.metadataCount; ++mi) {
            drwav_metadata *src = &in->wav.pMetadata[mi];
            drwav_metadata_type t = src->type;
            int idx;

            if (t != drwav_metadata_type_list_label &&
                t != drwav_metadata_type_list_note &&
                t != drwav_metadata_type_list_labelled_cue_region)
                continue;

            job->outMeta[cursor] = *src;   /* shallow copy: strings stay owned by the input */
            if (t == drwav_metadata_type_list_labelled_cue_region) {
                idx = bwf_find_local(in, src->data.labelledCueRegion.cuePointId);
                job->outMeta[cursor].data.labelledCueRegion.cuePointId = in->newIds[idx];
            } else {
                idx = bwf_find_local(in, src->data.labelOrNote.cuePointId);
                job->outMeta[cursor].data.labelOrNote.cuePointId = in->newIds[idx];
            }
            ++cursor;
        }
    }

    /* Merged smpl: shared non-loop parameters, loops collected in command order. */
    if (firstSmpl) {
        drwav_uint32 out = 0;
        job->outMeta[cursor].type = drwav_metadata_type_smpl;
        job->outMeta[cursor].data.smpl = firstSmpl->data.smpl;
        job->outMeta[cursor].data.smpl.sampleLoopCount = totalLoops;
        job->outMeta[cursor].data.smpl.pLoops = job->outLoops;
        job->outMeta[cursor].data.smpl.samplerSpecificDataSizeInBytes = smplSpecificSize;
        job->outMeta[cursor].data.smpl.pSamplerSpecificData = job->outSmplSpecific;
        ++cursor;

        for (i = 0; i < n; ++i) {
            bwf_input *in = &job->inputs[i];
            drwav_uint32 k;

            if (in->smpl == NULL) continue;
            delta = in->timeReference - job->origin;
            for (k = 0; k < in->smpl->data.smpl.sampleLoopCount; ++k) {
                drwav_smpl_loop *src = &in->smpl->data.smpl.pLoops[k];
                drwav_smpl_loop *dst = &job->outLoops[out++];
                int idx = bwf_find_local(in, src->cuePointId);

                *dst = *src;   /* type, play count and fraction are preserved */
                dst->cuePointId = in->newIds[idx];
                if (!bwf_shift_u32(src->firstSampleOffset, delta, &dst->firstSampleOffset) ||
                    !bwf_shift_u32(src->lastSampleOffset, delta, &dst->lastSampleOffset))
                    BWF_FAIL("%s: shifted smpl loop offset does not fit in 32 bits", in->path);
            }
        }
    }

    if (cursor != metaCount)
        BWF_FAIL("internal metadata bookkeeping error");
    return 1;
}
