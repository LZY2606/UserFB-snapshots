////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2025 David Bryant                  //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack_plan.c
//
// This module implements the per-channel hybrid bitrate plan. A plan is a
// time ordered list of sample positions with an independent Q8 bits/sample
// budget for every interlaced channel at each position. The plan is accepted
// once (after WavpackSetConfiguration64() and before WavpackPackInit()), fully
// copied into the context, and then drives block boundaries and the entropy
// coder while packing proceeds normally through WavpackPackSamples().

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

#define PLAN_MIN_BUDGET_Q8       576     // 2.25 bits/sample
#define PLAN_MAX_BUDGET_Q8       6144    // 24.0 bits/sample

// Release every resource owned by an accepted plan. Safe to call when no plan
// is installed and from WavpackCloseFile().

void WavpackFreeBitratePlan (WavpackContext *wpc)
{
    if (wpc->plan_points) {
        free (wpc->plan_points);
        wpc->plan_points = NULL;
    }

    if (wpc->plan_budgets) {
        free (wpc->plan_budgets);
        wpc->plan_budgets = NULL;
    }

    wpc->plan_count = wpc->plan_pos = 0;
}

// Install (or clear) a hybrid bitrate plan. Everything is validated against a
// temporary copy first so that a rejected call leaves the previous plan and the
// encoding state completely untouched. Returns 1 on acceptance and 0 otherwise.

int WavpackSetHybridBitratePlan (WavpackContext *wpc, const WavpackHybridBitratePoint *points, uint32_t count)
{
    int64_t *new_points = NULL;
    uint16_t *new_budgets = NULL;
    int num_channels, i, p;

    if (!wpc || !wpc->num_streams || !wpc->streams) {
        if (wpc)
            strcpy (wpc->error_message, "bitrate plan requires a successful configuration first!");

        return FALSE;
    }

    if (wpc->packing_started) {
        strcpy (wpc->error_message, "bitrate plan must be set before WavpackPackInit()!");
        return FALSE;
    }

    num_channels = wpc->config.num_channels;

    if (!(wpc->config.flags & CONFIG_HYBRID_FLAG) || (wpc->streams [0]->wphdr.flags & DSD_FLAG) ||
        wpc->config.float_norm_exp || num_channels < 1 || num_channels > 8 ||
        !((wpc->config.bits_per_sample == 16 && wpc->config.bytes_per_sample == 2) ||
          (wpc->config.bits_per_sample == 24 && wpc->config.bytes_per_sample == 3))) {
            strcpy (wpc->error_message, "bitrate plan requires hybrid 1-8 channel 16/24-bit integer PCM!");
            return FALSE;
    }

    // a count of zero simply clears any previously accepted plan

    if (!count) {
        WavpackFreeBitratePlan (wpc);
        return TRUE;
    }

    if (!points) {
        strcpy (wpc->error_message, "bitrate plan points cannot be NULL!");
        return FALSE;
    }

    if (points [0].sample_index != 0) {
        strcpy (wpc->error_message, "bitrate plan must start at sample index 0!");
        return FALSE;
    }

    if (wpc->total_samples >= 0 && points [count - 1].sample_index >= wpc->total_samples) {
        strcpy (wpc->error_message, "bitrate plan points must precede the total sample count!");
        return FALSE;
    }

    for (p = 0; p < (int) count; p++) {
        if (p && points [p].sample_index <= points [p - 1].sample_index) {
            strcpy (wpc->error_message, "bitrate plan sample indices must be strictly increasing!");
            return FALSE;
        }

        if (points [p].sample_index < 0 || !points [p].bits_per_sample_q8) {
            strcpy (wpc->error_message, "bitrate plan point is invalid!");
            return FALSE;
        }

        for (i = 0; i < num_channels; i++) {
            uint16_t budget = points [p].bits_per_sample_q8 [i];

            if (budget < PLAN_MIN_BUDGET_Q8 || budget > PLAN_MAX_BUDGET_Q8) {
                strcpy (wpc->error_message, "bitrate plan budget out of range (576 to 6144)!");
                return FALSE;
            }
        }
    }

    // the whole plan passed validation, so take a private copy of it all

    new_points = malloc ((size_t) count * sizeof (*new_points));
    new_budgets = malloc ((size_t) count * (size_t) num_channels * sizeof (*new_budgets));

    if (!new_points || !new_budgets) {
        free (new_points);
        free (new_budgets);
        strcpy (wpc->error_message, "can't allocate memory for bitrate plan!");
        return FALSE;
    }

    for (p = 0; p < (int) count; p++) {
        new_points [p] = points [p].sample_index;
        memcpy (new_budgets + (size_t) p * num_channels, points [p].bits_per_sample_q8,
            (size_t) num_channels * sizeof (*new_budgets));
    }

    WavpackFreeBitratePlan (wpc);
    wpc->plan_points = new_points;
    wpc->plan_budgets = new_budgets;
    wpc->plan_count = count;
    wpc->plan_pos = 1;       // point 0 is in effect until the first cut is reached

    // calloc leaves this at zero, but zero is a valid plan point index
    for (i = 0; i < wpc->num_streams; i++)
        wpc->streams [i]->plan_active = -1;

    return TRUE;
}

// Return the sample index of the next plan cut strictly after the given sample
// position, or -1 when the position is at or past the final point. This scans
// the plan directly instead of relying on the running plan_pos because blocks
// can be buffered (and even fill several times) before they are packed.

int64_t WavpackPlanNextPoint (const WavpackContext *wpc)
{
    int64_t position = wpc->streams [0]->sample_index;
    uint32_t p;

    if (!wpc->plan_count)
        return -1;

    for (p = 0; p < wpc->plan_count; p++)
        if (wpc->plan_points [p] > position)
            return wpc->plan_points [p];

    return -1;
}

// Restart the decorrelation / entropy state for a stream at a plan cut. The
// new segment encodes independently of the previous budget segment; this is
// the same reset WavpackPackInit() performs, minus its one-time allocations.

void WavpackResetPlanWordState (WavpackStream *wps)
{
    WavpackContext *wpc = (WavpackContext *) wps->wpc;

    wps->num_terms = 0;
    wps->joint_stereo = 0;
    wps->false_stereo = 0;
    wps->dc.error [0] = wps->dc.error [1] = 0;
    CLEARA (wps->decorr_passes);
    wps->dc.shaping_acc [0] = wps->dc.shaping_acc [1] = 0;
    wps->dc.shaping_delta [0] = wps->dc.shaping_delta [1] = 0;

    if (!(wpc->config.flags & CONFIG_DYNAMIC_SHAPING) && wpc->config.shaping_weight) {
        int32_t weight = (int32_t) floor (wpc->config.shaping_weight * 1024.0 + 0.5);

        if (weight <= -1000)
            weight = -1000;

        wps->dc.shaping_acc [0] = wps->dc.shaping_acc [1] = weight << 16;
    }

    if (!wpc->config.xmode)
        wps->num_passes = 0;
    else if (wpc->config.xmode == 1)
        wps->num_passes = 2;
    else if (wpc->config.xmode == 2)
        wps->num_passes = 4;
    else
        wps->num_passes = 9;

    init_words (wps);
}

// Apply the budget of the plan point that is active at the stream's current
// sample index. Each stereo stream keeps its existing channel pairing but runs
// as two independent channels: joint stereo, cross-channel decorrelation and
// hybrid balance are disabled so one channel's budget can never be moved to
// the other. Returns 0 when no plan is installed.

int WavpackPlanStreamBudget (WavpackStream *wps)
{
    WavpackContext *wpc = (WavpackContext *) wps->wpc;
    int num_channels, stream_index, channel_offset, active, stereo, i;

    if (!wpc->plan_count)
        return FALSE;

    num_channels = wpc->config.num_channels;

    while (wpc->plan_pos < wpc->plan_count &&
           wpc->plan_points [wpc->plan_pos] <= wps->sample_index)
        wpc->plan_pos++;

    active = (int) wpc->plan_pos - 1;

    channel_offset = 0;

    for (stream_index = 0; stream_index < wps->stream_index; stream_index++)
        channel_offset += (wpc->streams [stream_index]->wphdr.flags & MONO_FLAG) ? 1 : 2;

    stereo = !(wps->wphdr.flags & MONO_FLAG);

    for (i = 0; i < (stereo ? 2 : 1); i++)
        wps->plan_budget [i] = wpc->plan_budgets [(size_t) active * num_channels + channel_offset + i];

    // the "bits" field feeds the hybrid entropy coder directly; for a stereo
    // stream it is interpreted exactly like a mono stream's budget per channel

    wps->bits = wps->plan_budget [0];

    // a new segment starts here: strip the stereo coupling and restart the
    // decorrelation / entropy state so that the new budget takes effect cleanly

    if (active != wps->plan_active) {
        if (stereo)
            wps->wphdr.flags &= ~(JOINT_STEREO | CROSS_DECORR | HYBRID_BALANCE);

        WavpackResetPlanWordState (wps);
        wps->plan_active = active;
    }

    return TRUE;
}
