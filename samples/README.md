These are finite, local Wasm inputs, not an implementation of the analyzer.

From the source root, regenerate with:

    go run -buildvcs=false ./samples/generate

crosscall.json uses the public Plan JSON layout. Sources.Bytes and final snapshot bytes use encoding/json's normal base64 representation of []byte. Targets identify the sample author's intended object for each write opcode; offsets exclude the local declaration vector. The generated Wasm has no imports or start function, one unshared memory (1 to 2 pages), and i32-only functions.

The inputs include persistent state, a direct helper call, a narrow write, overlapping copy and fill. The sequence also contains a linear-memory trap; the analyzer should continue to the next listed call. No expected analysis classifications, provenance sets, or report JSON are bundled. Add independent inputs in the new contract tests.
