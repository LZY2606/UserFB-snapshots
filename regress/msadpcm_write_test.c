/*
Regression tests for the Microsoft ADPCM writer.

Three run modes:
    encode  Round-trips mono and stereo fixtures, checks the container layout, and validates encoded samples and
            predictor choices against an independent reference decoder.
    blocks  Checks block boundaries, partial-block padding, zero/one frame files and batching invariance.
    errors  Deterministically injects short writes, seek failures and allocation failures.

Every mode prints "PASS <name> checks=<positive integer>" on success.
*/
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------------------------------------------------------
Independent Microsoft ADPCM reference decoder. Deliberately does not call into dr_wav so the writer is validated
against a second implementation of the format rather than against its own reader.
------------------------------------------------------------------------------------------------------------------ */

static const int g_coeff1[7] = { 256, 512, 0, 192, 240, 460,  392 };
static const int g_coeff2[7] = { 0,  -256, 0, 64,  0,  -208, -232 };
static const int g_adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };

static int ref_clamp16(long long x)
{
    if (x < -32768) return -32768;
    if (x >  32767) return  32767;
    return (int)x;
}

/* Floor division by 256 without relying on the implementation-defined shift of negative values. */
static long long ref_floor256(long long x)
{
    long long q = x / 256;
    if ((x % 256) < 0) q -= 1;
    return q;
}

static int ref_nibble(long long difference, int delta)
{
    long long q = difference / delta;
    long long r = difference % delta;
    if (r != 0) {
        long long ar = (r < 0) ? -r : r;
        if (ar * 2 >= delta) q += (difference > 0) ? 1 : -1;
    }
    if (q < -8) q = -8;
    if (q >  7) q =  7;
    return (int)q;
}

static int ref_update_delta(int delta, int nibble)
{
    int nd = (g_adapt[nibble & 0x0F] * delta) / 256;
    if (nd < 16) nd = 16;
    return nd;
}

/* Reconstructs the full block (framesPerBlock frames, interleaved) into pOut. */
static void ref_decode_block(const drwav_uint8* block, int channels, int framesPerBlock, drwav_int16* pOut)
{
    int predictor[2];
    int delta[2];
    int s0[2];
    int s1[2];
    int prev0[2];
    int prev1[2];
    int ch;
    int frame;
    int pos;

    if (channels == 1) {
        predictor[0] = (int)block[0];
        delta[0]     = (int)((signed short)(block[1] | (block[2] << 8)));
        s1[0]        = (int)((signed short)(block[3] | (block[4] << 8)));
        s0[0]        = (int)((signed short)(block[5] | (block[6] << 8)));
        pos = 7;
    } else {
        predictor[0] = (int)block[0];
        predictor[1] = (int)block[1];
        delta[0]     = (int)((signed short)(block[2] | (block[3] << 8)));
        delta[1]     = (int)((signed short)(block[4] | (block[5] << 8)));
        s1[0]        = (int)((signed short)(block[6]  | (block[7]  << 8)));
        s1[1]        = (int)((signed short)(block[8]  | (block[9]  << 8)));
        s0[0]        = (int)((signed short)(block[10] | (block[11] << 8)));
        s0[1]        = (int)((signed short)(block[12] | (block[13] << 8)));
        pos = 14;
    }

    for (ch = 0; ch < channels; ch += 1) {
        prev0[ch] = s0[ch];
        prev1[ch] = s1[ch];
    }

    /* The block header carries the first two frames. */
    pOut[0*channels + 0] = (drwav_int16)s0[0];
    pOut[1*channels + 0] = (drwav_int16)s1[0];
    if (channels == 2) {
        pOut[0*channels + 1] = (drwav_int16)s0[1];
        pOut[1*channels + 1] = (drwav_int16)s1[1];
    }

    frame = 2;
    if (channels == 1) {
        while (frame < framesPerBlock) {
            int byte = block[pos++];
            int n0 = (byte >> 4) & 0x0F;
            int n1 = (byte >> 0) & 0x0F;
            if (n0 & 0x08) n0 -= 16;
            if (n1 & 0x08) n1 -= 16;

            {
                int v = ref_clamp16(ref_floor256((long long)prev1[0]*g_coeff1[predictor[0]] + (long long)prev0[0]*g_coeff2[predictor[0]]) + (long long)n0*delta[0]);
                delta[0] = ref_update_delta(delta[0], n0);
                prev0[0] = prev1[0];
                prev1[0] = v;
                pOut[(frame+0)*channels + 0] = (drwav_int16)v;
            }
            if (frame + 1 < framesPerBlock) {
                int v = ref_clamp16(ref_floor256((long long)prev1[0]*g_coeff1[predictor[0]] + (long long)prev0[0]*g_coeff2[predictor[0]]) + (long long)n1*delta[0]);
                delta[0] = ref_update_delta(delta[0], n1);
                prev0[0] = prev1[0];
                prev1[0] = v;
                pOut[(frame+1)*channels + 0] = (drwav_int16)v;
            }
            frame += 2;
        }
    } else {
        while (frame < framesPerBlock) {
            int byte = block[pos++];
            int n0 = (byte >> 4) & 0x0F;
            int n1 = (byte >> 0) & 0x0F;
            if (n0 & 0x08) n0 -= 16;
            if (n1 & 0x08) n1 -= 16;

            {
                int v = ref_clamp16(ref_floor256((long long)prev1[0]*g_coeff1[predictor[0]] + (long long)prev0[0]*g_coeff2[predictor[0]]) + (long long)n0*delta[0]);
                delta[0] = ref_update_delta(delta[0], n0);
                prev0[0] = prev1[0];
                prev1[0] = v;
                pOut[frame*channels + 0] = (drwav_int16)v;
            }
            {
                int v = ref_clamp16(ref_floor256((long long)prev1[1]*g_coeff1[predictor[1]] + (long long)prev0[1]*g_coeff2[predictor[1]]) + (long long)n1*delta[1]);
                delta[1] = ref_update_delta(delta[1], n1);
                prev0[1] = prev1[1];
                prev1[1] = v;
                pOut[frame*channels + 1] = (drwav_int16)v;
            }
            frame += 1;
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------------
Streams and allocation instrumentation.
------------------------------------------------------------------------------------------------------------------ */

typedef struct
{
    unsigned char* pData;
    size_t size;
    size_t capacity;
    size_t pos;

    int failWriteAt;     /* Byte offset at which the writer starts returning a short write. -1 = never. */
    int failSeekNumber;  /* 1-based seek call that fails. 0 = never. */
    int seekCount;
    int writeCount;
} MemStream;

static void mem_init(MemStream* m)
{
    memset(m, 0, sizeof(*m));
    m->failWriteAt    = -1;
    m->failSeekNumber = 0;
}

static void mem_free(MemStream* m)
{
    free(m->pData);
    memset(m, 0, sizeof(*m));
    m->failWriteAt = -1;
}

static size_t mem_write(void* pUserData, const void* pData, size_t bytesToWrite)
{
    MemStream* m = (MemStream*)pUserData;
    size_t allowed = bytesToWrite;

    m->writeCount += 1;

    if (m->failWriteAt >= 0) {
        if (m->pos >= (size_t)m->failWriteAt) {
            return 0;   /* Permanently failed. */
        }
        if (m->pos + bytesToWrite > (size_t)m->failWriteAt) {
            allowed = (size_t)m->failWriteAt - m->pos;
        }
    }

    if (allowed == 0) {
        return 0;
    }

    if (m->pos + allowed > m->capacity) {
        size_t newCap = (m->capacity == 0) ? 256 : m->capacity;
        unsigned char* pNew;
        while (newCap < m->pos + allowed) {
            newCap *= 2;
        }
        pNew = (unsigned char*)realloc(m->pData, newCap);
        if (pNew == NULL) return 0;
        m->pData = pNew;
        m->capacity = newCap;
    }

    memcpy(m->pData + m->pos, pData, allowed);
    m->pos += allowed;
    if (m->pos > m->size) m->size = m->pos;

    return allowed;
}

static drwav_bool32 mem_seek(void* pUserData, int offset, drwav_seek_origin origin)
{
    MemStream* m = (MemStream*)pUserData;
    long long newPos;

    m->seekCount += 1;

    if (m->failSeekNumber > 0 && m->seekCount >= m->failSeekNumber) {
        return DRWAV_FALSE;
    }

    if (origin == DRWAV_SEEK_SET) {
        newPos = 0;
    } else if (origin == DRWAV_SEEK_CUR) {
        newPos = (long long)m->pos;
    } else if (origin == DRWAV_SEEK_END) {
        newPos = (long long)m->size;
    } else {
        return DRWAV_FALSE;
    }

    newPos += offset;
    if (newPos < 0) return DRWAV_FALSE;
    if ((size_t)newPos > m->size) return DRWAV_FALSE;    /* Seeking beyond the end is not supported here. */

    m->pos = (size_t)newPos;
    return DRWAV_TRUE;
}

typedef struct
{
    int allocCount;
    int freeCount;
    int liveCount;
    int failAtAlloc;    /* 1-based allocation attempt that returns NULL. 0 = never. */
} AllocCounter;

static void* alloc_malloc(size_t sz, void* pUserData)
{
    AllocCounter* a = (AllocCounter*)pUserData;
    a->allocCount += 1;
    if (a->failAtAlloc > 0 && a->allocCount == a->failAtAlloc) {
        return NULL;
    }
    a->liveCount += 1;
    return malloc(sz);
}

static void* alloc_realloc(void* p, size_t sz, void* pUserData)
{
    AllocCounter* a = (AllocCounter*)pUserData;
    a->allocCount += 1;
    if (a->failAtAlloc > 0 && a->allocCount == a->failAtAlloc) {
        return NULL;
    }
    if (p == NULL) a->liveCount += 1;
    return realloc(p, sz);
}

static void alloc_free(void* p, void* pUserData)
{
    AllocCounter* a = (AllocCounter*)pUserData;
    if (p != NULL) {
        a->freeCount += 1;
        a->liveCount -= 1;
    }
    free(p);
}

static drwav_allocation_callbacks alloc_callbacks(AllocCounter* a)
{
    drwav_allocation_callbacks cb;
    cb.pUserData  = a;
    cb.onMalloc  = alloc_malloc;
    cb.onRealloc = alloc_realloc;
    cb.onFree    = alloc_free;
    return cb;
}

/* ------------------------------------------------------------------------------------------------------------------
Container parsing (manual, deliberately independent of dr_wav).
------------------------------------------------------------------------------------------------------------------ */

typedef struct
{
    drwav_uint32 riffSize;
    drwav_uint16 formatTag;
    drwav_uint16 channels;
    drwav_uint32 sampleRate;
    drwav_uint32 avgBytesPerSec;
    drwav_uint16 blockAlign;
    drwav_uint16 bitsPerSample;
    drwav_uint16 cbSize;
    drwav_uint16 samplesPerBlock;
    drwav_uint16 numCoef;
    drwav_int16  coef1[7];
    drwav_int16  coef2[7];
    drwav_uint32 factFrames;
    drwav_uint32 dataSize;
    size_t       dataOffset;
} ParsedWav;

static drwav_uint16 rd_u16(const unsigned char* p) { return (drwav_uint16)(p[0] | (p[1] << 8)); }
static drwav_uint32 rd_u32(const unsigned char* p) { return (drwav_uint32)(p[0] | (p[1] << 8) | ((drwav_uint32)p[2] << 16) | ((drwav_uint32)p[3] << 24)); }

static int parse_wav(const unsigned char* data, size_t size, ParsedWav* out)
{
    size_t pos = 12;
    int seenFmt = 0, seenFact = 0, seenData = 0;

    memset(out, 0, sizeof(*out));
    if (size < 12) return 0;
    if (memcmp(data, "RIFF", 4) != 0) return 0;
    if (memcmp(data + 8, "WAVE", 4) != 0) return 0;
    out->riffSize = rd_u32(data + 4);

    while (pos + 8 <= size) {
        const unsigned char* id = data + pos;
        drwav_uint32 chunkSize = rd_u32(data + pos + 4);
        size_t body = pos + 8;

        if (memcmp(id, "fmt ", 4) == 0) {
            if (chunkSize < 50 || body + 50 > size) return 0;
            out->formatTag      = rd_u16(data + body + 0);
            out->channels       = rd_u16(data + body + 2);
            out->sampleRate     = rd_u32(data + body + 4);
            out->avgBytesPerSec = rd_u32(data + body + 8);
            out->blockAlign     = rd_u16(data + body + 12);
            out->bitsPerSample  = rd_u16(data + body + 14);
            out->cbSize         = rd_u16(data + body + 16);
            out->samplesPerBlock= rd_u16(data + body + 18);
            out->numCoef        = rd_u16(data + body + 20);
            if (out->numCoef != 7 || out->cbSize != 32) return 0;
            {
                int i;
                for (i = 0; i < 7; ++i) {
                    out->coef1[i] = (drwav_int16)rd_u16(data + body + 22 + i*4 + 0);
                    out->coef2[i] = (drwav_int16)rd_u16(data + body + 22 + i*4 + 2);
                }
            }
            seenFmt = 1;
        } else if (memcmp(id, "fact", 4) == 0) {
            if (chunkSize < 4 || body + 4 > size) return 0;
            out->factFrames = rd_u32(data + body);
            seenFact = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            if (body + chunkSize > size) return 0;
            out->dataSize   = chunkSize;
            out->dataOffset = body;
            seenData = 1;
        }

        pos = body + chunkSize;
        if (chunkSize & 1) pos += 1;    /* RIFF padding. */
    }

    return seenFmt && seenFact && seenData;
}

/* ------------------------------------------------------------------------------------------------------------------
Fixtures.
------------------------------------------------------------------------------------------------------------------ */

static drwav_int16 clamp16(int x)
{
    if (x < -32768) return -32768;
    if (x >  32767) return  32767;
    return (drwav_int16)x;
}

/* A stereo signal whose channels are clearly different, including 16-bit extremes. The body of the signal is
   smooth enough for ADPCM to track while a handful of samples hit the full range. */
static void make_stereo_fixture(drwav_int16* p, int frames)
{
    int i;
    for (i = 0; i < frames; ++i) {
        int l = clamp16((int)(11000.0 * sin((double)i / 7.0) + 9000.0 * sin((double)i / 2.3)));
        int r = clamp16(-(int)(8000.0 * sin((double)i / 5.0)) + ((i & 1) ? 4000 : -4000));
        if (i == 3)  l = -32768;
        if (i == 7)  r =  32767;
        if (i == 11) { l = 32767; r = -32768; }
        p[i*2 + 0] = (drwav_int16)l;
        p[i*2 + 1] = (drwav_int16)r;
    }
}

static void make_mono_fixture(drwav_int16* p, int frames)
{
    int i;
    for (i = 0; i < frames; ++i) {
        int v = clamp16((int)(20000.0 * sin((double)i / 9.0)) + (i % 7) * 300);
        if (i == 1)  v = 32767;
        if (i == 5)  v = -32768;
        p[i] = (drwav_int16)v;
    }
}

/* ------------------------------------------------------------------------------------------------------------------
Encoding helpers.
------------------------------------------------------------------------------------------------------------------ */

static drwav_uint32 frames_per_block(drwav_uint32 channels, drwav_uint16 blockAlign)
{
    return (drwav_uint32)(2 + ((drwav_uint32)(blockAlign - 7*channels) * 2) / channels);
}

static int encode_file(const drwav_int16* pcm, drwav_uint64 frameCount, drwav_uint32 channels, drwav_uint32 sampleRate, drwav_uint16 blockAlign,
                       MemStream* outStream, AllocCounter* allocOut)
{
    drwav wav;
    drwav_msadpcm_config cfg;
    drwav_allocation_callbacks cb;
    drwav_allocation_callbacks* pCB = NULL;

    mem_init(outStream);
    if (allocOut != NULL) {
        memset(allocOut, 0, sizeof(*allocOut));
        cb = alloc_callbacks(allocOut);
        pCB = &cb;
    }

    cfg.channels   = channels;
    cfg.sampleRate = sampleRate;
    cfg.blockAlign = blockAlign;

    if (!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, outStream, pCB)) {
        return 0;
    }
    if (frameCount > 0) {
        if (drwav_write_msadpcm_pcm_frames(&wav, frameCount, pcm) != frameCount) {
            drwav_uninit(&wav);
            return 0;
        }
    }
    return drwav_uninit(&wav) == DRWAV_SUCCESS;
}

static int encode_file_batched(const drwav_int16* pcm, drwav_uint64 frameCount, drwav_uint32 channels, drwav_uint32 sampleRate, drwav_uint16 blockAlign,
                               drwav_uint64 batchSize, MemStream* outStream)
{
    drwav wav;
    drwav_msadpcm_config cfg;
    drwav_uint64 pos = 0;

    mem_init(outStream);

    cfg.channels   = channels;
    cfg.sampleRate = sampleRate;
    cfg.blockAlign = blockAlign;

    if (!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, outStream, NULL)) {
        return 0;
    }

    if (batchSize == 0) {
        if (frameCount > 0 && drwav_write_msadpcm_pcm_frames(&wav, frameCount, pcm) != frameCount) {
            drwav_uninit(&wav);
            return 0;
        }
    } else {
        while (pos < frameCount) {
            drwav_uint64 n = batchSize;
            drwav_uint64 got;
            if (n > frameCount - pos) n = frameCount - pos;
            got = drwav_write_msadpcm_pcm_frames(&wav, n, pcm + (size_t)pos*channels);
            if (got != n) {
                drwav_uninit(&wav);
                return 0;
            }
            pos += got;
        }
    }

    return drwav_uninit(&wav) == DRWAV_SUCCESS;
}

/* Independent recomputation of the best predictor for one channel of a padded block. */
static int reference_best_predictor(const drwav_int16* paddedBlock, int channels, int ch, int framesPerBlock, int realFrames)
{
    int s0 = paddedBlock[ch];
    int s1 = paddedBlock[channels + ch];
    int delta0 = s1 - s0;
    int best = 0;
    long long bestErr = 0;
    int p;

    if (delta0 < 0) delta0 = -delta0;
    if (delta0 < 16) delta0 = 16;
    if (delta0 > 32767) delta0 = 32767;

    for (p = 0; p < 7; ++p) {
        int prev0 = s0, prev1 = s1, delta = delta0;
        long long err = 0;
        int i;
        for (i = 2; i < framesPerBlock; ++i) {
            int sample = (int)paddedBlock[i*channels + ch];
            long long pred = ref_floor256((long long)prev1*g_coeff1[p] + (long long)prev0*g_coeff2[p]);
            int nib = ref_nibble((long long)sample - pred, delta);
            int recon = ref_clamp16(pred + (long long)nib*delta);
            delta = ref_update_delta(delta, nib);
            prev0 = prev1;
            prev1 = recon;
            if (i < realFrames) {
                long long d = (long long)recon - sample;
                err += d*d;
            }
        }
        if (p == 0 || err < bestErr) {
            bestErr = err;
            best = p;
        }
    }
    return best;
}

static int g_checks = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 0; } g_checks += 1; } while (0)

/* ------------------------------------------------------------------------------------------------------------------
encode mode.
------------------------------------------------------------------------------------------------------------------ */

static int check_encode_one(drwav_uint32 channels, drwav_uint32 sampleRate, drwav_uint16 blockAlign, int frames)
{
    size_t pcmAlloc = (size_t)frames * channels;
    drwav_int16* pcm;
    drwav_int16* paddedBlock = NULL;
    drwav_int16* refDecoded = NULL;
    drwav_int16* gotS16 = NULL;
    drwav_int32* gotS32 = NULL;
    float* gotF32 = NULL;
    MemStream stream;
    AllocCounter alloc;
    ParsedWav parsed;
    drwav_uint32 fpb;
    drwav_uint32 blockCount;
    drwav_uint32 expectedDataSize;
    drwav_uint32 bi;

    if (pcmAlloc == 0) pcmAlloc = 1;    /* Avoid zero-size allocations for the 0-frame case. */
    pcm = (drwav_int16*)malloc(pcmAlloc * sizeof(drwav_int16));
    if (pcm == NULL) { printf("FAIL out of memory\n"); return 0; }
    memset(pcm, 0, pcmAlloc * sizeof(drwav_int16));

    if (channels == 2) make_stereo_fixture(pcm, frames);
    else               make_mono_fixture(pcm, frames);

    if (!encode_file(pcm, (drwav_uint64)frames, channels, sampleRate, blockAlign, &stream, &alloc)) {
        printf("FAIL encode_file channels=%u blockAlign=%u frames=%d\n", channels, (unsigned)blockAlign, frames);
        goto fail;
    }

    CHECK(alloc.allocCount == 2);       /* Exactly the PCM buffer and the block buffer. At most one block is buffered. */
    CHECK(alloc.liveCount == 0);        /* Everything released by drwav_uninit(). */
    CHECK(alloc.freeCount == 2);

    if (!parse_wav(stream.pData, stream.size, &parsed)) {
        printf("FAIL parse_wav channels=%u blockAlign=%u frames=%d\n", channels, (unsigned)blockAlign, frames);
        goto fail;
    }

    fpb = frames_per_block(channels, blockAlign);
    blockCount = (frames == 0) ? 0 : ((drwav_uint32)frames + fpb - 1) / fpb;
    expectedDataSize = blockCount * (drwav_uint32)blockAlign;

    CHECK(parsed.formatTag == DR_WAVE_FORMAT_ADPCM);
    CHECK(parsed.channels == channels);
    CHECK(parsed.sampleRate == sampleRate);
    CHECK(parsed.blockAlign == blockAlign);
    CHECK(parsed.bitsPerSample == 4);
    CHECK(parsed.cbSize == 32);
    CHECK(parsed.samplesPerBlock == fpb);
    CHECK(parsed.numCoef == 7);
    CHECK(parsed.avgBytesPerSec == (drwav_uint32)(((drwav_uint64)sampleRate * blockAlign) / fpb));
    CHECK(parsed.factFrames == (drwav_uint32)frames);
    CHECK(parsed.dataSize == expectedDataSize);
    CHECK(parsed.dataOffset + parsed.dataSize <= stream.size);
    CHECK(parsed.riffSize == stream.size - 8);
    {
        int i;
        for (i = 0; i < 7; ++i) {
            CHECK(parsed.coef1[i] == (drwav_int16)g_coeff1[i]);
            CHECK(parsed.coef2[i] == (drwav_int16)g_coeff2[i]);
        }
    }

    if (blockCount > 0) {
        refDecoded = (drwav_int16*)malloc((size_t)blockCount * fpb * channels * sizeof(drwav_int16));
        paddedBlock = (drwav_int16*)malloc((size_t)fpb * channels * sizeof(drwav_int16));
        if (refDecoded == NULL || paddedBlock == NULL) { printf("FAIL out of memory\n"); goto fail; }

        for (bi = 0; bi < blockCount; ++bi) {
            const drwav_uint8* pBlock = stream.pData + parsed.dataOffset + (size_t)bi * blockAlign;
            drwav_int16* pOut = refDecoded + (size_t)bi * fpb * channels;
            int realStart = (int)(bi * fpb);
            int realFrames = frames - realStart;
            int ch, i;

            if (realFrames > (int)fpb) realFrames = (int)fpb;

            ref_decode_block(pBlock, (int)channels, (int)fpb, pOut);

            /* Reconstruct the padded input the encoder would have seen. */
            for (i = 0; i < (int)fpb; ++i) {
                int srcFrame = realStart + i;
                if (srcFrame >= frames) srcFrame = frames - 1;     /* Pad with the last real frame. */
                for (ch = 0; ch < (int)channels; ++ch) {
                    paddedBlock[i*channels + ch] = pcm[(size_t)srcFrame*channels + ch];
                }
            }

            for (ch = 0; ch < (int)channels; ++ch) {
                int expectedPredictor = reference_best_predictor(paddedBlock, (int)channels, ch, (int)fpb, realFrames);
                int actualPredictor = (int)pBlock[ch];
                CHECK(actualPredictor == expectedPredictor);
                CHECK(actualPredictor >= 0 && actualPredictor < 7);

                /* Initial delta per channel. */
                {
                    int d = (int)paddedBlock[(size_t)1*channels + ch] - (int)paddedBlock[0*channels + ch];
                    if (d < 0) d = -d;
                    if (d < 16) d = 16;
                    if (d > 32767) d = 32767;
                    if (channels == 1) {
                        CHECK((int)(signed short)(pBlock[1] | (pBlock[2] << 8)) == d);
                    } else {
                        CHECK((int)(signed short)(pBlock[2 + ch*2] | (pBlock[3 + ch*2] << 8)) == d);
                    }
                }
            }

            /* Header stores the first and second samples of the block. */
            {
                int s0 = (int)(signed short)((channels == 1) ? (pBlock[5] | (pBlock[6] << 8)) : (pBlock[10] | (pBlock[11] << 8)));
                CHECK(s0 == (int)pcm[(size_t)realStart*channels + 0]);

                if (channels == 2) {
                    int s0r = (int)(signed short)(pBlock[12] | (pBlock[13] << 8));
                    CHECK(s0r == (int)pcm[(size_t)realStart*channels + 1]);
                }

                if (realFrames >= 2) {
                    int s1 = (int)(signed short)((channels == 1) ? (pBlock[3] | (pBlock[4] << 8)) : (pBlock[6] | (pBlock[7] << 8)));
                    CHECK(s1 == (int)pcm[(size_t)(realStart+1)*channels + 0]);
                    if (channels == 2) {
                        int s1r = (int)(signed short)(pBlock[8] | (pBlock[9] << 8));
                        CHECK(s1r == (int)pcm[(size_t)(realStart+1)*channels + 1]);
                    }
                }
            }
        }
    }

    gotS16 = (drwav_int16*)malloc((pcmAlloc + 1) * sizeof(drwav_int16));
    gotS32 = (drwav_int32*)malloc((pcmAlloc + 1) * sizeof(drwav_int32));
    gotF32 = (float*)malloc((pcmAlloc + 1) * sizeof(float));
    if (gotS16 == NULL || gotS32 == NULL || gotF32 == NULL) { printf("FAIL out of memory\n"); goto fail; }

    /* Validate the generated file through dr_wav's existing read paths. A zero-frame file is skipped here because
       dr_wav treats a zero fact count as "no fact chunk" for RIFF and derives the frame count from the (empty) data
       chunk instead; the blocks mode checks that zero-frame file layout directly. */
    if (frames > 0) {
        drwav r;

        if (!drwav_init_memory(&r, stream.pData, stream.size, NULL)) {
            printf("FAIL drwav_init_memory\n");
            goto fail;
        }

        CHECK(r.totalPCMFrameCount == (drwav_uint64)frames);    /* Padding must not be counted via the fact chunk. */
        {
            CHECK(drwav_read_pcm_frames_s16(&r, (drwav_uint64)frames, gotS16) == (drwav_uint64)frames);
            for (bi = 0; bi < (drwav_uint32)frames * channels; ++bi) {
                size_t block = (size_t)bi / (channels * (size_t)fpb);
                size_t within = (size_t)bi % (channels * (size_t)fpb);
                CHECK(gotS16[bi] == refDecoded[block * fpb * channels + within]);
            }

            /* The decoded signal must track the source. The fixture's few full-scale steps are deliberately hard
               for a 4-bit codec, so the bound is permissive while still catching a broken encoder. */
            {
                double maxErr = 0.0;
                double meanErr = 0.0;
                for (bi = 0; bi < (drwav_uint32)frames * channels; ++bi) {
                    double e = fabs((double)gotS16[bi] - (double)pcm[bi]);
                    if (e > maxErr) maxErr = e;
                    meanErr += e;
                }
                meanErr /= (double)(frames * channels);
                CHECK(maxErr < 12000.0);
                /* Quality bounds are only meaningful once the codec has had some samples to settle. Very short
                   clips are dominated by the deliberately full-scale steps in the fixture. */
                if (frames >= 100) {
                    CHECK(meanErr < 1000.0);
                }
            }

            /* Seek around and re-read. */
            {
                drwav_uint64 at = (frames > 2) ? (drwav_uint64)(frames / 2) : 0;
                drwav_int16 small[2];
                CHECK(drwav_seek_to_pcm_frame(&r, at));
                CHECK(drwav_read_pcm_frames_s16(&r, 1, small) == 1);
                CHECK(small[0] == gotS16[(size_t)at * channels + 0]);
                if (channels == 2) CHECK(small[1] == gotS16[(size_t)at * channels + 1]);
                CHECK(drwav_seek_to_pcm_frame(&r, 0));  /* A backwards seek must reset the ADPCM decoder state. */
                CHECK(drwav_read_pcm_frames_s16(&r, 1, small) == 1);
                CHECK(small[0] == gotS16[0]);
            }
        }
        drwav_uninit(&r);

        {
            if (!drwav_init_memory(&r, stream.pData, stream.size, NULL)) {
                printf("FAIL drwav_init_memory (s32)\n");
                goto fail;
            }
            CHECK(drwav_read_pcm_frames_s32(&r, (drwav_uint64)frames, gotS32) == (drwav_uint64)frames);
            for (bi = 0; bi < (drwav_uint32)frames * channels; ++bi) {
                CHECK(gotS32[bi] == (drwav_int32)((drwav_int32)gotS16[bi] * 65536));
            }
            drwav_uninit(&r);

            if (!drwav_init_memory(&r, stream.pData, stream.size, NULL)) {
                printf("FAIL drwav_init_memory (f32)\n");
                goto fail;
            }
            CHECK(drwav_read_pcm_frames_f32(&r, (drwav_uint64)frames, gotF32) == (drwav_uint64)frames);
            for (bi = 0; bi < (drwav_uint32)frames * channels; ++bi) {
                CHECK(fabs((double)gotF32[bi] - (double)gotS16[bi] / 32768.0) < 1.0e-6);
            }
            drwav_uninit(&r);
        }
    }

    free(pcm);
    free(paddedBlock);
    free(refDecoded);
    free(gotS16);
    free(gotS32);
    free(gotF32);
    mem_free(&stream);
    return 1;

fail:
    free(pcm);
    free(paddedBlock);
    free(refDecoded);
    free(gotS16);
    free(gotS32);
    free(gotF32);
    mem_free(&stream);
    return 0;
}

static int run_encode(void)
{
    int total = 0;
    int nonzeroPredictors = 0;

    /* Stereo: 64-byte blocks give 52 frames per block; 500 frames leaves a 32-frame tail. */
    {
        int frames[] = { 0, 1, 2, 3, 4, 52, 53, 104, 500, 517 };
        size_t i;
        for (i = 0; i < sizeof(frames)/sizeof(frames[0]); ++i) {
            total += check_encode_one(2, 44100, 64, frames[i]);
        }
    }

    /* Mono: odd block alignment exercises the trailing RIFF padding byte. */
    {
        int frames[] = { 0, 1, 2, 6, 7, 54, 55, 333 };
        size_t i;
        for (i = 0; i < sizeof(frames)/sizeof(frames[0]); ++i) {
            total += check_encode_one(1, 22050, 33, frames[i]);
        }
    }

    /* Minimal blocks that hold exactly one nibble pair per block. */
    {
        int frames[] = { 1, 2, 4, 9 };
        size_t i;
        for (i = 0; i < sizeof(frames)/sizeof(frames[0]); ++i) {
            total += check_encode_one(2, 8000, 16, frames[i]);
            total += check_encode_one(1, 8000, 9, frames[i]);
        }
    }

    if (total != 26) {
        printf("FAIL encode sub-tests completed=%d expected=26\n", total);
        return 0;
    }

    /* Directly assert that the encoder does not always pick predictor 0 for non-trivial signals. */
    {
        int frames = 200;
        drwav_int16* pcm = (drwav_int16*)malloc((size_t)frames * 2 * sizeof(drwav_int16));
        MemStream s;
        ParsedWav p;
        int bi;
        if (pcm == NULL) { printf("FAIL out of memory\n"); return 0; }
        make_stereo_fixture(pcm, frames);
        if (!encode_file(pcm, (drwav_uint64)frames, 2, 44100, 64, &s, NULL)) {
            printf("FAIL encode_file (predictor survey)\n");
            free(pcm);
            return 0;
        }
        if (!parse_wav(s.pData, s.size, &p)) { printf("FAIL parse_wav\n"); free(pcm); mem_free(&s); return 0; }
        for (bi = 0; bi < (int)((frames + 51) / 52); ++bi) {
            const drwav_uint8* b = s.pData + p.dataOffset + (size_t)bi * 64;
            if (b[0] != 0 || b[1] != 0) nonzeroPredictors += 1;
        }
        CHECK(nonzeroPredictors > 0);
        free(pcm);
        mem_free(&s);
    }

    if (g_checks <= 0) return 0;
    printf("PASS encode checks=%d\n", g_checks);
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------
blocks mode.
------------------------------------------------------------------------------------------------------------------ */

static int run_blocks(void)
{
    int frames = 500;
    drwav_uint32 channels = 2;
    drwav_uint16 blockAlign = 64;
    drwav_uint64 batches[6] = { 0, 1, 13, 100, 1000, 52 };
    size_t i;

    drwav_int16* pcm = (drwav_int16*)malloc((size_t)frames * channels * sizeof(drwav_int16));
    MemStream base;
    ParsedWav parsed;
    drwav_uint32 fpb;

    if (pcm == NULL) { printf("FAIL out of memory\n"); return 0; }
    make_stereo_fixture(pcm, frames);

    if (!encode_file(pcm, (drwav_uint64)frames, channels, 44100, blockAlign, &base, NULL)) {
        printf("FAIL encode_file\n");
        free(pcm);
        return 0;
    }

    CHECK(parse_wav(base.pData, base.size, &parsed));
    fpb = frames_per_block(channels, blockAlign);
    CHECK(fpb == 52);
    CHECK(parsed.dataSize == 10 * (drwav_uint32)blockAlign);    /* ceil(500/52) = 10 blocks. */
    CHECK(base.size == 90 + parsed.dataSize);                  /* The data size is even, so there is no padding. */
    CHECK(parsed.factFrames == 500);

    /* Batching invariance: the same PCM must produce byte-identical output no matter how it is split. */
    for (i = 0; i < sizeof(batches)/sizeof(batches[0]); ++i) {
        MemStream batched;
        if (!encode_file_batched(pcm, (drwav_uint64)frames, channels, 44100, blockAlign, batches[i], &batched)) {
            printf("FAIL batched encode batch=%u\n", (unsigned)batches[i]);
            mem_free(&base);
            free(pcm);
            return 0;
        }
        CHECK(batched.size == base.size);
        CHECK(memcmp(batched.pData, base.pData, base.size) == 0);
        mem_free(&batched);
    }

    /* A 1-frame file: the second header sample is padded with the only real frame. */
    {
        drwav_int16 one[2] = { 12345, -23456 };
        MemStream s;
        ParsedWav p1;
        CHECK(encode_file(one, 1, 2, 8000, 16, &s, NULL));
        CHECK(parse_wav(s.pData, s.size, &p1));
        CHECK(p1.factFrames == 1);
        CHECK(p1.dataSize == 16);
        {
            const drwav_uint8* b = s.pData + p1.dataOffset;
            CHECK(b[10] == 0x39 && b[11] == 0x30);   /* First sample 12345 (0x3039), little-endian. */
            CHECK(b[6]  == 0x39 && b[7]  == 0x30);   /* Padded second sample repeats the first. */
            CHECK(b[2]  == 16 && b[3] == 0);          /* Delta is the minimum of 16. */
            CHECK(b[8]  == (drwav_uint8)(-23456 & 0xFF));
            CHECK(b[9]  == (drwav_uint8)(((unsigned)(-23456) >> 8) & 0xFF));
        }
        mem_free(&s);
    }

    /* A 0-frame file: no audio block, fact count 0, empty data chunk. */
    {
        MemStream s;
        ParsedWav p0;
        CHECK(encode_file(NULL, 0, 2, 8000, 16, &s, NULL));
        CHECK(parse_wav(s.pData, s.size, &p0));
        CHECK(p0.factFrames == 0);
        CHECK(p0.dataSize == 0);
        CHECK(p0.dataOffset == 90);
        CHECK(s.size == 90);
        mem_free(&s);
    }

    /* The partial tail block must repeat the final real frame and every block must start at the right input frame. */
    {
        MemStream s;
        ParsedWav p;
        int bi;
        CHECK(encode_file(pcm, (drwav_uint64)frames, channels, 44100, blockAlign, &s, NULL));
        CHECK(parse_wav(s.pData, s.size, &p));
        {
            const drwav_uint8* lastBlock = s.pData + p.dataOffset + (size_t)9 * blockAlign;
            /* 500 frames means the last block holds 500 - 9*52 = 32 real frames. Its header's second sample is
               real input frame 469; only the frames beyond 499 in the body are padded. */
            drwav_int16 secondL = pcm[(size_t)469*2 + 0];
            drwav_int16 secondR = pcm[(size_t)469*2 + 1];
            CHECK(lastBlock[6] == (drwav_uint8)(secondL & 0xFF));
            CHECK(lastBlock[7] == (drwav_uint8)(((unsigned)secondL >> 8) & 0xFF));
            CHECK(lastBlock[8] == (drwav_uint8)(secondR & 0xFF));
            CHECK(lastBlock[9] == (drwav_uint8)(((unsigned)secondR >> 8) & 0xFF));
        }
        for (bi = 0; bi < 10; ++bi) {
            const drwav_uint8* b = s.pData + p.dataOffset + (size_t)bi * blockAlign;
            drwav_int32 expect = (drwav_int32)pcm[(size_t)(bi*52)*2 + 0];
            CHECK((drwav_int32)(signed short)(b[10] | (b[11] << 8)) == expect);
        }
        mem_free(&s);
    }

    mem_free(&base);
    free(pcm);
    if (g_checks <= 0) return 0;
    printf("PASS blocks checks=%d\n", g_checks);
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------
errors mode.
------------------------------------------------------------------------------------------------------------------ */

static int run_errors(void)
{
    int frames = 200;
    drwav_int16* pcm = (drwav_int16*)malloc((size_t)frames * 2 * sizeof(drwav_int16));
    drwav_msadpcm_config cfg;
    drwav wav;

    if (pcm == NULL) { printf("FAIL out of memory\n"); return 0; }
    make_stereo_fixture(pcm, frames);

    /* Invalid configurations are rejected before any output is written. */
    {
        static const drwav_uint32 badChannels[] = { 0, 3, 4, 8 };
        static const drwav_uint16 badAlign[] = { 0, 1, 7, 14 };   /* Nothing at or below 7 * channels. */
        static const drwav_uint16 badAlignMono[] = { 0, 1, 7 };
        MemStream s;
        size_t i;
        for (i = 0; i < sizeof(badChannels)/sizeof(badChannels[0]); ++i) {
            mem_init(&s);
            cfg.channels = badChannels[i]; cfg.sampleRate = 8000; cfg.blockAlign = 16;
            CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
            CHECK(s.size == 0);
            CHECK(s.writeCount == 0);
            mem_free(&s);
        }
        for (i = 0; i < sizeof(badAlign)/sizeof(badAlign[0]); ++i) {
            mem_init(&s);
            cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = badAlign[i];
            CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
            CHECK(s.size == 0);
            CHECK(s.writeCount == 0);
            mem_free(&s);
        }
        for (i = 0; i < sizeof(badAlignMono)/sizeof(badAlignMono[0]); ++i) {
            mem_init(&s);
            cfg.channels = 1; cfg.sampleRate = 8000; cfg.blockAlign = badAlignMono[i];
            CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
            CHECK(s.size == 0);
            CHECK(s.writeCount == 0);
            mem_free(&s);
        }

        /* The smallest legal block aligns must be accepted. */
        {
            static const drwav_uint16 minimalAlign[] = { 15, 9 };
            mem_init(&s);
            cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = minimalAlign[0];
            CHECK(drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
            CHECK(drwav_uninit(&wav) == DRWAV_SUCCESS);
            mem_free(&s);
            mem_init(&s);
            cfg.channels = 1; cfg.sampleRate = 8000; cfg.blockAlign = minimalAlign[1];
            CHECK(drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
            CHECK(drwav_uninit(&wav) == DRWAV_SUCCESS);
            mem_free(&s);
        }
        /* Sample rate limits follow the library. */
        mem_init(&s);
        cfg.channels = 2; cfg.sampleRate = 0; cfg.blockAlign = 16;
        CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
        mem_free(&s);
        mem_init(&s);
        cfg.channels = 2; cfg.sampleRate = 384001; cfg.blockAlign = 16;
        CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
        mem_free(&s);
        mem_init(&s);
        cfg.channels = 2; cfg.sampleRate = 384000; cfg.blockAlign = 16;
        CHECK(drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
        CHECK(drwav_uninit(&wav) == DRWAV_SUCCESS);
        mem_free(&s);

        /* Missing callbacks are rejected. */
        cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = 16;
        CHECK(!drwav_init_msadpcm_write(&wav, &cfg, NULL, mem_seek, NULL, NULL));
        CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, NULL, NULL, NULL));
    }

    /* A short write during a data block leaves a visible failure state. */
    {
        MemStream s;
        AllocCounter a;
        drwav_allocation_callbacks cb;
        drwav_uint64 written;

        mem_init(&s);
        s.failWriteAt = 90 + 16;    /* Fail part-way into the second block. */
        memset(&a, 0, sizeof(a));
        cb = alloc_callbacks(&a);

        cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = 16;
        CHECK(drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, &cb));
        CHECK(frames_per_block(2, 16) == 4);

        /* Submit three blocks worth of frames (12 frames). The second block commit must fail. */
        written = drwav_write_msadpcm_pcm_frames(&wav, 12, pcm);
        CHECK(written < 12);
        CHECK(written >= 4);

        /* The writer is now in a failed state: no further output is produced. */
        CHECK(drwav_write_msadpcm_pcm_frames(&wav, 4, pcm + (size_t)written*2) == 0);
        {
            size_t sizeAfterFailure = s.size;
            drwav_result res = drwav_uninit(&wav);
            CHECK(res != DRWAV_SUCCESS);
            CHECK(s.size == sizeAfterFailure);      /* No more bytes were written during finalization. */
        }
        CHECK(a.liveCount == 0);                    /* Buffers released even on failure. */
        mem_free(&s);
    }

    /* A seek failure during finalization must not be reported as success. */
    {
        MemStream s;
        AllocCounter a;
        drwav_allocation_callbacks cb;

        mem_init(&s);
        s.failSeekNumber = 1;   /* The first backwards seek (patching "fact") fails. */
        memset(&a, 0, sizeof(a));
        cb = alloc_callbacks(&a);

        cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = 16;
        CHECK(drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, &cb));
        CHECK(drwav_write_msadpcm_pcm_frames(&wav, 6, pcm) == 6);
        CHECK(drwav_uninit(&wav) != DRWAV_SUCCESS);
        CHECK(a.liveCount == 0);
        mem_free(&s);
    }

    /* A failure while writing the header is reported up front, and no buffers are leaked. */
    {
        MemStream s;
        AllocCounter a;
        drwav_allocation_callbacks cb;

        mem_init(&s);
        s.failWriteAt = 0;      /* Fail immediately. */
        memset(&a, 0, sizeof(a));
        cb = alloc_callbacks(&a);

        cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = 16;
        CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, &cb));
        CHECK(a.liveCount == 0);
        CHECK(a.allocCount == 2);
        mem_free(&s);
    }

    /* A failure while writing the RIFF padding byte (only needed for an odd data size) is reported by drwav_uninit(). */
    {
        MemStream s;
        AllocCounter a;
        drwav_allocation_callbacks cb;
        drwav_uint32 fpb;

        mem_init(&s);
        memset(&a, 0, sizeof(a));
        cb = alloc_callbacks(&a);

        /* Mono with an odd block align produces an odd data size for a single block, so the finalizer writes one padding byte. */
        cfg.channels = 1; cfg.sampleRate = 8000; cfg.blockAlign = 9;
        fpb = frames_per_block(1, 9);
        CHECK(fpb == 6);
        CHECK(drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, &cb));
        CHECK(drwav_write_msadpcm_pcm_frames(&wav, 6, pcm) == 6);    /* Exactly one block. */
        /* Header is 90 bytes + one 9-byte block = 99; the padding byte would land at offset 99. */
        s.failWriteAt = 99;
        CHECK(drwav_uninit(&wav) != DRWAV_SUCCESS);
        CHECK(a.liveCount == 0);
        mem_free(&s);
    }

    /* Allocation failure at each allocation site. */
    {
        int failAt;
        cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = 16;
        for (failAt = 1; failAt <= 2; failAt += 1) {
            MemStream s;
            AllocCounter a;
            drwav_allocation_callbacks cb;
            mem_init(&s);
            memset(&a, 0, sizeof(a));
            a.failAtAlloc = failAt;
            cb = alloc_callbacks(&a);
            CHECK(!drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, &cb));
            CHECK(a.liveCount == 0);
            CHECK(s.size == 0);
            mem_free(&s);
        }
    }

    /* The ordinary PCM writer must not be routed through the ADPCM finalizer, and the ADPCM writer must not accept
       the ordinary PCM write entry point. */
    {
        drwav_data_format fmt;
        void* data = NULL;
        size_t size = 0;
        drwav_int16 samples[6] = { 10, -20, 30, -40, 50, -60 };

        memset(&fmt, 0, sizeof(fmt));
        fmt.container = drwav_container_riff;
        fmt.format = DR_WAVE_FORMAT_PCM;
        fmt.channels = 2;
        fmt.sampleRate = 8000;
        fmt.bitsPerSample = 16;

        CHECK(drwav_init_memory_write(&wav, &data, &size, &fmt, NULL));
        CHECK(drwav_write_pcm_frames(&wav, 3, samples) == 3);
        CHECK(drwav_uninit(&wav) == DRWAV_SUCCESS);
        CHECK(size > 0);
        drwav_free(data, NULL);
    }
    {
        MemStream s;
        cfg.channels = 2; cfg.sampleRate = 8000; cfg.blockAlign = 16;
        mem_init(&s);
        CHECK(drwav_init_msadpcm_write(&wav, &cfg, mem_write, mem_seek, &s, NULL));
        CHECK(drwav_write_pcm_frames(&wav, 2, pcm) == 0);      /* Compressed writers only accept the dedicated entry point. */
        CHECK(drwav_write_msadpcm_pcm_frames(&wav, 2, pcm) == 2);
        CHECK(drwav_uninit(&wav) == DRWAV_SUCCESS);
        mem_free(&s);
    }

    free(pcm);
    if (g_checks <= 0) return 0;
    printf("PASS errors checks=%d\n", g_checks);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: %s <encode|blocks|errors>\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "encode") == 0) return run_encode() ? 0 : 1;
    if (strcmp(argv[1], "blocks") == 0) return run_blocks() ? 0 : 1;
    if (strcmp(argv[1], "errors") == 0) return run_errors() ? 0 : 1;

    printf("unknown mode: %s\n", argv[1]);
    return 2;
}
