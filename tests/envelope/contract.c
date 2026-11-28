/*
Volume envelope contract tests for ma_sound.

The envelope is attached to the source material: frame indices are source frame positions
relative to the start of the data source's range (the same clock as
ma_data_source_get_cursor_in_pcm_frames()), not engine time. These tests verify the API
contract, the source-timeline semantics across range/loop points and pitch, and that the
engine output matches playing an independently pre-weighted copy of the material.

Written with task_support/check.h and registered via task_cases/task_case_count.
No main() - task_support/driver.c provides the entry point.
*/
#include "miniaudio.h"
#include "task_support/check.h"

#include <math.h>
#include <string.h>

/* C99 math.h may not expose INFINITY/NAN under strict conformance; define fallbacks. */
#ifndef INFINITY
#define INFINITY (1.0f / 0.0f)
#endif
#ifndef NAN
#define NAN (0.0f / 0.0f)
#endif

#define TEST_SAMPLE_RATE    48000
#define TEST_SRC_FRAMES     192
#define TEST_SRC_CHANNELS   2
#define TEST_MAX_FRAMES     512

/* Material shared by the tests. */
static float g_f32Source[TEST_SRC_FRAMES * TEST_SRC_CHANNELS];
static ma_int16 g_s16Source[TEST_SRC_FRAMES * TEST_SRC_CHANNELS];
static ma_bool32 g_materialReady = MA_FALSE;

static void build_material(void)
{
    ma_uint32 i;
    ma_uint32 s;

    if (g_materialReady) {
        return;
    }

    for (i = 0; i < TEST_SRC_FRAMES; i += 1) {
        float l = 0.55f * sinf((float)i * 0.173f) + 0.11f * cosf((float)i * 0.041f);
        float r = 0.40f * cosf((float)i * 0.097f) - 0.17f * sinf((float)i * 0.263f);

        for (s = 0; s < 2; s += 1) {
            float x = (s == 0) ? l : r;

            /* Quantize so the f32 and s16 source paths represent exactly the same material. */
            ma_int16 q = (ma_int16)lrintf(x * 32767.0f);
            g_s16Source[i * 2 + s] = q;
            g_f32Source[i * 2 + s] = (float)q * 0.000030517578125f;
        }
    }

    g_materialReady = MA_TRUE;
}

static float s16_to_f32(ma_int16 x)
{
    /* Matches miniaudio's fast-path conversion. */
    return (float)x * 0.000030517578125f;
}

/* Reference interpolation, intentionally independent of the implementation. */
static float eval_envelope(const ma_volume_envelope_point* pPoints, ma_uint32 count, ma_uint64 frameIndex)
{
    ma_uint32 i;

    if (count == 0) {
        return 1.0f;
    }
    if (frameIndex <= pPoints[0].frameIndex) {
        return pPoints[0].volume;
    }
    if (frameIndex >= pPoints[count - 1].frameIndex) {
        return pPoints[count - 1].volume;
    }

    for (i = 1; i < count; i += 1) {
        if (frameIndex < pPoints[i].frameIndex) {
            ma_uint64 f0 = pPoints[i - 1].frameIndex;
            ma_uint64 f1 = pPoints[i].frameIndex;
            float v0 = pPoints[i - 1].volume;
            float v1 = pPoints[i].volume;
            float t = (float)(frameIndex - f0) / (float)(f1 - f0);
            return v0 + (v1 - v0) * t;
        }
    }

    return pPoints[count - 1].volume;
}

/*
Builds a pre-weighted copy of the source in f32. `pre` must hold TEST_SRC_FRAMES stereo
frames. The gain for absolute source frame `i` uses `i - rangeBeg`, matching the source
cursor clock.
*/
static void preweight_f32(const ma_volume_envelope_point* pPoints, ma_uint32 count, ma_uint64 rangeBeg, float* pPre)
{
    ma_uint32 i;

    for (i = 0; i < TEST_SRC_FRAMES; i += 1) {
        float gain = eval_envelope(pPoints, count, (i >= rangeBeg) ? (i - rangeBeg) : 0);
        ma_uint32 s;

        for (s = 0; s < 2; s += 1) {
            pPre[i * 2 + s] = g_f32Source[i * 2 + s] * gain;
        }
    }
}

static void preweight_s16(const ma_volume_envelope_point* pPoints, ma_uint32 count, ma_uint64 rangeBeg, float* pPre)
{
    ma_uint32 i;

    for (i = 0; i < TEST_SRC_FRAMES; i += 1) {
        float gain = eval_envelope(pPoints, count, (i >= rangeBeg) ? (i - rangeBeg) : 0);
        ma_uint32 s;

        for (s = 0; s < 2; s += 1) {
            pPre[i * 2 + s] = s16_to_f32(g_s16Source[i * 2 + s]) * gain;
        }
    }
}

typedef struct
{
    ma_format format;
    ma_uint64 rangeBeg;
    ma_uint64 rangeEnd;
    ma_uint64 loopBeg;
    ma_uint64 loopEnd;
    float pitch;
    ma_uint64 seekTo;
    ma_bool32 seek;
    ma_bool32 noPitch;      /* Disables the resampler so output is bit-exact against the source. */
    const ma_volume_envelope_point* pPoints;
    ma_uint32 pointCount;
} render_config;

/* Plays a caller-supplied f32 buffer with the given configuration. Defined before use. */
static ma_result render_engine_preweighted(const render_config* pConfig, const float* pData, float* pOut, ma_uint32 frameCount, ma_uint32 chunk);

/*
Renders `frameCount` output frames from a fresh engine/sound using the given configuration.
If `chunk` is greater than zero the engine is read in pieces of at most that many frames,
which also exercises partial/short reads. Returns MA_SUCCESS on a complete read.
*/
static ma_result render_engine(const render_config* pConfig, float* pOut, ma_uint32 frameCount, ma_uint32 chunk)
{
    ma_engine engine;
    ma_engine_config engineConfig = ma_engine_config_init();
    ma_audio_buffer_ref source;
    ma_sound sound;
    ma_uint64 totalRead = 0;
    ma_result result;

    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = TEST_SRC_CHANNELS;
    engineConfig.sampleRate = TEST_SAMPLE_RATE;
    engineConfig.periodSizeInFrames = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

    result = ma_engine_init(&engineConfig, &engine);
    if (result != MA_SUCCESS) {
        return result;
    }

    if (pConfig->format == ma_format_s16) {
        result = ma_audio_buffer_ref_init(ma_format_s16, TEST_SRC_CHANNELS, g_s16Source, TEST_SRC_FRAMES, &source);
    } else {
        result = ma_audio_buffer_ref_init(ma_format_f32, TEST_SRC_CHANNELS, g_f32Source, TEST_SRC_FRAMES, &source);
    }
    if (result != MA_SUCCESS) {
        ma_engine_uninit(&engine);
        return result;
    }

    if (pConfig->rangeBeg != 0 || pConfig->rangeEnd != ~((ma_uint64)0)) {
        result = ma_data_source_set_range_in_pcm_frames(&source, pConfig->rangeBeg, pConfig->rangeEnd);
        if (result != MA_SUCCESS) {
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return result;
        }
    }
    if (pConfig->loopBeg != 0 || pConfig->loopEnd != ~((ma_uint64)0)) {
        result = ma_data_source_set_loop_point_in_pcm_frames(&source, pConfig->loopBeg, pConfig->loopEnd);
        if (result != MA_SUCCESS) {
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return result;
        }
    }

    result = ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | (pConfig->noPitch ? MA_SOUND_FLAG_NO_PITCH : 0), NULL, &sound);
    if (result != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return result;
    }

    ma_sound_set_looping(&sound, MA_TRUE);
    ma_sound_set_pitch(&sound, pConfig->pitch);

    if (pConfig->pPoints != NULL || pConfig->pointCount != 0) {
        result = ma_sound_set_volume_envelope(&sound, pConfig->pPoints, pConfig->pointCount);
        if (result != MA_SUCCESS) {
            ma_sound_uninit(&sound);
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return result;
        }
    }

    if (pConfig->seek) {
        result = ma_sound_seek_to_pcm_frame(&sound, pConfig->seekTo);
        if (result != MA_SUCCESS) {
            ma_sound_uninit(&sound);
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return result;
        }
    }

    result = ma_sound_start(&sound);
    if (result != MA_SUCCESS) {
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return result;
    }

    while (totalRead < frameCount) {
        ma_uint64 toRead = frameCount - totalRead;
        ma_uint64 justRead = 0;

        if (chunk > 0 && toRead > chunk) {
            toRead = chunk;
        }

        result = ma_engine_read_pcm_frames(&engine, pOut + totalRead * TEST_SRC_CHANNELS, toRead, &justRead);
        if (result != MA_SUCCESS) {
            break;
        }

        totalRead += justRead;

        if (justRead == 0) {
            result = MA_AT_END;
            break;
        }
    }

    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);

    if (result == MA_SUCCESS && totalRead != frameCount) {
        result = MA_AT_END;
    }

    return result;
}

/* Compares two stereo buffers and returns the maximum absolute error. */
static float max_error(const float* a, const float* b, ma_uint32 frameCount)
{
    float maxErr = 0.0f;
    ma_uint32 i;

    for (i = 0; i < frameCount * TEST_SRC_CHANNELS; i += 1) {
        float e = fabsf(a[i] - b[i]);
        if (e > maxErr) {
            maxErr = e;
        }
    }

    return maxErr;
}

/*
Renders the same configuration twice: once with the envelope attached to the real source,
and once with an independently pre-weighted material and no envelope. Returns the maximum
absolute difference. This is the core "consistent with pre-weighted PCM" check.
*/
static float compare_with_preweighted(const ma_volume_envelope_point* pPoints, ma_uint32 count,
                                      ma_uint64 rangeBeg, ma_uint64 rangeEnd,
                                      ma_uint64 loopBeg, ma_uint64 loopEnd,
                                      ma_format format, float pitch,
                                      ma_bool32 seek, ma_uint64 seekTo,
                                      ma_uint32 frameCount, ma_uint32 chunk)
{
    render_config envConfig;
    render_config refConfig;
    float envOut[TEST_MAX_FRAMES * TEST_SRC_CHANNELS];
    float refOut[TEST_MAX_FRAMES * TEST_SRC_CHANNELS];
    float pre[TEST_SRC_FRAMES * TEST_SRC_CHANNELS];
    ma_result result;

    memset(&envConfig, 0, sizeof(envConfig));
    envConfig.format = format;
    envConfig.rangeBeg = rangeBeg;
    envConfig.rangeEnd = rangeEnd;
    envConfig.loopBeg = loopBeg;
    envConfig.loopEnd = loopEnd;
    envConfig.pitch = pitch;
    envConfig.seek = seek;
    envConfig.seekTo = seekTo;
    envConfig.noPitch = MA_FALSE;
    envConfig.pPoints = pPoints;
    envConfig.pointCount = count;

    result = render_engine(&envConfig, envOut, frameCount, chunk);
    if (result != MA_SUCCESS) {
        return 1000.0f;    /* Sentinels a failure to render. */
    }

    /*
    The reference plays an f32 buffer containing the already-weighted samples, so it has the
    same range/loop/pitch configuration but no envelope. It is seeked the same way when the
    test asks for a seek so both sides start from the same source position.
    */
    if (format == ma_format_s16) {
        preweight_s16(pPoints, count, rangeBeg, pre);
    } else {
        preweight_f32(pPoints, count, rangeBeg, pre);
    }

    memset(&refConfig, 0, sizeof(refConfig));
    refConfig.format = ma_format_f32;
    refConfig.rangeBeg = rangeBeg;
    refConfig.rangeEnd = rangeEnd;
    refConfig.loopBeg = loopBeg;
    refConfig.loopEnd = loopEnd;
    refConfig.pitch = pitch;
    refConfig.seek = seek;
    refConfig.seekTo = seekTo;
    refConfig.noPitch = MA_FALSE;
    refConfig.pPoints = NULL;
    refConfig.pointCount = 0;

    result = render_engine_preweighted(&refConfig, pre, refOut, frameCount, chunk);
    if (result != MA_SUCCESS) {
        return 1000.0f;
    }

    return max_error(envOut, refOut, frameCount);
}

/* Variant of render_engine() that plays a caller-supplied f32 buffer instead of the shared material. */
static ma_result render_engine_preweighted(const render_config* pConfig, const float* pData, float* pOut, ma_uint32 frameCount, ma_uint32 chunk)
{
    ma_engine engine;
    ma_engine_config engineConfig = ma_engine_config_init();
    ma_audio_buffer_ref source;
    ma_sound sound;
    ma_uint64 totalRead = 0;
    ma_result result;

    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = TEST_SRC_CHANNELS;
    engineConfig.sampleRate = TEST_SAMPLE_RATE;
    engineConfig.periodSizeInFrames = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

    result = ma_engine_init(&engineConfig, &engine);
    if (result != MA_SUCCESS) {
        return result;
    }

    result = ma_audio_buffer_ref_init(ma_format_f32, TEST_SRC_CHANNELS, pData, TEST_SRC_FRAMES, &source);
    if (result != MA_SUCCESS) {
        ma_engine_uninit(&engine);
        return result;
    }

    if (pConfig->rangeBeg != 0 || pConfig->rangeEnd != ~((ma_uint64)0)) {
        result = ma_data_source_set_range_in_pcm_frames(&source, pConfig->rangeBeg, pConfig->rangeEnd);
        if (result != MA_SUCCESS) {
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return result;
        }
    }
    if (pConfig->loopBeg != 0 || pConfig->loopEnd != ~((ma_uint64)0)) {
        result = ma_data_source_set_loop_point_in_pcm_frames(&source, pConfig->loopBeg, pConfig->loopEnd);
        if (result != MA_SUCCESS) {
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return result;
        }
    }

    result = ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | (pConfig->noPitch ? MA_SOUND_FLAG_NO_PITCH : 0), NULL, &sound);
    if (result != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return result;
    }

    ma_sound_set_looping(&sound, MA_TRUE);
    ma_sound_set_pitch(&sound, pConfig->pitch);

    if (pConfig->seek) {
        result = ma_sound_seek_to_pcm_frame(&sound, pConfig->seekTo);
        if (result != MA_SUCCESS) {
            ma_sound_uninit(&sound);
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return result;
        }
    }

    result = ma_sound_start(&sound);
    if (result != MA_SUCCESS) {
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return result;
    }

    while (totalRead < frameCount) {
        ma_uint64 toRead = frameCount - totalRead;
        ma_uint64 justRead = 0;

        if (chunk > 0 && toRead > chunk) {
            toRead = chunk;
        }

        result = ma_engine_read_pcm_frames(&engine, pOut + totalRead * TEST_SRC_CHANNELS, toRead, &justRead);
        if (result != MA_SUCCESS) {
            break;
        }

        totalRead += justRead;

        if (justRead == 0) {
            result = MA_AT_END;
            break;
        }
    }

    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);

    if (result == MA_SUCCESS && totalRead != frameCount) {
        result = MA_AT_END;
    }

    return result;
}

/* ---------------------------------------------------------------------------------------------
curve group
--------------------------------------------------------------------------------------------- */

static int case_curve_interpolation(void)
{
    ma_volume_envelope_point points[4];

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 0.0f;
    points[1].frameIndex = 4;
    points[1].volume = 1.0f;
    points[2].frameIndex = 9;
    points[2].volume = 0.25f;
    points[3].frameIndex = 15;
    points[3].volume = 2.0f;

    /* Hold before first point. */
    TASK_CHECK(fabsf(eval_envelope(points, 4, 0) - 0.0f) <= 1e-7f);
    /* Linear interpolation between points. */
    TASK_CHECK(fabsf(eval_envelope(points, 4, 2) - 0.5f) <= 1e-6f);
    TASK_CHECK(fabsf(eval_envelope(points, 4, 6) - 0.7f) <= 1e-6f);
    /* Hold after last point. */
    TASK_CHECK(fabsf(eval_envelope(points, 4, 100) - 2.0f) <= 1e-6f);

    /* The engine output must follow that curve at those source frames. */
    TASK_CHECK(compare_with_preweighted(points, 4, 0, ~((ma_uint64)0), 0, ~((ma_uint64)0),
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 40, 0) <= 2e-5f);

    return 0;
}

static int case_curve_copy_and_validation(void)
{
    ma_engine engine;
    ma_engine_config engineConfig = ma_engine_config_init();
    ma_audio_buffer_ref source;
    ma_sound sound;
    ma_volume_envelope_point points[3];
    ma_volume_envelope_point bad[3];
    float out[32 * TEST_SRC_CHANNELS];
    ma_uint64 read = 0;

    build_material();

    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = TEST_SRC_CHANNELS;
    engineConfig.sampleRate = TEST_SAMPLE_RATE;
    engineConfig.periodSizeInFrames = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

    TASK_CHECK(ma_engine_init(&engineConfig, &engine) == MA_SUCCESS);
    TASK_CHECK(ma_audio_buffer_ref_init(ma_format_f32, TEST_SRC_CHANNELS, g_f32Source, TEST_SRC_FRAMES, &source) == MA_SUCCESS);
    TASK_CHECK(ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &sound) == MA_SUCCESS);

    /* Count of zero clears and is valid with a NULL pointer. */
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, NULL, 0) == MA_SUCCESS);

    /* Build a valid envelope and copy it. */
    points[0].frameIndex = 0;
    points[0].volume = 1.0f;
    points[1].frameIndex = 5;
    points[1].volume = 0.5f;
    points[2].frameIndex = 11;
    points[2].volume = 1.5f;
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, points, 3) == MA_SUCCESS);

    /* Mutating the caller's array must not change the sound's configuration. */
    points[0].volume = 0.0f;
    points[1].volume = 0.0f;
    points[2].volume = 0.0f;

    ma_sound_set_looping(&sound, MA_TRUE);
    TASK_CHECK(ma_sound_start(&sound) == MA_SUCCESS);
    TASK_CHECK(ma_engine_read_pcm_frames(&engine, out, 8, &read) == MA_SUCCESS);
    TASK_CHECK(read == 8);

    /* Frame 0 uses the copied 1.0 gain, not the mutated 0.0. */
    TASK_CHECK(fabsf(out[0] - g_f32Source[0]) <= 1e-6f);
    TASK_CHECK(fabsf(out[1] - g_f32Source[1]) <= 1e-6f);

    /* Invalid: NULL points with non-zero count. */
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, NULL, 2) == MA_INVALID_ARGS);

    /* Invalid: non-increasing frame indices. */
    bad[0] = points[0];
    bad[1] = points[1];
    bad[2] = points[2];
    bad[0].frameIndex = 6;
    bad[1].frameIndex = 6;
    TASK_CHECK(bad[0].volume == 0.0f);  /* silence unused warnings on some compilers */
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, bad, 3) == MA_INVALID_ARGS);

    /* Invalid: negative volume. */
    bad[0].frameIndex = 0;
    bad[0].volume = -0.1f;
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, bad, 3) == MA_INVALID_ARGS);

    /* Invalid: non-finite volume. */
    bad[0].volume = INFINITY;
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, bad, 3) == MA_INVALID_ARGS);
    bad[0].volume = NAN;
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, bad, 3) == MA_INVALID_ARGS);

    /* Failed calls must leave the original configuration intact: the next frame still uses the copy. */
    TASK_CHECK(ma_sound_seek_to_pcm_frame(&sound, 0) == MA_SUCCESS);
    TASK_CHECK(ma_engine_read_pcm_frames(&engine, out, 1, &read) == MA_SUCCESS);
    TASK_CHECK(read == 1);
    TASK_CHECK(fabsf(out[0] - g_f32Source[0]) <= 1e-6f);

    ma_sound_uninit(&sound);

    /*
    Clearing the envelope must restore default behaviour. Do this on a sound that has never
    played so the check does not depend on the seek-cache flush, which is only specified for
    envelope-enabled sounds.
    */
    {
        ma_volume_envelope_point clearPoints[2];

        clearPoints[0].frameIndex = 0;
        clearPoints[0].volume = 0.0f;
        clearPoints[1].frameIndex = 4;
        clearPoints[1].volume = 0.0f;

        TASK_CHECK(ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &sound) == MA_SUCCESS);
        TASK_CHECK(ma_sound_set_volume_envelope(&sound, clearPoints, 2) == MA_SUCCESS);
        TASK_CHECK(ma_sound_set_volume_envelope(&sound, NULL, 0) == MA_SUCCESS);
        TASK_CHECK(ma_data_source_seek_to_pcm_frame(&source, 0) == MA_SUCCESS);
        TASK_CHECK(ma_sound_start(&sound) == MA_SUCCESS);
        TASK_CHECK(ma_engine_read_pcm_frames(&engine, out, 1, &read) == MA_SUCCESS);
        TASK_CHECK(read == 1);
        TASK_CHECK(fabsf(out[0] - g_f32Source[0]) <= 1e-6f);
        TASK_CHECK(fabsf(out[1] - g_f32Source[1]) <= 1e-6f);
    }

    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);

    return 0;
}

static int case_curve_zero_points_and_replace(void)
{
    ma_volume_envelope_point first[2];
    ma_volume_envelope_point second[2];
    float pre[TEST_SRC_FRAMES * TEST_SRC_CHANNELS];
    render_config config;
    float out[16 * TEST_SRC_CHANNELS];
    ma_uint32 i;

    build_material();

    first[0].frameIndex = 0;
    first[0].volume = 0.0f;
    first[1].frameIndex = 16;
    first[1].volume = 1.0f;

    /*
    Replacing an envelope must fully discard the old points. Render with `second` via the
    engine and compare against a pre-weighted copy built only from `second`.
    */
    second[0].frameIndex = 0;
    second[0].volume = 1.0f;
    second[1].frameIndex = 8;
    second[1].volume = 0.0f;

    memset(&config, 0, sizeof(config));
    config.format = ma_format_f32;
    config.rangeBeg = 0;
    config.rangeEnd = ~((ma_uint64)0);
    config.loopBeg = 0;
    config.loopEnd = ~((ma_uint64)0);
    config.pitch = 1.0f;
    config.noPitch = MA_TRUE;
    config.pPoints = second;
    config.pointCount = 2;

    /*
    First set a different envelope, then replace it. The rendering must reflect only the
    replacement, which also proves the points are copied rather than retained by pointer.
    */
    {
        ma_engine engine;
        ma_engine_config engineConfig = ma_engine_config_init();
        ma_audio_buffer_ref source;
        ma_sound sound;
        ma_uint64 read = 0;

        engineConfig.noDevice = MA_TRUE;
        engineConfig.channels = TEST_SRC_CHANNELS;
        engineConfig.sampleRate = TEST_SAMPLE_RATE;
        engineConfig.periodSizeInFrames = 0;
        engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

        TASK_CHECK(ma_engine_init(&engineConfig, &engine) == MA_SUCCESS);
        TASK_CHECK(ma_audio_buffer_ref_init(ma_format_f32, TEST_SRC_CHANNELS, g_f32Source, TEST_SRC_FRAMES, &source) == MA_SUCCESS);
        TASK_CHECK(ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &sound) == MA_SUCCESS);
        TASK_CHECK(ma_sound_set_volume_envelope(&sound, first, 2) == MA_SUCCESS);
        TASK_CHECK(ma_sound_set_volume_envelope(&sound, second, 2) == MA_SUCCESS);
        TASK_CHECK(ma_sound_start(&sound) == MA_SUCCESS);
        TASK_CHECK(ma_engine_read_pcm_frames(&engine, out, 16, &read) == MA_SUCCESS);
        TASK_CHECK(read == 16);
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
    }

    preweight_f32(second, 2, 0, pre);
    for (i = 0; i < 16; i += 1) {
        TASK_CHECK(fabsf(out[i * 2 + 0] - pre[i * 2 + 0]) <= 2e-5f);
        TASK_CHECK(fabsf(out[i * 2 + 1] - pre[i * 2 + 1]) <= 2e-5f);
    }

    return 0;
}

/* ---------------------------------------------------------------------------------------------
source_clock group
--------------------------------------------------------------------------------------------- */

static int case_clock_nonzero_range_and_loop(void)
{
    ma_volume_envelope_point points[3];
    ma_uint64 rangeBeg = 16;
    ma_uint64 rangeEnd = 40;
    ma_uint64 loopBeg = 8;      /* Relative to the range. */
    ma_uint64 loopEnd = 20;     /* Relative to the range. */

    build_material();

    /*
    The envelope frame indices are relative to the range start. The loop point begins at
    source offset 8 within the range, so the curve must resume there rather than at 0.
    */
    points[0].frameIndex = 0;
    points[0].volume = 0.1f;
    points[1].frameIndex = 8;
    points[1].volume = 1.0f;
    points[2].frameIndex = 18;
    points[2].volume = 0.2f;

    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 64, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 64, 5) <= 2e-5f);

    return 0;
}

static int case_clock_loop_resumes_curve(void)
{
    ma_volume_envelope_point points[2];
    ma_uint64 rangeBeg = 4;
    ma_uint64 rangeEnd = 28;
    ma_uint64 loopBeg = 6;      /* Relative to the range. */
    ma_uint64 loopEnd = 12;

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 0.0f;
    points[1].frameIndex = 20;
    points[1].volume = 1.0f;

    /*
    With a loop from 6 to 12, playback after the first pass must evaluate the envelope at
    frames 6..11 again, not restart from frame 0. Rendering more than a full loop period
    and comparing to the pre-weighted material catches a reset-to-zero bug.
    */
    TASK_CHECK(compare_with_preweighted(points, 2, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 80, 0) <= 2e-5f);

    return 0;
}

static int case_clock_seek_discards_prefetch(void)
{
    ma_volume_envelope_point points[3];
    ma_uint64 rangeBeg = 0;
    ma_uint64 rangeEnd = 60;

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 1.0f;
    points[1].frameIndex = 30;
    points[1].volume = 0.0f;
    points[2].frameIndex = 40;
    points[2].volume = 1.0f;

    /*
    A seek to frame 24 must behave identically to a new sound that was seeked there before
    it ever played. The pre-weighted reference is seeked to the same position, so any
    prefetched or resampler history from before the jump would show up as an error.
    */
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, 0, ~((ma_uint64)0),
                                        ma_format_f32, 1.0f, MA_TRUE, 24, 48, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, 0, ~((ma_uint64)0),
                                        ma_format_f32, 1.5f, MA_TRUE, 17, 48, 0) <= 2e-5f);

    return 0;
}

static int case_clock_seek_midplayback(void)
{
    ma_engine engine;
    ma_engine_config engineConfig = ma_engine_config_init();
    ma_audio_buffer_ref source;
    ma_sound sound;
    ma_volume_envelope_point points[3];
    float afterSeek[40 * TEST_SRC_CHANNELS];
    float newSound[40 * TEST_SRC_CHANNELS];
    render_config config;
    ma_uint64 read = 0;
    ma_uint64 rangeBeg = 0;
    ma_uint64 rangeEnd = 60;

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 1.0f;
    points[1].frameIndex = 30;
    points[1].volume = 0.0f;
    points[2].frameIndex = 40;
    points[2].volume = 1.0f;

    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = TEST_SRC_CHANNELS;
    engineConfig.sampleRate = TEST_SAMPLE_RATE;
    engineConfig.periodSizeInFrames = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

    /*
    Play an envelope sound for a while (priming the processing cache and the resampler with
    pre-seek data), then seek it. The output after the jump must match a brand-new sound that
    was configured identically and seeked to the same target before it ever played.
    */
    TASK_CHECK(ma_engine_init(&engineConfig, &engine) == MA_SUCCESS);
    TASK_CHECK(ma_audio_buffer_ref_init(ma_format_f32, TEST_SRC_CHANNELS, g_f32Source, TEST_SRC_FRAMES, &source) == MA_SUCCESS);
    TASK_CHECK(ma_data_source_set_range_in_pcm_frames(&source, rangeBeg, rangeEnd) == MA_SUCCESS);
    TASK_CHECK(ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &sound) == MA_SUCCESS);
    ma_sound_set_looping(&sound, MA_TRUE);
    ma_sound_set_pitch(&sound, 1.25f);
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, points, 3) == MA_SUCCESS);
    TASK_CHECK(ma_sound_start(&sound) == MA_SUCCESS);
    {
        float warm[19 * TEST_SRC_CHANNELS];
        TASK_CHECK(ma_engine_read_pcm_frames(&engine, warm, 19, &read) == MA_SUCCESS);
        TASK_CHECK(read == 19);
    }
    TASK_CHECK(ma_sound_seek_to_pcm_frame(&sound, 12) == MA_SUCCESS);
    TASK_CHECK(ma_engine_read_pcm_frames(&engine, afterSeek, 40, &read) == MA_SUCCESS);
    TASK_CHECK(read == 40);
    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);

    memset(&config, 0, sizeof(config));
    config.format = ma_format_f32;
    config.rangeBeg = rangeBeg;
    config.rangeEnd = rangeEnd;
    config.loopBeg = 0;
    config.loopEnd = ~((ma_uint64)0);
    config.pitch = 1.25f;
    config.seek = MA_TRUE;
    config.seekTo = 12;
    config.pPoints = points;
    config.pointCount = 3;

    TASK_CHECK(render_engine(&config, newSound, 40, 0) == MA_SUCCESS);
    TASK_CHECK(max_error(afterSeek, newSound, 40) <= 2e-5f);

    return 0;
}

static int case_clock_pitch(void)
{
    ma_volume_envelope_point points[3];
    ma_uint64 rangeBeg = 8;
    ma_uint64 rangeEnd = 48;
    ma_uint64 loopBeg = 4;
    ma_uint64 loopEnd = 30;

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 0.3f;
    points[1].frameIndex = 12;
    points[1].volume = 1.2f;
    points[2].frameIndex = 26;
    points[2].volume = 0.0f;

    /* The envelope is on the source timeline, so it must scale with pitch. */
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 0.5f, MA_FALSE, 0, 64, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 2.0f, MA_FALSE, 0, 64, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.25f, MA_FALSE, 0, 64, 7) <= 2e-5f);

    return 0;
}

/* ---------------------------------------------------------------------------------------------
render_paths group
--------------------------------------------------------------------------------------------- */

static int case_paths_f32_and_s16(void)
{
    ma_volume_envelope_point points[3];
    ma_uint64 rangeBeg = 10;
    ma_uint64 rangeEnd = 50;
    ma_uint64 loopBeg = 3;
    ma_uint64 loopEnd = 30;

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 0.25f;
    points[1].frameIndex = 10;
    points[1].volume = 1.0f;
    points[2].frameIndex = 25;
    points[2].volume = 0.5f;

    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 64, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_s16, 1.0f, MA_FALSE, 0, 64, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_s16, 0.75f, MA_FALSE, 0, 64, 0) <= 2e-5f);

    return 0;
}

static int case_paths_asymmetric_stereo(void)
{
    ma_volume_envelope_point points[3];
    float pre[TEST_SRC_FRAMES * TEST_SRC_CHANNELS];
    render_config config;
    float out[24 * TEST_SRC_CHANNELS];
    ma_uint32 i;

    build_material();

    /* Non-monotonic curve with a gain above 1.0, applied identically to both channels. */
    points[0].frameIndex = 0;
    points[0].volume = 0.0f;
    points[1].frameIndex = 6;
    points[1].volume = 1.6f;
    points[2].frameIndex = 20;
    points[2].volume = 0.3f;

    memset(&config, 0, sizeof(config));
    config.format = ma_format_f32;
    config.rangeBeg = 0;
    config.rangeEnd = ~((ma_uint64)0);
    config.loopBeg = 0;
    config.loopEnd = ~((ma_uint64)0);
    config.pitch = 1.0f;
    config.noPitch = MA_TRUE;
    config.pPoints = points;
    config.pointCount = 3;

    /* Whole reads and uneven chunks must both reproduce the pre-weighted material exactly. */
    TASK_CHECK(compare_with_preweighted(points, 3, 0, ~((ma_uint64)0), 0, ~((ma_uint64)0),
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 24, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, 0, ~((ma_uint64)0), 0, ~((ma_uint64)0),
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 24, 5) <= 2e-5f);

    TASK_CHECK(render_engine(&config, out, 24, 0) == MA_SUCCESS);
    preweight_f32(points, 3, 0, pre);

    /*
    The same gain applies to both channels of a frame, but the channels themselves must not
    be mixed, swapped or scaled differently.
    */
    for (i = 0; i < 24; i += 1) {
        float gain = eval_envelope(points, 3, i);
        TASK_CHECK(fabsf(out[i * 2 + 0] - pre[i * 2 + 0]) <= 2e-5f);
        TASK_CHECK(fabsf(out[i * 2 + 1] - pre[i * 2 + 1]) <= 2e-5f);

        /* Cross-check the gain itself against the source samples. */
        TASK_CHECK(fabsf(out[i * 2 + 0] - g_f32Source[i * 2 + 0] * gain) <= 2e-5f);
        TASK_CHECK(fabsf(out[i * 2 + 1] - g_f32Source[i * 2 + 1] * gain) <= 2e-5f);
    }

    /* The two channels must not have been mixed or swapped. */
    TASK_CHECK(fabsf(out[2 * 5 + 0] - out[2 * 5 + 1]) > 1e-6f);

    return 0;
}

static int case_paths_short_reads_and_chunks(void)
{
    ma_volume_envelope_point points[3];
    ma_uint64 rangeBeg = 12;
    ma_uint64 rangeEnd = 52;
    ma_uint64 loopBeg = 5;
    ma_uint64 loopEnd = 33;

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 0.5f;
    points[1].frameIndex = 7;
    points[1].volume = 1.1f;
    points[2].frameIndex = 21;
    points[2].volume = 0.2f;

    /* Whole, small, and uneven chunk sizes must all match the pre-weighted reference. */
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 100, 0) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 100, 1) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 100, 3) <= 2e-5f);
    TASK_CHECK(compare_with_preweighted(points, 3, rangeBeg, rangeEnd, loopBeg, loopEnd,
                                        ma_format_f32, 1.0f, MA_FALSE, 0, 100, 13) <= 2e-5f);

    return 0;
}

static int case_paths_mono_source(void)
{
    ma_volume_envelope_point points[2];
    ma_engine engine;
    ma_engine_config engineConfig = ma_engine_config_init();
    ma_audio_buffer_ref source;
    ma_sound sound;
    float mono[TEST_SRC_FRAMES];
    float out[24 * TEST_SRC_CHANNELS];
    ma_uint32 i;
    ma_uint64 read = 0;

    build_material();

    /* Use the left channel of the material as a mono source. */
    for (i = 0; i < TEST_SRC_FRAMES; i += 1) {
        mono[i] = g_f32Source[i * 2 + 0];
    }

    points[0].frameIndex = 0;
    points[0].volume = 0.0f;
    points[1].frameIndex = 12;
    points[1].volume = 1.0f;

    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = TEST_SRC_CHANNELS;  /* Mono source is expanded to the stereo engine. */
    engineConfig.sampleRate = TEST_SAMPLE_RATE;
    engineConfig.periodSizeInFrames = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

    TASK_CHECK(ma_engine_init(&engineConfig, &engine) == MA_SUCCESS);
    TASK_CHECK(ma_audio_buffer_ref_init(ma_format_f32, 1, mono, TEST_SRC_FRAMES, &source) == MA_SUCCESS);
    TASK_CHECK(ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &sound) == MA_SUCCESS);
    TASK_CHECK(ma_sound_set_volume_envelope(&sound, points, 2) == MA_SUCCESS);
    TASK_CHECK(ma_sound_start(&sound) == MA_SUCCESS);
    TASK_CHECK(ma_engine_read_pcm_frames(&engine, out, 24, &read) == MA_SUCCESS);
    TASK_CHECK(read == 24);
    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);

    /*
    The gain must be applied to the source before the mono expansion. With default mono
    expansion both output channels receive the same weighted sample.
    */
    for (i = 0; i < 24; i += 1) {
        float expected = mono[i] * eval_envelope(points, 2, i);
        TASK_CHECK(fabsf(out[i * 2 + 0] - expected) <= 2e-5f);
        TASK_CHECK(fabsf(out[i * 2 + 1] - expected) <= 2e-5f);
    }

    return 0;
}

/*
Renders a sound with the given material and optional envelope, volume and fade. A separate
engine is used per call so the global clock (which drives fades) starts at zero both times.
*/
static ma_result render_with_volume(const float* pData, const ma_volume_envelope_point* pPoints, ma_uint32 pointCount,
                                    float volume, float fadeBeg, float fadeEnd, ma_uint64 fadeFrames,
                                    float* pOut, ma_uint32 frameCount)
{
    ma_engine engine;
    ma_engine_config engineConfig = ma_engine_config_init();
    ma_audio_buffer_ref source;
    ma_sound sound;
    ma_uint64 read = 0;
    ma_result result;

    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = TEST_SRC_CHANNELS;
    engineConfig.sampleRate = TEST_SAMPLE_RATE;
    engineConfig.periodSizeInFrames = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 8;

    result = ma_engine_init(&engineConfig, &engine);
    if (result != MA_SUCCESS) {
        return result;
    }
    result = ma_audio_buffer_ref_init(ma_format_f32, TEST_SRC_CHANNELS, pData, TEST_SRC_FRAMES, &source);
    if (result != MA_SUCCESS) {
        ma_engine_uninit(&engine);
        return result;
    }
    result = ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &sound);
    if (result != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return result;
    }

    if (pointCount > 0) {
        ma_sound_set_volume_envelope(&sound, pPoints, pointCount);
    }
    ma_sound_set_volume(&sound, volume);
    if (fadeFrames > 0) {
        ma_sound_set_fade_in_pcm_frames(&sound, fadeBeg, fadeEnd, fadeFrames);
    }

    ma_sound_start(&sound);
    result = ma_engine_read_pcm_frames(&engine, pOut, frameCount, &read);

    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);

    if (result != MA_SUCCESS || read != frameCount) {
        return (result != MA_SUCCESS) ? result : MA_AT_END;
    }

    return MA_SUCCESS;
}

static int case_paths_volume_and_fade_coexist(void)
{
    ma_volume_envelope_point points[2];
    float pre[TEST_SRC_FRAMES * TEST_SRC_CHANNELS];
    float envOut[32 * TEST_SRC_CHANNELS];
    float refOut[32 * TEST_SRC_CHANNELS];
    ma_uint32 i;

    build_material();

    points[0].frameIndex = 0;
    points[0].volume = 0.2f;
    points[1].frameIndex = 15;
    points[1].volume = 0.9f;

    /* Pre-weight the material, then play both sides with the same volume and fade settings. */
    preweight_f32(points, 2, 0, pre);

    TASK_CHECK(render_with_volume(g_f32Source, points, 2, 0.7f, 0.7f, 0.3f, 16, envOut, 32) == MA_SUCCESS);
    TASK_CHECK(render_with_volume(pre, NULL, 0, 0.7f, 0.7f, 0.3f, 16, refOut, 32) == MA_SUCCESS);

    /* The envelope must not displace the existing volume or fade processing. */
    for (i = 0; i < 32 * TEST_SRC_CHANNELS; i += 1) {
        TASK_CHECK(fabsf(envOut[i] - refOut[i]) <= 2e-5f);
    }

    return 0;
}

static int case_paths_no_envelope_unchanged(void)
{
    render_config config;
    float out[16 * TEST_SRC_CHANNELS];

    build_material();

    /* With no envelope set, the sound must emit the unmodified source. */
    memset(&config, 0, sizeof(config));
    config.format = ma_format_f32;
    config.rangeBeg = 0;
    config.rangeEnd = ~((ma_uint64)0);
    config.loopBeg = 0;
    config.loopEnd = ~((ma_uint64)0);
    config.pitch = 1.0f;
    config.noPitch = MA_TRUE;
    config.pPoints = NULL;
    config.pointCount = 0;

    TASK_CHECK(render_engine(&config, out, 16, 0) == MA_SUCCESS);

    {
        ma_uint32 i;
        for (i = 0; i < 16 * TEST_SRC_CHANNELS; i += 1) {
            TASK_CHECK(fabsf(out[i] - g_f32Source[i]) <= 1e-6f);
        }
    }

    return 0;
}

const struct task_case task_cases[] = {
    {"curve", "interpolation", case_curve_interpolation},
    {"curve", "copy_and_validation", case_curve_copy_and_validation},
    {"curve", "zero_points_and_replace", case_curve_zero_points_and_replace},
    {"source_clock", "nonzero_range_and_loop", case_clock_nonzero_range_and_loop},
    {"source_clock", "loop_resumes_curve", case_clock_loop_resumes_curve},
    {"source_clock", "seek_discards_prefetch", case_clock_seek_discards_prefetch},
    {"source_clock", "seek_midplayback", case_clock_seek_midplayback},
    {"source_clock", "pitch", case_clock_pitch},
    {"render_paths", "f32_and_s16", case_paths_f32_and_s16},
    {"render_paths", "asymmetric_stereo", case_paths_asymmetric_stereo},
    {"render_paths", "mono_source", case_paths_mono_source},
    {"render_paths", "short_reads_and_chunks", case_paths_short_reads_and_chunks},
    {"render_paths", "volume_and_fade_coexist", case_paths_volume_and_fade_coexist},
    {"render_paths", "no_envelope_unchanged", case_paths_no_envelope_unchanged}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
