These are finite, local Wasm inputs, not an implementation of the analyzer.

From the source root, regenerate with:

    go run -buildvcs=false ./samples/generate

crosscall.json uses the public Plan JSON layout. Sources.Bytes and final snapshot bytes use encoding/json's normal base64 representation of []byte. Targets identify the sample author's intended object for each write opcode; offsets exclude the local declaration vector. The generated Wasm has no imports or start function, one unshared memory (1 to 2 pages), and i32-only functions.

The inputs include persistent state, a direct helper call, a narrow write, overlapping copy and fill. The sequence also contains a linear-memory trap; the analyzer should continue to the next listed call. No expected analysis classifications, provenance sets, or report JSON are bundled. Add independent inputs in the new contract tests.

## Analysis entry point

`experimental/memanalysis` consumes these inputs through
`memanalysis.Analyze(ctx, binary, plan)`; `examples/memanalysis` runs the
crosscall sample and prints the full JSON report.

The analysis is a **manifest-constrained dynamic boundary analysis**. The
object intervals, input sources and intended write targets are trusted
side-channel information supplied by the sample author: WebAssembly modules do
not carry object boundaries inside linear memory, and the analyzer never
infers an object from an observed address. Only the calls listed in the plan
are executed, so a report describes just those executions under that manifest.
It cannot prove that arbitrary WebAssembly is safe, and an event classified as
`legal` means only that the write stayed inside the object the manifest bound
it to and within the real linear memory.

