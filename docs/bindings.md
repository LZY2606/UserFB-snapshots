# Reading Mach-O dyld binding streams

`File.ReadBindings(kind BindKind) ([]Binding, error)` is a read-only decoder
for the classic dyld binding opcodes. It interprets bytes that
`NewFile` already copied into `File.DylinkInfo`:

| kind             | stream bytes                | declared length             |
| ---------------- | --------------------------- | --------------------------- |
| `BindNormal`     | `DylinkInfo.BindingInfoDat` | `DylinkInfo.BindingInfoLen` |
| `BindWeak`       | `DylinkInfo.WeakBindingDat` | `DylinkInfo.WeakBindingLen` |
| `BindLazy`       | `DylinkInfo.LazyBindingDat` | `DylinkInfo.LazyBindingLen` |

The decoder never loads a dylib, executes machine code, resolves a symbol, or
writes a binding into the image. It only reports what the file says.

## Result fields

Bindings are returned in opcode order. Two binds to the same address are two
entries; they are never merged.

| field            | meaning                                                                  | unit                    |
| ---------------- | ------------------------------------------------------------------------ | ----------------------- |
| `Kind`           | stream the entry came from                                               | enum                    |
| `StreamOffset`   | offset of the `DO_BIND*` opcode that triggered the entry, from stream 0  | bytes                   |
| `Symbol`         | symbol from the most recent `SET_SYMBOL_TRAILING_FLAGS_IMM`               | text                    |
| `Flags`          | immediate flags of that symbol opcode                                    | 0–15                    |
| `DylibOrdinal`   | signed dylib ordinal; special ordinals are sign-extended (`0xF` → `-1`)  | library ordinal         |
| `Addend`         | value from the most recent `SET_ADDEND_SLEB`                              | bytes (signed)          |
| `Type`           | `BindTypePointer` (1), `BindTypeTextAbsolute32` (2), `BindTypeTextPCRel32` (3) | enum             |
| `SegmentIndex`   | index among `*Segment` loads in `File.Loads`, counting `__PAGEZERO`      | index                   |
| `SegmentName`    | that segment's name (informational; names may repeat)                    | text                    |
| `SegmentOffset`  | offset from the start of the segment                                     | bytes                   |
| `VirtualAddress` | `Segment.Addr + SegmentOffset`                                           | virtual address         |
| `HasFileBytes`   | the whole bound field lies within `Segment.Filesz`                       | bool                    |
| `FileOffset`     | `Segment.Offset + SegmentOffset`; valid only when `HasFileBytes`         | file byte offset        |

The field width is 4 bytes for `text_abs32` and `text_pcrel32`, and the
pointer width (4 or 8, chosen by the file's magic) for every other type.
`HasFileBytes` requires the **complete** field to fit in `Filesz`; a field
that starts in file-backed memory and runs past the end of the file is
reported as virtual-only with `HasFileBytes == false` and `FileOffset == 0`.

Segment indices count `*Segment` load commands in `File.Loads` order,
including `__PAGEZERO`. Names are not unique and are never used as keys.

## Decoder semantics

Initial state for a stream: type = pointer, ordinal = 0, addend = 0, empty
symbol, no segment selected. `DO_BIND` uses the state as it stands *before*
it advances the location.

After every bind the location advances by the pointer width — not by the
field width. The advancing opcodes are:

- `DO_BIND`: advance pointer width.
- `DO_BIND_ADD_ADDR_ULEB`: advance ULEB immediate + pointer width.
- `DO_BIND_ADD_ADDR_IMM_SCALED`: advance `imm * pointerWidth + pointerWidth`
  (scale by pointer width, again not by field width).
- `DO_BIND_ULEB_TIMES_SKIPPING`: repeat `count` times, advancing
  `skip + pointerWidth` between binds. `count` and `skip` really move the
  location.

`ADD_ADDR_ULEB` advances the location without binding.

In normal and weak streams, `DONE` ends the whole stream; only zero bytes may
follow. In a lazy stream, `DONE` ends one record and clears **all** decoder
state, so the next record starts from the initial state; consecutive zero
bytes are empty records. A weak-stream symbol declaration without a
`DO_BIND` does not create a binding.

An empty stream, or a file with no `DylinkInfo`, yields an empty slice and a
nil error.

## Errors

Malformed or unsupported input returns a `*BindingError`:

```go
type BindingError struct {
    Kind    BindKind // which stream
    Offset  int      // byte offset in that stream
    Message string
}
```

Errors include an invalid kind, a declared length that disagrees with the
slice length, an unknown opcode (including threaded binding, `0xD0`), a
segment index past the last segment, a field outside `Memsz`, overflow while
adding addresses or decoding ULEB/SLEB, and a non-zero byte after `DONE` in a
normal or weak stream.

No partial list is returned on error, and neither the `File` nor its byte
slices are modified, so a later call on the same `File` still works.

Expansion is capped at `MaxBindings` (1,048,576) entries per call. A stream
that would exceed the cap fails with a `*BindingError` instead of growing
without bound.

## Offline byte example

Consider a 32-bit file with one segment named `__DATA`:

```
Segment.__DATA:  Addr=0x00001000  Memsz=0x00001000  Offset=0x400  Filesz=0x400
```

and this normal binding stream (hex):

```
11 45 73 79 6d 00 51 60 78 70 08 90 90 00
```

Decoded opcode by opcode:

| bytes        | opcode                                     | state after                                     |
| ------------ | ------------------------------------------ | ----------------------------------------------- |
| `11`         | `SET_DYLIB_ORDINAL_IMM` 1                  | ordinal = 1                                     |
| `45 73 79 6d 00` | `SET_SYMBOL_TRAILING_FLAGS_IMM` flags 5, name "sym" | symbol = "sym", flags = 5          |
| `51`         | `SET_TYPE_IMM` 1 (pointer)                 | type = pointer                                  |
| `60 78`      | `SET_ADDEND_SLEB` −8                       | addend = −8                                     |
| `70 08`      | `SET_SEGMENT_AND_OFFSET_ULEB` seg 0, 0x08  | segment 0, offset 0x08                          |
| `90`         | `DO_BIND`                                  | emits @0x08, location → 0x0C                    |
| `90`         | `DO_BIND`                                  | emits @0x0C, location → 0x10                    |
| `00`         | `DONE`                                     | end of stream                                   |

Two bindings result, both with `Kind = BindNormal`, `Symbol = "sym"`,
`Flags = 5`, `DylibOrdinal = 1`, `Addend = -8`, `Type = BindTypePointer`,
`SegmentIndex = 0`, `SegmentName = "__DATA"`:

| StreamOffset | SegmentOffset | VirtualAddress | HasFileBytes | FileOffset |
| ------------ | ------------- | -------------- | ------------ | ---------- |
| 11           | 0x08          | 0x1008         | true         | 0x408      |
| 12           | 0x0C          | 0x100C         | true         | 0x40C      |

Their locations are distinct, so both entries are kept even though every
other field is identical. A third `DO_BIND` would land at `0x10`, because the
stride is the 4-byte pointer width of this file.
