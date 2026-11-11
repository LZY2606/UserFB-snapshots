Powerful Disassembler Library For x86/AMD64
-----------

Welcome to the diStorm3 binary stream disassembler library project.

diStorm3 is really a decomposer, which means it takes an instruction and returns a binary structure which describes it rather than static text, which is great for advanced binary code analysis.

diStorm3 is super lightweight (~45KB), ultra fast and easy to use (a single API), licensed under BSD!

For a light hooking library see the https://github.com/gdabah/distormx project.

"We benchmarked five popular open-source disassembly libraries and chose diStorm3, which had the best performance (and furthermore, has complete 64-bit support).", July 2014, Quoting David Williams-King in his Thesis about Binary Shuffling.

Installing diStorm3 -
'python -m pip install distorm3'

RTFM, the wiki has plenty of info.

Control-flow analysis (optional) -
include/flow.h + src/flow.c add distorm_flow_analyze(), which recovers the
basic blocks reachable from a set of entry points (buffer offsets) using the
regular distorm_decompose() decoder. It reports each block's offset range, its
raw _DInst sequence and its typed out-edges (taken / call / fallthrough /
unresolved), plus diagnostics for mid-instruction start-point conflicts and
undecodable bytes. The caller's code buffer is only borrowed for the duration
of the call; the returned FlowGraph is owned by the caller and must be
released with distorm_flow_free(). See include/flow.h for the full contract
and tests/flow_cases.c for examples (run: bash tests/run_flow.sh).
