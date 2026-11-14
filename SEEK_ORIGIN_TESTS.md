# Seek-origin regression

`tests/seek_origin_test.c` is a standalone, in-memory C regression for the
WavPack sample-origin / seeking path. It never touches a sound device or the
network: all material is produced through the public encoding API and fed
back through `WavpackStreamReader64` memory callbacks.

The cases are registered with the task harness (`task_cases`,
`task_case_count`) in three groups — `origin`, `lanes`, `restart` — each case
returns `0` and performs its comparisons through `TASK_CHECK`; the file
defines no `main` (the harness in `task_support/driver.c` supplies it).

## Files

| File | Role |
| --- | --- |
| `tests/seek_origin_test.c` | The three case groups and all assertions. |
| `tests/seek_origin_catalog.h` | Independent physical-block catalog and per-lane memory callbacks / position ledgers. |
| `tests/seek_origin_materials.h` | Public-API material generator, 40-bit re-heading with checksum recomputation, and sequential self-check. |

## Independent physical catalog

`seek_origin_catalog.h` walks the raw bytes itself, block by block, using only
the documented 32-byte WavPack header layout. It does **not** call
`GET_BLOCK_INDEX` / `GET_TOTAL_SAMPLES` and never consults decoder output:

* the 40-bit block index is assembled from the low 32 bits at offset 16 and
  the high byte at offset 10;
* the 40-bit total is read from offset 12/11, with the all-ones low word
  interpreted as "unknown";
* each record stores its byte interval (`offset`, `ckSize + 8` length),
  lane (WV or WVC), multichannel segment membership, raw sample origin,
  sample count and flags;
* the playback-relative window is derived independently as
  `rel = raw_index - origin_of_first_initial_block`;
* the total frame count is summed from the segment-leading
  (`INITIAL_BLOCK`) blocks rather than trusted from any header field.

The expected PCM is the exact pre-encode `int32_t` array (a distinct marker
per frame/channel, see `so_marker`). First-pass decode results and the
library header macros are never used as an answer.

## Material generation and re-heading

`seek_origin_materials.h` encodes integer PCM with the public API
(`WavpackOpenFileOutput` / `WavpackSetConfiguration64` / `WavpackPackSamples`
/ `WavpackFlushSamples`). Re-headed material leaves the compressed audio
unchanged and translates every block's 40-bit origin by a constant; the
block checksum is then recomputed with the on-disk
`csum = csum * 3 + word` algorithm and rewritten, so checksum verification
stays enabled (`OPEN_NO_CHECKSUM` is never passed). Each material is
sequentially decoded once at build time and must match the reference PCM
with zero decoder errors before any test uses it.

Origin classes covered (mono and stereo): zero, a small non-zero value, a
value straddling the 32-bit boundary, and a value already beyond 32 bits —
all inside the 40-bit range — with both known and unknown header totals.

## Activated paths

* **origin (10 cases).** Scrambled seek targets, several landing on or just
  past block boundaries. For every target the case verifies the returned
  frame count, the relative cursor, sample-accurate integer PCM, guard words
  around the output buffer, and zero decoder errors, against the independent
  catalog's containing segment.
* **lanes (2 cases).** Hybrid stereo material that genuinely requires the
  WVC stream. WV and WVC run on separate memory callbacks with separate
  position ledgers and traces. The chosen target group sits at different
  byte offsets in the two lanes; the case asserts that the header actually
  loaded in each lane equals that lane's own catalog entry (the WV position
  is never reused as the WVC position), that every header seen on each
  search trajectory belongs to that lane, and that decoding with the WVC
  lane deliberately ignored still runs but yields different PCM.
* **restart (2 cases).** Backward seeks inside one block, cross-block jumps,
  consecutive seeks with no read between them, an end-of-stream short read
  followed by a return to the start, EOF reached purely by reading (no seek
  to the total sample count), and two decoder instances interleaved on one
  material to prove their state does not cross. Reads alternate counts of
  1, 7 and a block-spanning size, with sentinels on both sides of the output
  buffer. A zero-origin six-channel material exercises preroll recovery of
  the non-first substreams. Per the task, multi-channel material with a
  non-zero origin is not treated as supported by the upstream library and is
  deliberately excluded; the six-channel case uses origin zero.

## First-difference evidence

On the first PCM mismatch of any comparison the harness prints one line

```
SEEK_PCM_DIFF material=<name> target=<frame> channel=<ch> actual=<v> expected=<v>
```

before the failing `TASK_CHECK`. `task_support/variants.sh` applies two
mutations to the library and expects the same test to still locate the
target, reach a PCM difference, and exit with the harness assertion code (4):

* `variants/origin.patch` under-counts the non-zero-origin preroll by one
  frame. Observed first difference:

  ```
  SEEK_PCM_DIFF material=mono-small-known target=130 channel=0 actual=-11825 expected=-3647
  ```

* `variants/restart.patch` reduces multi-substream preroll to advancing only
  the first substream. Observed first difference (a non-leading substream):

  ```
  SEEK_PCM_DIFF material=sixch-zero-preroll target=33 channel=2 actual=-26914 expected=-2324
  ```

On the unmodified library all three groups report `COMPLETE` and the run
finishes with `TARGET_COMPLETE`; the existing baseline round-trips and the
upstream integer (`wvtest`) suite continue to pass unchanged.
