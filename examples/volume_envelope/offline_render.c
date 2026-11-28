/*
Offline volume-envelope example.

Renders a short stereo clip through an envelope with no audio device, using
periodSizeInFrames = 0. The envelope frame indices are source frame positions relative to the
start of the data source's range, so the same curve follows the material when the range, loop
points or pitch change.

Build (from the repository root):

    cc -std=c99 -O2 -DMA_NO_DEVICE_IO -DMA_NO_RESOURCE_MANAGER \
       -DMA_NO_DECODING -DMA_NO_ENCODING -I. \
       examples/volume_envelope/offline_render.c miniaudio.c -lm -pthread \
       -o offline_render

Run:

    ./offline_render
*/
#include "miniaudio.h"

#include <math.h>
#include <stdio.h>

#define SAMPLE_RATE     48000
#define CHANNELS        2
#define SOURCE_FRAMES   96
#define RENDER_FRAMES   64
#define RANGE_BEG       8
#define RANGE_END       72

int main(void)
{
    ma_engine engine;
    ma_engine_config engineConfig;
    ma_audio_buffer_ref source;
    ma_sound sound;
    ma_volume_envelope_point envelope[3];
    float sourceData[SOURCE_FRAMES * CHANNELS];
    float output[RENDER_FRAMES * CHANNELS];
    ma_uint64 framesRead = 0;
    ma_uint32 i;
    ma_result result;

    /* Build a simple stereo signal. Its absolute amplitude is arbitrary. */
    for (i = 0; i < SOURCE_FRAMES; i += 1) {
        sourceData[i * 2 + 0] = 0.6f * sinf((float)i * 0.21f);
        sourceData[i * 2 + 1] = 0.4f * cosf((float)i * 0.13f);
    }

    /*
    The envelope is defined in source-frame space relative to the start of the range:
       - frame 0 (range start): gain 0.0 (fade in)
       - frame 6:               gain 1.0
       - frame 20:              gain 0.2
    Before the first point and after the last, the endpoint values are held.
    */
    envelope[0].frameIndex = 0;
    envelope[0].volume = 0.0f;
    envelope[1].frameIndex = 6;
    envelope[1].volume = 1.0f;
    envelope[2].frameIndex = 20;
    envelope[2].volume = 0.2f;

    engineConfig = ma_engine_config_init();
    engineConfig.noDevice = MA_TRUE;                        /* Offline rendering. */
    engineConfig.channels = CHANNELS;
    engineConfig.sampleRate = SAMPLE_RATE;
    engineConfig.periodSizeInFrames = 0;                    /* Do not force a fixed processing size. */
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;    /* Raw source-level output. */

    result = ma_engine_init(&engineConfig, &engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_engine_init() failed: %d\n", result);
        return 1;
    }

    result = ma_audio_buffer_ref_init(ma_format_f32, CHANNELS, sourceData, SOURCE_FRAMES, &source);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_audio_buffer_ref_init() failed: %d\n", result);
        ma_engine_uninit(&engine);
        return 1;
    }

    /* Restrict playback to a sub-range of the source. The envelope stays relative to this range. */
    ma_data_source_set_range_in_pcm_frames(&source, RANGE_BEG, RANGE_END);

    result = ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &sound);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_sound_init_from_data_source() failed: %d\n", result);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return 1;
    }

    /*
    The envelope can be set any number of times before the first play. It is copied into the
    sound using the engine's allocation callbacks, so `envelope` can be reused afterwards.
    */
    result = ma_sound_set_volume_envelope(&sound, envelope, 3);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_sound_set_volume_envelope() failed: %d\n", result);
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return 1;
    }

    ma_sound_set_looping(&sound, MA_TRUE);
    ma_sound_start(&sound);

    result = ma_engine_read_pcm_frames(&engine, output, RENDER_FRAMES, &framesRead);
    if (result != MA_SUCCESS || framesRead != RENDER_FRAMES) {
        fprintf(stderr, "ma_engine_read_pcm_frames() failed: %d (read %llu)\n",
                result, (unsigned long long)framesRead);
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return 1;
    }

    printf("Rendered %llu stereo frames offline.\n", (unsigned long long)framesRead);
    printf("First eight frames (L, R):\n");
    for (i = 0; i < 8; i += 1) {
        printf("  %2u: % .6f  % .6f\n", i, output[i * 2 + 0], output[i * 2 + 1]);
    }

    /* Clear the envelope (count 0, points may be NULL). */
    ma_sound_set_volume_envelope(&sound, NULL, 0);

    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);

    return 0;
}
