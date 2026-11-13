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

#### Multiple AVC configurations in one track

If the encoder switches resolution or SPS/PPS mid-recording, several AVC
decoder configurations can share one video track. Each sample records which
configuration was selected when it was submitted; the resulting file has one
`avc1` sample entry per configuration and an `stsc` mapping per run of
samples.

```c
#include "minimp4.h"

/* avcc is a complete AVCDecoderConfigurationRecord, without a box header.
   The muxer copies it, so the buffer may be reused right afterwards. */
int desc_a = MP4E_add_avc_description(mux, track_id, 1280, 720, avcc_a, avcc_a_bytes);
int desc_b = MP4E_add_avc_description(mux, track_id, 640, 360, avcc_b, avcc_b_bytes);
if (desc_a < 0 || desc_b < 0)
    return -1;                      /* MP4E_STATUS_* error code */

/* Samples submitted after this call use description 2 ('640x360'). The
   switch does not touch any already written sample. A sample that changes
   the active configuration must be flagged as a random access point. */
if (MP4E_select_avc_description(mux, track_id, desc_b))
    return -1;

MP4E_put_sample(mux, track_id, frame_b, frame_b_bytes, duration,
                MP4E_SAMPLE_RANDOM_ACCESS);

/* Additional slices of the same frame are appended to it. A continuation is
   only accepted when the selected configuration is still the one the frame
   was written with. */
MP4E_put_sample(mux, track_id, slice_b2, slice_b2_bytes, 0,
                MP4E_SAMPLE_CONTINUATION);
```

On the reading side every entry has a read-only view that stays valid until
`MP4D_close()`:

```c
const MP4D_avc_description_t *d = MP4D_avc_description(&mp4, track, 2);
if (d)
    use_sps_pps(d->avcc, d->avcc_bytes, d->width, d->height);

/* 1-based description governing a zero-based sample; 0 if invalid. */
unsigned idx = MP4D_sample_description_index(&mp4, track, sample);
```

`track[ntrack].dsi`, `MP4D_read_sps()` and `MP4D_read_pps()` keep describing
the first entry, independent of any other query.

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
