/*
 * Streaming PCM to audio-only OP1a MXF wrapper
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
 *       from this software without specific written permission.
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

#include <mxf/mxf.h>

#include "pcm_wrap.h"
#include "pcm_frame.h"
#include "pcm_wrap_mxf.h"


struct PCMWrapWriter
{
    MXFFile *file;
    PCMFrameConfig config;
    PCMFramer *framer;
    PCMMxfMuxer *muxer;
    int failed;
    int finished;
};


static int write_unit_callback(void *context, const uint8_t *unit,
                               uint32_t byteLength, uint32_t sampleFrames)
{
    PCMWrapWriter *writer = (PCMWrapWriter*)context;

    (void)sampleFrames;
    return pcm_mxf_write_unit(writer->muxer, unit, byteLength);
}

PCMWrapWriter *pcm_wrap_begin(MXFFile *mxfFile,
                              PCMWrapPCMFormat pcmFormat,
                              PCMWrapEditRate editRate,
                              uint32_t sequenceOffset,
                              uint32_t maxEditUnitsPerBodyPartition)
{
    PCMWrapWriter *writer = NULL;
    PCMFrameConfig config;
    int64_t cursorPos;

    if (mxfFile == NULL ||
        !mxf_file_is_seekable(mxfFile) ||
        !pcm_frame_config_init(&config, pcmFormat, editRate, sequenceOffset) ||
        maxEditUnitsPerBodyPartition < 1 ||
        maxEditUnitsPerBodyPartition > PCM_WRAP_MAX_UNITS_PER_BODY)
    {
        return NULL;
    }

    /* the output must be empty with the cursor at zero */
    if (mxf_file_size(mxfFile) != 0)
    {
        return NULL;
    }
    cursorPos = mxf_file_tell(mxfFile);
    if (cursorPos != 0 || !mxf_file_seek(mxfFile, 0, SEEK_SET))
    {
        return NULL;
    }

    writer = calloc(1, sizeof(PCMWrapWriter));
    if (writer == NULL)
    {
        return NULL;
    }
    writer->file = mxfFile;
    writer->config = config;

    writer->framer = pcm_framer_create(&config);
    if (writer->framer == NULL)
    {
        goto fail;
    }

    writer->muxer = pcm_mxf_create(mxfFile, &config, maxEditUnitsPerBodyPartition);
    if (writer->muxer == NULL)
    {
        goto fail;
    }

    return writer;

fail:
    pcm_wrap_destroy(writer);
    return NULL;
}

int pcm_wrap_push(PCMWrapWriter *writer, const uint8_t *data, uint64_t length)
{
    if (writer == NULL || writer->failed || writer->finished)
    {
        return 0;
    }
    if (length > 0 && data == NULL)
    {
        writer->failed = 1;
        return 0;
    }

    if (!pcm_framer_push(writer->framer, data, length, write_unit_callback, writer))
    {
        writer->failed = 1;
        return 0;
    }

    return 1;
}

int pcm_wrap_finish(PCMWrapWriter *writer)
{
    uint64_t completedUnits;

    if (writer == NULL || writer->failed)
    {
        return 0;
    }
    if (writer->finished)
    {
        return 1;
    }

    /* any buffered bytes short of a complete edit unit make the input invalid */
    if (pcm_framer_buffered_bytes(writer->framer) != 0)
    {
        writer->failed = 1;
        return 0;
    }

    completedUnits = pcm_framer_completed_units(writer->framer);
    if (completedUnits == 0 ||
        !pcm_mxf_finish(writer->muxer, completedUnits))
    {
        writer->failed = 1;
        return 0;
    }

    writer->finished = 1;
    return 1;
}

void pcm_wrap_destroy(PCMWrapWriter *writer)
{
    if (writer == NULL)
    {
        return;
    }
    pcm_framer_destroy(writer->framer);
    pcm_mxf_destroy(writer->muxer);
    free(writer);
}
