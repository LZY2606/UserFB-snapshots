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


## Consecutive video sequences

The video decoder accepts streams that concatenate several MPEG-1 video
sequences. Each sequence may have its own sequence header with different
dimensions, pixel aspect ratio, framerate and quantizer matrices, and should
be terminated by a sequence end code. This makes it possible to play a
playlist of separately encoded segments as one continuous stream without an
external splicing tool.

When one sequence ends and the next begins:

- a still unpublished reference frame of the ending sequence is delivered
  once, in display order and with its original size and plane content,
  before the first frame of the new sequence,
- the new sequence starts with a fresh set of reference frames; no motion
  prediction crosses the boundary,
- `frame->time` keeps accumulating with each segment's actual frame period -
  it is neither reset nor recomputed with the new framerate,
- `plm_get_width()`, `plm_get_height()`, `plm_get_framerate()` and
  `plm_get_pixel_aspect_ratio()` (and their `plm_video_*` counterparts)
  always describe the frame that is currently being delivered,
- a repeated sequence header with identical parameters (and no sequence
  end) is part of the current sequence; it may update the quantizer
  matrices but does not produce a frame or clear the reference frames.

This works the same way for raw video streams (`plm_video_decode()`), for
MPEG-PS containers (`plm_decode_video()`) and for the video decode callback
used with `plm_decode()`. When feeding a `plm_buffer` incrementally, the
decoder simply returns `NULL` until a sequence header (including its
quantizer matrices) has fully arrived.


## Limitations

- no error reporting. PL_MPEG will silently ignore any invalid data.
- the pts (presentation time stamp) for packets in the MPEG-PS container is
ignored. This may cause sync issues with some files.
- bugs, probably.
