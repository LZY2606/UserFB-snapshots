# x87 stack shape analysis

`inc/bdx86_x87.h` (implemented in `bddisasm/bdx86_x87.c`, part of the `bddisasm` CMake target)
provides a static, value-independent analysis of the x87 register stack. It answers the question
"what does the x87 stack look like at each reachable instruction?" without computing any floating
point values: it tracks only the TOP pointer and the set of physical slots whose tag is not empty.

The analyzer:

* works on raw machine code, decoded with `NdDecodeEx`;
* never reads memory as data and never executes the input;
* does not model exception handling, masked-exception results, or floating point rounding — a
  diagnostic means "this stack operation cannot complete normally on this shape";
* resolves the logical `st0`..`st7` names produced by the decoder against the TOP value of the
  shape that is active when the instruction executes, so `st0` is never confused with a fixed
  physical register.

## Instruction-level interface

```c
INSTRUX ix;
NDSTATUS st = NdDecodeEx(&ix, bytes, size, ND_CODE_64, ND_DATA_64);
BDX87_STACK_ACTION action;
st = BdX87GetStackAction(&ix, &action);
```

`BdX87GetStackAction` decomposes one decoded instruction into an ordered list of stack operations
(`BDX87_SOP_READ`, `WRITE`, `PUSH`, `POP`, `FREE`, `ROTATE`, `RESET`) expressed in *logical*
`st(i)` indices. The ordering matches the hardware semantics, which is what a raw register access
map cannot express:

* `FLD st(i)` reads the source under the old TOP, then pushes;
* `FSTP st(i)` writes the destination under the old TOP, then clears the old `st0` and pops
  (including `i == 0`);
* `FFREE` clears only the tag; `FINCSTP`/`FDECSTP` rotate only TOP;
* `FADDP`/`FMULP` read both sources and write the destination back before popping;
* `FNINIT` resets TOP to 0 and empties all tags.

Modeled instructions: `FLD` (memory and `st(i)`), `FLD1`, `FLDZ`, `FST`/`FSTP` (memory and
`st(i)`), `FXCH`, `FFREE`, `FINCSTP`, `FDECSTP`, `FNINIT`, `FADD`/`FMUL` and their popping forms.
Plain integer instructions yield a supported, empty action. Any other x87 instruction, any MMX
instruction, and instructions that restore the full FP state (`FRSTOR`, `XRSTOR`, ...) yield
`Supported == ND_FALSE` — their effect on the stack is not assumed to be nil.

## Whole-region analysis

```c
BDX87_REPORT *report = NULL;
NDSTATUS st = BdX87AnalyzeCode(code, codeSize, baseAddress,
                               entryOffset, initialTop, initialTagMask,
                               &report);
...
BdX87FreeReport(report);
```

The input is a read-only contiguous byte buffer, its runtime base address, a single entry offset,
the initial TOP (0-7) and the initial 8-bit physical non-empty tag mask. Exploration follows real
instruction lengths:

* direct `JMP` follows only the target; `Jcc` follows both edges (conditions are never guessed);
* `RET` and edges leaving the buffer terminate a path;
* `CALL` and indirect branches are located barriers: they are not followed, and the post-return
  stack shape is not guessed;
* branch targets landing in the middle of an instruction, truncated instructions and invalid
  encodings are reported (`BDX87_DIAG_BAD_TARGET`, `BDX87_DIAG_BAD_ENCODING`).

Every program point keeps the full set of reachable `(TOP, tag mask)` shapes; joins union whole
shape pairs, never TOP and tags separately. Loops converge because the shape space is finite
(8 x 256) and already-seen shapes are not expanded again.

For every reachable instruction and every reachable incoming shape, the report contains the entry
shape, the successor shape, the physical slots actually read/written/cleared, and the TOP delta.
Reading an empty slot records `BDX87_DIAG_UNDERFLOW`; pushing onto a non-empty slot records
`BDX87_DIAG_OVERFLOW` — both name the physical slot, stop that shape's successors, and leave the
other shapes to be analyzed normally.

Instructions, shapes and diagnostics are stably sorted, so analyzing the same bytes with the same
initial shape always produces the same report. The report is independently owned: it stays valid
after the input buffer is freed, and `BdX87FreeReport` releases it. On failure no borrowed
pointers are returned.

## Example

`examples/x87_shape_dump.c` prints the per-instruction shapes and slot masks for a small
hand-written function:

```sh
cc -std=c99 -DBDDISASM_HAS_MEMSET -DBDDISASM_HAS_VSNPRINTF -Iinc -Ibddisasm/include \
   examples/x87_shape_dump.c bddisasm/*.c -o x87_shape_dump
./x87_shape_dump
```
