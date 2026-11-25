# memanalysis example

This example runs `experimental/memanalysis` over the controlled sample in
`samples/crosscall.wasm` with the sample author's manifest in
`samples/crosscall.json`, and prints the complete JSON analysis report.

From the repository root:

```bash
go run ./examples/memanalysis
```

Optional flags allow pointing at other inputs:

```bash
go run ./examples/memanalysis -wasm path/to/sample.wasm -plan path/to/plan.json
```

## What the report means

The analysis is a **manifest-constrained dynamic boundary analysis**:

- Object boundaries, input sources and intended write targets come from the
  trusted `Plan`, not from the Wasm binary. WebAssembly itself has no notion of
  objects inside linear memory.
- Only the calls listed in the plan are executed, on wazero's interpreter
  engine with per-value and per-byte provenance tracking.
- Every executed `i32.store`, `i32.store8`, `i32.store16`, `memory.copy` and
  `memory.fill` becomes an event classified as `legal`, `object-overflow` or
  `linear-oob`, with the address, length and data origins and, for overflow
  events, the overlapping victim objects.

It cannot prove that arbitrary WebAssembly is safe, and it does not imply that
the module carries any object boundary of its own. A clean report only
describes the behavior of the listed calls under the supplied manifest.
