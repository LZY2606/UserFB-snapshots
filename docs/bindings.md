# Reading Mach-O dyld bindings offline

`(*macho.File).ReadBindings(kind macho.BindKind) ([]macho.Binding, error)`
decodes the classic dyld binding opcodes stored in `File.DylinkInfo`. It is a
read-only, purely offline operation:

- it never loads a dylib or resolves a symbol,
- it never executes machine code,
- it never applies a bind to file bytes,

so the returned `Binding` values describe what the binding stream *says*
should be bound, not what any process actually did.

`kind` selects one of the three streams that `DylinkInfo` already exposes:

| `BindKind` | `DylinkInfo` slice | declared length |
| ---------- | ------------------ | --------------- |
| `BindNormal` | `BindingInfoDat`   | `BindingInfoLen`   |
| `BindWeak`   | `WeakBindingDat`   | `WeakBindingLen`   |
| `BindLazy`   | `LazyBindingDat`   | `LazyBindingLen`   |

If `File.DylinkInfo` is `nil`, or the selected stream is empty, `ReadBindings`
returns an empty result. An invalid `kind`, or a stream whose declared length
in `DylinkInfo` differs from the length of the corresponding byte slice, is an
error.

## The `Binding` type

Every field is exported and documented in `macho/bindings.go`. Units:

| Field | Unit / meaning |
| ----- | -------------- |
| `StreamOffset` | byte offset of the triggering `DO_BIND`-family opcode inside the raw stream |
| `Symbol` | symbol name from `SET_SYMBOL_TRAILING_FLAGS_IMM` |
| `Flags` | low four bits of the symbol-trailing-flags immediate |
| `Ordinal` | signed dylib ordinal (special ordinals are sign-extended from four bits) |
| `Addend` | signed 64-bit addend |
| `Type` | bind type: 1 pointer, 2 text_abs32, 3 text_pcrel32, 4 text_abs64 |
| `SegmentIndex` | index among the `Segment` load commands in `File.Loads` order, counting `__PAGEZERO` |
| `SegmentOffset` | byte offset from the selected segment's `Addr` |
| `Address` | virtual address, `segment.Addr + SegmentOffset` |
| `HasFileBytes` | whether the whole bound field lies within the segment's `Filesz` |
| `FileOffset` | file offset of the field; set only when `HasFileBytes` |

Results are ordered by the position in the stream where each operation occurs.
Repeated binds expanded from one `DO_BIND_ULEB_TIMES_SKIPPING_ULEB` opcode
share that opcode's `StreamOffset`. Duplicate locations are never merged.

`SegmentIndex` is a positional index into the `Segment` loads, not a name
lookup; duplicate segment names and `__PAGEZERO` are counted normally.

`HasFileBytes` is true only when the entire bound field is inside `Filesz`.
A binding in a segment's zero-filled tail (inside `Memsz` but past `Filesz`)
is a valid virtual reference but has `HasFileBytes == false` and
`FileOffset == 0`. A field that straddles the file end is likewise reported as
having no file bytes. The pointer width that decides the field size and the
stride comes from the file magic (`Magic32` → 4, `Magic64` → 8).

## Decoded opcodes

The decoder understands the classic (non-threaded) binding opcodes:

| Opcode | Name | Effect |
| ------ | ---- | ------ |
| `0x00` | `DONE` | normal/weak: ends the stream; lazy: ends one record and resets state |
| `0x10` | `SET_DYLIB_ORDINAL_IMM` | ordinal from the low nibble |
| `0x20` | `SET_DYLIB_ORDINAL_ULEB` | ordinal from a ULEB128 |
| `0x30` | `SET_DYLIB_SPECIAL_IMM` | special ordinal, sign-extended from the low nibble |
| `0x40` | `SET_SYMBOL_TRAILING_FLAGS_IMM` | flags from the low nibble, then a NUL-terminated symbol |
| `0x50` | `SET_TYPE_IMM` | bind type from the low nibble |
| `0x60` | `SET_ADDEND_SLEB` | addend from a SLEB128 |
| `0x70` | `SET_SEGMENT_AND_OFFSET_ULEB` | segment index from the low nibble, offset from a ULEB128 |
| `0x80` | `ADD_ADDR_ULEB` | add a ULEB128 to the current offset |
| `0x90` | `DO_BIND` | emit one binding, then advance by the pointer width |
| `0xa0` | `DO_BIND_ADD_ADDR_ULEB` | emit, advance by a ULEB128, then by the pointer width |
| `0xb0` | `DO_BIND_ADD_ADDR_IMM_SCALED` | emit, advance by `imm * pointerWidth`, then by the pointer width |
| `0xc0` | `DO_BIND_ULEB_TIMES_SKIPPING_ULEB` | emit `count` times, advancing by `skip + pointerWidth` after each |

The initial state is: type `pointer`, ordinal `0`, addend `0`, empty symbol,
no segment selected. `DO_BIND`-family opcodes emit using the state *before*
they advance.

Two stride rules differ subtly:

- The field size used for `HasFileBytes` is 4 bytes for `text_abs32` and
  `text_pcrel32`, but the pointer width otherwise.
- The post-bind stride is **always** the pointer width, even when the bind
  type is `text_abs32` or `text_pcrel32`. The scaled immediate in
  `DO_BIND_ADD_ADDR_IMM_SCALED` is also scaled by the pointer width, not the
  field width.

## `DONE` and padding

- **normal** and **weak**: `DONE` ends the whole stream. Afterwards only zero
  bytes are allowed; non-zero padding is reported as corruption. Reaching the
  end of the stream without a `DONE` is accepted.
- **lazy**: `DONE` ends one record and resets all state so the next record is
  interpreted independently. Consecutive zero bytes are treated as empty
  records. A later record inherits nothing from an earlier one.

A weak stream that only sets a symbol and never reaches a `DO_BIND` produces
no binding; definition declarations are not fabricated into binds.

## Errors and safety

Unsupported opcodes, including all threaded-binding opcodes (`0xd0`), are
reported as errors. ULEB128/SLEB128 overflow and truncation, segment indices
out of range, address addition overflow, and segment-offset arithmetic past
`Memsz` are all checked. On any error, `ReadBindings` returns a nil list (never
a partial one) and an error whose text names the stream kind and the stream
offset, for example:

```
macho: normal binding stream: unsupported threaded binding opcode 0xd0 at stream offset 0x5
```

The call never mutates the file or its input bytes, so a failed call does not
affect later calls or the underlying data.

Repeated-binding expansion is capped at `MaxBindingExpansions` (1,048,576)
records per call; a stream that would exceed this is rejected.

## Offline worked example

Consider a 64-bit file (`Magic64`, pointer width 8) with one segment:

```
index 0: "__DATA"  Addr=0x10000  Memsz=0x4000  Fileoff=0x1000  Filesz=0x40
```

and this normal binding stream at `DylinkInfo.BindingInfoDat`:

```
offset  bytes            decode
0x00    11               SET_DYLIB_ORDINAL_IMM 1
0x01    42 66 6f 6f 00   SET_SYMBOL_TRAILING_FLAGS_IMM flags=2, symbol "foo"
0x06    51               SET_TYPE_IMM 1 (pointer)
0x07    70 20            SET_SEGMENT_AND_OFFSET_ULEB seg 0, offset 0x20
0x09    b2               DO_BIND_ADD_ADDR_IMM_SCALED imm 2
0x0a    00               DONE
```

Working through it:

1. `0x09` emits a binding using state at that moment:
   - `Symbol = "foo"`, `Flags = 2`, `Ordinal = 1`, `Addend = 0`,
     `Type = 1` (pointer), `SegmentIndex = 0`, `SegmentOffset = 0x20`,
     `Address = 0x10000 + 0x20 = 0x10020`, `StreamOffset = 0x09`.
   - Field size is the pointer width (8). `0x20 + 8 <= Filesz 0x40`, so
     `HasFileBytes = true` and `FileOffset = 0x1000 + 0x20 = 0x1020`.
2. `b2` then advances by `imm(2) * pointerWidth(8) = 0x10`, followed by the
   mandatory pointer-width step `8`, leaving `SegmentOffset = 0x38`.
3. `0x0a` is `DONE`, so the stream ends.

The result is a single `Binding`:

```go
macho.Binding{
    StreamOffset:  0x09,
    Symbol:        "foo",
    Flags:         2,
    Ordinal:       1,
    Addend:        0,
    Type:          1,
    SegmentIndex:  0,
    SegmentOffset: 0x20,
    Address:       0x10020,
    HasFileBytes:  true,
    FileOffset:    0x1020,
}
```

Note that the record's `SegmentOffset` and `Address` are captured at emit time
(`0x20` / `0x10020`), before the advance.

### Lazy example

A lazy stream:

```
offset  bytes            decode
0x00    12               SET_DYLIB_ORDINAL_IMM 2
0x01    40 61 00         SET_SYMBOL_TRAILING_FLAGS_IMM flags=0, symbol "a"
0x04    70 00            SET_SEGMENT_AND_OFFSET_ULEB seg 0, offset 0
0x06    90               DO_BIND
0x07    00               DONE  -> end record 1, reset state
0x08    00               empty record
0x09    40 62 00         SET_SYMBOL_TRAILING_FLAGS_IMM symbol "b"   (fresh state)
0x0c    70 08            SET_SEGMENT_AND_OFFSET_ULEB seg 0, offset 8
0x0e    90               DO_BIND
0x0f    00               DONE  -> end record 2
```

produces two bindings. The second has `Ordinal = 0` and `Addend = 0` because
`DONE` reset the state; only the default type (pointer) persists, which is
itself part of the reset state.
