# Threaded bind/rebase chains (legacy arm64e)

Early arm64e images describe their pointers through `LC_DYLD_INFO` /
`LC_DYLD_INFO_ONLY` **threaded** opcodes instead of
`LC_DYLD_CHAINED_FIXUPS`. The bind bytecode first fills a *bind ordinal table*
(templates) and then runs one or more `THREADED_APPLY` opcodes; each APPLY walks
a chain of eight-byte slots inside a segment.

This package only **interprets the file**. It never loads a dependency library,
never executes the image, and never verifies or generates pointer
authentication (PAC) codes. Authenticated slots are decoded into descriptive
metadata only.

## Records

`types.Bind` and `types.Rebase` both gained a nullable field:

```go
Threaded *types.ThreadedPointerInfo
```

It is non-nil for every slot produced by a threaded APPLY chain and nil for
classic bind/rebase opcode records, so existing consumers can tell the two
formats apart.

The ordinal table only carries **template** data (symbol name, dylib, flags,
template addend). The real segment, section, virtual address and file location
of a record are always taken from the slot the APPLY is currently visiting. The
same table may be applied in several segments; table-fill segment information is
never copied into a result.

## `types.ThreadedPointerInfo`

| Field              | Type   | Unit / meaning |
|--------------------|--------|----------------|
| `Target`           | `uint64` | Bytes; a preferred virtual address. Used by **rebase** slots. Plain rebases store the absolute preferred VM address; authenticated rebases store a 32-bit image-relative offset, so the image base is added here. Unused by bind slots. |
| `Ordinal`          | `uint16` | Zero-based index into the bind ordinal table. Used by **bind** slots; `0` for rebase slots. |
| `Authenticated`    | `bool`   | Whether the slot carries authenticated pointer metadata (slot bit 63). |
| `Key`              | `uint8`  | Raw 2-bit ptrauth key (bits 49–50), only meaningful when `Authenticated`: `0`=IA, `1`=IB, `2`=DA, `3`=DB. |
| `Diversity`        | `uint16` | Raw 16-bit ptrauth discriminator (bits 32–47), only meaningful when `Authenticated`. |
| `AddressDiversity` | `bool`   | Whether the address discriminator is enabled (bit 48), only meaningful when `Authenticated`. |

Plain records leave `Authenticated == false` and do not fabricate key,
diversity or address-diversity values.

## Slot encoding (eight bytes, little-endian)

All four slot kinds share the chain framing:

| Bits    | Field |
|---------|-------|
| 63      | authenticated flag |
| 62      | kind: `0` rebase, `1` bind |
| 51–61   | `next`: distance to the following slot, in **eight-byte pointer units**; `0` ends the chain |

### Plain rebase (bits 63=0, 62=0)

| Bits    | Field |
|---------|-------|
| 43–50   | high 8 bits of the target |
| 0–42    | low 43 bits, sign-extended |

The target is reassembled as `(high8 << 13) | signExtend43(low43)` and is the
absolute preferred VM address (dyld would add only the runtime slide).

### Authenticated rebase (bits 63=1, 62=0)

| Bits    | Field |
|---------|-------|
| 49–50   | key |
| 48      | address diversity |
| 32–47   | diversity |
| 0–31    | unsigned 32-bit offset relative to the image base |

`Target = image base + low32`.

### Plain bind (bits 63=0, 62=1)

| Bits    | Field |
|---------|-------|
| 32–50   | signed 19-bit slot addend |
| 0–15    | ordinal table index |

The record addend is `template addend + signExtend19(slot addend)`.

### Authenticated bind (bits 63=1, 62=1)

| Bits    | Field |
|---------|-------|
| 49–50   | key |
| 48      | address diversity |
| 32–47   | diversity |
| 0–15    | ordinal table index |

There is no slot addend; the record addend is the template addend unchanged.

## Reading behavior

* `GetRebaseInfo` and `GetBindInfo` keep their signatures and cache their
  results. Calling either first, or calling `GetSlidPointerAtAddress` first,
  produces the same complete records; repeated calls never add entries.
* Reading a rebase slot by address returns the decoded `Target`.
* A self-image (`BIND_SPECIAL_DYLIB_SELF`) bind is resolved through the existing
  symbol lookup; an external bind keeps its raw slot value.
* `GetBindName` still queries legacy streams by the slot's virtual address.
* Pointers that are not part of any chain are read as ordinary values and are
  never interpreted as chain slots.
