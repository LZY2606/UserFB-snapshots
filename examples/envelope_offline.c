/*
Offline volume envelope example for ma_sound.

This example renders a short piece of test audio to a WAV file using a device-less engine, applying
a volume envelope that is tied to the source material's own frame clock. It does not require an
audio device, a resource manager or a decoder, which makes it easy to run anywhere (including CI).

Build (from the repository root). This example is self-contained and includes the implementation,
so compile it on its own:

    cc -std=c99 -O2 -DMA_NO_DEVICE_IO -DMA_NO_RESOURCE_MANAGER -DMA_NO_DECODING -DMA_NO_ENCODING -I. \
       examples/envelope_offline.c -lm -pthread -o envelope_offline

Write the default output file (envelope_example.wav in the current directory):

    ./envelope_offline

Write to a different file:

    ./envelope_offline my_output.wav

The example demonstrates the two properties that matter most for an audio editor preview:

  1. The envelope is indexed by source frames relative to the range start, so it moves with the
     material when the pitch changes.
  2. A natural loop re-enters the curve at the loop's own source position instead of restarting the
     curve at frame 0.

The file it writes contains the engine's real output, not a hand-computed approximation. You can
listen to it or diff it against another render.
*/
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SAMPLE_RATE         48000
#define CHANNELS            2
#define SOURCE_FRAMES       1024
#define RANGE_BEG           0
#define RANGE_END           480             /* Play only the first 10ms of the source. */
#define LOOP_BEG            160             /* Loop the middle 5ms region. */
#define LOOP_END            400
#define OUTPUT_FRAMES       2400            /* 50ms of output. */
#define RENDER_CHUNK        97              /* Deliberately not a round number. */

/* The curve. Frame indices are relative to RANGE_BEG, not the engine clock. */
static const ma_volume_envelope_point g_envelope[] = {
    {   0, 0.05f },   /* Nearly silent at the start of the range. */
    {  40, 1.20f },
    { 120, 0.30f },
    { 240, 1.00f }
};

static ma_volume_envelope_point g_envelopeCopy[sizeof(g_envelope) / sizeof(g_envelope[0])];

/* Builds a source signal with a clear pitch contour so envelope movement is easy to hear. */
static void build_source(float* pFrames, ma_uint32 frameCount)
{
    ma_uint32 i;
    for (i = 0; i < frameCount; i += 1) {
        double t = (double)i / (double)SAMPLE_RATE;
        float tone = (float)(0.6 * sin(2.0 * MA_PI * 220.0 * t) + 0.3 * sin(2.0 * MA_PI * 880.0 * t));
        pFrames[2 * i]     = tone;
        pFrames[2 * i + 1] = tone * 0.75f;   /* Deliberately asymmetric channels. */
    }
}

/* Writes a canonical 16-bit PCM WAV file without relying on any codec support. */
static int write_wav(const char* pFilePath, const float* pFrames, ma_uint64 frameCount, ma_uint32 channels, ma_uint32 sampleRate)
{
    FILE* pFile;
    ma_uint32 byteRate = sampleRate * channels * 2;
    ma_uint16 blockAlign = (ma_uint16)(channels * 2);
    ma_uint32 dataSize = (ma_uint32)(frameCount * channels * 2);
    ma_uint32 riffSize = 36 + dataSize;
    ma_uint16 formatTag = 1;
    ma_uint16 channelCount = (ma_uint16)channels;
    ma_uint32 rate = sampleRate;
    ma_uint32 bitsPerSample = 16;
    ma_uint8 header[44];
    ma_uint64 i;

    pFile = fopen(pFilePath, "wb");
    if (pFile == NULL) {
        return -1;
    }

    memcpy(header +  0, "RIFF", 4);
    memcpy(header +  4, &riffSize, 4);
    memcpy(header +  8, "WAVEfmt ", 8);
    memcpy(header + 16, &(ma_uint32){ 16 }, 4);
    memcpy(header + 20, &formatTag, 2);
    memcpy(header + 22, &channelCount, 2);
    memcpy(header + 24, &rate, 4);
    memcpy(header + 28, &byteRate, 4);
    memcpy(header + 32, &blockAlign, 2);
    memcpy(header + 34, &bitsPerSample, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &dataSize, 4);

    if (fwrite(header, 1, sizeof(header), pFile) != sizeof(header)) {
        fclose(pFile);
        return -1;
    }

    /* Convert and write in blocks to avoid a second full-size buffer. */
    for (i = 0; i < frameCount; i += 1) {
        ma_int16 block[64 * 2];
        ma_uint32 blockFrames = (ma_uint32)((frameCount - i) < 64 ? (frameCount - i) : 64);
        ma_uint32 j;

        for (j = 0; j < blockFrames * channels; j += 1) {
            float x = pFrames[i * channels + j];
            if (x > 1.0f)  { x = 1.0f; }
            if (x < -1.0f) { x = -1.0f; }
            block[j] = (ma_int16)(x * 32767.0f);
        }

        if (fwrite(block, sizeof(ma_int16), blockFrames * channels, pFile) != (size_t)(blockFrames * channels)) {
            fclose(pFile);
            return -1;
        }
    }

    fclose(pFile);
    return 0;
}

/* Renders the sound through the engine and writes the result as a 16-bit stereo WAV file. */
static int render_and_write(const char* pOutputPath)
{
    ma_engine engine;
    ma_engine_config engineConfig;
    ma_audio_buffer_ref source;
    ma_sound sound;
    ma_result result;
    float sourceFrames[CHANNELS * SOURCE_FRAMES];
    float outputFrames[CHANNELS * OUTPUT_FRAMES];
    ma_uint64 totalFramesRead = 0;
    ma_uint32 envelopePointCount = sizeof(g_envelope) / sizeof(g_envelope[0]);

    build_source(sourceFrames, SOURCE_FRAMES);

    engineConfig = ma_engine_config_init();
    engineConfig.noDevice                           = MA_TRUE;
    engineConfig.channels                           = CHANNELS;
    engineConfig.sampleRate                         = SAMPLE_RATE;
    engineConfig.periodSizeInFrames                 = 0;
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 0;

    result = ma_engine_init(&engineConfig, &engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_engine_init() failed: %s\n", ma_result_description(result));
        return 1;
    }

    result = ma_audio_buffer_ref_init(ma_format_f32, CHANNELS, sourceFrames, SOURCE_FRAMES, &source);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_audio_buffer_ref_init() failed: %s\n", ma_result_description(result));
        ma_engine_uninit(&engine);
        return 1;
    }

    /* Range and loop points are configured before playback and then left alone. */
    result = ma_data_source_set_range_in_pcm_frames((ma_data_source*)&source, RANGE_BEG, RANGE_END);
    if (result != MA_SUCCESS) { fprintf(stderr, "range: %s\n", ma_result_description(result)); ma_audio_buffer_ref_uninit(&source); ma_engine_uninit(&engine); return 1; }

    result = ma_data_source_set_loop_point_in_pcm_frames((ma_data_source*)&source, LOOP_BEG, LOOP_END);
    if (result != MA_SUCCESS) { fprintf(stderr, "loop: %s\n", ma_result_description(result)); ma_audio_buffer_ref_uninit(&source); ma_engine_uninit(&engine); return 1; }

    result = ma_sound_init_from_data_source(&engine, (ma_data_source*)&source, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &sound);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_sound_init_from_data_source() failed: %s\n", ma_result_description(result));
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return 1;
    }

    ma_sound_set_pitch(&sound, 1.6f);   /* A pitch change must not move the curve. */
    ma_sound_set_looping(&sound, MA_TRUE);

    /*
    Install the envelope. The library copies the points, so the caller's array can be stack
    allocated or reused immediately afterwards. We scribble over the stack copy here to prove it.
    */
    memcpy(g_envelopeCopy, g_envelope, sizeof(g_envelope));
    result = ma_sound_set_volume_envelope(&sound, g_envelopeCopy, envelopePointCount);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_sound_set_volume_envelope() failed: %s\n", ma_result_description(result));
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return 1;
    }
    memset(g_envelopeCopy, 0xFF, sizeof(g_envelopeCopy));

    result = ma_sound_start(&sound);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_sound_start() failed: %s\n", ma_result_description(result));
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return 1;
    }

    /* Read in chunks that are not a multiple of anything, mimicking an editor preview loop. */
    while (totalFramesRead < OUTPUT_FRAMES) {
        ma_uint64 framesRead = 0;
        ma_uint32 framesToRead = RENDER_CHUNK;

        if ((ma_uint64)framesToRead > (OUTPUT_FRAMES - totalFramesRead)) {
            framesToRead = (ma_uint32)(OUTPUT_FRAMES - totalFramesRead);
        }

        result = ma_engine_read_pcm_frames(&engine, outputFrames + (totalFramesRead * CHANNELS), framesToRead, &framesRead);
        if (result != MA_SUCCESS || framesRead == 0) {
            fprintf(stderr, "ma_engine_read_pcm_frames() stopped: %s\n", ma_result_description(result));
            ma_sound_uninit(&sound);
            ma_audio_buffer_ref_uninit(&source);
            ma_engine_uninit(&engine);
            return 1;
        }

        totalFramesRead += framesRead;
    }

    /* Write the rendered audio out. Engine output is always f32 in the engine's channel count. */
    if (write_wav(pOutputPath, outputFrames, totalFramesRead, CHANNELS, SAMPLE_RATE) != 0) {
        fprintf(stderr, "failed to write '%s'\n", pOutputPath);
        ma_sound_uninit(&sound);
        ma_audio_buffer_ref_uninit(&source);
        ma_engine_uninit(&engine);
        return 1;
    }

    printf("Rendered %llu frames at pitch 1.6 with a source-relative envelope.\n", (unsigned long long)totalFramesRead);
    printf("Range [%u, %u), loop [%u, %u). Curve points: %u.\n", (unsigned)RANGE_BEG, (unsigned)RANGE_END, (unsigned)LOOP_BEG, (unsigned)LOOP_END, envelopePointCount);
    printf("Wrote '%s'.\n", pOutputPath);

    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);
    return 0;
}

int main(int argc, char** argv)
{
    const char* pOutputPath = (argc > 1) ? argv[1] : "envelope_example.wav";
    return render_and_write(pOutputPath);
}
