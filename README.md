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


## Concatenated video sequences

A single stream may contain several MPEG-1 video sequences back to back. Each
sequence carries its own sequence header and ends with a `sequence_end` code,
and the next sequence starts with an I picture. The resolution, pixel aspect
ratio, frame rate and quantizer matrices can change between sequences.

PL_MPEG decodes such a stream as one continuous presentation. No external
concatenating player is needed: `plm_video_decode()`, `plm_decode_video()` and
the video decode callback all return the same ordered frames.

- Frames of the previous sequence that were queued for display (including the
  I/P/B reordering) are delivered once more with their original dimensions and
  plane contents before the new sequence starts.
- The new configuration is installed only when its first picture is decoded.
  The three frame slots and macroblock planes are reallocated for the new
  size, and prediction references never carry over from the previous sequence.
- `frame->time` accumulates with each sequence's own frame period and is never
  reset at a boundary. `plm_get_time()`/`plm_video_get_time()` report the time
  of the next frame to display.
- `plm_get_width()`, `plm_get_height()`, `plm_get_framerate()` and
  `plm_get_pixel_aspect_ratio()` (and their `plm_video_*` counterparts) report
  the parameters of the sequence whose frame was most recently delivered.
- A frame returned by a decode call is borrowed until the next call, as before.
  A repeated sequence header with identical parameters (without a
  `sequence_end`) stays in the same sequence: it adds no picture and keeps the
  existing references.
- Data may be fed into a capacity buffer incrementally. A sequence header (or
  its quantizer matrices) that has not fully arrived makes the decode call
  return `NULL` until more bytes are written; no half header is consumed.
- `plm_rewind()` restarts from the first sequence at time zero. Seeking and the
  audio format are unchanged.

Because the frame dimensions can change mid-stream, applications should size
output buffers and textures from each delivered frame rather than caching the
dimensions read once at startup.


## Limitations

- no error reporting. PL_MPEG will silently ignore any invalid data.
- the pts (presentation time stamp) for packets in the MPEG-PS container is
ignored. This may cause sync issues with some files.
- bugs, probably.
