#include "common.h"
#include "structs.h"

#include <string.h>

#include "conceal.h"
#include "filtbank.h"
#include "output.h"
#include "syntax.h"

static uint8_t conceal_geometry_is_short(uint8_t sequence)
{
    return sequence == EIGHT_SHORT_SEQUENCE;
}

static uint8_t conceal_next_sequence(uint8_t sequence)
{
    switch (sequence)
    {
    case LONG_START_SEQUENCE:
        return EIGHT_SHORT_SEQUENCE;
    case EIGHT_SHORT_SEQUENCE:
        return EIGHT_SHORT_SEQUENCE;
    case LONG_STOP_SEQUENCE:
        return ONLY_LONG_SEQUENCE;
    case ONLY_LONG_SEQUENCE:
    default:
        return ONLY_LONG_SEQUENCE;
    }
}

void conceal_reinit(NeAACDecStruct *hDecoder)
{
    conceal_spectrum_observer observer;
    void *observer_user;

    if (hDecoder == NULL)
        return;

    observer = hDecoder->conceal.observer;
    observer_user = hDecoder->conceal.observer_user;
    memset(&hDecoder->conceal, 0, sizeof(hDecoder->conceal));
    hDecoder->conceal.observer = observer;
    hDecoder->conceal.observer_user = observer_user;
}

void conceal_post_seek_reset(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;

    hDecoder->conceal.history_valid = 0;
    hDecoder->conceal.lost_frames = 0;
    memset(hDecoder->conceal.pending_valid, 0,
           sizeof(hDecoder->conceal.pending_valid));
}

void conceal_decode_begin(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;

    /* An old history must not survive a failed decode attempt.  A successful
       AU atomically commits the pending spectrum below. */
    hDecoder->conceal.history_valid = 0;
    hDecoder->conceal.lost_frames = 0;
    memset(hDecoder->conceal.pending_valid, 0,
           sizeof(hDecoder->conceal.pending_valid));
}

void conceal_capture_spectrum(NeAACDecStruct *hDecoder, uint8_t channel,
                              uint8_t window_sequence, uint8_t window_shape,
                              const real_t *spectrum)
{
    conceal_state *c;

    if (hDecoder == NULL || spectrum == NULL)
        return;

    c = &hDecoder->conceal;
    if (c->observer != NULL)
        c->observer(c->observer_user, channel, window_sequence,
                    window_shape, spectrum);

    if (channel >= CONCEAL_MAX_CHANNELS || hDecoder->frameLength != CONCEAL_FRAME_LEN)
        return;

    memcpy(c->pending_spectrum[channel], spectrum,
           CONCEAL_FRAME_LEN * sizeof(real_t));
    c->pending_sequence[channel] = window_sequence;
    c->pending_shape[channel] = window_shape;
    c->pending_valid[channel] = 1;
}

static uint8_t conceal_scope_supported(const NeAACDecStruct *hDecoder)
{
    const conceal_state *c;
    uint32_t rate;

    if (hDecoder == NULL || hDecoder->fb == NULL)
        return 0;

    c = &hDecoder->conceal;

#ifndef FIXED_POINT
    /* The first supported configuration is the normal floating-point build. */
#else
    (void)c;
    return 0;
#endif

    if (!c->raw_init2)
        return 0;
    if (hDecoder->adts_header_present || hDecoder->adif_header_present ||
        hDecoder->latm_header_present)
        return 0;
    if (hDecoder->pce_set)
        return 0;
    if (hDecoder->object_type != LC)
        return 0;
    if (hDecoder->frameLength != CONCEAL_FRAME_LEN)
        return 0;

    rate = get_sample_rate(hDecoder->sf_index);
    if (rate != 44100 && rate != 48000)
        return 0;

    if (hDecoder->channelConfiguration != 1 &&
        hDecoder->channelConfiguration != 2)
        return 0;
    if (hDecoder->config.downMatrix || hDecoder->downMatrix)
        return 0;

#ifdef SBR_DEC
    if (hDecoder->sbr_present_flag == 1 || hDecoder->forceUpSampling == 1)
        return 0;
#endif
#if (defined(PS_DEC) || defined(DRM_PS))
    if (hDecoder->ps_used_global || hDecoder->ps_present_flag == 1)
        return 0;
#endif

    if (hDecoder->config.outputFormat != FAAD_FMT_16BIT &&
        hDecoder->config.outputFormat != FAAD_FMT_FLOAT)
        return 0;

    return 1;
}

void conceal_commit_good_au(NeAACDecStruct *hDecoder)
{
    conceal_state *c;
    uint8_t out_ch;

    if (hDecoder == NULL)
        return;

    c = &hDecoder->conceal;
    c->history_valid = 0;
    c->channels = 0;

    if (!conceal_scope_supported(hDecoder))
        return;
    if (hDecoder->fr_ch_ele != 1)
        return;
    if (hDecoder->first_syn_ele != ID_SCE &&
        hDecoder->first_syn_ele != ID_CPE)
        return;
    if (hDecoder->fr_channels < 1 || hDecoder->fr_channels > CONCEAL_MAX_CHANNELS)
        return;
    if (hDecoder->channelConfiguration != hDecoder->fr_channels)
        return;

    for (out_ch = 0; out_ch < hDecoder->fr_channels; out_ch++)
    {
        uint8_t ch = hDecoder->internal_channel[out_ch];

        if (ch >= CONCEAL_MAX_CHANNELS || !c->pending_valid[ch])
            return;
        if (hDecoder->time_out[ch] == NULL || hDecoder->fb_intermed[ch] == NULL)
            return;

        memcpy(c->spectrum[ch], c->pending_spectrum[ch],
               CONCEAL_FRAME_LEN * sizeof(real_t));
        c->window_sequence[ch] = c->pending_sequence[ch];
        c->window_shape[ch] = c->pending_shape[ch];
        c->synthesis_sequence[ch] = c->pending_sequence[ch];
    }

    c->channels = hDecoder->fr_channels;
    c->lost_frames = 0;
    c->history_valid = 1;
}

static unsigned char conceal_output_stride(uint8_t format)
{
    switch (format)
    {
    case FAAD_FMT_16BIT:
        return sizeof(int16_t);
    case FAAD_FMT_FLOAT:
        return sizeof(float32_t);
    default:
        return 0;
    }
}

static void* conceal_decode(NeAACDecHandle hpDecoder,
                            NeAACDecFrameInfo *hInfo,
                            void **sample_buffer2,
                            unsigned long sample_buffer_size,
                            int caller_buffer)
{
    NeAACDecStruct *hDecoder = (NeAACDecStruct*)hpDecoder;
    conceal_state *c;
    uint8_t channels, out_ch;
    unsigned char stride;
    unsigned long required_size;
    void *sample_buffer;

    if (hInfo != NULL)
        memset(hInfo, 0, sizeof(*hInfo));

    /* The buffer contract is checked before touching decoder state.  For
       Conceal2 an empty pointer/buffer always reports the existing buffer
       error code 27. */
    if (caller_buffer &&
        (sample_buffer2 == NULL || *sample_buffer2 == NULL ||
         sample_buffer_size == 0))
    {
        if (hInfo != NULL)
            hInfo->error = 27;
        return NULL;
    }

    if (hDecoder == NULL)
    {
        if (hInfo != NULL)
            hInfo->error = CONCEAL_ERR_UNSUPPORTED;
        return NULL;
    }
    if (hInfo == NULL)
        return NULL;

    c = &hDecoder->conceal;

    if (!conceal_scope_supported(hDecoder))
    {
        hInfo->error = CONCEAL_ERR_UNSUPPORTED;
        return NULL;
    }

    channels = hDecoder->channelConfiguration;
    stride = conceal_output_stride(hDecoder->config.outputFormat);
    if (stride == 0)
    {
        hInfo->error = CONCEAL_ERR_UNSUPPORTED;
        return NULL;
    }
    required_size = CONCEAL_FRAME_LEN * channels * stride;

    if (caller_buffer)
    {
        if (sample_buffer2 == NULL || *sample_buffer2 == NULL)
        {
            hInfo->error = 27;
            return NULL;
        }
        if (sample_buffer_size < required_size)
        {
            hInfo->error = 27;
            return NULL;
        }
    }

    if (!c->history_valid || c->channels != channels)
    {
        hInfo->error = CONCEAL_ERR_NO_HISTORY;
        return NULL;
    }

    for (out_ch = 0; out_ch < channels; out_ch++)
    {
        uint8_t ch = hDecoder->internal_channel[out_ch];
        if (ch >= CONCEAL_MAX_CHANNELS || hDecoder->time_out[ch] == NULL ||
            hDecoder->fb_intermed[ch] == NULL)
        {
            hInfo->error = CONCEAL_ERR_NO_HISTORY;
            return NULL;
        }
    }

    if (!caller_buffer)
    {
        if (hDecoder->sample_buffer_size != required_size)
        {
            if (hDecoder->sample_buffer != NULL)
                faad_free(hDecoder->sample_buffer);
            hDecoder->sample_buffer = faad_malloc(required_size);
            hDecoder->sample_buffer_size = required_size;
        }
        if (hDecoder->sample_buffer == NULL)
        {
            hDecoder->sample_buffer_size = 0;
            hInfo->error = CONCEAL_ERR_UNSUPPORTED;
            return NULL;
        }
        sample_buffer = hDecoder->sample_buffer;
    }
    else
    {
        sample_buffer = *sample_buffer2;
    }

    /* All fallible checks and allocation are complete.  From here on the call
       is one real missing AU and must advance synthesis state. */
    if (c->lost_frames < 32)
        c->lost_frames++;

    for (out_ch = 0; out_ch < channels; out_ch++)
    {
        uint8_t ch = hDecoder->internal_channel[out_ch];
        uint8_t cached_sequence = c->window_sequence[ch];
        uint8_t synthesis_sequence = conceal_next_sequence(c->synthesis_sequence[ch]);
        uint8_t shape = c->window_shape[ch];
        real_t gain = 0;
        uint16_t i;

        c->synthesis_sequence[ch] = synthesis_sequence;

        if (c->lost_frames < 32 &&
            conceal_geometry_is_short(cached_sequence) ==
            conceal_geometry_is_short(synthesis_sequence))
        {
            /* Compute 0.75^k from the immutable good spectrum generation.
               Never repeatedly scale c->spectrum itself. */
            gain = REAL_CONST(1.0);
            for (uint32_t k = 0; k < c->lost_frames; k++)
                gain = MUL_F(gain, REAL_CONST(0.75));

            for (i = 0; i < CONCEAL_FRAME_LEN; i++)
                c->work[ch][i] = MUL_F(c->spectrum[ch][i], gain);
        }
        else
        {
            /* k >= 32, or long coefficients cannot be reinterpreted as the
               short geometry selected by the window transition. */
            memset(c->work[ch], 0, sizeof(c->work[ch]));
        }

        ifilter_bank(hDecoder->fb, synthesis_sequence, shape,
                      hDecoder->window_shape_prev[ch], c->work[ch],
                      hDecoder->time_out[ch], hDecoder->fb_intermed[ch],
                      hDecoder->object_type, hDecoder->frameLength);

        /* Same state update as reconstruct_*_channel(). */
        hDecoder->window_shape_prev[ch] = shape;
    }

    create_channel_config(hDecoder, hInfo);

    hInfo->bytesconsumed = 0;
    hInfo->samples = CONCEAL_FRAME_LEN * channels;
    hInfo->channels = channels;
    hInfo->samplerate = get_sample_rate(hDecoder->sf_index);
    hInfo->object_type = hDecoder->object_type;
    hInfo->sbr = NO_SBR;
    hInfo->header_type = RAW;
#if (defined(PS_DEC) || defined(DRM_PS))
    hInfo->ps = hDecoder->ps_used_global;
#endif

    sample_buffer = output_to_PCM(hDecoder, hDecoder->time_out, sample_buffer,
                                  channels, CONCEAL_FRAME_LEN,
                                  hDecoder->config.outputFormat);

    hDecoder->postSeekResetFlag = 0;
    hDecoder->frame++;

    return sample_buffer;
}

NEAACDECAPI void* NeAACDecConceal(NeAACDecHandle hDecoder,
                                  NeAACDecFrameInfo *hInfo)
{
    return conceal_decode(hDecoder, hInfo, NULL, 0, 0);
}

NEAACDECAPI void* NeAACDecConceal2(NeAACDecHandle hDecoder,
                                   NeAACDecFrameInfo *hInfo,
                                   void **sample_buffer,
                                   unsigned long sample_buffer_size)
{
    return conceal_decode(hDecoder, hInfo, sample_buffer, sample_buffer_size, 1);
}
