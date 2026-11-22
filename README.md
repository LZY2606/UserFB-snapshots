[![Build Status](https://github.com/mozilla/nestegg/actions/workflows/build.yml/badge.svg)](https://github.com/mozilla/nestegg/actions/workflows/build.yml)

See INSTALL for build instructions.

## Chained WebM streams

`nestegg_init_chained` opens a stream of complete WebM files (members)
concatenated back to back and presents them as one continuous stream:
the first member establishes the public track set, later members are
matched to it by `TrackUID`, and packet timestamps are shifted onto a
single timeline by the sum of the declared `Duration` of all previous
members.  Seeking is not supported in this mode; reading, packet
inspection, `nestegg_read_reset` and `nestegg_destroy` work as usual.

```c
nestegg_io io = { file_read, file_seek, file_tell, file_userdata };
nestegg * ctx = NULL;

if (nestegg_init_chained(&ctx, io, NULL, -1) != 0)
  return -1;

nestegg_packet * pkt;
int r;
while ((r = nestegg_read_packet(ctx, &pkt)) > 0) {
  unsigned int track;
  uint64_t tstamp;

  nestegg_packet_track(pkt, &track);   /* first member's track order */
  nestegg_packet_tstamp(pkt, &tstamp); /* continuous timeline, ns */

  /* ... decode packet ... */

  nestegg_free_packet(pkt);
}
if (r < 0) {
  /* Truncated or incompatible member: optionally resume after a
     transient IO error. */
  nestegg_read_reset(ctx);
}

nestegg_destroy(ctx);
```

Licensed under an ISC-style license.  See LICENSE for details.
