/*
 * PCM audio edit unit framing
 *
 * Copyright (C) 2026
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the British Broadcasting Corporation nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>

#include "pcm_frame.h"


static const uint32_t NTSC_CADENCE[PCM_FRAME_NTSC_CADENCE_LEN] = PCM_FRAME_NTSC_CADENCE;


struct PCMFramer
{
    PCMFrameConfig config;
    uint8_t *buffer;        /* bytes accumulated for the current edit unit */
    uint32_t bufferSize;    /* allocated buffer size (largest unit) */
    uint32_t buffered;      /* valid bytes in buffer */
    uint32_t target;        /* byte size of the current edit unit */
    uint32_t cadencePos;    /* cadence position of the current edit unit */
    uint64_t completedUnits;
};


int pcm_frame_config_init(PCMFrameConfig *config,
                          PCMWrapPCMFormat pcmFormat,
                          PCMWrapEditRate editRate,
                          uint32_t sequenceOffset)
{
    if (config == NULL)
    {
        return 0;
    }
    memset(config, 0, sizeof(*config));

    config->pcmFormat = pcmFormat;
    config->editRate = editRate;
    config->sequenceOffset = sequenceOffset;

    switch (pcmFormat)
    {
        case PCM_WRAP_PCM_S16LE_1CH:
            config->bitsPerSample = 16;
            config->channelCount = 1;
            break;
        case PCM_WRAP_PCM_S16LE_2CH:
            config->bitsPerSample = 16;
            config->channelCount = 2;
            break;
        case PCM_WRAP_PCM_S24LE_1CH:
            config->bitsPerSample = 24;
            config->channelCount = 1;
            break;
        case PCM_WRAP_PCM_S24LE_2CH:
            config->bitsPerSample = 24;
            config->channelCount = 2;
            break;
        default:
            return 0;
    }
    config->blockAlign = (config->bitsPerSample / 8) * config->channelCount;

    switch (editRate)
    {
        case PCM_WRAP_EDIT_RATE_25_1:
            if (sequenceOffset != 0)
            {
                return 0;
            }
            config->startCadencePosition = 0;
            break;
        case PCM_WRAP_EDIT_RATE_30000_1001:
            if (sequenceOffset >= PCM_FRAME_NTSC_CADENCE_LEN)
            {
                return 0;
            }
            config->startCadencePosition = sequenceOffset;
            break;
        default:
            return 0;
    }

    return 1;
}

uint32_t pcm_frame_unit_sample_frames(const PCMFrameConfig *config, uint32_t cadencePosition)
{
    if (config->editRate == PCM_WRAP_EDIT_RATE_25_1)
    {
        return PCM_FRAME_PAL_UNIT_FRAMES;
    }
    return NTSC_CADENCE[cadencePosition % PCM_FRAME_NTSC_CADENCE_LEN];
}

uint32_t pcm_frame_max_unit_bytes(const PCMFrameConfig *config)
{
    uint32_t maxFrames = PCM_FRAME_PAL_UNIT_FRAMES;
    uint32_t i;

    if (config->editRate == PCM_WRAP_EDIT_RATE_30000_1001)
    {
        maxFrames = 0;
        for (i = 0; i < PCM_FRAME_NTSC_CADENCE_LEN; i++)
        {
            if (NTSC_CADENCE[i] > maxFrames)
            {
                maxFrames = NTSC_CADENCE[i];
            }
        }
    }

    return maxFrames * config->blockAlign;
}

PCMFramer *pcm_framer_create(const PCMFrameConfig *config)
{
    PCMFramer *framer;

    if (config == NULL || (framer = calloc(1, sizeof(PCMFramer))) == NULL)
    {
        return NULL;
    }

    framer->config = *config;
    framer->bufferSize = pcm_frame_max_unit_bytes(config);
    framer->buffer = malloc(framer->bufferSize);
    if (framer->buffer == NULL)
    {
        free(framer);
        return NULL;
    }
    framer->cadencePos = config->startCadencePosition;
    framer->target = pcm_frame_unit_sample_frames(config, framer->cadencePos) * config->blockAlign;

    return framer;
}

void pcm_framer_destroy(PCMFramer *framer)
{
    if (framer == NULL)
    {
        return;
    }
    free(framer->buffer);
    free(framer);
}

int pcm_framer_push(PCMFramer *framer, const uint8_t *data, uint64_t length,
                    PCMFrameUnitCallback callback, void *context)
{
    uint64_t offset = 0;

    if (framer == NULL || callback == NULL || (length > 0 && data == NULL))
    {
        return 0;
    }

    while (offset < length)
    {
        uint32_t needed = framer->target - framer->buffered;
        uint32_t chunk = (uint32_t)(((length - offset) < needed) ? (length - offset) : needed);

        memcpy(framer->buffer + framer->buffered, data + offset, chunk);
        framer->buffered += chunk;
        offset += chunk;

        if (framer->buffered == framer->target)
        {
            uint32_t unitBytes = framer->target;
            uint32_t unitFrames = pcm_frame_unit_sample_frames(&framer->config, framer->cadencePos);

            if (!callback(context, framer->buffer, unitBytes, unitFrames))
            {
                return 0;
            }

            framer->completedUnits++;
            framer->buffered = 0;
            framer->cadencePos = (framer->cadencePos + 1) % PCM_FRAME_NTSC_CADENCE_LEN;
            framer->target = pcm_frame_unit_sample_frames(&framer->config, framer->cadencePos) *
                             framer->config.blockAlign;
        }
    }

    return 1;
}

uint64_t pcm_framer_completed_units(const PCMFramer *framer)
{
    return framer->completedUnits;
}

uint32_t pcm_framer_buffered_bytes(const PCMFramer *framer)
{
    return framer->buffered;
}

uint32_t pcm_framer_next_cadence_position(const PCMFramer *framer)
{
    return framer->cadencePos;
}
