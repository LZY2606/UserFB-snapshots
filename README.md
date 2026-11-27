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

#### Fast-start rewrite

Files produced in the default (non-sequential) offline mode keep the `moov`
index behind the media data, so a player has to download up to the end of the
file before playback can start.  `MP4E_faststart()` rewrites an existing
non-fragmented MP4 file into "fast start" layout (`ftyp`, `moov`, then the
remaining top-level boxes in their original order, top-level `free`/`skip`
dropped) without decoding or re-muxing: `mdat` payloads are copied
byte-for-byte and only the chunk offset tables (`stco`/`co64`) inside `moov`
are updated.  An `stco` table is upgraded to `co64` if any of its rewritten
offsets exceeds 32 bits, and the layout is recomputed until it is stable,
since growing one table can push another table's chunks past the limit.

The input must be a complete, valid, self-contained, unencrypted,
non-fragmented MP4 file (exactly one `ftyp` and one `moov`, at least one
`mdat`, no `sidx`/`mfra`).  Files with size-0 boxes, compressed moov
(`cmov`), fragments (`moof`) or external data references are rejected with
`MP4E_STATUS_BAD_ARGUMENTS`.

Only the `moov` box is held in memory; media data and unrelated large boxes
are streamed through a fixed 64 KiB buffer, and no single callback delivers
more than 64 KiB of payload.  No temporary files or seek-back patching are
used, so the output can go to any sequential sink.  The write callback
receives offsets growing contiguously from 0; input and output must be
different storages.  If either callback fails, the function stops
immediately and returns `MP4E_STATUS_FILE_WRITE_ERROR`.

```c
#include "minimp4.h"

typedef struct { FILE *in, *out; } io_t;

static int read_at(int64_t offset, void *buffer, size_t size, void *token)
{
    io_t *io = (io_t *)token;
    if (fseeko(io->in, (off_t)offset, SEEK_SET))
        return 1;
    return fread(buffer, 1, size, io->in) != size;
}

static int write_at(int64_t offset, const void *buffer, size_t size, void *token)
{
    io_t *io = (io_t *)token;
    (void)offset;   /* offsets grow contiguously from 0 */
    return fwrite(buffer, 1, size, io->out) != size;
}

int faststart(const char *src, const char *dst)
{
    io_t io;
    long size;
    int rc;
    io.in = fopen(src, "rb");
    io.out = fopen(dst, "wb");
    if (!io.in || !io.out)
        return -1;
    fseek(io.in, 0, SEEK_END);
    size = ftell(io.in);
    rc = MP4E_faststart((uint64_t)size, &io, read_at, write_at);
    fclose(io.in);
    fclose(io.out);
    return rc;  /* MP4E_STATUS_OK (0) on success, MP4E_STATUS_* otherwise */
}
```

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
