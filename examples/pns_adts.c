/*
 * pns_adts.c - continuous ADTS AAC-LC encoding with optional PNS.
 *
 * Build from the repository root after building the static library:
 *   cc -std=c99 -O2 -Icommon/include -Iaacenc/inc \
 *      examples/pns_adts.c .task-build/libcodec.a -lm -o pns_adts
 *
 * Usage:
 *   ./pns_adts output.aac [frames] [pns]
 *
 * pns defaults to 1.  The input is a deterministic broadband-noise-like
 * integer generator, so the resulting file is suitable for trying PNS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "voAAC.h"
#include "cmnMemory.h"

#define SAMPLE_RATE 48000
#define CHANNELS    1
#define FRAME_SAMPLES 1024

static int16_t next_sample(uint32_t *state, unsigned i)
{
    uint32_t x;
    int noise, tone;
    double s;

    *state = *state * 1664525u + 1013904223u;
    x = *state;
    noise = (int32_t)(x >> 17) - 16384;

    /* A quiet 6 kHz component keeps a little tonal anchor in the example. */
    s = 0.18 * sin(2.0 * 3.141592653589793 * 6000.0 * i / SAMPLE_RATE);
    tone = (int)(s * 32767.0);
    return (int16_t)(noise / 5 + tone);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "pns_adts.aac";
    int frames = argc > 2 ? atoi(argv[2]) : 100;
    int pns = argc > 3 ? atoi(argv[3]) : 1;

    VO_AUDIO_CODECAPI api;
    VO_HANDLE handle = NULL;
    VO_MEM_OPERATOR mem;
    VO_CODEC_INIT_USERDATA user;
    AACENC_PARAM params;
    VO_S32 pns_value = pns ? 1 : 0;
    FILE *out;
    int16_t pcm[FRAME_SAMPLES * CHANNELS];
    uint8_t packet[8192];
    uint32_t rng = 0x243f6a88u;

    if (frames <= 0 || frames > 100000) {
        fprintf(stderr, "frames must be between 1 and 100000\n");
        return 2;
    }
    if (pns_value != 0 && pns_value != 1) {
        fprintf(stderr, "pns must be 0 or 1\n");
        return 2;
    }

    out = fopen(path, "wb");
    if (!out) {
        perror(path);
        return 2;
    }

    memset(&api, 0, sizeof(api));
    memset(&mem, 0, sizeof(mem));
    memset(&user, 0, sizeof(user));
    memset(&params, 0, sizeof(params));

    mem.Alloc = cmnMemAlloc;
    mem.Copy = cmnMemCopy;
    mem.Free = cmnMemFree;
    mem.Set = cmnMemSet;
    mem.Check = cmnMemCheck;
    user.memflag = VO_IMF_USERMEMOPERATOR;
    user.memData = &mem;

    if (voGetAACEncAPI(&api) != VO_ERR_NONE ||
        api.Init(&handle, VO_AUDIO_CodingAAC, &user) != VO_ERR_NONE) {
        fprintf(stderr, "encoder initialization failed\n");
        fclose(out);
        return 1;
    }

    params.sampleRate = SAMPLE_RATE;
    params.bitRate = 96000;
    params.nChannels = CHANNELS;
    params.adtsUsed = 1;
    if (api.SetParam(handle, VO_PID_AAC_ENCPARAM, &params) != VO_ERR_NONE) {
        fprintf(stderr, "AAC parameters were rejected\n");
        api.Uninit(handle);
        fclose(out);
        return 1;
    }

    /* Must be a pointer to VO_S32: 0=off (default), 1=on. */
    if (api.SetParam(handle, VO_PID_AAC_PNS, &pns_value) != VO_ERR_NONE) {
        fprintf(stderr, "VO_PID_AAC_PNS was rejected\n");
        api.Uninit(handle);
        fclose(out);
        return 1;
    }

    for (int f = 0; f < frames; ++f) {
        VO_CODECBUFFER in;
        VO_CODECBUFFER outbuf;
        VO_AUDIO_OUTPUTINFO info;
        size_t packet_len;

        for (int i = 0; i < FRAME_SAMPLES; ++i)
            pcm[i] = next_sample(&rng, (unsigned)f * FRAME_SAMPLES + i);

        memset(&in, 0, sizeof(in));
        memset(&outbuf, 0, sizeof(outbuf));
        memset(&info, 0, sizeof(info));
        in.Buffer = (VO_PBYTE)pcm;
        in.Length = sizeof(pcm);
        outbuf.Buffer = packet;
        outbuf.Length = sizeof(packet);

        if (api.SetInputData(handle, &in) != VO_ERR_NONE ||
            api.GetOutputData(handle, &outbuf, &info) != VO_ERR_NONE) {
            fprintf(stderr, "encoding failed at frame %d\n", f);
            api.Uninit(handle);
            fclose(out);
            return 1;
        }

        packet_len = outbuf.Length;
        if (packet_len < 7 || packet[0] != 0xff ||
            (packet[1] & 0xf0) != 0xf0) {
            fprintf(stderr, "frame %d is not an ADTS packet\n", f);
            api.Uninit(handle);
            fclose(out);
            return 1;
        }
        if (fwrite(packet, 1, packet_len, out) != packet_len) {
            perror("fwrite");
            api.Uninit(handle);
            fclose(out);
            return 1;
        }
    }

    if (fclose(out) != 0) {
        perror("fclose");
        api.Uninit(handle);
        return 1;
    }
    api.Uninit(handle);
    printf("wrote %d ADTS frames to %s (PNS %s)\n",
           frames, path, pns_value ? "on" : "off");
    return 0;
}
