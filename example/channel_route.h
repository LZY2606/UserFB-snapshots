/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef SOUNDIO_EXAMPLE_CHANNEL_ROUTE_H
#define SOUNDIO_EXAMPLE_CHANNEL_ROUTE_H

#include <soundio/soundio.h>

/*
 * Channel routing for the sio_microphone example.
 *
 * This module is deliberately free of any libsoundio backend calls so that
 * it can also be exercised by an offline test program (see
 * test/channel_route/). It does two things:
 *
 *  1. Route negotiation: pick one input layout and one output layout from
 *     the devices' layout lists. Output layouts are considered in the order
 *     given (the caller may sort them by channel count first). For each
 *     output layout, an input layout with the exact same channel order is
 *     preferred; if none exists, the first input layout with the same set
 *     of channel roles (in any order) is used, and an output_to_input index
 *     map is built so every output channel can be fed from the input
 *     channel with the same role.
 *
 *  2. Sample moving: copy frames between the stream areas and the
 *     intermediary buffer. The buffer always stores frames interleaved in
 *     the input device's native channel order; on playback each output
 *     channel pulls the sample of the matching input role. Samples are
 *     moved as raw bytes; no mixing, resampling, or value conversion.
 */

struct ChannelRoute {
    // Points into the input layout list passed to channel_route_negotiate.
    const struct SoundIoChannelLayout *in_layout;
    // Points into the output layout list passed to channel_route_negotiate.
    const struct SoundIoChannelLayout *out_layout;
    // For each output channel index, the input channel index carrying the
    // same role. Only the first out_layout->channel_count entries are used.
    int output_to_input[SOUNDIO_MAX_CHANNELS];
};

// Negotiate a route between the given layout lists. On success returns true
// and fills route. On failure (no compatible pair) returns false and leaves
// route untouched. Layouts with an out-of-range channel count, invalid or
// duplicate channel roles are skipped as candidates.
bool channel_route_negotiate(struct ChannelRoute *route,
        const struct SoundIoChannelLayout *out_layouts, int out_layout_count,
        const struct SoundIoChannelLayout *in_layouts, int in_layout_count);

// Stream and error hooks used by the pump functions below. All hooks are
// called with the userdata pointer. None of the hooks may allocate memory;
// everything is set up before the streams are opened.
struct ChannelRoutePipe {
    struct ChannelRoute route;
    int bytes_per_sample;
    int bytes_per_frame; // bytes_per_sample * channel count (same both sides)
    void *userdata;

    // Mirrors soundio_instream_begin_read / soundio_instream_end_read.
    // begin_read may set *areas to NULL to signal a hole (dropped frames).
    int (*begin_read)(void *userdata, struct SoundIoChannelArea **areas, int *frame_count);
    int (*end_read)(void *userdata);

    // Mirrors soundio_outstream_begin_write / soundio_outstream_end_write.
    int (*begin_write)(void *userdata, struct SoundIoChannelArea **areas, int *frame_count);
    int (*end_write)(void *userdata);

    // Fatal error handler. err is a SoundIoError code, or 0 when msg fully
    // describes the problem. Must not return.
    void (*panic)(void *userdata, int err, const char *msg);
};

// Pump input frames into buf, interleaved in the input device's native
// channel order. buf must hold at least free_frames frames. Hole chunks
// (areas == NULL) are filled with silence and still count as frames.
// Returns the number of frames actually written to buf; the caller advances
// its ring buffer write pointer by exactly that many frames.
int channel_route_pump_read(struct ChannelRoutePipe *pipe,
        char *buf, int free_frames,
        int frame_count_min, int frame_count_max);

// Pump frames from buf to the output, remapping channels according to the
// negotiated route. If frame_count_min exceeds fill_frames, frame_count_min
// frames of silence are written to the output instead and no buf data is
// consumed. Returns the number of frames consumed from buf; the caller
// advances its ring buffer read pointer by exactly that many frames.
int channel_route_pump_write(struct ChannelRoutePipe *pipe,
        const char *buf, int fill_frames,
        int frame_count_min, int frame_count_max);

#endif
