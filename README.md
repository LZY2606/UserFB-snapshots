# libchdr

libchdr is a standalone library for reading MAME's CHDv1-v5 formats.

The code is based off of MAME's old C codebase which read up to CHDv4 with OS-dependent features removed, and CHDv5 support backported from MAME's current C++ codebase.

libchdr is licensed under the BSD 3-Clause (see [LICENSE.txt](LICENSE.txt)) and uses third party libraries that are each distributed under their own terms (see each library's license in [deps/](deps/)).

## CD sector repair

By default nothing changes how reads behave: `chd_read()` returns the raw
bytes on disk and `ecc_generate()`/`ecc_verify()` only rebuild or check the
P/Q error-correction codes. On top of that, libchdr offers an explicit,
opt-in single-byte repair path for raw CD sectors (requires
`WANT_RAW_DATA_SECTOR`, on by default).

`cdrom_repair_sector()` (in `libchdr/cdrom.h`) works on one 2352-byte raw
sector whose layout is described by a `CD_TRACK_*` kind. Only
`CD_TRACK_MODE1_RAW` and `CD_TRACK_MODE2_FORM1` carry repairable EDC + P/Q
ECC; every other kind (audio, Mode 2 Form 2, cooked modes, ...) returns
`CD_REPAIR_SKIPPED` with the sector byte-for-byte unchanged - an audio
frame is never reinterpreted as a data sector, even if its body happens to
start with the sync pattern. A damaged byte anywhere in offsets 16..2351
(user data, EDC, reserved area, or the P/Q parity itself) is located from
the P/Q syndromes and its delta solved for arithmetically; the fix is
applied only if the whole sector then passes every check (EDC, P/Q ECC,
and for Mode 2 Form 1 the duplicated subheader with the Form 2 bit clear).
The sync pattern, address and mode bytes (offsets 0..15) are never
modified. The result is one of:

* `CD_REPAIR_UNCHANGED` - the sector already passed every check
* `CD_REPAIR_CORRECTED` - exactly one byte was fixed; `corrected_offset`
  (which may be `NULL`) holds its 0-based position, and is -1 otherwise
* `CD_REPAIR_UNCORRECTABLE` - no unique single-byte fix exists; the sector
  is returned unmodified
* `CD_REPAIR_SKIPPED` - the kind carries no repairable ECC

`chd_read_cd_repaired()` (in `libchdr/chd.h`) applies this to a whole hunk
of a CD image (`unitbytes == 2448`, hunk size a multiple of 2448). The hunk
is fetched exactly as `chd_read()` fetches it - decompression, CRC and I/O
errors are reported unchanged - and each 2352-byte slot is then repaired
according to its kind. The 96-byte subchannel area of every slot is never
touched, repairs live only in the caller's buffer (nothing is written back
to the image or to any internal cache), and a slot that cannot be repaired
is delivered with its original bytes without affecting the other slots:

```c
#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

chd_file *chd;
uint8_t hunk[CD_FRAMES_PER_HUNK * CD_FRAME_SIZE];
uint8_t kinds[CD_FRAMES_PER_HUNK] = {
    CD_TRACK_MODE1_RAW, CD_TRACK_MODE1_RAW, CD_TRACK_MODE1_RAW, CD_TRACK_MODE1_RAW,
    CD_TRACK_AUDIO,     CD_TRACK_AUDIO,     CD_TRACK_AUDIO,     CD_TRACK_AUDIO
};
int statuses[CD_FRAMES_PER_HUNK];
uint32_t i;

if (chd_open("game.chd", CHD_OPEN_READ, NULL, &chd) == CHDERR_NONE)
{
    if (chd_read_cd_repaired(chd, 0, hunk, kinds, CD_FRAMES_PER_HUNK, statuses) == CHDERR_NONE)
    {
        for (i = 0; i < CD_FRAMES_PER_HUNK; i++)
        {
            if (statuses[i] == CD_REPAIR_CORRECTED)
                printf("frame %u: one byte repaired\n", i);
            else if (statuses[i] == CD_REPAIR_UNCORRECTABLE)
                printf("frame %u: damaged beyond single-byte repair\n", i);
        }
    }
    chd_close(chd);
}
```
