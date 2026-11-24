# PL_MPEG - MPEG1 Video decoder, MP2 Audio decoder, MPEG-PS demuxer

Single-file MIT licensed library for C/C++

See [pl_mpeg.h](https://github.com/phoboslab/pl_mpeg/blob/master/pl_mpeg.h) for
the documentation.


## Why?

This is meant as a simple way to get video playback into your app or game. Other
solutions, such as ffmpeg require huge libraries and a lot of glue code.

MPEG1 is an old and inefficient codec, but it's still good enough for many use
cases. All patents related to MPEG1 and MP2 have expired, so it's completely
free now.

This library does not make use of any SIMD instructions, but because of
the relative simplicity of the codec it still manages to decode 4k60fps video
on a single CPU core (on my i7-6700k at least).


## Building

A Makefile to build the example applications is included. 

- `pl_mpeg_extract_frames`: a command line tool that dumps all frames into BMPs
- `pl_mpeg_player_gl`: a video player using SDL2 and OpenGL
- `pl_mpeg_player_sdl`: a video player using SDL2 and it's built-in 2d renderer

The players require the SDL2 library to be installed.

```shell
make # build all
make extract # only build pl_mpeg_extract_frames
make player_gl # only build pl_mpeg_player_gl
make player_sdl # only build pl_mpeg_player_sdl
```


## Encoding for PL_MPEG

Most [MPEG-PS](https://en.wikipedia.org/wiki/MPEG_program_stream) (`.mpg`) files
containing MPEG1 Video ("mpeg1") and MPEG1 Audio Layer II ("mp2") streams should
work with PL_MPEG. Note that `.mpg` files can also contain MPEG2 Video, which is
not supported by this library.

You can encode video in a suitable format using ffmpeg:

```
ffmpeg -i input.mp4 -c:v mpeg1video -q:v 0 -c:a libtwolame -b:a 224k -format mpeg output.mpg
```

`-q:v` sets a fixed video quality with a variable bitrate, where `0` is the 
highest. You may use `-b:v` to set a fixed bitrate instead; e.g. 
`-b:v 2000k` for 2000 kbit/s. Please refer to the 
[ffmpeg documentation](http://ffmpeg.org/ffmpeg.html#Options) for more details.

If you just want to quickly test the library, try this file:

https://phoboslab.org/files/bjork-all-is-full-of-love.mpg


## Limitations

- no error reporting. PL_MPEG will silently ignore any invalid data.
- the pts (presentation time stamp) for packets in the MPEG-PS container is
ignored. This may cause sync issues with some files.
- bugs, probably.


## Multi-stream audio mixing (`plm_audio_mix_t`)

For audio-only MPEG-1 Program Streams that carry several MP2 audio tracks
(PES stream ids `0xC0`–`0xC3`, e.g. a main voice and a commentary),
`plm_audio_mix_t` demuxes the selected tracks, decodes each with its own
MP2 decoder and resamples everything onto one 48 kHz stereo timeline. Frame
*n* is presented at `origin90k/90000 + n/48000` seconds; source samples are
mapped with integer rational math and linearly interpolated. Each track
keeps at most two decoded PCM frames, so memory does not grow with the
stream length.

The mixer reads from a normal `plm_buffer_t`, so bytes can be appended
incrementally. When the appended bytes do not yet prove every selected
track's contribution for the next frame, `plm_audio_mix_read()` returns the
provable prefix (possibly 0) without inserting fake silence; use
`plm_audio_mix_has_ended()` to tell genuine exhaustion from a temporary
shortage of bytes.

### Buffer ownership

`plm_audio_mix_create(buffer, destroy_buffer, ...)` never copies the
compressed stream. If `destroy_buffer` is non-zero the mixer takes ownership
and calls `plm_buffer_destroy()` on the buffer when the mixer is destroyed;
pass zero to keep ownership on the caller side, in which case the caller
must destroy the buffer itself after the mixer is gone. The same rule
applies to the underlying storage of memory/appending buffers.

```c
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

void mix_program_stream(const uint8_t *ps_bytes, size_t ps_length) {
    // Caller-owned appending buffer: destroy_buffer == 0 means the mixer
    // must not free it. Use 1 to transfer ownership instead.
    plm_buffer_t *buffer = plm_buffer_create_for_appending(16 * 1024);
    plm_buffer_write(buffer, (uint8_t *)ps_bytes, ps_length);
    plm_buffer_signal_end(buffer);

    // Select streams 0xC0 and 0xC1 (bits 0 and 1); timeline origin at 0.
    plm_audio_mix_t *mix =
        plm_audio_mix_create(buffer, 0, 0x3 /*stream_mask*/, 0 /*origin90k*/);

    // Optional per-track gains, indices map to 0xC0..0xC3. All four values
    // must be finite; an invalid call returns 0 and leaves the gains intact.
    float gains[4] = {1.0f, 0.7f, 1.0f, 1.0f};
    plm_audio_mix_set_gains(mix, gains);

    float stereo[256]; // interleaved L/R, 48 kHz
    for (;;) {
        size_t frames = plm_audio_mix_read(mix, stereo, 128);
        if (frames == 0) {
            if (plm_audio_mix_has_ended(mix)) {
                break; // real end of stream; otherwise just wait for bytes
            }
            continue;
        }
        uint64_t position = plm_audio_mix_get_position(mix); // output frames
        /* hand stereo[0 .. frames*2) to the audio device here */
        (void)position;
    }

    plm_audio_mix_destroy(mix);
    plm_buffer_destroy(buffer); // caller still owns the buffer
}
```

For streaming use, call `plm_buffer_write()` repeatedly (between reads) and
`plm_buffer_signal_end()` only once the whole stream has been appended.
`plm_audio_mix_rewind()` rewinds an addressable source and clears all
decoding/interpolation state, so replaying yields the same output as a
freshly created mixer (including the default unit gains).

