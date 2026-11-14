/*
** FAAD2 LOAS/LATM decoding example.
**
** Decodes a LOAS (Low Overhead Audio Stream) file containing LATM
** encapsulated AAC-LC audio to 16 bit PCM samples.  The LOAS transport
** is handled directly by NeAACDecInit/NeAACDecDecode, there is no need
** to extract raw access units first.
**
** Supported streams: audioMuxVersion 0, allStreamsSameTimeFraming 1,
** single program, single layer, frameLengthType 0, plain AAC-LC
** (1024 sample frames, mono or stereo, no SBR/PS).
**
** Build (from the repository root, after building libfaad):
**   cc -Iinclude examples/latm2pcm.c .task-build/libfaad.a -lm -o latm2pcm
** Usage:
**   ./latm2pcm input.loas output.pcm
*/

#include <neaacdec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INBUF_SIZE  (64 * 1024)

int main(int argc, char **argv)
{
    static unsigned char inbuf[INBUF_SIZE];
    FILE *fin, *fout;
    NeAACDecHandle hDecoder;
    NeAACDecConfigurationPtr config;
    NeAACDecFrameInfo frameInfo;
    unsigned long samplerate = 0;
    unsigned char channels = 0;
    size_t filled = 0, pos = 0;

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s input.loas output.pcm\n", argv[0]);
        return 1;
    }

    fin = fopen(argv[1], "rb");
    if (!fin)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    fout = fopen(argv[2], "wb");
    if (!fout)
    {
        fprintf(stderr, "cannot create %s\n", argv[2]);
        fclose(fin);
        return 1;
    }

    hDecoder = NeAACDecOpen();
    config = NeAACDecGetCurrentConfiguration(hDecoder);
    config->outputFormat = FAAD_FMT_16BIT; /* 16 bit or float output */
    config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(hDecoder, config);

    /* read the first chunk and initialise from the first complete
       LOAS frame; the frame is NOT consumed and must be passed to
       NeAACDecDecode again */
    filled = fread(inbuf, 1, sizeof(inbuf), fin);
    if (NeAACDecInit(hDecoder, inbuf, (unsigned long)filled,
                     &samplerate, &channels) != 0)
    {
        fprintf(stderr, "NeAACDecInit failed (no valid LOAS/LATM stream)\n");
        NeAACDecClose(hDecoder);
        fclose(fin);
        fclose(fout);
        return 1;
    }
    printf("LOAS/LATM stream: %lu Hz, %u channel(s)\n", samplerate, channels);

    while (1)
    {
        void *sample_buffer;

        /* keep a whole frame in the buffer: a LOAS frame is at most
           3 + 0x1FFF bytes; compact and refill when running low */
        if (filled - pos < 3 + 0x1FFF && !feof(fin))
        {
            memmove(inbuf, inbuf + pos, filled - pos);
            filled -= pos;
            pos = 0;
            filled += fread(inbuf + filled, 1, sizeof(inbuf) - filled, fin);
        }
        if (pos == filled)
            break; /* end of input */

        /* one call decodes exactly one LOAS frame; bytesconsumed tells
           where the next frame starts */
        sample_buffer = NeAACDecDecode(hDecoder, &frameInfo,
                                       inbuf + pos, (unsigned long)(filled - pos));
        if (frameInfo.error != 0)
        {
            fprintf(stderr, "decode error: %s\n",
                    NeAACDecGetErrorMessage(frameInfo.error));
            break;
        }
        if (frameInfo.bytesconsumed == 0)
            break;
        pos += frameInfo.bytesconsumed;

        if (sample_buffer && frameInfo.samples > 0)
            fwrite(sample_buffer, sizeof(short), frameInfo.samples, fout);
    }

    NeAACDecClose(hDecoder);
    fclose(fin);
    fclose(fout);
    return 0;
}
