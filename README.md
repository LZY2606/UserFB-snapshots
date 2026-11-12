Mini MP4
==========

[![Build Status](https://travis-ci.org/lieff/minimp4.svg)](https://travis-ci.org/lieff/minimp4)

Easy embeddable MP4 mux/demux library.

## Usage
#### Muxing

Muxing can be done using 3 modes.
Default mode uses one big mdat chunk:

![default](images/mux_mode_default.png?raw=true)

This is most efficient mode, but disadvantage is that we need go back and patch mdat chunk size.
This can be a problem in some cases, for example if stream transfered over network.
To workaround this sequential mode is used:

![default](images/mux_mode_sequential.png?raw=true)

This mode do not make any backwards seek.
And last mode is fragmented aka fMP4.

![default](images/mux_mode_fragmented.png?raw=true)

This mode stores track information first and spreads indexes across all stream, so decoding can start before whole stream available.
This mode is sequential too and usually used by browsers and HLS streaming.

#### Batched fragmentation

Passing `enable_fragmentation = 2` to `MP4E_open()` selects a batched fragment
mode intended for recorders. Samples are first collected, in the exact order
they are submitted, and a single call to `MP4E_flush_fragment()` then emits one
`moof` + `mdat` pair for the whole batch. `MP4E_close()` commits the last
non-empty batch. `enable_fragmentation = 0` and `1` keep their original behavior
and byte output.

```c
MP4E_mux_t *mux = MP4E_open(0, 2, token, write_callback);

/* interleaved audio/video submission, order is preserved in the mdat */
MP4E_put_sample(mux, video_track, v1, sizeof(v1), 3000, MP4E_SAMPLE_RANDOM_ACCESS);
MP4E_put_sample(mux, audio_track, a1, sizeof(a1), 1024, MP4E_SAMPLE_DEFAULT);
MP4E_put_sample(mux, video_track, v2, sizeof(v2), 3000, MP4E_SAMPLE_DEFAULT);
MP4E_put_sample(mux, video_track, slice, sizeof(slice), 0, MP4E_SAMPLE_CONTINUATION);

/* one moof + one mdat for all samples above; the video track gets two truns
   (v1) and (v2 + its continuation slice), the audio track gets one trun */
MP4E_flush_fragment(mux);

/* start the next batch ... MP4E_close(mux) commits the final one */
```

Per-track `tfdt` is version 1 and advances by the real durations of the samples
that track actually contributed, so a track absent from a batch simply keeps its
own clock. Empty flushes write nothing and do not consume a fragment sequence
number.

## Bindings

 * https://github.com/darkskygit/minimp4.rs - rust bindings

## Interesting links

 * https://github.com/aspt/mp4
 * https://github.com/l30nnguyen/minimum_mp4_muxer
 * https://github.com/DolbyLaboratories/dlb_mp4demux
 * https://github.com/DolbyLaboratories/dlb_mp4base
 * https://github.com/ireader/media-server/tree/master/libmov
 * https://github.com/wlanjie/mp4
 * https://github.com/MPEGGroup/isobmff
 * http://www.itscj.ipsj.or.jp/sc29/open/29view/29n7644t.doc
 * http://atomicparsley.sourceforge.net/mpeg-4files.html
 * http://cpansearch.perl.org/src/JHAR/MP4-Info-1.12/Info.pm
 * https://developer.apple.com/library/archive/documentation/QuickTime/QTFF/QTFFPreface/qtffPreface.html
 * http://xhelmboyx.tripod.com/formats/mp4-layout.txt
