/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "channel_route.h"

#include <stdio.h>
#include <string.h>

static int min_int(int a, int b) {
    return (a < b) ? a : b;
}

// A layout is only a usable candidate when its channel count is in range
// and every channel is a valid, non-Invalid, non-duplicated role.
static bool layout_is_supported(const struct SoundIoChannelLayout *layout) {
    if (layout->channel_count < 1 || layout->channel_count > SOUNDIO_MAX_CHANNELS)
        return false;
    for (int i = 0; i < layout->channel_count; i += 1) {
        enum SoundIoChannelId id = layout->channels[i];
        if (id <= SoundIoChannelIdInvalid || id > SoundIoChannelIdAux15)
            return false;
        for (int j = 0; j < i; j += 1) {
            if (layout->channels[j] == id)
                return false;
        }
    }
    return true;
}

// Same semantics as soundio_channel_layout_equal.
static bool layouts_equal(const struct SoundIoChannelLayout *a,
        const struct SoundIoChannelLayout *b)
{
    if (a->channel_count != b->channel_count)
        return false;
    for (int i = 0; i < a->channel_count; i += 1) {
        if (a->channels[i] != b->channels[i])
            return false;
    }
    return true;
}

// True when both layouts carry exactly the same set of channel roles,
// regardless of order. Roles are unique per layout (enforced by
// layout_is_supported), so set containment in one direction is enough.
static bool layouts_same_role_set(const struct SoundIoChannelLayout *a,
        const struct SoundIoChannelLayout *b)
{
    if (a->channel_count != b->channel_count)
        return false;
    for (int i = 0; i < a->channel_count; i += 1) {
        bool found = false;
        for (int j = 0; j < b->channel_count; j += 1) {
            if (b->channels[j] == a->channels[i]) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

bool channel_route_negotiate(struct ChannelRoute *route,
        const struct SoundIoChannelLayout *out_layouts, int out_layout_count,
        const struct SoundIoChannelLayout *in_layouts, int in_layout_count)
{
    for (int o = 0; o < out_layout_count; o += 1) {
        const struct SoundIoChannelLayout *out_layout = &out_layouts[o];
        if (!layout_is_supported(out_layout))
            continue;

        // Prefer an input layout with the exact same channel order.
        const struct SoundIoChannelLayout *in_layout = NULL;
        for (int i = 0; i < in_layout_count; i += 1) {
            if (!layout_is_supported(&in_layouts[i]))
                continue;
            if (layouts_equal(out_layout, &in_layouts[i])) {
                in_layout = &in_layouts[i];
                break;
            }
        }

        // Fall back to the first input layout with the same role set.
        if (!in_layout) {
            for (int i = 0; i < in_layout_count; i += 1) {
                if (!layout_is_supported(&in_layouts[i]))
                    continue;
                if (layouts_same_role_set(out_layout, &in_layouts[i])) {
                    in_layout = &in_layouts[i];
                    break;
                }
            }
        }

        if (!in_layout)
            continue;

        route->in_layout = in_layout;
        route->out_layout = out_layout;
        for (int out_ch = 0; out_ch < out_layout->channel_count; out_ch += 1) {
            for (int in_ch = 0; in_ch < in_layout->channel_count; in_ch += 1) {
                if (in_layout->channels[in_ch] == out_layout->channels[out_ch]) {
                    route->output_to_input[out_ch] = in_ch;
                    break;
                }
            }
        }
        return true;
    }
    return false;
}

// Copy frame_count frames from areas into interleaved storage, keeping the
// channel order of the layout. The caller's area descriptors are not
// modified; padding between samples (step > bytes_per_sample) is untouched.
static void areas_to_interleaved(char *dst, const struct SoundIoChannelArea *areas,
        int channel_count, int frame_count, int bytes_per_sample)
{
    for (int ch = 0; ch < channel_count; ch += 1) {
        const char *src = areas[ch].ptr;
        char *out = dst + ch * bytes_per_sample;
        for (int frame = 0; frame < frame_count; frame += 1) {
            memcpy(out, src, bytes_per_sample);
            src += areas[ch].step;
            out += channel_count * bytes_per_sample;
        }
    }
}

// Copy frame_count frames from interleaved storage (input channel order)
// into the output areas, routing each output channel to the input channel
// with the same role.
static void interleaved_to_areas(const struct ChannelRoute *route, const char *src,
        const struct SoundIoChannelArea *areas, int frame_count, int bytes_per_sample)
{
    int in_channel_count = route->in_layout->channel_count;
    int out_channel_count = route->out_layout->channel_count;
    for (int out_ch = 0; out_ch < out_channel_count; out_ch += 1) {
        int in_ch = route->output_to_input[out_ch];
        char *dst = areas[out_ch].ptr;
        const char *in = src + in_ch * bytes_per_sample;
        for (int frame = 0; frame < frame_count; frame += 1) {
            memcpy(dst, in, bytes_per_sample);
            dst += areas[out_ch].step;
            in += in_channel_count * bytes_per_sample;
        }
    }
}

static void silence_areas(const struct SoundIoChannelArea *areas,
        int channel_count, int frame_count, int bytes_per_sample)
{
    for (int ch = 0; ch < channel_count; ch += 1) {
        char *dst = areas[ch].ptr;
        for (int frame = 0; frame < frame_count; frame += 1) {
            memset(dst, 0, bytes_per_sample);
            dst += areas[ch].step;
        }
    }
}

int channel_route_pump_read(struct ChannelRoutePipe *pipe,
        char *buf, int free_frames,
        int frame_count_min, int frame_count_max)
{
    if (frame_count_min > free_frames)
        pipe->panic(pipe->userdata, 0, "ring buffer overflow");

    int write_frames = min_int(free_frames, frame_count_max);
    int frames_left = write_frames;
    int frames_done = 0;
    char *write_ptr = buf;

    while (frames_left > 0) {
        struct SoundIoChannelArea *areas;
        int frame_count = frames_left;

        int err = pipe->begin_read(pipe->userdata, &areas, &frame_count);
        if (err)
            pipe->panic(pipe->userdata, err, "begin read error");

        if (frame_count <= 0)
            break;

        if (!areas) {
            // Due to an overflow there is a hole. Fill the buffer with
            // silence for the size of the hole; it still counts as frames.
            memset(write_ptr, 0, frame_count * pipe->bytes_per_frame);
            fprintf(stderr, "Dropped %d frames due to internal overflow\n", frame_count);
        } else {
            areas_to_interleaved(write_ptr, areas,
                    pipe->route.in_layout->channel_count,
                    frame_count, pipe->bytes_per_sample);
        }

        if ((err = pipe->end_read(pipe->userdata)))
            pipe->panic(pipe->userdata, err, "end read error");

        write_ptr += frame_count * pipe->bytes_per_frame;
        frames_left -= frame_count;
        frames_done += frame_count;
    }

    return frames_done;
}

int channel_route_pump_write(struct ChannelRoutePipe *pipe,
        const char *buf, int fill_frames,
        int frame_count_min, int frame_count_max)
{
    int out_channel_count = pipe->route.out_layout->channel_count;

    if (frame_count_min > fill_frames) {
        // The buffer does not have enough data; fill with silence. This
        // consumes nothing from buf.
        int frames_left = frame_count_min;
        while (frames_left > 0) {
            struct SoundIoChannelArea *areas;
            int frame_count = frames_left;

            int err = pipe->begin_write(pipe->userdata, &areas, &frame_count);
            if (err)
                pipe->panic(pipe->userdata, err, "begin write error");

            if (frame_count <= 0)
                break;

            silence_areas(areas, out_channel_count, frame_count, pipe->bytes_per_sample);

            if ((err = pipe->end_write(pipe->userdata)))
                pipe->panic(pipe->userdata, err, "end write error");

            frames_left -= frame_count;
        }
        return 0;
    }

    int read_count = min_int(frame_count_max, fill_frames);
    int frames_left = read_count;
    int frames_done = 0;
    const char *read_ptr = buf;

    while (frames_left > 0) {
        struct SoundIoChannelArea *areas;
        int frame_count = frames_left;

        int err = pipe->begin_write(pipe->userdata, &areas, &frame_count);
        if (err)
            pipe->panic(pipe->userdata, err, "begin write error");

        if (frame_count <= 0)
            break;

        interleaved_to_areas(&pipe->route, read_ptr, areas,
                frame_count, pipe->bytes_per_sample);

        if ((err = pipe->end_write(pipe->userdata)))
            pipe->panic(pipe->userdata, err, "end write error");

        read_ptr += frame_count * pipe->bytes_per_frame;
        frames_left -= frame_count;
        frames_done += frame_count;
    }

    return frames_done;
}
