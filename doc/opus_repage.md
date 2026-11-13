# Ogg Opus re-pager (`ogg/opus_repage.h`)

`ogg_opus_repage` re-wraps an existing Ogg Opus recording so that every
output page stays under a hard byte limit, without decoding or
re-encoding any audio and without buffering the recording. It consumes
one input page at a time and hands each completed output page to a
callback, so it can sit directly between a demuxer and a transport
that enforces a maximum packet size (libogg's `flush_fill` argument is
only a watermark and cannot provide this guarantee).

## What it does

* Accepts the pages of **one complete Ogg Opus stream**: a single
  serial number, page sequence starting at 0 with no gaps, one audio
  track, no multiplexing and no chaining.
* Verifies the stream skeleton: a 19-byte, version 1, mapping family 0
  `OpusHead` (mono or stereo) alone on the BOS page, a complete
  `OpusTags` header that ends at a page boundary, then audio packets,
  terminated by EOS. Other layouts are rejected.
* Re-paginates with a fixed rule: packets keep their original order
  and their original lacing values (including the zero-length
  terminator of packets whose length is a multiple of 255); a new page
  is started only when the next lacing value would exceed
  `max_page_bytes` (counting header + segment table + body) or the 255
  segment limit. Input page boundaries never force an output page.
  Forced breaks happen only after `OpusHead`, after the end of
  `OpusTags`, and at EOS.
* Recomputes granule positions from the Opus TOC of every audio packet
  (48 kHz samples per channel, no fixed frame-size assumption),
  anchored to the source pages' granule positions. Tail-trimmed EOS
  granules are preserved; inconsistent clocks, unfinished packets and
  missing EOS are reported as errors.
* Keeps the original serial number, renumbers pages from 0, sets
  BOS/EOS exactly once, and recomputes CRCs.

## Memory and threading

* All heap allocations go through the caller-supplied allocator (or
  `malloc`/`free` when `NULL` is passed). The sum of simultaneously
  outstanding bytes never exceeds `max_page_bytes + 80000`, regardless
  of packet sizes, comment length, or stream duration.
* No global buffers, temporary files, or whole-packet collectors are
  used. Pages delivered to the emit callback borrow instance storage
  and are valid only until the callback returns; input pages are not
  referenced after `ogg_opus_repage_pagein` returns.
* Instances are fully independent; any number may be interleaved.

## Example

```c
#include <ogg/opus_repage.h>
#include <ogg/ogg.h>
#include <stdio.h>

static int send_page(void *user, const ogg_page *page) {
    FILE *out = (FILE *)user;
    /* page is borrowed: consume it before returning */
    if (fwrite(page->header, 1, page->header_len, out) != (size_t)page->header_len)
        return -1;
    if (fwrite(page->body, 1, page->body_len, out) != (size_t)page->body_len)
        return -1;
    return 0;   /* 0 = page accepted */
}

int repage(FILE *out) {
    ogg_opus_repage *rp = ogg_opus_repage_new(4096, NULL, send_page, out);
    ogg_page page;
    int ok = 0;
    if (!rp) return -1;

    /* ... feed pages from your demuxer in order ... */
    while (next_demuxed_page(&page)) {
        if (ogg_opus_repage_pagein(rp, &page) != 0) { ok = -1; break; }
    }
    if (!ok && ogg_opus_repage_finish(rp) != 0)
        ok = -1;   /* missing EOS or truncated packet */

    ogg_opus_repage_free(rp);   /* always releases, also after errors */
    return ok;
}
```

With a custom allocator:

```c
ogg_opus_repage_allocator al = { pool, pool_alloc, pool_release };
ogg_opus_repage *rp = ogg_opus_repage_new(512, &al, send_page, out);
```

## Supported scope and limits

* `max_page_bytes` must be within **512 … 65307**; otherwise
  `ogg_opus_repage_new` fails.
* `OpusHead`: exactly 19 bytes, version 1, channel count 1 or 2,
  mapping family 0. Anything else is rejected as unsupported.
* `OpusTags` is preserved byte-for-byte but never interpreted; it may
  be several MiB and span arbitrarily many input pages.
* Packet durations come from the Opus TOC: configurations 0–11 select
  480/960/1920/2880 samples by the low two bits, 12–15 select 480/960
  by the low bit, 16–31 select 120/240/480/960 by the low two bits;
  packet code 0/1/2 means 1/2/2 frames, code 3 takes the frame count
  from the low 6 bits of the second byte. A zero frame count or a
  total over 5760 samples is rejected.
* The audio clock is anchored at the first source page that completes
  an audio packet (granule minus that page's completed durations) and
  must advance linearly afterwards. EOS may end early (tail trim
  strictly smaller than the last packet's duration; larger trims are
  rejected) and its granule is preserved on the output. The EOS
  granule must not be smaller than the `OpusHead` pre-skip.
* After any error (structural, clock, callback, or allocation
  failure), the instance is terminal: further `pagein`/`finish` calls
  return -1, and `ogg_opus_repage_free` still releases all resources.
  Pages already delivered to the callback are not revoked.
