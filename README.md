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

## Fast start rewrite

`MP4E_faststart()` rewrites an existing MP4 stream into "fast start" layout:
the `moov` index is emitted right after `ftyp`, before the media data, so a
player does not have to download the file tail first. Samples are not decoded
or re-muxed — `mdat` payload is copied byte-for-byte and only `stco`/`co64`
chunk offsets are adjusted (upgrading `stco` tables to `co64` when an offset
stops fitting in 32 bits, iterating until the layout is stable).

```c
#include <stdio.h>
#include <stdint.h>
#include "minimp4.h"

typedef struct { FILE *in, *out; } faststart_io_t;

/* random-access read; return 0 on success */
static int reader(int64_t offset, void *buffer, size_t size, void *token)
{
    FILE *f = ((faststart_io_t *)token)->in;
    if (fseek(f, (long)offset, SEEK_SET)) return 1;   /* use fseeko() for >2GB */
    return fread(buffer, 1, size, f) != size;
}

/* offsets arrive sequentially, starting at 0; return 0 on success */
static int writer(int64_t offset, const void *buffer, size_t size, void *token)
{
    FILE *f = ((faststart_io_t *)token)->out;
    (void)offset;
    return fwrite(buffer, 1, size, f) != size;
}

int main(void)
{
    faststart_io_t io;
    long size;
    int rc;
    io.in = fopen("input.mp4", "rb");
    io.out = fopen("output.mp4", "wb");
    fseek(io.in, 0, SEEK_END);
    size = ftell(io.in);
    rc = MP4E_faststart((uint64_t)size, &io, reader, writer);
    fclose(io.in);
    fclose(io.out);
    return rc == MP4E_STATUS_OK ? 0 : 1;
}
```

Input requirements and behavior:

 * The input must be a complete, valid, self-contained, unencrypted,
   non-fragmented MP4 stream of at most `INT64_MAX` bytes, with exactly one
   `ftyp` and one `moov` box and at least one `mdat` box. Input and output
   must be different storages.
 * Size-0 boxes, compressed `moov` (`cmov`), movie fragments (`moof`) and
   external data references are rejected with `MP4E_STATUS_BAD_ARGUMENTS`.
 * The output is: the original `ftyp`, the new `moov`, then the remaining
   top-level boxes in their original relative order. Top-level `free`/`skip`
   boxes are dropped; unknown boxes and `mdat` payload are preserved
   byte-for-byte. Already fast-starting inputs are handled.
 * Both callbacks return 0 on success. The read callback may be called at any
   offset; write offsets start at 0 and grow contiguously. Each callback
   invocation transfers at most 64 KiB of payload.
 * Only the `moov` box plus a 64 KiB scratch buffer are held in memory;
   media data is streamed, no temporary files or seek-back writes are used.
 * A failing callback aborts the rewrite immediately with
   `MP4E_STATUS_FILE_WRITE_ERROR`.

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
