#include <math.h>
#include <string.h>

#include "common.h"
#include "structs.h"

#include "conceal.h"
#include "filtbank.h"
#include "output.h"
#include "syntax.h"

/* Implemented in decoder.c; fills the channel-position part of FrameInfo. */
void create_channel_config(NeAACDecStruct *hDecoder, NeAACDecFrameInfo *hInfo);

void conceal_reset(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;

    hDecoder->conceal_initialized = 0;
    conceal_invalidate(hDecoder);
}

void conceal_invalidate(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;

    hDecoder->conceal_history_valid = 0;
    hDecoder->conceal_channels = 0;
    hDecoder->conceal_capture_mask = 0;
    hDecoder->conceal_loss_count = 0;
}

void conceal_decode_begin(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;

    /* An attempted normal AU does not leave the previous good frame usable:
       either this attempt succeeds and installs fresh history, or failure
       must leave no history. */
    hDecoder->conceal_history_valid = 0;
    hDecoder->conceal_capture_mask = 0;
    hDecoder->conceal_loss_count = 0;
}

void conceal_capture_spectrum(NeAACDecStruct *hDecoder,
                              uint8_t channel,
                              const real_t *spectrum,
                              uint8_t window_sequence,
                              uint8_t window_shape)
{
    if (hDecoder == NULL || spectrum == NULL || channel >= 2 ||
        hDecoder->frameLength != 1024)
        return;

    memcpy(hDecoder->conceal_spectrum[channel], spectrum,
           1024 * sizeof(real_t));
    hDecoder->conceal_history_sequence[channel] = window_sequence;
    hDecoder->conceal_history_shape[channel] = window_shape;
    hDecoder->conceal_capture_mask |= (uint8_t)(1u << channel);
}

static int conceal_profile_supported(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL || !hDecoder->conceal_initialized)
        return 0;

#ifdef FIXED_POINT
    /* This task first targets the ordinary floating-point libfaad build. */
    return 0;
#else
    if (hDecoder->frameLength != 1024 || hDecoder->fb == NULL)
        return 0;

    /* AudioSpecificConfig(LC, 44.1/48 kHz, mono/stereo), raw payload. */
    if (hDecoder->object_type != LC)
        return 0;
    if (hDecoder->sf_index != 3 && hDecoder->sf_index != 4)
        return 0;
    if (hDecoder->adts_header_present || hDecoder->adif_header_present ||
        hDecoder->latm_header_present)
        return 0;
    if (hDecoder->pce_set || hDecoder->channelConfiguration < 1 ||
        hDecoder->channelConfiguration > 2)
        return 0;
    if (hDecoder->fr_channels != 0 &&
        hDecoder->fr_channels != hDecoder->channelConfiguration)
        return 0;

    /* Scope is one SCE or one CPE, without downmixing/SBR/PS. */
    if (hDecoder->config.downMatrix || hDecoder->downMatrix)
        return 0;
    if (hDecoder->sbr_present_flag == 1 || hDecoder->forceUpSampling == 1)
        return 0;
#if (defined(PS_DEC) || defined(DRM_PS))
    if (hDecoder->ps_present_flag == 1 || hDecoder->ps_used_global)
        return 0;
#endif

    if (hDecoder->config.outputFormat != FAAD_FMT_16BIT &&
        hDecoder->config.outputFormat != FAAD_FMT_FLOAT)
        return 0;

    return 1;
#endif
}

void conceal_commit(NeAACDecStruct *hDecoder, NeAACDecFrameInfo *hInfo)
{
    uint8_t channels, expected_mask, ch;

    if (hDecoder == NULL)
        return;

    if (hInfo == NULL || hInfo->error != 0 || !conceal_profile_supported(hDecoder))
    {
        conceal_invalidate(hDecoder);
        return;
    }

    channels = hDecoder->fr_channels;
    if (channels != 1 && channels != 2)
    {
        conceal_invalidate(hDecoder);
        return;
    }
    if (hDecoder->fr_ch_ele != 1)
    {
        conceal_invalidate(hDecoder);
        return;
    }
    if (channels == 1 && hDecoder->first_syn_ele != ID_SCE)
    {
        conceal_invalidate(hDecoder);
        return;
    }
    if (channels == 2 && hDecoder->first_syn_ele != ID_CPE)
    {
        conceal_invalidate(hDecoder);
        return;
    }

    expected_mask = (uint8_t)((1u << channels) - 1u);
    if (hDecoder->conceal_capture_mask != expected_mask)
    {
        conceal_invalidate(hDecoder);
        return;
    }

    for (ch = 0; ch < channels; ch++)
    {
        uint8_t internal_ch = hDecoder->internal_channel[ch];
        if (internal_ch >= 2 ||
            !(hDecoder->conceal_capture_mask & (1u << internal_ch)) ||
            hDecoder->time_out[internal_ch] == NULL ||
            hDecoder->fb_intermed[internal_ch] == NULL)
        {
            conceal_invalidate(hDecoder);
            return;
        }
        hDecoder->conceal_synthesis_sequence[ch] =
            hDecoder->conceal_history_sequence[internal_ch];
    }

    hDecoder->conceal_channels = channels;
    hDecoder->conceal_loss_count = 0;
    hDecoder->conceal_history_valid = 1;
}

static int sequence_is_short(uint8_t sequence)
{
    return sequence == EIGHT_SHORT_SEQUENCE;
}

static uint8_t advance_sequence(uint8_t sequence)
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

static unsigned long conceal_buffer_bytes(uint8_t output_format,
                                          uint8_t channels)
{
    unsigned long sample_bytes;

    if (channels != 1 && channels != 2)
        return 0;

    if (output_format == FAAD_FMT_16BIT)
        sample_bytes = 2;
    else if (output_format == FAAD_FMT_FLOAT)
        sample_bytes = 4; /* FAAD_FMT_FLOAT in the floating build */
    else
        return 0;

    return 1024UL * channels * sample_bytes;
}

static unsigned long conceal_active_buffer_bytes(NeAACDecStruct *hDecoder)
{
    uint8_t channels;

    if (hDecoder == NULL)
        return 0;
    channels = hDecoder->conceal_history_valid ?
        hDecoder->conceal_channels : hDecoder->channelConfiguration;
    return conceal_buffer_bytes(hDecoder->config.outputFormat, channels);
}

static void *conceal_render(NeAACDecStruct *hDecoder,
                            NeAACDecFrameInfo *hInfo,
                            void *sample_buffer)
{
    uint8_t out_ch, channels = hDecoder->conceal_channels;
    uint32_t k;
    double gain;

    if (hDecoder->conceal_loss_count < UINT32_MAX)
        hDecoder->conceal_loss_count++;
    k = hDecoder->conceal_loss_count;

    /* 0.75 is exactly representable; pow() gives the specified independent
       power for this k rather than compounding a stored attenuated buffer. */
    gain = (k >= 32) ? 0.0 : pow(0.75, (double)k);

    for (out_ch = 0; out_ch < channels; out_ch++)
    {
        uint8_t decoder_ch = hDecoder->internal_channel[out_ch];
        uint8_t next_sequence =
            advance_sequence(hDecoder->conceal_synthesis_sequence[out_ch]);
        int geometry_matches =
            sequence_is_short(hDecoder->conceal_history_sequence[decoder_ch]) ==
            sequence_is_short(next_sequence);
        uint16_t i;

        if (gain != 0.0 && geometry_matches)
        {
            for (i = 0; i < 1024; i++)
            {
                hDecoder->conceal_work[out_ch][i] =
                    (real_t)((double)hDecoder->conceal_spectrum[decoder_ch][i] *
                             gain);
            }
        }
        else
        {
            memset(hDecoder->conceal_work[out_ch], 0,
                   sizeof(hDecoder->conceal_work[out_ch]));
        }

        ifilter_bank(hDecoder->fb, next_sequence,
                     hDecoder->conceal_history_shape[decoder_ch],
                     hDecoder->window_shape_prev[decoder_ch],
                     hDecoder->conceal_work[out_ch],
                     hDecoder->time_out[decoder_ch],
                     hDecoder->fb_intermed[decoder_ch],
                     hDecoder->object_type, hDecoder->frameLength);

        /* Match the normal decoder's bookkeeping after ifilter_bank(). */
        hDecoder->window_shape_prev[decoder_ch] =
            hDecoder->conceal_history_shape[decoder_ch];
        hDecoder->conceal_synthesis_sequence[out_ch] = next_sequence;
    }

    sample_buffer = output_to_PCM(hDecoder, hDecoder->time_out, sample_buffer,
                                  channels, 1024,
                                  hDecoder->config.outputFormat);

    hDecoder->postSeekResetFlag = 0;
    hDecoder->frame++;

    memset(hInfo, 0, sizeof(*hInfo));
    hInfo->bytesconsumed = 0;
    hInfo->samples = 1024UL * channels;
    hInfo->channels = channels;
    hInfo->error = 0;
    hInfo->samplerate = get_sample_rate(hDecoder->sf_index);
    hInfo->object_type = hDecoder->object_type;
    hInfo->sbr = NO_SBR;
    hInfo->header_type = RAW;
#if (defined(PS_DEC) || defined(DRM_PS))
    hInfo->ps = hDecoder->ps_used_global;
#endif
    create_channel_config(hDecoder, hInfo);

    return sample_buffer;
}

void *NeAACDecConceal(NeAACDecHandle hpDecoder, NeAACDecFrameInfo *hInfo)
{
    NeAACDecStruct *hDecoder = (NeAACDecStruct *)hpDecoder;
    unsigned long required_size;
    void *sample_buffer;

    if (hInfo == NULL)
        return NULL;
    memset(hInfo, 0, sizeof(*hInfo));

    if (hDecoder == NULL || !conceal_profile_supported(hDecoder) ||
        !hDecoder->conceal_history_valid)
    {
        hInfo->error = CONCEAL_ERROR_UNAVAILABLE;
        hInfo->samples = 0;
        return NULL;
    }

    required_size = conceal_buffer_bytes(hDecoder->config.outputFormat,
                                         hDecoder->conceal_channels);
    if (hDecoder->sample_buffer_size != required_size)
    {
        if (hDecoder->sample_buffer)
            faad_free(hDecoder->sample_buffer);
        hDecoder->sample_buffer = faad_malloc(required_size);
        hDecoder->sample_buffer_size = required_size;
    }
    if (hDecoder->sample_buffer == NULL)
    {
        hDecoder->sample_buffer_size = 0;
        hInfo->error = CONCEAL_ERROR_UNAVAILABLE;
        return NULL;
    }

    sample_buffer = hDecoder->sample_buffer;
    return conceal_render(hDecoder, hInfo, sample_buffer);
}

void *NeAACDecConceal2(NeAACDecHandle hpDecoder,
                       NeAACDecFrameInfo *hInfo,
                       void **sample_buffer,
                       unsigned long sample_buffer_size)
{
    NeAACDecStruct *hDecoder = (NeAACDecStruct *)hpDecoder;
    unsigned long required_size;

    if (hInfo == NULL)
        return NULL;
    memset(hInfo, 0, sizeof(*hInfo));

    /* Argument/buffer rejection must be observable as error 27 and must not
       touch decoder state. */
    if (sample_buffer == NULL || *sample_buffer == NULL ||
        sample_buffer_size == 0)
    {
        hInfo->error = 27;
        hInfo->samples = 0;
        return NULL;
    }

    if (hDecoder != NULL)
    {
        unsigned long available_frame_bytes =
            conceal_active_buffer_bytes(hDecoder);
        if (available_frame_bytes != 0 &&
            sample_buffer_size < available_frame_bytes)
        {
            hInfo->error = 27;
            hInfo->samples = 0;
            return NULL;
        }
    }

    if (hDecoder == NULL || !conceal_profile_supported(hDecoder) ||
        !hDecoder->conceal_history_valid)
    {
        hInfo->error = CONCEAL_ERROR_UNAVAILABLE;
        hInfo->samples = 0;
        return NULL;
    }

    required_size = conceal_buffer_bytes(hDecoder->config.outputFormat,
                                         hDecoder->conceal_channels);
    if (sample_buffer_size < required_size)
    {
        hInfo->error = 27;
        hInfo->samples = 0;
        return NULL;
    }

    return conceal_render(hDecoder, hInfo, *sample_buffer);
}
