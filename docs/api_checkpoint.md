---
layout: default
---

# Persistent Checkpoints

The normal **src\_clone** function duplicates a converter inside the running
process. Persistent checkpoints go further: they serialise the complete
resumable state of a sinc converter into a flat byte buffer which can be
written to a file and read back in a different process. Conversion then
continues from the exact same resampling position, producing bit identical
PCM to an uninterrupted run. The already consumed input audio is not part of
the checkpoint and does not have to be re-read.

Checkpointing is only available for the three sinc converters:

- `SRC_SINC_BEST_QUALITY`
- `SRC_SINC_MEDIUM_QUALITY`
- `SRC_SINC_FASTEST`

`SRC_ZERO_ORDER_HOLD` and `SRC_LINEAR` do not support it; their checkpoint
calls fail explicitly.

## API

```c
size_t src_checkpoint_size (SRC_STATE *state) ;

int src_checkpoint_write (SRC_STATE *state, void *buffer, size_t bytes) ;

SRC_STATE* src_checkpoint_read (const void *buffer, size_t bytes,
                                src_callback_t callback, void *cb_data,
                                int *error) ;
```

**src\_checkpoint\_size** returns the number of bytes the checkpoint needs.
It returns zero for a NULL state, an invalid state or an unsupported
converter.

**src\_checkpoint\_write** serialises the state into `buffer`, which must
provide at least `src_checkpoint_size()` bytes. It returns
`SRC_ERR_NO_ERROR` on success and never modifies the source instance; writing
the same unchanged state twice yields byte identical buffers. A buffer that
is too small is rejected with an error rather than being partially written.

**src\_checkpoint\_read** validates the buffer and returns a new,
independently owned `SRC_STATE`. On failure it returns NULL and stores a
non-zero error code. The `callback` argument must match the mode stored in
the checkpoint:

- a checkpoint written from a process mode state (created with **src\_new**)
  may only be restored with `callback == NULL`;
- a checkpoint written from a callback mode state (created with
  **src\_callback\_new**) must be restored with a non-NULL callback and the
  callback context to use from now on.

When a callback mode checkpoint is restored, the new callback's data cursor
must be positioned at the end of the data that had been *returned* at save
time. Any frames the old callback had delivered but the filter had not
consumed yet are stored inside the checkpoint and are fed to the filter
first; the new callback is only invoked once those remainder frames are
exhausted, so it must not redeliver them.

After a successful restore the original state, the original callback buffers
and the checkpoint bytes may all be destroyed. A restored state can itself be
cloned with **src\_clone** (the clone gets its own copy of every buffer),
reset with **src\_reset** (which discards the restored audio and any
remainder frames) and deleted with **src\_delete**.

## On-disk format

The format is architecture independent. Every integer is stored at a fixed
width in little endian byte order. Floating point values are stored as their
IEEE-754 bit patterns, also little endian. The buffer contains no pointers,
no structure padding and no host dependent types such as `long`.

Format version 1, in storage order:

| Offset | Size    | Type   | Field |
|--------|---------|--------|-------|
| 0      | 4       | bytes  | Magic: the ASCII characters `S`, `R`, `C`, `P` |
| 4      | 2       | u16    | Format version, currently 1 |
| 6      | 2       | u16    | Converter number (0, 1 or 2, the three sinc converters) |
| 8      | 2       | u16    | Channel count (>= 1) |
| 10     | 2       | u16    | Mode: 0 = process mode, 1 = callback mode |
| 12     | 8       | double | `last_ratio`, the current conversion ratio (0.0 if never used) |
| 20     | 8       | double | `last_position`, the fractional position in the range [0.0, 1.0) |
| 28     | 4       | u32    | Callback remainder frame count `P` (always 0 in process mode) |
| 32     | 4·P·C   | float  | Callback remainder samples, interleaved over `C` channels |
| ...    | 4       | u32    | Converter payload length `N` in bytes |
| ...    | N       | bytes  | Converter payload (see below) |

The 32 byte header ends at the remainder frame count. All multi-byte fields
are naturally aligned relative to the start of the buffer, but readers and
writers process the stream byte by byte, so the buffer itself needs no
alignment.

### Sinc converter payload

`C` is the channel count from the header and `L` is the ring buffer length,
which is fixed for a given converter/channel pair. Ring indices count
individual samples (not frames) and are always multiples of `C`.

| Offset | Size  | Type  | Field |
|--------|-------|-------|-------|
| 0      | 4     | u32   | `b_current`, ring read position |
| 4      | 4     | u32   | `b_end`, ring write position (<= `L`) |
| 8      | 4     | i32   | `b_real_end`: -1 if end of input has not been announced, otherwise the ring position of the last real sample |
| 12     | 4     | u32   | Ring length `L`, must match the converter/channel pair |
| 16     | 4·L   | float | Complete ring buffer in storage order |

The ring holds the FIR delay history together with the samples not yet
consumed; its size is fixed by the converter and channel count and does not
grow with the length of the stream. Per-call scratch counters and the
`left_calc`/`right_calc` working arrays are not stored. Filter coefficients
are derived from the converter number, and the channel specific virtual
table is chosen from the channel count.

## Validation

A checkpoint is rejected, without any attempt to repair it, when any of the
following holds:

- the magic bytes do not match;
- the format version is not supported;
- the converter number is unknown or is not enabled in the reading build;
- the channel count or mode is invalid;
- the mode contradicts the callback argument or the remainder frame count;
- a floating point field is NaN or infinite, or otherwise out of range;
- the buffer is truncated before every declared field has been read;
- the converter payload does not end exactly at the end of the buffer;
- any payload field contradicts another field (misaligned ring indices, a
  ring length that does not match the converter/channel pair, indices
  outside the ring, and so on).
