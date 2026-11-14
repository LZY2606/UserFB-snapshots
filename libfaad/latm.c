/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**
** LOAS/LATM transport support.
**
** Supported subset: audioMuxVersion == 0, allStreamsSameTimeFraming == 1,
** single program, single layer, frameLengthType == 0, 1..64 subframes,
** plain AAC-LC AudioSpecificConfig (1024 sample frames, mono or stereo,
** no PCE, no SBR, no PS).  Anything outside this subset is rejected with
** an explicit error.
**/

#include "common.h"
#include "structs.h"

#include <stdlib.h>
#include <string.h>

#include "latm.h"
#include "bits.h"
#include "syntax.h"
#include "output.h"
#include "error.h"
#include "neaacdec.h"

/* error codes from error.c */
#define LATM_ERR_CHANNELS       12 /* Invalid number of channels */
#define LATM_ERR_BUFFER_SMALL   14 /* Input data buffer too small */
#define LATM_ERR_CH_CONFIG      21 /* Unexpected channel configuration change */
#define LATM_ERR_OUTPUT_SMALL   27 /* Output data buffer too small */
#define LATM_ERR_NOT_ALLOWED    32 /* Bitstream value not allowed by specification */

#define LOAS_MIN_SIZE 3

/* Check the 11 bit LOAS syncword (0x2B7) */
static uint8_t latm_loas_sync(const unsigned char *buffer, unsigned long buffer_size)
{
    if (buffer_size < LOAS_MIN_SIZE)
        return 0;
    if (buffer[0] != 0x56 || (buffer[1] & 0xE0) != 0xE0)
        return 0;
    return 1;
}

static uint32_t latm_mux_length(const unsigned char *buffer)
{
    return ((uint32_t)(buffer[1] & 0x1F) << 8) | buffer[2];
}

/* Advance n bits while staying inside the current mux */
static uint8_t latm_skip_bits(bitfile *ld, uint32_t n, uint32_t mux_bits)
{
    if (faad_get_processed_bits(ld) + n > mux_bits)
        return LATM_ERR_BUFFER_SMALL;
    while (n > 0)
    {
        uint32_t chunk = min(n, 32);
        faad_getbits(ld, chunk);
        n -= chunk;
    }
    return 0;
}

/* Extract bits [start, start+nbits) from the bitstream buffer, MSB first */
static void latm_extract_bits(const bitfile *ld, uint32_t start, uint32_t nbits,
                              uint8_t *out)
{
    uint32_t i;
    const uint8_t *buf = (const uint8_t *)ld->buffer;

    memset(out, 0, (nbits + 7) / 8);
    for (i = 0; i < nbits; i++)
    {
        uint32_t p = start + i;
        uint8_t bit = (uint8_t)((buf[p >> 3] >> (7 - (p & 7))) & 1);
        out[i >> 3] |= (uint8_t)(bit << (7 - (i & 7)));
    }
}

/* Parse StreamMuxConfig (audioMuxVersion == 0 form) and validate it
** against the supported subset.  On success the configuration is stored
** in latm; a repeated configuration must carry the identical ASC.
** Returns 0 on success, an error code otherwise.
*/
static uint8_t latm_parse_stream_mux_config(latm_header *latm, bitfile *ld,
                                            uint32_t mux_bits)
{
    uint8_t tmpASC[MAX_ASC_BYTES];
    uint32_t asc_start, asc_bits;
    uint8_t aot, sf_index, ch_conf, frame_length_flag, depends_on_core, extension_flag;
    uint8_t tmp, esc;

    /* audioMuxVersion: only version 0 is supported */
    if (faad_getbits(ld, 1) != 0)
        return LATM_ERR_NOT_ALLOWED;

    latm->allStreamsSameTimeFraming = (uint8_t)faad_getbits(ld, 1);
    latm->numSubFrames = (uint8_t)faad_getbits(ld, 6) + 1;
    latm->numPrograms = (uint8_t)faad_getbits(ld, 4) + 1;
    latm->numLayers = (uint8_t)faad_getbits(ld, 3) + 1;
    if (faad_get_processed_bits(ld) > mux_bits)
        return LATM_ERR_BUFFER_SMALL;
    if (!latm->allStreamsSameTimeFraming || latm->numPrograms != 1 ||
        latm->numLayers != 1)
    {
        return LATM_ERR_NOT_ALLOWED;
    }

    /* AudioSpecificConfig: plain AAC-LC, 1024 sample frames, mono/stereo */
    asc_start = faad_get_processed_bits(ld);
    aot = (uint8_t)faad_getbits(ld, 5);
    sf_index = (uint8_t)faad_getbits(ld, 4);
    if (sf_index == 0x0F)
    {
        /* explicit sampling frequency */
        if (faad_getbits(ld, 24) == 0)
            return LATM_ERR_NOT_ALLOWED;
    }
    ch_conf = (uint8_t)faad_getbits(ld, 4);
    /* GASpecificConfig() */
    frame_length_flag = (uint8_t)faad_getbits(ld, 1);
    depends_on_core = (uint8_t)faad_getbits(ld, 1);
    if (depends_on_core)
        faad_getbits(ld, 14); /* coreCoderDelay */
    extension_flag = (uint8_t)faad_getbits(ld, 1);
    if (faad_get_processed_bits(ld) > mux_bits)
        return LATM_ERR_BUFFER_SMALL;

    if (aot != 2) /* AAC LC only: no SBR/PS object types */
        return LATM_ERR_NOT_ALLOWED;
    if (sf_index != 0x0F && get_sample_rate(sf_index) == 0)
        return LATM_ERR_NOT_ALLOWED;
    if (ch_conf < 1 || ch_conf > 2) /* mono or stereo, no PCE */
        return LATM_ERR_NOT_ALLOWED;
    if (frame_length_flag != 0) /* 1024 sample frames only */
        return LATM_ERR_NOT_ALLOWED;
    if (extension_flag != 0)
        return LATM_ERR_NOT_ALLOWED;

    asc_bits = faad_get_processed_bits(ld) - asc_start;
    if (asc_bits == 0 || asc_bits > MAX_ASC_BYTES * 8)
        return LATM_ERR_NOT_ALLOWED;
    latm_extract_bits(ld, asc_start, asc_bits, tmpASC);

    /* frameLengthType: only 0 (payload length signalled per subframe) */
    latm->framelen_type = (uint8_t)faad_getbits(ld, 3);
    if (latm->framelen_type != 0)
        return LATM_ERR_NOT_ALLOWED;
    faad_getbits(ld, 8); /* latmBufferFullness, not needed */

    latm->otherDataPresent = (uint8_t)faad_getbits(ld, 1);
    latm->otherDataLenBits = 0;
    if (latm->otherDataPresent)
    {
        do {
            esc = (uint8_t)faad_getbits(ld, 1);
            tmp = (uint8_t)faad_getbits(ld, 8);
            if (latm->otherDataLenBits > mux_bits)
                return LATM_ERR_NOT_ALLOWED;
            latm->otherDataLenBits = (latm->otherDataLenBits << 8) + tmp;
            if (faad_get_processed_bits(ld) > mux_bits)
                return LATM_ERR_BUFFER_SMALL;
        } while (esc);
    }

    if (faad_getbits(ld, 1)) /* crcCheckPresent */
        faad_getbits(ld, 8); /* crcCheckSum */
    if (faad_get_processed_bits(ld) > mux_bits)
        return LATM_ERR_BUFFER_SMALL;

    if (latm->inited)
    {
        /* a repeated StreamMuxConfig must carry the identical ASC */
        if (latm->ASCbits != asc_bits ||
            memcmp(latm->ASC, tmpASC, (asc_bits + 7) / 8) != 0)
        {
            return LATM_ERR_NOT_ALLOWED;
        }
    }
    memcpy(latm->ASC, tmpASC, (asc_bits + 7) / 8);
    latm->ASCbits = asc_bits;
    latm->inited = 1;

    return 0;
}

long faad_latm_init(NeAACDecStruct *hDecoder,
                    unsigned char *buffer,
                    unsigned long buffer_size,
                    unsigned long *samplerate,
                    unsigned char *channels)
{
    bitfile ld;
    latm_header *latm = &hDecoder->latm_config;
    uint32_t mux_len;
    uint8_t err;
    int8_t rc;

    if (!latm_loas_sync(buffer, buffer_size))
        return -1;
    mux_len = latm_mux_length(buffer);
    if (mux_len == 0)
        return -1;
    if (buffer_size < LOAS_MIN_SIZE + mux_len)
        return -1; /* truncated first frame */

    faad_initbits(&ld, buffer + LOAS_MIN_SIZE, mux_len);
    if (ld.error != 0)
        return -1;

    memset(latm, 0, sizeof(latm_header));

    /* the first frame must carry a StreamMuxConfig */
    if (faad_getbits(&ld, 1) != 0)
    {
        faad_endbits(&ld);
        return -1; /* useSameStreamMux without a previous configuration */
    }
    err = latm_parse_stream_mux_config(latm, &ld, mux_len * 8);
    faad_endbits(&ld);
    if (err != 0)
        return -1;

    /* flag LATM mode before Init2 so the ASC is parsed in short form */
    hDecoder->latm_header_present = 1;
    rc = NeAACDecInit2(hDecoder, latm->ASC, (latm->ASCbits + 7) / 8,
                       samplerate, channels);
    if (rc != 0)
    {
        hDecoder->latm_header_present = 0;
        latm->inited = 0;
        return -1;
    }

    return 0;
}

void *faad_latm_decode(NeAACDecStruct *hDecoder,
                       NeAACDecFrameInfo *hInfo,
                       unsigned char *buffer,
                       unsigned long buffer_size,
                       void **sample_buffer2,
                       unsigned long sample_buffer_size)
{
    bitfile ld;
    latm_header *latm = &hDecoder->latm_config;
    uint32_t mux_len, mux_bits, frame_len;
    uint32_t au_count, au;
    uint32_t required_buffer_size, stride;
    uint32_t out_samples = 0;
    uint8_t output_channels = 0;
    uint8_t err = 0;
    uint8_t *sample_buffer = NULL;
    uint16_t frame_samples = hDecoder->frameLength;
    uint16_t i;

    /* only 16 bit integer and float output are supported for LOAS/LATM */
    if (hDecoder->config.outputFormat != FAAD_FMT_16BIT &&
        hDecoder->config.outputFormat != FAAD_FMT_FLOAT)
    {
        hInfo->error = LATM_ERR_NOT_ALLOWED;
        return NULL;
    }
    stride = (hDecoder->config.outputFormat == FAAD_FMT_16BIT) ?
        sizeof(int16_t) : sizeof(float32_t);

    /* LOAS header: sync layer is a hard boundary, never read past it */
    if (!latm_loas_sync(buffer, buffer_size))
    {
        hInfo->error = LATM_ERR_NOT_ALLOWED;
        return NULL;
    }
    mux_len = latm_mux_length(buffer);
    if (mux_len == 0)
    {
        hInfo->error = LATM_ERR_NOT_ALLOWED;
        return NULL;
    }
    frame_len = LOAS_MIN_SIZE + mux_len;
    if (buffer_size < frame_len)
    {
        hInfo->error = LATM_ERR_BUFFER_SMALL;
        return NULL; /* truncated frame: do not borrow from the next one */
    }

    /* limit the bitreader to this frame only */
    faad_initbits(&ld, buffer + LOAS_MIN_SIZE, mux_len);
    if (ld.error != 0)
    {
        hInfo->error = LATM_ERR_BUFFER_SMALL;
        return NULL;
    }
    mux_bits = mux_len * 8;

    /* AudioMuxElement: configuration or reuse of the previous one */
    if (faad_get_processed_bits(&ld) + 1 > mux_bits)
    {
        err = LATM_ERR_BUFFER_SMALL;
        goto error;
    }
    if (faad_getbits(&ld, 1) == 0)
    {
        err = latm_parse_stream_mux_config(latm, &ld, mux_bits);
        if (err != 0)
            goto error;
    } else {
        if (!latm->inited)
        {
            err = LATM_ERR_NOT_ALLOWED; /* reuse without a configuration */
            goto error;
        }
    }

    au_count = (uint32_t)latm->numSubFrames;
    if (au_count == 0 || au_count > 64)
    {
        err = LATM_ERR_NOT_ALLOWED;
        goto error;
    }

    /* reserve room for the concatenated output of all access units */
    if (hDecoder->channelConfiguration < 1 || hDecoder->channelConfiguration > 2)
    {
        err = LATM_ERR_CHANNELS;
        goto error;
    }
    required_buffer_size = au_count * frame_samples *
        hDecoder->channelConfiguration * stride;
    if (sample_buffer_size == 0)
    {
        if (hDecoder->sample_buffer_size != required_buffer_size)
        {
            if (hDecoder->sample_buffer)
                faad_free(hDecoder->sample_buffer);
            hDecoder->sample_buffer = NULL;
            hDecoder->sample_buffer = faad_malloc(required_buffer_size);
            hDecoder->sample_buffer_size = required_buffer_size;
        }
        sample_buffer = (uint8_t *)hDecoder->sample_buffer;
    } else {
        if (sample_buffer_size < required_buffer_size)
        {
            err = LATM_ERR_OUTPUT_SMALL;
            goto error;
        }
        sample_buffer = (uint8_t *)*sample_buffer2;
    }
    if (sample_buffer == NULL)
    {
        err = LATM_ERR_OUTPUT_SMALL;
        goto error;
    }

    /* PayloadMux: allStreamsSameTimeFraming == 1, so every subframe
       carries its own payloadLengthInfo followed by the payload */
    for (au = 0; au < au_count; au++)
    {
        uint32_t payload_bytes = 0, payload_start, payload_end, used;
        uint8_t len_byte;

        /* payloadLengthInfo: 255 bytes are extended by another byte */
        do {
            if (faad_get_processed_bits(&ld) + 8 > mux_bits)
            {
                err = LATM_ERR_BUFFER_SMALL;
                goto error;
            }
            len_byte = (uint8_t)faad_getbits(&ld, 8);
            payload_bytes += len_byte;
            if (payload_bytes > mux_len)
            {
                err = LATM_ERR_BUFFER_SMALL;
                goto error;
            }
        } while (len_byte == 0xFF);

        if (payload_bytes == 0)
        {
            err = LATM_ERR_NOT_ALLOWED;
            goto error;
        }
        payload_start = faad_get_processed_bits(&ld);
        payload_end = payload_start + payload_bytes * 8;
        if (payload_end > mux_bits)
        {
            err = LATM_ERR_BUFFER_SMALL; /* payload runs past this frame */
            goto error;
        }

        /* decode this access unit on the running decoder history */
        raw_data_block(hDecoder, hInfo, &ld, &hDecoder->pce, hDecoder->drc);
        if (hInfo->error > 0)
            goto error;

        used = faad_get_processed_bits(&ld);
        if (used > payload_end)
        {
            err = LATM_ERR_BUFFER_SMALL; /* AU read past its own payload */
            goto error;
        }
        /* skip the padding up to the declared payload end */
        err = latm_skip_bits(&ld, payload_end - used, mux_bits);
        if (err != 0)
            goto error;

        if (hDecoder->fr_channels == 0 ||
            hDecoder->fr_channels != hDecoder->channelConfiguration)
        {
            err = LATM_ERR_CH_CONFIG;
            goto error;
        }
        output_channels = hDecoder->fr_channels;

        /* stream level first frame suppression, exactly as for raw
           access units: only the very first AU of the stream is dropped,
           it does not retrigger per transport frame */
        hDecoder->frame++;
        if (hDecoder->frame > 1)
        {
            output_to_PCM(hDecoder, hDecoder->time_out,
                sample_buffer + (size_t)out_samples * stride,
                output_channels, frame_samples, hDecoder->config.outputFormat);
            out_samples += (uint32_t)frame_samples * output_channels;
        }
    }

    /* otherData: skip by its declared bit length, bounded by this frame */
    if (latm->otherDataPresent && latm->otherDataLenBits > 0)
    {
        err = latm_skip_bits(&ld, latm->otherDataLenBits, mux_bits);
        if (err != 0)
            goto error;
    }

    faad_endbits(&ld);

    /* exactly this frame is consumed, never bytes of a following frame */
    hInfo->bytesconsumed = frame_len;
    hInfo->samples = out_samples;
    hInfo->channels = output_channels;
    hInfo->samplerate = get_sample_rate(hDecoder->sf_index);
    hInfo->object_type = hDecoder->object_type;
    hInfo->sbr = NO_SBR;
    hInfo->header_type = LATM;
#if (defined(PS_DEC) || defined(DRM_PS))
    hInfo->ps = 0;
#endif
    create_channel_config(hDecoder, hInfo);

    hDecoder->postSeekResetFlag = 0;

    return sample_buffer;

error:
    faad_endbits(&ld);

    /* reset filterbank state, mirroring the raw decode error path */
    for (i = 0; i < MAX_CHANNELS; i++)
    {
        if (hDecoder->fb_intermed[i] != NULL)
        {
            memset(hDecoder->fb_intermed[i], 0, hDecoder->frameLength*sizeof(real_t));
        }
    }

    if (hInfo->error == 0)
        hInfo->error = err;
    hInfo->samples = 0;
    hInfo->bytesconsumed = 0;
    return NULL;
}
