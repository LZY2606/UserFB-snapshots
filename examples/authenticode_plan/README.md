# Authenticode image-hash planning example

`main.cpp` shows how to use `BuildAuthenticodePlan` and
`FeedAuthenticodePlan` to determine exactly which bytes of an on-disk PE
enter the Authenticode image hash, in feed order, without pulling any
cryptography into the library and without loading or executing the image.

The program parses a PE, prints the plan (ordered hash spans, excluded header
fields, parsed `WIN_CERTIFICATE` entries), destroys the parsed PE, then feeds
the same raw file through the plan. The callback accumulates a non
cryptographic FNV-1a value only to demonstrate delivery; a real integrity
check would forward the same calls to its own hash and verify the signature
with a separate, trusted stack.

Like `peaddrconv`, this example is not part of the main build. After
installing the library it can be built standalone:

```sh
cmake -S examples/authenticode_plan -B examples_build/authenticode_plan
cmake --build examples_build/authenticode_plan
./examples_build/authenticode_plan/authenticode_plan path/to/file.exe
```

## What the plan contains

- **Ordered hash spans** with file offset, length, stage (header, section or
  trailing) and the originating section-table index.
- **Excluded header ranges** for the 4-byte `CheckSum` and, only when the
  Certificate Table directory entry is declared, its 8 bytes.
- **Certificate entries** with revision/type, the raw entry range, the
  payload range (padding excluded) and the 8-byte-rounded storage range.

The feed order follows the simplified PE image-hash procedure: the
`SizeOfHeaders` range minus the excluded fields, then every non-empty
section's full `SizeOfRawData` ordered by `PointerToRawData`, then one
trailing range starting at the accumulated `SUM_OF_BYTES_HASHED`. Because
`SUM_OF_BYTES_HASHED` is an accumulated size rather than the physical end of
the last section, inter-section gaps can make the trailing range re-cover
already-fed bytes. That ordered duplication is preserved on purpose; spans
are never coalesced.

## Supported layout and limitations

- Every offset is interpreted as a raw **file offset**. The Certificate Table
  directory's address is never passed through RVA mapping.
- The Certificate Table, when present, must be 8-byte aligned, located at the
  end of the file, and free of overlap with the headers and section data.
- A declared directory whose offset and size are both zero means "no embedded
  certificates"; the 8-byte directory entry is still excluded. If the entry is
  not declared (`NumberOfRvaAndSizes <= 4`), those eight bytes are treated as
  ordinary content and are not excluded. Exactly one of offset/size being zero
  is rejected.
- Each `WIN_CERTIFICATE` must have `dwLength >= 8`; entries are walked from one
  8-byte-rounded end to the next. Unknown revision/type values are kept as
  opaque entries. Alignment padding is never part of the payload. Truncated
  headers, out-of-table lengths and rounding overflows are rejected.
- Non-empty section raw ranges must lie inside the file, must not overlap each
  other, and must not intrude into the header range or the certificate table.
  Inter-section gaps are allowed; zero `SizeOfRawData` sections contribute no
  hash input.
- Rejections report the structural category and the offending file position,
  and never return a half-populated plan.
- A plan is bound to the file length and to the layout descriptors (header,
  section table, certificate directory). `FeedAuthenticodePlan` re-verifies
  them against the supplied bytes before any callback runs, so a
  layout-modified file cannot be paired with a stale plan. Content-only edits
  (section bytes, `CheckSum`, certificate payloads) reuse the same plan;
  editing `CheckSum` or a certificate payload does not change the fed byte
  stream.
