/*
Offline contract tests for ma_sound_set_volume_envelope().

These tests run against a device-less engine (periodSizeInFrames = 0) and use only data buffer
sources, so no resource manager or decoder is involved.
*/
#include "miniaudio.h"
#include "task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SENTINEL        (~(ma_uint64)0)
#define TEST_RATE       48000
#define ENGINE_CHANNELS 2
#define MAX_FRAMES      1024
#define ENV_POINTS_MAX  8

typedef struct
{
    ma_engine         engine;
    ma_audio_buffer_ref buffer;
    ma_sound          sound;
    ma_data_source*   pSource;
    ma_bool32         bufferInitialized;
} rig;

typedef struct
{
    ma_format format;
    ma_uint32 channels;
    const void* pData;
    ma_uint32 frameCount;
    ma_uint64 rangeBeg;
    ma_uint64 rangeEnd;     /* SENTINEL for no explicit range. */
    ma_uint64 loopBeg;
    ma_uint64 loopEnd;      /* SENTINEL for no explicit loop point. */
    ma_bool32 looping;
    float pitch;
    ma_bool32 usePitch;
    ma_uint64 seekBeforeStart;  /* SENTINEL for no seek. */
} rig_desc;

/* Static storage. ma_engine and ma_sound are large, so keep them off the stack. */
static rig g_rigA;
static rig g_rigB;

static float g_outA[ENGINE_CHANNELS * MAX_FRAMES];
static float g_outB[ENGINE_CHANNELS * MAX_FRAMES];

static float g_srcF32[2 * 128];
static float g_refF32[2 * 128];
static ma_int16 g_srcS16[2 * 128];


/* --------------------------------------------------------------------------------------------- */
/* Reference envelope evaluation. This is intentionally independent of the library implementation. */
/* --------------------------------------------------------------------------------------------- */
static float ref_gain(const ma_volume_envelope_point* pPoints, ma_uint32 pointCount, ma_uint64 frame)
{
    ma_uint32 i;

    if (pPoints == NULL || pointCount == 0) {
        return 1.0f;
    }

    if (frame <= pPoints[0].frameIndex) {
        return pPoints[0].volume;
    }
    if (frame >= pPoints[pointCount - 1].frameIndex) {
        return pPoints[pointCount - 1].volume;
    }

    for (i = 0; i + 1 < pointCount; i += 1) {
        if (frame >= pPoints[i].frameIndex && frame <= pPoints[i + 1].frameIndex) {
            double t = (double)(frame - pPoints[i].frameIndex) / (double)(pPoints[i + 1].frameIndex - pPoints[i].frameIndex);
            return (float)((double)pPoints[i].volume + ((double)pPoints[i + 1].volume - (double)pPoints[i].volume) * t);
        }
    }

    return pPoints[pointCount - 1].volume;
}

/*
Builds a reference copy of the source material with the envelope already applied the way the engine
is supposed to apply it: to f32 samples, indexed by source frame relative to the start of the range.
*/
static void ref_preweight_f32(float* pDst, const float* pSrc, ma_uint32 frameCount, ma_uint32 channels, ma_uint64 rangeBeg, const ma_volume_envelope_point* pPoints, ma_uint32 pointCount)
{
    ma_uint32 iFrame;
    ma_uint32 iChannel;

    for (iFrame = 0; iFrame < frameCount; iFrame += 1) {
        ma_uint64 relative = (iFrame >= rangeBeg) ? (iFrame - rangeBeg) : 0;
        float gain = ref_gain(pPoints, pointCount, relative);
        for (iChannel = 0; iChannel < channels; iChannel += 1) {
            pDst[iFrame * channels + iChannel] = pSrc[iFrame * channels + iChannel] * gain;
        }
    }
}


/* --------------------------------------------------------------------------------------------- */
/* Rig helpers.                                                                                   */
/* --------------------------------------------------------------------------------------------- */
static void rig_uninit(rig* pRig)
{
    ma_sound_uninit(&pRig->sound);

    if (pRig->bufferInitialized) {
        ma_audio_buffer_ref_uninit(&pRig->buffer);
        pRig->bufferInitialized = MA_FALSE;
    }

    ma_engine_uninit(&pRig->engine);
    memset(pRig, 0, sizeof(*pRig));
}

static ma_result rig_init(rig* pRig, const rig_desc* pDesc, const ma_allocation_callbacks* pAllocationCallbacks)
{
    ma_result result;
    ma_engine_config engineConfig;
    ma_uint32 flags;

    memset(pRig, 0, sizeof(*pRig));

    engineConfig = ma_engine_config_init();
    engineConfig.noDevice                      = MA_TRUE;
    engineConfig.channels                      = ENGINE_CHANNELS;
    engineConfig.sampleRate                    = TEST_RATE;
    engineConfig.periodSizeInFrames            = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;
    if (pAllocationCallbacks != NULL) {
        engineConfig.allocationCallbacks = *pAllocationCallbacks;
    }

    result = ma_engine_init(&engineConfig, &pRig->engine);
    if (result != MA_SUCCESS) {
        return result;
    }

    result = ma_audio_buffer_ref_init(pDesc->format, pDesc->channels, pDesc->pData, pDesc->frameCount, &pRig->buffer);
    if (result != MA_SUCCESS) {
        ma_engine_uninit(&pRig->engine);
        return result;
    }
    pRig->bufferInitialized = MA_TRUE;
    pRig->pSource = (ma_data_source*)&pRig->buffer;

    if (pDesc->rangeEnd != SENTINEL) {
        result = ma_data_source_set_range_in_pcm_frames(pRig->pSource, pDesc->rangeBeg, pDesc->rangeEnd);
        if (result != MA_SUCCESS) {
            rig_uninit(pRig);
            return result;
        }
    }

    if (pDesc->loopEnd != SENTINEL) {
        result = ma_data_source_set_loop_point_in_pcm_frames(pRig->pSource, pDesc->loopBeg, pDesc->loopEnd);
        if (result != MA_SUCCESS) {
            rig_uninit(pRig);
            return result;
        }
    }

    flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (pDesc->usePitch == MA_FALSE) {
        flags |= MA_SOUND_FLAG_NO_PITCH;
    }

    result = ma_sound_init_from_data_source(&pRig->engine, pRig->pSource, flags, NULL, &pRig->sound);
    if (result != MA_SUCCESS) {
        rig_uninit(pRig);
        return result;
    }

    if (pDesc->usePitch) {
        ma_sound_set_pitch(&pRig->sound, pDesc->pitch);
    }

    ma_sound_set_looping(&pRig->sound, pDesc->looping);

    if (pDesc->seekBeforeStart != SENTINEL) {
        result = ma_sound_seek_to_pcm_frame(&pRig->sound, pDesc->seekBeforeStart);
        if (result != MA_SUCCESS) {
            rig_uninit(pRig);
            return result;
        }
    }

    return MA_SUCCESS;
}

/* Renders up to frameCount frames using a repeating chunk-size pattern. Stops early if the engine
   returns no data. Returns the number of frames actually produced. */
static ma_uint64 rig_render(rig* pRig, float* pOut, ma_uint64 frameCount, const ma_uint32* pChunkSizes, ma_uint32 chunkCount)
{
    ma_uint64 total = 0;
    ma_uint32 chunkIndex = 0;

    while (total < frameCount) {
        ma_uint64 want = frameCount - total;
        ma_uint32 chunk = pChunkSizes[chunkIndex % chunkCount];
        ma_uint64 got = 0;
        ma_result result;

        chunkIndex += 1;
        if ((ma_uint64)chunk < want) {
            want = chunk;
        }

        result = ma_engine_read_pcm_frames(&pRig->engine, pOut + (total * ENGINE_CHANNELS), (ma_uint32)want, &got);
        if (result != MA_SUCCESS || got == 0) {
            break;
        }

        total += got;
    }

    return total;
}

/* Renders both rigs with independent chunk patterns and compares the overlapping output. Returns 0
   on success. */
static int render_and_compare(const char* pLabel, rig* pA, rig* pB, ma_uint64 frameCount, const ma_uint32* pChunksA, ma_uint32 chunksACount, const ma_uint32* pChunksB, ma_uint32 chunksBCount, float tolerance)
{
    ma_uint64 framesA;
    ma_uint64 framesB;
    ma_uint64 frames;
    ma_uint64 i;
    float maxError = 0;

    framesA = rig_render(pA, g_outA, frameCount, pChunksA, chunksACount);
    framesB = rig_render(pB, g_outB, frameCount, pChunksB, chunksBCount);

    if (framesA != framesB) {
        return 0;   /* Test failure (length mismatch). Reported by the caller via TASK_CHECK. */
    }

    frames = framesA;
    for (i = 0; i < frames * ENGINE_CHANNELS; i += 1) {
        float d = fabsf(g_outA[i] - g_outB[i]);
        if (d > maxError) {
            maxError = d;
        }
    }

    (void)pLabel;
    return (maxError <= tolerance);
}

/* Checks the direct output of a unity-amplitude source against an expected per-frame gain sequence
   that the test computes from the cursor rules. */
static int check_direct_gain(rig* pRig, const ma_volume_envelope_point* pPoints, ma_uint32 pointCount, ma_uint32 frameCount, ma_uint64 startCursor, ma_uint64 lengthInRange, ma_bool32 looping, ma_uint64 loopBeg, ma_uint64 loopEnd, const ma_uint32* pChunks, ma_uint32 chunkCount, float tolerance)
{
    ma_uint64 got;
    ma_uint64 cursor = startCursor;
    ma_uint64 boundedEnd = lengthInRange;
    ma_uint32 i;

    if (looping && loopEnd != SENTINEL) {
        boundedEnd = loopEnd;
    }

    got = rig_render(pRig, g_outA, frameCount, pChunks, chunkCount);
    if (got != frameCount) {
        return 0;
    }

    for (i = 0; i < frameCount; i += 1) {
        float expected;
        ma_uint32 iChannel;

        if (cursor >= boundedEnd) {
            if (looping) {
                cursor = loopBeg;
            } else {
                break;
            }
        }

        expected = ref_gain(pPoints, pointCount, cursor);
        for (iChannel = 0; iChannel < ENGINE_CHANNELS; iChannel += 1) {
            /* The source is unity in the carrier channel and half amplitude in the other, so the
               expected output is the shared gain scaled by the channel's base level. */
            float base = (iChannel == 0) ? 1.0f : 0.5f;
            if (fabsf(g_outA[i * ENGINE_CHANNELS + iChannel] - base * expected) > tolerance) {
                return 0;
            }
        }

        cursor += 1;
    }

    return 1;
}


/* --------------------------------------------------------------------------------------------- */
/* Custom data source with short reads.                                                           */
/* --------------------------------------------------------------------------------------------- */
typedef struct
{
    ma_data_source_base base;
    const float* pData;
    ma_uint32 channels;
    ma_uint64 lengthInFrames;
    ma_uint64 cursor;
    ma_uint64 maxFramesPerRead;
} short_source;

static ma_result short_source_on_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    short_source* pSource = (short_source*)pDataSource;
    ma_uint64 framesAvailable = pSource->lengthInFrames - pSource->cursor;
    ma_uint64 framesToRead = frameCount;

    if (framesToRead > pSource->maxFramesPerRead) {
        framesToRead = pSource->maxFramesPerRead;
    }
    if (framesToRead > framesAvailable) {
        framesToRead = framesAvailable;
    }

    if (framesToRead > 0) {
        memcpy(pFramesOut, pSource->pData + (pSource->cursor * pSource->channels), (size_t)framesToRead * pSource->channels * sizeof(float));
        pSource->cursor += framesToRead;
    }

    if (pFramesRead != NULL) {
        *pFramesRead = framesToRead;
    }

    /* Report the end only when nothing was read. Short reads that did produce data are a success. */
    return (framesToRead == 0) ? MA_AT_END : MA_SUCCESS;
}

static ma_result short_source_on_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    short_source* pSource = (short_source*)pDataSource;

    if (frameIndex > pSource->lengthInFrames) {
        return MA_INVALID_ARGS;
    }

    pSource->cursor = frameIndex;
    return MA_SUCCESS;
}

static ma_result short_source_on_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
{
    short_source* pSource = (short_source*)pDataSource;

    if (pFormat != NULL) {
        *pFormat = ma_format_f32;
    }
    if (pChannels != NULL) {
        *pChannels = pSource->channels;
    }
    if (pSampleRate != NULL) {
        *pSampleRate = 0;
    }
    ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, channelMapCap, pSource->channels);
    return MA_SUCCESS;
}

static ma_result short_source_on_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
    short_source* pSource = (short_source*)pDataSource;
    *pCursor = pSource->cursor;
    return MA_SUCCESS;
}

static ma_result short_source_on_get_length(ma_data_source* pDataSource, ma_uint64* pLength)
{
    short_source* pSource = (short_source*)pDataSource;
    *pLength = pSource->lengthInFrames;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_shortSourceVTable = {
    short_source_on_read,
    short_source_on_seek,
    short_source_on_get_data_format,
    short_source_on_get_cursor,
    short_source_on_get_length,
    NULL,
    0
};

static ma_result short_source_init(short_source* pSource, const float* pData, ma_uint32 channels, ma_uint64 lengthInFrames, ma_uint64 maxFramesPerRead)
{
    ma_data_source_config config = ma_data_source_config_init();
    ma_result result;

    memset(pSource, 0, sizeof(*pSource));
    config.vtable = &g_shortSourceVTable;

    result = ma_data_source_init(&config, &pSource->base);
    if (result != MA_SUCCESS) {
        return result;
    }

    pSource->pData = pData;
    pSource->channels = channels;
    pSource->lengthInFrames = lengthInFrames;
    pSource->cursor = 0;
    pSource->maxFramesPerRead = maxFramesPerRead;
    return MA_SUCCESS;
}


/* --------------------------------------------------------------------------------------------- */
/* Allocation tracking.                                                                           */
/* --------------------------------------------------------------------------------------------- */
typedef struct
{
    unsigned mallocCount;
    unsigned reallocCount;
    unsigned freeCount;
    ma_int64 outstanding;
} alloc_stats;

static void* alloc_on_malloc(size_t sizeInBytes, void* pUserData)
{
    alloc_stats* pStats = (alloc_stats*)pUserData;
    pStats->mallocCount += 1;
    pStats->outstanding += 1;
    return malloc(sizeInBytes);
}

static void* alloc_on_realloc(void* p, size_t sizeInBytes, void* pUserData)
{
    alloc_stats* pStats = (alloc_stats*)pUserData;
    pStats->reallocCount += 1;
    if (p == NULL) {
        pStats->outstanding += 1;
    }
    return realloc(p, sizeInBytes);
}

static void alloc_on_free(void* p, void* pUserData)
{
    alloc_stats* pStats = (alloc_stats*)pUserData;
    if (p != NULL) {
        pStats->freeCount += 1;
        pStats->outstanding -= 1;
        free(p);
    }
}


/* --------------------------------------------------------------------------------------------- */
/* Shared test data.                                                                              */
/* --------------------------------------------------------------------------------------------- */
static void init_sources(void)
{
    ma_uint32 i;
    for (i = 0; i < 128; i += 1) {
        /* Asymmetric stereo with non-trivial signs and slopes. */
        g_srcF32[2 * i]     = ((float)((i * 37) % 101) - 50.0f) / 60.0f;
        g_srcF32[2 * i + 1] = ((float)((i * 19) % 83) - 40.0f) / 45.0f;
        g_srcS16[2 * i]     = (ma_int16)(g_srcF32[2 * i] * 20000.0f);
        g_srcS16[2 * i + 1] = (ma_int16)(g_srcF32[2 * i + 1] * 12000.0f);
    }
}

/* Unity amplitude source for direct gain inspection. */
static void init_unity_source(void)
{
    ma_uint32 i;
    for (i = 0; i < 128; i += 1) {
        g_srcF32[2 * i]     = 1.0f;
        g_srcF32[2 * i + 1] = 0.5f;
        g_refF32[2 * i]     = 1.0f;
        g_refF32[2 * i + 1] = 0.5f;
    }
}

static const ma_volume_envelope_point g_curve[] = {
    { 0,  0.40f },
    { 6,  2.50f },
    { 15, 0.10f },
    { 31, 1.90f }
};
static const ma_uint32 g_curveCount = sizeof(g_curve) / sizeof(g_curve[0]);


/* --------------------------------------------------------------------------------------------- */
/* Group: curve                                                                                   */
/* --------------------------------------------------------------------------------------------- */

/* Interpolation between points, endpoint holds, and one shared gain for every channel. */
static int case_curve_interpolation_and_holds(void)
{
    rig_desc desc;
    ma_volume_envelope_point points[3] = {
        { 4,  1.25f },
        { 12, 0.20f },
        { 25, 3.00f }
    };
    const ma_uint32 chunks[3] = { 5, 1, 9 };
    ma_uint64 got;
    ma_uint32 i;

    init_unity_source();

    memset(&desc, 0, sizeof(desc));
    desc.format            = ma_format_f32;
    desc.channels          = 2;
    desc.pData             = g_srcF32;
    desc.frameCount        = 64;
    desc.rangeBeg          = 0;
    desc.rangeEnd          = SENTINEL;
    desc.loopBeg           = 0;
    desc.loopEnd           = SENTINEL;
    desc.looping           = MA_FALSE;
    desc.pitch             = 1.0f;
    desc.usePitch          = MA_FALSE;
    desc.seekBeforeStart   = SENTINEL;

    TASK_CHECK(rig_init(&g_rigA, &desc, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, points, 3) == MA_SUCCESS);
    ma_sound_start(&g_rigA.sound);

    got = rig_render(&g_rigA, g_outA, 40, chunks, 3);
    TASK_CHECK(got == 40);

    for (i = 0; i < 40; i += 1) {
        float gain = ref_gain(points, 3, i);
        TASK_CHECK(fabsf(g_outA[2 * i]     - gain)       < 1e-6f);
        TASK_CHECK(fabsf(g_outA[2 * i + 1] - 0.5f * gain) < 1e-6f);
    }

    rig_uninit(&g_rigA);
    return 0;
}

/* The point list is copied, and invalid lists leave the previous configuration untouched. */
static int case_curve_copy_and_validation(void)
{
    rig_desc desc;
    ma_volume_envelope_point stackPoints[3] = {
        { 3,  0.75f },
        { 10, 2.25f },
        { 21, 1.00f }
    };
    ma_volume_envelope_point keep[3];
    ma_volume_envelope_point badPoints[4];
    const ma_uint32 chunks[2] = { 6, 3 };
    ma_uint64 got;
    ma_uint32 i;
    void* pDataSourceBefore;

    init_unity_source();
    memcpy(keep, stackPoints, sizeof(keep));

    memset(&desc, 0, sizeof(desc));
    desc.format            = ma_format_f32;
    desc.channels          = 2;
    desc.pData             = g_srcF32;
    desc.frameCount        = 64;
    desc.rangeBeg          = 0;
    desc.rangeEnd          = SENTINEL;
    desc.loopBeg           = 0;
    desc.loopEnd           = SENTINEL;
    desc.looping           = MA_FALSE;
    desc.pitch             = 1.0f;
    desc.usePitch          = MA_FALSE;
    desc.seekBeforeStart   = SENTINEL;

    TASK_CHECK(rig_init(&g_rigA, &desc, NULL) == MA_SUCCESS);
    pDataSourceBefore = (void*)ma_sound_get_data_source(&g_rigA.sound);

    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, stackPoints, 3) == MA_SUCCESS);
    ma_sound_start(&g_rigA.sound);

    /* Wreck the caller's array. The sound must keep its own private copy. */
    for (i = 0; i < 3; i += 1) {
        stackPoints[i].frameIndex = 1000 + i;
        stackPoints[i].volume     = 1234.5f;
    }

    TASK_CHECK((void*)ma_sound_get_data_source(&g_rigA.sound) == pDataSourceBefore);

    /* Every rejected list must leave the copied curve in place. */
    badPoints[0].frameIndex = 1;  badPoints[0].volume = 1.0f;
    badPoints[1].frameIndex = 1;  badPoints[1].volume = 2.0f;
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, badPoints, 2) == MA_INVALID_ARGS);   /* Not strictly increasing. */

    badPoints[0].frameIndex = 5;  badPoints[0].volume = -0.5f;
    badPoints[1].frameIndex = 9;  badPoints[1].volume =  1.0f;
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, badPoints, 2) == MA_INVALID_ARGS);   /* Negative gain. */

    badPoints[0].frameIndex = 5;  badPoints[0].volume = (float)NAN;
    badPoints[1].frameIndex = 9;  badPoints[1].volume = 1.0f;
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, badPoints, 2) == MA_INVALID_ARGS);   /* NaN gain. */

    badPoints[0].frameIndex = 5;  badPoints[0].volume = (float)INFINITY;
    badPoints[1].frameIndex = 9;  badPoints[1].volume = 1.0f;
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, badPoints, 2) == MA_INVALID_ARGS);   /* Infinite gain. */

    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, NULL, 2) == MA_INVALID_ARGS);        /* Non-empty list with a NULL array. */
    TASK_CHECK(ma_sound_set_volume_envelope(NULL, keep, 3) == MA_INVALID_ARGS);                 /* NULL sound. */

    got = rig_render(&g_rigA, g_outA, 30, chunks, 2);
    TASK_CHECK(got == 30);
    for (i = 0; i < 30; i += 1) {
        float gain = ref_gain(keep, 3, i);
        TASK_CHECK(fabsf(g_outA[2 * i]     - gain)       < 1e-6f);
        TASK_CHECK(fabsf(g_outA[2 * i + 1] - 0.5f * gain) < 1e-6f);
    }

    /* Clearing with a zero count and a NULL array restores unity gain. */
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, NULL, 0) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, stackPoints, 0) == MA_SUCCESS);

    TASK_CHECK(ma_sound_seek_to_pcm_frame(&g_rigA.sound, 0) == MA_SUCCESS);
    got = rig_render(&g_rigA, g_outA, 10, chunks, 2);
    TASK_CHECK(got == 10);
    for (i = 0; i < 10; i += 1) {
        TASK_CHECK(fabsf(g_outA[2 * i]     - 1.0f) < 1e-6f);
        TASK_CHECK(fabsf(g_outA[2 * i + 1] - 0.5f) < 1e-6f);
    }

    /* A single point is a constant curve that applies everywhere. */
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, keep, 1) == MA_SUCCESS);
    TASK_CHECK(ma_sound_seek_to_pcm_frame(&g_rigA.sound, 0) == MA_SUCCESS);
    got = rig_render(&g_rigA, g_outA, 10, chunks, 2);
    TASK_CHECK(got == 10);
    for (i = 0; i < 10; i += 1) {
        TASK_CHECK(fabsf(g_outA[2 * i]     - keep[0].volume) < 1e-6f);
        TASK_CHECK(fabsf(g_outA[2 * i + 1] - 0.5f * keep[0].volume) < 1e-6f);
    }

    rig_uninit(&g_rigA);
    return 0;
}


/* --------------------------------------------------------------------------------------------- */
/* Group: source_clock                                                                            */
/* --------------------------------------------------------------------------------------------- */

/* Frame indices are relative to the start of the range, not to the underlying source's absolute
   position and not to the number of frames already sent to the speaker. */
static int case_clock_relative_to_range(void)
{
    rig_desc desc;

    init_unity_source();

    memset(&desc, 0, sizeof(desc));
    desc.format            = ma_format_f32;
    desc.channels          = 2;
    desc.pData             = g_srcF32;
    desc.frameCount        = 96;
    desc.rangeBeg          = 9;
    desc.rangeEnd          = 29;      /* 20 frames long. */
    desc.loopBeg           = 0;
    desc.loopEnd           = SENTINEL;
    desc.looping           = MA_FALSE;
    desc.pitch             = 1.0f;
    desc.usePitch          = MA_FALSE;
    desc.seekBeforeStart   = SENTINEL;

    TASK_CHECK(rig_init(&g_rigA, &desc, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);
    ma_sound_start(&g_rigA.sound);

    {
        const ma_uint32 chunks[3] = { 4, 1, 7 };
        TASK_CHECK(check_direct_gain(&g_rigA, g_curve, g_curveCount, 20, 0, 20, MA_FALSE, 0, SENTINEL, chunks, 3, 1e-6f));
    }

    rig_uninit(&g_rigA);
    return 0;
}

/* A natural loop re-enters the curve at the loop's own source position, not at frame 0 and not at
   the first key frame. This also proves the clock is not the output frame counter. */
static int case_clock_loop_returns_to_loop_point(void)
{
    rig_desc desc;
    ma_volume_envelope_point points[3] = {
        { 0,  3.00f },
        { 5,  0.50f },
        { 12, 7.00f }
    };

    init_unity_source();

    memset(&desc, 0, sizeof(desc));
    desc.format            = ma_format_f32;
    desc.channels          = 2;
    desc.pData             = g_srcF32;
    desc.frameCount        = 96;
    desc.rangeBeg          = 4;
    desc.rangeEnd          = 28;      /* 24 frames long. */
    desc.loopBeg           = 5;
    desc.loopEnd           = 12;
    desc.looping           = MA_TRUE;
    desc.pitch             = 1.0f;
    desc.usePitch          = MA_FALSE;
    desc.seekBeforeStart   = SENTINEL;

    TASK_CHECK(rig_init(&g_rigA, &desc, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, points, 3) == MA_SUCCESS);
    ma_sound_start(&g_rigA.sound);

    {
        const ma_uint32 chunks[4] = { 2, 5, 1, 13 };
        TASK_CHECK(check_direct_gain(&g_rigA, points, 3, 30, 0, 24, MA_TRUE, 5, 12, chunks, 4, 1e-6f));
    }

    rig_uninit(&g_rigA);
    return 0;
}

/* An explicit seek moves the envelope clock with the source cursor. */
static int case_clock_seek_repositions_curve(void)
{
    rig_desc desc;
    ma_volume_envelope_point points[3] = {
        { 2,  1.50f },
        { 9,  0.25f },
        { 18, 4.00f }
    };
    const ma_uint32 chunks[2] = { 6, 2 };
    ma_uint64 got;

    init_unity_source();

    memset(&desc, 0, sizeof(desc));
    desc.format            = ma_format_f32;
    desc.channels          = 2;
    desc.pData             = g_srcF32;
    desc.frameCount        = 96;
    desc.rangeBeg          = 5;
    desc.rangeEnd          = 45;
    desc.loopBeg           = 8;
    desc.loopEnd           = 20;
    desc.looping           = MA_TRUE;
    desc.pitch             = 1.0f;
    desc.usePitch          = MA_FALSE;
    desc.seekBeforeStart   = SENTINEL;

    TASK_CHECK(rig_init(&g_rigA, &desc, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, points, 3) == MA_SUCCESS);
    ma_sound_start(&g_rigA.sound);

    got = rig_render(&g_rigA, g_outA, 7, chunks, 2);
    TASK_CHECK(got == 7);

    TASK_CHECK(ma_sound_seek_to_pcm_frame(&g_rigA.sound, 11) == MA_SUCCESS);

    {
        /* After the seek the sequence starts at relative frame 11, runs to the loop end (20), then
           restarts at relative frame 8 forever. */
        TASK_CHECK(check_direct_gain(&g_rigA, points, 3, 24, 11, 40, MA_TRUE, 8, 20, chunks, 2, 1e-6f));
    }

    rig_uninit(&g_rigA);
    return 0;
}


/* --------------------------------------------------------------------------------------------- */
/* Group: render_paths                                                                            */
/* --------------------------------------------------------------------------------------------- */

/* f32 stereo, non-zero range, internal loop points, asymmetric channels, unequal chunking. */
static int case_render_f32_asymmetric_unequal_chunks(void)
{
    rig_desc descA;
    rig_desc descB;
    const ma_uint32 chunksA[4] = { 7, 1, 23, 3 };
    const ma_uint32 chunksB[3] = { 32, 5, 11 };

    init_sources();
    ref_preweight_f32(g_refF32, g_srcF32, 128, 2, 3, g_curve, g_curveCount);

    memset(&descA, 0, sizeof(descA));
    descA.format          = ma_format_f32;
    descA.channels        = 2;
    descA.pData           = g_srcF32;
    descA.frameCount      = 96;
    descA.rangeBeg        = 3;
    descA.rangeEnd        = 40;
    descA.loopBeg         = 6;
    descA.loopEnd         = 20;
    descA.looping         = MA_TRUE;
    descA.pitch           = 1.0f;
    descA.usePitch        = MA_FALSE;
    descA.seekBeforeStart = SENTINEL;

    descB = descA;
    descB.pData = g_refF32;

    TASK_CHECK(rig_init(&g_rigA, &descA, NULL) == MA_SUCCESS);
    TASK_CHECK(rig_init(&g_rigB, &descB, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);

    ma_sound_start(&g_rigA.sound);
    ma_sound_start(&g_rigB.sound);

    TASK_CHECK(render_and_compare("f32 asymmetric unequal chunks", &g_rigA, &g_rigB, 400, chunksA, 4, chunksB, 3, 2e-5f));

    rig_uninit(&g_rigA);
    rig_uninit(&g_rigB);
    return 0;
}

/* s16 stereo path with pitching and an internal loop point. */
static int case_render_s16_pitch_stereo(void)
{
    rig_desc descA;
    rig_desc descB;
    const ma_uint32 chunksA[3] = { 3, 17, 1 };
    const ma_uint32 chunksB[2] = { 29, 8 };

    init_sources();
    ma_convert_pcm_frames_format(g_refF32, ma_format_f32, g_srcS16, ma_format_s16, 128, 2, ma_dither_mode_none);
    ref_preweight_f32(g_refF32, g_refF32, 128, 2, 4, g_curve, g_curveCount);

    memset(&descA, 0, sizeof(descA));
    descA.format          = ma_format_s16;
    descA.channels        = 2;
    descA.pData           = g_srcS16;
    descA.frameCount      = 96;
    descA.rangeBeg        = 4;
    descA.rangeEnd        = 44;
    descA.loopBeg         = 7;
    descA.loopEnd         = 21;
    descA.looping         = MA_TRUE;
    descA.pitch           = 1.7f;
    descA.usePitch        = MA_TRUE;
    descA.seekBeforeStart = SENTINEL;

    descB = descA;
    descB.format = ma_format_f32;
    descB.pData  = g_refF32;

    TASK_CHECK(rig_init(&g_rigA, &descA, NULL) == MA_SUCCESS);
    TASK_CHECK(rig_init(&g_rigB, &descB, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);

    ma_sound_start(&g_rigA.sound);
    ma_sound_start(&g_rigB.sound);

    TASK_CHECK(render_and_compare("s16 stereo pitch 1.7 loop", &g_rigA, &g_rigB, 400, chunksA, 3, chunksB, 2, 2e-5f));

    rig_uninit(&g_rigA);
    rig_uninit(&g_rigB);
    return 0;
}

/* s16 mono source converted to the engine's stereo output, with down-pitching. */
static int case_render_s16_mono_to_stereo(void)
{
    rig_desc descA;
    rig_desc descB;
    static ma_int16 mono[128];
    static float ref[128];
    ma_uint32 i;
    const ma_uint32 chunksA[3] = { 11, 2, 5 };
    const ma_uint32 chunksB[2] = { 23, 3 };

    init_sources();
    for (i = 0; i < 128; i += 1) {
        mono[i] = (ma_int16)(g_srcF32[2 * i] * 30000.0f);
    }
    ma_convert_pcm_frames_format(ref, ma_format_f32, mono, ma_format_s16, 128, 1, ma_dither_mode_none);
    ref_preweight_f32(ref, ref, 128, 1, 2, g_curve, g_curveCount);

    memset(&descA, 0, sizeof(descA));
    descA.format          = ma_format_s16;
    descA.channels        = 1;
    descA.pData           = mono;
    descA.frameCount      = 96;
    descA.rangeBeg        = 2;
    descA.rangeEnd        = 60;
    descA.loopBeg         = 5;
    descA.loopEnd         = 17;
    descA.looping         = MA_TRUE;
    descA.pitch           = 0.8f;
    descA.usePitch        = MA_TRUE;
    descA.seekBeforeStart = SENTINEL;

    descB = descA;
    descB.format   = ma_format_f32;
    descB.channels = 1;
    descB.pData    = ref;

    TASK_CHECK(rig_init(&g_rigA, &descA, NULL) == MA_SUCCESS);
    TASK_CHECK(rig_init(&g_rigB, &descB, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);

    ma_sound_start(&g_rigA.sound);
    ma_sound_start(&g_rigB.sound);

    TASK_CHECK(render_and_compare("s16 mono->stereo pitch 0.8", &g_rigA, &g_rigB, 400, chunksA, 3, chunksB, 2, 2e-5f));

    rig_uninit(&g_rigA);
    rig_uninit(&g_rigB);
    return 0;
}

/* Pitching stretches the material, and the envelope must follow the source clock rather than the
   output clock. Compare two pitches against independently pre-weighted material. */
static int case_render_pitch_variants(void)
{
    const float pitches[2] = { 0.5f, 2.25f };
    const ma_uint32 chunksA[3] = { 9, 4, 2 };
    const ma_uint32 chunksB[2] = { 31, 6 };
    ma_uint32 i;

    init_sources();
    ref_preweight_f32(g_refF32, g_srcF32, 128, 2, 0, g_curve, g_curveCount);

    for (i = 0; i < 2; i += 1) {
        rig_desc descA;
        rig_desc descB;

        memset(&descA, 0, sizeof(descA));
        descA.format          = ma_format_f32;
        descA.channels        = 2;
        descA.pData           = g_srcF32;
        descA.frameCount      = 96;
        descA.rangeBeg        = 0;
        descA.rangeEnd        = SENTINEL;
        descA.loopBeg         = 0;
        descA.loopEnd         = SENTINEL;
        descA.looping         = MA_TRUE;
        descA.pitch           = pitches[i];
        descA.usePitch        = MA_TRUE;
        descA.seekBeforeStart = SENTINEL;

        descB = descA;
        descB.pData = g_refF32;

        TASK_CHECK(rig_init(&g_rigA, &descA, NULL) == MA_SUCCESS);
        TASK_CHECK(rig_init(&g_rigB, &descB, NULL) == MA_SUCCESS);
        TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);

        ma_sound_start(&g_rigA.sound);
        ma_sound_start(&g_rigB.sound);

        TASK_CHECK(render_and_compare("pitch variant", &g_rigA, &g_rigB, 400, chunksA, 3, chunksB, 2, 2e-5f));

        rig_uninit(&g_rigA);
        rig_uninit(&g_rigB);
    }

    return 0;
}

/* After an explicit seek the next output must match a fresh sound that was seeked to the same
   position before it started. Pre-read source cache and resampler history must not leak. */
static int case_render_seek_matches_fresh(void)
{
    rig_desc desc;
    const ma_uint32 chunks[3] = { 13, 1, 6 };
    const ma_uint32 chunksB[2] = { 22, 4 };
    const ma_uint64 seekTargets[2] = { 7, 61 };
    ma_uint32 i;

    init_sources();

    for (i = 0; i < 2; i += 1) {
        memset(&desc, 0, sizeof(desc));
        desc.format          = ma_format_f32;
        desc.channels        = 2;
        desc.pData           = g_srcF32;
        desc.frameCount      = 96;
        desc.rangeBeg        = 2;
        desc.rangeEnd        = 90;
        desc.loopBeg         = 5;
        desc.loopEnd         = 24;
        desc.looping         = MA_TRUE;
        desc.pitch           = 1.5f;
        desc.usePitch        = MA_TRUE;
        desc.seekBeforeStart = SENTINEL;

        TASK_CHECK(rig_init(&g_rigA, &desc, NULL) == MA_SUCCESS);
        TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);
        ma_sound_start(&g_rigA.sound);
        {
            ma_uint64 got;
            got = rig_render(&g_rigA, g_outA, 37, chunks, 3);
            TASK_CHECK(got == 37);
        }

        TASK_CHECK(ma_sound_seek_to_pcm_frame(&g_rigA.sound, seekTargets[i]) == MA_SUCCESS);

        desc.seekBeforeStart = seekTargets[i];
        TASK_CHECK(rig_init(&g_rigB, &desc, NULL) == MA_SUCCESS);
        TASK_CHECK(ma_sound_set_volume_envelope(&g_rigB.sound, g_curve, g_curveCount) == MA_SUCCESS);
        ma_sound_start(&g_rigB.sound);

        TASK_CHECK(render_and_compare("seek matches fresh", &g_rigA, &g_rigB, 300, chunks, 3, chunksB, 2, 2e-5f));

        rig_uninit(&g_rigA);
        rig_uninit(&g_rigB);
    }

    return 0;
}

/* Existing volume and fade processing still run at their original place in the pipeline, on top
   of the source envelope. */
static int case_render_volume_and_fade(void)
{
    rig_desc descA;
    rig_desc descB;
    const ma_uint32 chunksA[2] = { 5, 19 };
    const ma_uint32 chunksB[2] = { 27, 2 };

    init_sources();
    ref_preweight_f32(g_refF32, g_srcF32, 128, 2, 0, g_curve, g_curveCount);

    memset(&descA, 0, sizeof(descA));
    descA.format          = ma_format_f32;
    descA.channels        = 2;
    descA.pData           = g_srcF32;
    descA.frameCount      = 96;
    descA.rangeBeg        = 0;
    descA.rangeEnd        = 80;
    descA.loopBeg         = 0;
    descA.loopEnd         = SENTINEL;
    descA.looping         = MA_TRUE;
    descA.pitch           = 1.0f;
    descA.usePitch        = MA_FALSE;
    descA.seekBeforeStart = SENTINEL;

    descB = descA;
    descB.pData = g_refF32;

    TASK_CHECK(rig_init(&g_rigA, &descA, NULL) == MA_SUCCESS);
    TASK_CHECK(rig_init(&g_rigB, &descB, NULL) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);

    ma_sound_set_volume(&g_rigA.sound, 0.5f);
    ma_sound_set_volume(&g_rigB.sound, 0.5f);
    ma_sound_set_fade_in_pcm_frames(&g_rigA.sound, 1.0f, 0.25f, 200);
    ma_sound_set_fade_in_pcm_frames(&g_rigB.sound, 1.0f, 0.25f, 200);

    ma_sound_start(&g_rigA.sound);
    ma_sound_start(&g_rigB.sound);

    TASK_CHECK(render_and_compare("volume+fade on top of envelope", &g_rigA, &g_rigB, 300, chunksA, 2, chunksB, 2, 2e-5f));

    rig_uninit(&g_rigA);
    rig_uninit(&g_rigB);
    return 0;
}

/* A data source that never returns more than a few frames per read. The envelope must still land
   on exact source frames regardless of how the source is chunked. */
static int case_render_short_reads(void)
{
    short_source sourceA;
    short_source sourceB;
    rig rigA;
    rig rigB;
    ma_engine_config engineConfig;
    ma_result result;
    const ma_uint32 chunksA[3] = { 9, 2, 5 };
    const ma_uint32 chunksB[2] = { 17, 4 };

    init_sources();
    ref_preweight_f32(g_refF32, g_srcF32, 128, 2, 2, g_curve, g_curveCount);

    memset(&rigA, 0, sizeof(rigA));
    memset(&rigB, 0, sizeof(rigB));

    engineConfig = ma_engine_config_init();
    engineConfig.noDevice    = MA_TRUE;
    engineConfig.channels    = ENGINE_CHANNELS;
    engineConfig.sampleRate  = TEST_RATE;
    engineConfig.periodSizeInFrames = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

    TASK_CHECK(ma_engine_init(&engineConfig, &rigA.engine) == MA_SUCCESS);
    TASK_CHECK(ma_engine_init(&engineConfig, &rigB.engine) == MA_SUCCESS);

    result = short_source_init(&sourceA, g_srcF32, 2, 96, 3);
    TASK_CHECK(result == MA_SUCCESS);
    result = short_source_init(&sourceB, g_refF32, 2, 96, 3);
    TASK_CHECK(result == MA_SUCCESS);

    TASK_CHECK(ma_data_source_set_range_in_pcm_frames((ma_data_source*)&sourceA, 2, 40) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_set_loop_point_in_pcm_frames((ma_data_source*)&sourceA, 4, 15) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_set_range_in_pcm_frames((ma_data_source*)&sourceB, 2, 40) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_set_loop_point_in_pcm_frames((ma_data_source*)&sourceB, 4, 15) == MA_SUCCESS);

    TASK_CHECK(ma_sound_init_from_data_source(&rigA.engine, (ma_data_source*)&sourceA, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &rigA.sound) == MA_SUCCESS);
    TASK_CHECK(ma_sound_init_from_data_source(&rigB.engine, (ma_data_source*)&sourceB, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &rigB.sound) == MA_SUCCESS);

    TASK_CHECK(ma_sound_set_volume_envelope(&rigA.sound, g_curve, g_curveCount) == MA_SUCCESS);
    ma_sound_set_looping(&rigA.sound, MA_TRUE);
    ma_sound_set_looping(&rigB.sound, MA_TRUE);

    ma_sound_start(&rigA.sound);
    ma_sound_start(&rigB.sound);

    TASK_CHECK(render_and_compare("short reads", &rigA, &rigB, 300, chunksA, 3, chunksB, 2, 2e-5f));

    ma_sound_uninit(&rigA.sound);
    ma_sound_uninit(&rigB.sound);
    ma_engine_uninit(&rigA.engine);
    ma_engine_uninit(&rigB.engine);
    return 0;
}

/* Memory comes from the engine's allocation callbacks, is copied out of the caller's array, and
   the read callback never allocates. */
static int case_render_allocation_and_copy(void)
{
    alloc_stats stats;
    ma_allocation_callbacks callbacks;
    rig_desc desc;
    ma_volume_envelope_point points[4] = {
        { 1,  0.5f },
        { 8,  1.75f },
        { 20, 0.25f },
        { 33, 2.0f }
    };
    const ma_uint32 chunks[3] = { 3, 17, 2 };
    ma_uint64 got;

    memset(&stats, 0, sizeof(stats));
    callbacks.onMalloc  = alloc_on_malloc;
    callbacks.onRealloc = alloc_on_realloc;
    callbacks.onFree    = alloc_on_free;
    callbacks.pUserData = &stats;

    init_sources();

    memset(&desc, 0, sizeof(desc));
    desc.format          = ma_format_f32;
    desc.channels        = 2;
    desc.pData           = g_srcF32;
    desc.frameCount      = 96;
    desc.rangeBeg        = 0;
    desc.rangeEnd        = SENTINEL;
    desc.loopBeg         = 0;
    desc.loopEnd         = SENTINEL;
    desc.looping         = MA_FALSE;
    desc.pitch           = 1.0f;
    desc.usePitch        = MA_FALSE;
    desc.seekBeforeStart = SENTINEL;

    TASK_CHECK(rig_init(&g_rigA, &desc, &callbacks) == MA_SUCCESS);
    {
        ma_int64 baseline = stats.outstanding;

        TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, points, 4) == MA_SUCCESS);
        TASK_CHECK(stats.outstanding > baseline);   /* Point list allocated via the callbacks. */

        ma_sound_start(&g_rigA.sound);
        got = rig_render(&g_rigA, g_outA, 40, chunks, 3);
        TASK_CHECK(got == 40);
        TASK_CHECK(stats.outstanding == baseline + 1);   /* No allocation from the read path. */

        /* Replacing frees the previous list and allocates exactly one replacement. */
        TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, points, 3) == MA_SUCCESS);
        TASK_CHECK(stats.outstanding == baseline + 1);

        /* Clearing releases it. */
        TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, NULL, 0) == MA_SUCCESS);
        TASK_CHECK(stats.outstanding == baseline);

        /* Validating a bad list must not change the allocation at all. */
        points[1].frameIndex = points[0].frameIndex;
        TASK_CHECK(ma_sound_set_volume_envelope(&g_rigA.sound, points, 4) == MA_INVALID_ARGS);
        TASK_CHECK(stats.outstanding == baseline);
        points[1].frameIndex = 8;
    }

    rig_uninit(&g_rigA);
    TASK_CHECK(stats.outstanding == 0);   /* Every allocation, including the point list, was returned. */

    return 0;
}


/* --------------------------------------------------------------------------------------------- */
/* Registration.                                                                                  */
/* --------------------------------------------------------------------------------------------- */
const struct task_case task_cases[] = {
    { "curve",        "interpolation_and_holds",      case_curve_interpolation_and_holds },
    { "curve",        "copy_and_validation",          case_curve_copy_and_validation },
    { "source_clock", "relative_to_range",            case_clock_relative_to_range },
    { "source_clock", "loop_returns_to_loop_point",   case_clock_loop_returns_to_loop_point },
    { "source_clock", "seek_repositions_curve",       case_clock_seek_repositions_curve },
    { "render_paths", "f32_asymmetric_unequal_chunks",case_render_f32_asymmetric_unequal_chunks },
    { "render_paths", "s16_pitch_stereo",             case_render_s16_pitch_stereo },
    { "render_paths", "s16_mono_to_stereo",           case_render_s16_mono_to_stereo },
    { "render_paths", "pitch_variants",               case_render_pitch_variants },
    { "render_paths", "seek_matches_fresh",           case_render_seek_matches_fresh },
    { "render_paths", "volume_and_fade",              case_render_volume_and_fade },
    { "render_paths", "short_reads",                  case_render_short_reads },
    { "render_paths", "allocation_and_copy",          case_render_allocation_and_copy },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

