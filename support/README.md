# ID3v2 unsynchronisation regression harness

`target_cases.cpp` is a self-contained regression for the two ID3v2
unsynchronisation read paths:

* **ID3v2.3** restores unsynchronisation once, over the whole tag body.
  Frame lengths inside the body are ordinary big-endian integers holding the
  restored (pre-escaping) content length; the tag header size counts the bytes
  actually stored on disk.
* **ID3v2.4** restores unsynchronisation per frame. A frame's 4-byte frame
  length is the **on-disk** (escaped) body length used to find the next frame;
  when a data length indicator (DLI) is present, its 4-byte syncsafe prefix
  holds the **restored** body length instead. Both a tag-level and a
  frame-level unsynchronisation flag may be set, in which case the frame body
  is restored exactly once.

All wire material is generated in C++ by small, independent writers for
ordinary big-endian and 7-bit syncsafe integers plus an unsynchronisation
encoder. No TagLib `render()`, `SynchData::fromUInt()` or `SynchData::decode()`
is used to compute an expectation. Every assertion compares raw content bytes
(via `ByteVector` / `String::data()`), not just string lengths or a
render/read round trip.

## Running

```sh
bash support/target.sh
```

This builds the library, compiles `support/target_main.cpp` together with
`target_cases.cpp`, checks that all four groups (`tag23`, `frame24`, `lengths`,
`neighbors`) ran non-empty assertions, runs the existing text-render baseline,
and then builds and runs the three behaviour-mutated versions in
`support/mutant-*.patch`. Each mutant must exit with a semantic `FB_CHECK`
failure in `target_cases.cpp`, not a compile error, crash or zero-case run.

The per-case groups:

| group | what it covers |
| --- | --- |
| `tag23` | whole-body restoration, 127 / 16383 syncsafe boundaries, plain tags |
| `frame24` | tag, frame and both flags; DLI; factory-only comparisons |
| `lengths` | DLI vs. stored-length conventions across both boundaries, adjacent frames, and the externally visible stop/reject rules for truncated frames, DLI-only frames and invalid frame IDs |
| `neighbors` | binary PRIV/unknown content followed by recognisable text frames, plus v2.3/v2.4 render-then-read checks |
