#include "wavpack.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FRAMES 1379
#define CHANNELS 6
struct sink { FILE *file; unsigned blocks; };
static int put_block(void *id, void *data, int32_t size) {
    struct sink *s = id;
    if (size < 32 || fwrite(data, 1, (size_t)size, s->file) != (size_t)size) return 0;
    ++s->blocks;
    return 1;
}
static int fail(const char *s) { fprintf(stderr, "BASELINE_FAIL %s\n", s); return 1; }
int main(void) {
    int32_t *pcm = malloc(FRAMES * CHANNELS * sizeof(*pcm));
    int32_t result[73 * CHANNELS];
    struct sink wv = {0}, wvc = {0};
    WavpackConfig config;
    WavpackContext *encoder, *decoder;
    uint32_t i, offset, got, changes = 0;
    uint32_t state = 1925;
    char error[128];
    if (!pcm) return fail("allocation");
    for (i = 0; i < FRAMES * CHANNELS; ++i) {
        state = state * 1664525u + 1013904223u;
        pcm[i] = (int32_t)((state >> 16) & 65535u) - 32768;
    }
    wv.file = fopen(".task-build/baseline.wv", "wb");
    wvc.file = fopen(".task-build/baseline.wvc", "wb");
    if (!wv.file || !wvc.file) return fail("output");
    encoder = WavpackOpenFileOutput(put_block, &wv, &wvc);
    memset(&config, 0, sizeof(config));
    config.bytes_per_sample = 2; config.bits_per_sample = 16;
    config.num_channels = CHANNELS; config.channel_mask = 0x3f;
    config.sample_rate = 48000; config.block_samples = 128;
    config.bitrate = 3.0; config.worker_threads = 2;
    config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC | CONFIG_SHAPE_OVERRIDE;
    if (!encoder || !WavpackSetConfiguration64(encoder, &config, FRAMES, NULL) ||
        !WavpackPackInit(encoder)) return fail("encoder setup");
    for (offset = 0; offset < FRAMES;) {
        uint32_t n = FRAMES - offset > 97 ? 97 : FRAMES - offset;
        if (!WavpackPackSamples(encoder, pcm + offset * CHANNELS, n)) return fail("pack");
        offset += n;
    }
    if (!WavpackFlushSamples(encoder)) return fail("flush");
    WavpackCloseFile(encoder); fclose(wv.file); fclose(wvc.file);
    decoder = WavpackOpenFileInput(".task-build/baseline.wv", error, OPEN_WVC, 0);
    if (!decoder || WavpackGetNumChannels(decoder) != CHANNELS) return fail("open correction");
    offset = 0;
    while ((got = WavpackUnpackSamples(decoder, result, 73)) != 0) {
        if (offset + got > FRAMES || memcmp(result, pcm + offset * CHANNELS,
            got * CHANNELS * sizeof(*pcm))) return fail("correction mismatch");
        offset += got;
    }
    if (offset != FRAMES || WavpackGetNumErrors(decoder)) return fail("correction CRC/count");
    WavpackCloseFile(decoder);
    decoder = WavpackOpenFileInput(".task-build/baseline.wv", error, 0, 0);
    if (!decoder) return fail("open lossy");
    offset = 0;
    while ((got = WavpackUnpackSamples(decoder, result, 73)) != 0) {
        if (offset + got > FRAMES) return fail("lossy count");
        for (i = 0; i < got * CHANNELS; ++i) changes += result[i] != pcm[offset * CHANNELS + i];
        offset += got;
    }
    if (offset != FRAMES || !changes || WavpackGetNumErrors(decoder)) return fail("lossy semantics");
    WavpackCloseFile(decoder); free(pcm);
    printf("BASELINE_OK frames=%u channels=%u wv_blocks=%u wvc_blocks=%u exact=1 lossy_changes=%u\n",
        FRAMES, CHANNELS, wv.blocks, wvc.blocks, changes);
    return 0;
}
