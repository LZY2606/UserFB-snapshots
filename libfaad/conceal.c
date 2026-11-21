/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder
**
** Error concealment: NeAACDecConceal / NeAACDecConceal2 and the cache that
** records the most recent complete access unit's spectrum, window shape and
** window sequence.
**
** The cache stores the spectrum after PNS, M/S, intensity stereo, TNS and DRC
** but before the inverse filter bank.  Quantised spectra and already emitted
** PCM are never used.  Only the synthesis stage is run for a missing access
** unit, so PNS random numbers, TNS and DRC state are left untouched.
*/

#include "common.h"
#include "structs.h"

#include <string.h>

#include "syntax.h"
#include "filtbank.h"
#include "output.h"
#include "error.h"
#include "conceal.h"

/* Number of consecutive missing access units after which the cached spectrum
   carries no energy any more.  k only ever grows, so every later call stays
   silent instead of wrapping back to full energy. */
#define CONCEAL_SILENT_AFTER 32

/* amplitude decay applied to the cached spectrum for the k-th missing AU */
#define CONCEAL_DECAY 0.75

/* the output-buffer-too-small code matches the existing entry points */
#define CONCEAL_ERR_BUFFER 27

/* last code of the (extended) error table */
#define CONCEAL_ERR_UNAVAILABLE (NUM_ERROR_MESSAGES - 1)

/* ------------------------------------------------------------------------- */
/* cache bookkeeping                                                         */
/* ------------------------------------------------------------------------- */

static void conceal_invalidate_cache(NeAACDecStruct *hDecoder)
{
    conceal_state *st = &hDecoder->conceal;

    memset(&st->cache, 0, sizeof(st->cache));
    st->cache.valid = 0;
    st->good = 0;
    st->lost = 0;
    st->pending_mask = 0;
    memset(&st->pending, 0, sizeof(st->pending));
}

void faad_conceal_reset(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;
    /* an internal concealment buffer belongs to this handle only */
    if (hDecoder->conceal.conceal_sample_buffer)
        faad_free(hDecoder->conceal.conceal_sample_buffer);
    memset(&hDecoder->conceal, 0, sizeof(hDecoder->conceal));
}

void faad_conceal_set_initialized(NeAACDecStruct *hDecoder)
{
    conceal_state *st;

    if (hDecoder == NULL)
        return;
    st = &hDecoder->conceal;

    if (st->conceal_sample_buffer)
        faad_free(st->conceal_sample_buffer);

    /* Init2 establishes the active configuration.  Re-initialisation must not
       carry hidden history across. */
    memset(st->last_window_sequence, 0, sizeof(st->last_window_sequence));
    conceal_invalidate_cache(hDecoder);
    st->initialized = 1;
    st->unsupported = 0;
    st->output_format = hDecoder->config.outputFormat;
    st->channel_configuration = hDecoder->channelConfiguration;
    st->conceal_sample_buffer = NULL;
    st->conceal_sample_buffer_size = 0;
}

void faad_conceal_invalidate(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;
    conceal_invalidate_cache(hDecoder);
}

void faad_conceal_begin_au(NeAACDecStruct *hDecoder)
{
    if (hDecoder == NULL)
        return;

    /* A fresh decode attempt starts: the previously committed AU must not be
       reused when this attempt fails. */
    conceal_invalidate_cache(hDecoder);
    hDecoder->conceal.unsupported = 0;
    hDecoder->conceal.pending_mask = 0;
}

void faad_conceal_capture(NeAACDecStruct *hDecoder, uint8_t channel,
                          uint8_t window_sequence, uint8_t window_shape,
                          const real_t *spec, uint16_t frame_len)
{
    conceal_state *st;

    if (hDecoder == NULL)
        return;
    st = &hDecoder->conceal;

    if (!st->initialized || spec == NULL)
        return;
    if (frame_len != CONCEAL_SPEC_LEN || channel >= CONCEAL_MAX_CHANNELS)
    {
        /* out of the fixed-size scope: do not build history for this frame */
        st->unsupported = 1;
        return;
    }

    memcpy(st->pending.spec[channel], spec, CONCEAL_SPEC_LEN * sizeof(real_t));
    st->pending.window_sequence[channel] = window_sequence;
    st->pending.window_shape[channel] = window_shape;
    st->pending_mask |= (uint8_t)(1u << channel);
}

/* Is the active configuration one the concealment entry points support? */
static uint8_t conceal_scope_ok(NeAACDecStruct *hDecoder)
{
    uint32_t rate = get_sample_rate(hDecoder->sf_index);

    if (!hDecoder->conceal.initialized)
        return 0;
    if (hDecoder->object_type != LC)
        return 0;
    if (hDecoder->adts_header_present || hDecoder->adif_header_present ||
        hDecoder->latm_header_present)
        return 0;
    if (hDecoder->pce_set || hDecoder->channelConfiguration == 0)
        return 0;
    if (!(rate == 44100 || rate == 48000))
        return 0;
    if (hDecoder->sbr_present_flag == 1 || hDecoder->forceUpSampling == 1)
        return 0;
#if (defined(PS_DEC) || defined(DRM_PS))
    if (hDecoder->ps_used_global)
        return 0;
#endif
    if (hDecoder->downMatrix)
        return 0;
    if (hDecoder->config.outputFormat != FAAD_FMT_16BIT &&
        hDecoder->config.outputFormat != FAAD_FMT_FLOAT)
        return 0;
    /* a configuration change after Init2 is out of scope */
    if (hDecoder->conceal.output_format != hDecoder->config.outputFormat)
        return 0;
    if (hDecoder->channelConfiguration != hDecoder->conceal.channel_configuration)
        return 0;
    if (hDecoder->fr_channels != 1 && hDecoder->fr_channels != 2)
        return 0;
    return 1;
}

void faad_conceal_commit_au(NeAACDecStruct *hDecoder)
{
    conceal_state *st;
    uint8_t channels;
    uint8_t ch;

    if (hDecoder == NULL)
        return;
    st = &hDecoder->conceal;

    if (!st->initialized)
        return;

    channels = hDecoder->fr_channels;
    if (st->unsupported || channels == 0 || channels > CONCEAL_MAX_CHANNELS ||
        !conceal_scope_ok(hDecoder))
    {
        conceal_invalidate_cache(hDecoder);
        return;
    }

    if ((st->pending_mask & (uint8_t)((1u << channels) - 1)) !=
        (uint8_t)((1u << channels) - 1))
    {
        conceal_invalidate_cache(hDecoder);
        return;
    }

    st->cache.channels = channels;
    for (ch = 0; ch < channels; ch++)
    {
        memcpy(st->cache.spec[ch], st->pending.spec[ch],
               CONCEAL_SPEC_LEN * sizeof(real_t));
        st->cache.window_sequence[ch] = st->pending.window_sequence[ch];
        st->cache.window_shape[ch] = st->pending.window_shape[ch];
    }
    st->cache.valid = 1;
    st->good = 1;
    st->lost = 0;
    st->pending_mask = 0;
    st->unsupported = 0;

    /* the next concealment continues from the sequence this AU actually
       synthesised with */
    for (ch = 0; ch < channels; ch++)
        st->last_window_sequence[ch] = st->cache.window_sequence[ch];
}

/* ------------------------------------------------------------------------- */
/* concealment synthesis                                                     */
/* ------------------------------------------------------------------------- */

/* Window sequence used for the synthesis following a concealment of an AU
   whose cached sequence is `previous`:
     ONLY_LONG   -> ONLY_LONG
     LONG_START  -> EIGHT_SHORT
     EIGHT_SHORT -> EIGHT_SHORT
     LONG_STOP   -> ONLY_LONG
*/
static uint8_t conceal_advance_sequence(uint8_t previous)
{
    if (previous == LONG_START_SEQUENCE || previous == EIGHT_SHORT_SEQUENCE)
        return EIGHT_SHORT_SEQUENCE;
    return ONLY_LONG_SEQUENCE;
}

/* Long-window coefficient layouts cannot be reinterpreted as short-window
   spectra and vice versa; the spectrum then has to be zero. */
static int conceal_geometry_matches(uint8_t cached, uint8_t synthesis)
{
    return (cached == EIGHT_SHORT_SEQUENCE) ==
           (synthesis == EIGHT_SHORT_SEQUENCE);
}

static void conceal_build_spectrum_one(conceal_state *st, uint8_t ch,
                                       uint8_t synthesis_seq, uint32_t lost)
{

    /* `lost` is the 1-based index k of this missing AU, so the k-th
       consecutive loss scales the cached spectrum by 0.75^k.  The factor is
       always derived from the untouched cache, never from the previous
       concealed spectrum. */
    if (lost >= CONCEAL_SILENT_AFTER ||
        !conceal_geometry_matches(st->cache.window_sequence[ch], synthesis_seq))
    {
        memset(st->scratch[ch], 0, CONCEAL_SPEC_LEN * sizeof(real_t));
    }
    else
    {
        /* scale = 0.75^lost, computed without libm so the fixed-point build
           (which redefines lrintf through fixed.h) is unaffected */
        real_t scale = REAL_CONST(1.0);
        uint32_t n;
        uint16_t i;

        for (n = 0; n < lost; n++)
            scale = scale * REAL_CONST(CONCEAL_DECAY);
        for (i = 0; i < CONCEAL_SPEC_LEN; i++)
            st->scratch[ch][i] = st->cache.spec[ch][i] * scale;
    }
}

static void conceal_fill_info(NeAACDecStruct *hDecoder, NeAACDecFrameInfo *hInfo,
                              uint8_t channels)
{
    memset(hInfo, 0, sizeof(*hInfo));
    hInfo->bytesconsumed = 0;
    hInfo->error = 0;
    hInfo->samples = (unsigned long)CONCEAL_SPEC_LEN * channels;
    hInfo->channels = channels;
    hInfo->samplerate = get_sample_rate(hDecoder->sf_index);
    hInfo->object_type = hDecoder->object_type;
    hInfo->sbr = NO_SBR;
    hInfo->header_type = RAW;

    /* channel layout mirrors create_channel_config() for the active setup */
    if (channels == 1)
    {
        hInfo->num_front_channels = 1;
        hInfo->channel_position[0] = FRONT_CHANNEL_CENTER;
    }
    else
    {
        hInfo->num_front_channels = 2;
        hInfo->channel_position[0] = FRONT_CHANNEL_LEFT;
        hInfo->channel_position[1] = FRONT_CHANNEL_RIGHT;
    }
}

static unsigned long conceal_required_buffer_size(NeAACDecStruct *hDecoder,
                                                  uint8_t channels)
{
    uint8_t fmt = hDecoder->config.outputFormat;
    unsigned long bytes;

    switch (fmt)
    {
    case FAAD_FMT_16BIT: bytes = sizeof(int16_t); break;
    case FAAD_FMT_24BIT:
    case FAAD_FMT_32BIT: bytes = sizeof(int32_t); break;
    case FAAD_FMT_FLOAT: bytes = sizeof(float32_t); break;
    case FAAD_FMT_DOUBLE: bytes = sizeof(double); break;
    default: return 0;
    }
    return (unsigned long)hDecoder->frameLength * channels * bytes;
}

/* Run the real inverse filter bank per channel and convert the result to PCM.
   All state changes happen here, after every validation has passed. */
static void *conceal_synthesise(NeAACDecStruct *hDecoder, uint8_t channels,
                                void *sample_buffer)
{
    conceal_state *st = &hDecoder->conceal;
    uint8_t ch;

    /* k = 1 for the first missing AU of this run */
    if (st->lost < 0xFFFFFFFFu)
        st->lost++;

    /* The spectrum is built from the untouched cache with a single 0.75^k
       factor; it is never taken from an already attenuated copy. */
    for (ch = 0; ch < channels; ch++)
    {
        uint8_t synthesis = conceal_advance_sequence(st->last_window_sequence[ch]);

        conceal_build_spectrum_one(st, ch, synthesis, st->lost);

        ifilter_bank(hDecoder->fb, synthesis, st->cache.window_shape[ch],
                     hDecoder->window_shape_prev[ch], st->scratch[ch],
                     hDecoder->time_out[ch], hDecoder->fb_intermed[ch],
                     hDecoder->object_type, hDecoder->frameLength);

        /* next synthesis continues from the sequence just used */
        hDecoder->window_shape_prev[ch] = st->cache.window_shape[ch];
        st->last_window_sequence[ch] = synthesis;
    }

    sample_buffer = output_to_PCM(hDecoder, hDecoder->time_out, sample_buffer,
                                  channels, hDecoder->frameLength,
                                  hDecoder->config.outputFormat);

    hDecoder->postSeekResetFlag = 0;
    hDecoder->frame++;

    return sample_buffer;
}

static void *conceal_entry(NeAACDecStruct *hDecoder, NeAACDecFrameInfo *hInfo,
                           void **sample_buffer, unsigned long sample_buffer_size,
                           uint8_t use_external)
{
    conceal_state *st;
    unsigned long required;
    uint8_t channels;

    if (hInfo == NULL)
        return NULL;
    if (hDecoder == NULL)
    {
        hInfo->error = CONCEAL_ERR_UNAVAILABLE;
        hInfo->samples = 0;
        return NULL;
    }

    st = &hDecoder->conceal;

    if (!st->initialized || !st->cache.valid || !st->good ||
        !conceal_scope_ok(hDecoder))
    {
        memset(hInfo, 0, sizeof(*hInfo));
        hInfo->error = CONCEAL_ERR_UNAVAILABLE;
        hInfo->samples = 0;
        return NULL;
    }

    channels = st->cache.channels;
    if (channels == 0 || channels > CONCEAL_MAX_CHANNELS ||
        hDecoder->time_out[0] == NULL || hDecoder->fb_intermed[0] == NULL ||
        (channels > 1 && (hDecoder->time_out[1] == NULL ||
                          hDecoder->fb_intermed[1] == NULL)))
    {
        memset(hInfo, 0, sizeof(*hInfo));
        hInfo->error = CONCEAL_ERR_UNAVAILABLE;
        hInfo->samples = 0;
        return NULL;
    }

    required = conceal_required_buffer_size(hDecoder, channels);
    if (required == 0)
    {
        memset(hInfo, 0, sizeof(*hInfo));
        hInfo->error = CONCEAL_ERR_UNAVAILABLE;
        hInfo->samples = 0;
        return NULL;
    }

    if (use_external)
    {
        if (sample_buffer == NULL || *sample_buffer == NULL ||
            sample_buffer_size < required)
        {
            /* reject before any state changes: cache, counters, overlap, RNG
               and frame counter stay untouched */
            memset(hInfo, 0, sizeof(*hInfo));
            hInfo->error = CONCEAL_ERR_BUFFER;
            hInfo->samples = 0;
            return NULL;
        }
    }
    else
    {
        if (st->conceal_sample_buffer_size != required)
        {
            if (st->conceal_sample_buffer)
                faad_free(st->conceal_sample_buffer);
            st->conceal_sample_buffer = NULL;
            st->conceal_sample_buffer = faad_malloc(required);
            st->conceal_sample_buffer_size =
                st->conceal_sample_buffer ? required : 0;
        }
        if (st->conceal_sample_buffer == NULL)
        {
            memset(hInfo, 0, sizeof(*hInfo));
            hInfo->error = CONCEAL_ERR_UNAVAILABLE;
            hInfo->samples = 0;
            return NULL;
        }
    }

    conceal_fill_info(hDecoder, hInfo, channels);

    if (use_external)
        return conceal_synthesise(hDecoder, channels, *sample_buffer);
    return conceal_synthesise(hDecoder, channels, st->conceal_sample_buffer);
}

void *NeAACDecConceal(NeAACDecHandle hpDecoder, NeAACDecFrameInfo *hInfo)
{
    return conceal_entry((NeAACDecStruct*)hpDecoder, hInfo, NULL, 0, 0);
}

void *NeAACDecConceal2(NeAACDecHandle hpDecoder, NeAACDecFrameInfo *hInfo,
                       void **sample_buffer, unsigned long sample_buffer_size)
{
    return conceal_entry((NeAACDecStruct*)hpDecoder, hInfo, sample_buffer,
                         sample_buffer_size, 1);
}

uint8_t faad_conceal_test_peek(NeAACDecStruct *hDecoder, uint8_t channel,
                               const real_t **spectrum, uint8_t *window_sequence,
                               uint8_t *window_shape, uint32_t *lost,
                               uint8_t *next_sequence,
                               const real_t **overlap, uint8_t *window_shape_prev,
                               uint32_t *frame_counter,
                               uint32_t *rng1, uint32_t *rng2)
{
    conceal_state *st;

    if (hDecoder == NULL)
        return 0;
    st = &hDecoder->conceal;
    if (!st->cache.valid || channel >= st->cache.channels)
        return 0;

    if (spectrum)
        *spectrum = st->cache.spec[channel];
    if (window_sequence)
        *window_sequence = st->cache.window_sequence[channel];
    if (window_shape)
        *window_shape = st->cache.window_shape[channel];
    if (lost)
        *lost = st->lost;
    if (next_sequence)
        *next_sequence = conceal_advance_sequence(st->last_window_sequence[channel]);
    if (overlap)
        *overlap = hDecoder->fb_intermed[channel];
    if (window_shape_prev)
        *window_shape_prev = hDecoder->window_shape_prev[channel];
    if (frame_counter)
        *frame_counter = hDecoder->frame;
    if (rng1)
        *rng1 = hDecoder->__r1;
    if (rng2)
        *rng2 = hDecoder->__r2;
    return 1;
}

void *task_test_fb_create(uint16_t frame_len)
{
    return (void*)filter_bank_init(frame_len);
}

void task_test_fb_run(void *fb_state, uint8_t window_sequence,
                      uint8_t window_shape, uint8_t window_shape_prev,
                      const real_t *spectrum, real_t *time_out,
                      real_t *overlap, uint16_t frame_len)
{
    if (fb_state == NULL)
        return;
    ifilter_bank((fb_info*)fb_state, window_sequence, window_shape,
                 window_shape_prev, (real_t*)spectrum, time_out, overlap,
                 LC, frame_len);
}

void task_test_fb_end(void *fb_state)
{
    filter_bank_end((fb_info*)fb_state);
}
