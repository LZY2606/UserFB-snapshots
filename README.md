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

#### Opus audio

Opus tracks use an OpusHead identification header (RFC 7845 section 5.1)
and one complete multi-stream packet per sample (RFC 6716 Appendix B
self-delimiting framing for every stream but the last). Only channel
mapping families 0 and 255 are supported, and fragmented mode does not
support Opus.

```c
static const unsigned char opus_head[] = {
    'O','p','u','s','H','e','a','d',
    1,                    /* version                                        */
    7,                    /* output channels                               */
    0x78, 0x01,           /* pre-skip, little endian (376 samples)         */
    0x44, 0xac, 0x00, 0x00, /* input sample rate, little endian (44100 Hz) */
    0x00, 0x00,           /* output gain, little endian                    */
    255,                  /* channel mapping family                        */
    4, 2,                 /* stream count, coupled count                   */
    5, 0, 255, 3, 1, 4, 5 /* one mapping byte per output channel          */
};

MP4E_mux_t *mux = MP4E_open(0 /*sequential*/, 0 /*fragmented*/, fout, write_callback);
int track = MP4E_add_opus_track(mux, opus_head, sizeof(opus_head)); /* returns track id */

/* 'packet' is one full sample: streams 0..N-2 self-delimited, last stream
   regular. The library validates it and copies the bytes verbatim. */
if (MP4E_put_opus_packet(mux, track, packet, packet_bytes) != MP4E_STATUS_OK)
    /* rejected: fix the input and keep recording */;

/* Discard the encoder delay and the tail padding, then seal the track. */
MP4E_finish_opus(mux, track, end_trim_48k_frames);
MP4E_close(mux);
```

On playback, `MP4D_open()` reports Opus tracks through
`MP4D_track_t::sample_entry == 0x4f707573` ("Opus"). `dsi`/`dsi_bytes`
hold a reconstructed version 1 OpusHead (free it with `MP4D_close()`),
`opus_trim_start` / `opus_trim_end` give the edit-list start and end
trims in 48 kHz frames, and `MP4D_frame_offset()` keeps returning the
original packet range and untrimmed timestamps.

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
