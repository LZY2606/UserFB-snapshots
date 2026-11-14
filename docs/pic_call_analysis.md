# PIC call-source recovery (MIPS32 O32)

The `RabbitizerPicCallAnalysis` API recovers the source of every reachable
function call inside one MIPS32 O32 position-independent function. It is a
pure, read-only analysis:

- machine code words are decoded and simulated abstractly but **never executed**;
- **no ELF file is parsed** — the caller supplies the graph, relocations and GOT
  map up front;
- the input graph, relocations and GOT map are never modified.

All declarations live in `analysis/RabbitizerPicCallAnalysis.h`, which is also
pulled in by `rabbitizer.h`.

## 1. Inputs

### Control flow graph (`RabbitizerPicGraph`)

The caller provides an already-recovered CFG as flat node and edge arrays.

- Every node executes exactly one instruction (`word` plus its `vram`).
- A call node (`RABBITIZER_PIC_NODE_CALL`) holds `jal`/`jalr` and points at
  exactly one ordinary delay-slot node through `delaySlotNode`.
- Ordinary branch delay slots and branch-likely annul paths are already expanded
  into ordinary nodes and edges, so the analyzer neither rebuilds the CFG nor
  evaluates branch conditions. Every edge is one possible path.
- Edges leaving a call node represent the continuation **after the call
  returns**.
- Nodes unreachable from `entryNodeIndex` are never analyzed.
- Each node owns a contiguous edge range `[firstEdge, firstEdge + edgeCount)`.

At function entry `$t9` holds the function entry address (`entryVram`), `$zero`
is the constant zero, and every other GPR is unknown.

### Relocations (`RabbitizerPicRelocation`)

Relocations are matched to instruction nodes by **node index** and to symbols
by **symbol table index** (`objectId` + `symbolIndex`), never by display name.
This keeps two same-named symbols in different objects distinguishable.

Supported kinds:

| Kind | Meaning |
| --- | --- |
| `RABBITIZER_PIC_RELOC_GP_DISP_HI16` / `..._LO16` | the in-function `_gp_disp` pair; the LO record names the HI node in `pairNodeIndex` |
| `RABBITIZER_PIC_RELOC_CALL16` / `..._GOT16` | GP-relative GOT loads |
| `RABBITIZER_PIC_RELOC_HI16` / `..._LO16` | generic symbolic `%hi`/`%lo` pair |
| `RABBITIZER_PIC_RELOC_LOCAL_LO16` | local `%lo` adjustment of a GOT-derived value |

The explicit `addend`, the owning object and the pairing source are all caller
data. Implicit ELF addends are not consulted. A local `LO16` is only combined
with a GOT value whose symbol identity is identical.

### GOT map (`RabbitizerPicGotSlot`)

Each entry gives the owning object, the slot's **signed offset relative to GP**,
the identity of the value stored in the slot and how that value is produced
(`CALL16`, `GOT16` or `LOCAL`). A plain `lw` without a `CALL16`/`GOT16`
relocation can never resolve into a GOT symbol just because an offset matches.

## 2. Abstract value model

Every GPR holds a `RabbitizerPicValue`:

| Kind | Meaning |
| --- | --- |
| `UNKNOWN` | no fact; carries accumulated reason bits, origin is `-1` |
| `CONST` | a pure 32-bit constant |
| `ENTRY` | the value `$t9` arrives with (`constValue == entryVram`) |
| `GP_PARTIAL` | a `_gp_disp` hi/lo accumulation not yet joined with entry `$t9` |
| `GP` | a complete, consistently associated GP for one `objectId` |
| `GOT` | a value loaded from a mapped GOT slot; symbol identity is valid |
| `SYMBOL` | a generic hi/lo value or a local-`%lo`-adjusted GOT value |

Values keep their identity, explicit addend and producing node. All 32-bit
arithmetic wraps modulo 2^32.

The modeled register operations are deliberately narrow:

- `lui` introduces a constant or a relocated `GP_PARTIAL`/`SYMBOL` upper half;
- `addiu` adds a constant offset, or completes a relocated lo half;
- `addu` performs zero-register copies, constant folding, or the
  `GP_PARTIAL + ENTRY` join that produces a usable `GP`;
- `or` (and the disassembler's `move` pseudo) only copies through the real zero
  register;
- `lw` resolves a GOT slot only for a complete GP with a matching relocation;
- any other instruction that writes a GPR kills that register to `UNKNOWN`.

A GP is usable for GOT loads only when the whole `lui`/`addiu`/`addu` cpload
chain is present and its object association is consistent.

## 3. Fixpoint and the state cap

Each node holds a set of **full states** (all 32 GPRs together). States are
merged by whole-state equality, so a `callee` and its `a0`–`a3` correlations
stay paired on the same path — there is no per-register Cartesian product, and
unknown paths are retained rather than discarded.

A worklist algorithm runs until every node's state set reaches a fixpoint, so
loops terminate. `RabbitizerPicAnalysisConfig.maxStateCount` bounds the number
of full states per node:

- when a node would exceed the cap its set collapses into a single explicit
  **saturated unknown wildcard** (the `STATE_LIMIT` reason bit is set);
- the wildcard still reaches every successor, so no path is dropped because of
  traversal order;
- definite writes on later nodes overwrite the wildcard, so a fact assigned
  after saturation is recovered.

`RabbitizerPicCallResult.saturated` reports whether the cap was hit anywhere and
`peakStateCount` reports the largest state set observed.

## 4. Call semantics

For each reachable call node and each incoming full state:

1. the **callee** is the call's source register value read *before* the delay
   slot (a slot rewrite of `$t9` cannot change it); a direct `jal` instead keeps
   its static target and does not follow into the callee;
2. the link register receives `PC+8`, then the single delay-slot instruction
   executes;
3. the four arguments are `a0`–`a3` read *after* the slot, so a slot assignment
   is visible in the arguments;
4. the post-return continuation state applies the O32 caller-saved clobber and
   also destroys `$gp`; callee-saved registers (`$s0`–`$s7`, `$sp`, …) survive,
   which is what lets GP saved to `$s0` be restored for later GOT loads.

Observations are de-duplicated by full content and returned in a stable order
(`RabbitizerPicCallResult.records`). Each record carries `unknownReasons`, a
bitmask of the `RABBITIZER_PIC_REASON_*` values describing exactly which part
was not recoverable and why.

## 5. Minimal example

```c
#include "rabbitizer.h"
#include <stddef.h>
#include <stdint.h>

/*
 * lui  $gp, 0x1234            ; GP_DISP_HI16
 * addiu $gp, $gp, -16         ; GP_DISP_LO16 (paired with node 0)
 * addu $gp, $gp, $t9
 * lw   $t9, -32752($gp)       ; CALL16 symbol 5
 * jalr $t9
 *  nop                        ; delay slot (node 5)
 *  nop                        ; post-return continuation (node 6)
 */
int main(void) {
    enum { NODE_COUNT = 7, EDGE_COUNT = 5 };
    RabbitizerPicGraphNode nodes[NODE_COUNT] = {0};
    RabbitizerPicGraphEdge edges[EDGE_COUNT];
    RabbitizerPicRelocation relocs[3];
    RabbitizerPicGotSlot slots[1];
    RabbitizerPicCallRecord records[16];
    RabbitizerPicCallResult result = { records, 16, 0, 0, false };
    RabbitizerPicAnalysisConfig config = { 64 };
    RabbitizerPicGraph graph;
    size_t i;

    uint32_t words[6] = {
        0x3c1c1234u, 0x279cfff0u, 0x0399e021u, 0x8f998010u,
        0x0320f809u, 0x00000000u,
    };
    for (i = 0; i < 6; i++) {
        nodes[i].word = words[i];
        nodes[i].vram = 0x00401000u + (uint32_t)i * 4u;
        nodes[i].delaySlotNode = RABBITIZER_PIC_NO_NODE;
    }
    nodes[6].word = 0x00000000u; // continuation nop
    nodes[6].vram = 0x00401018u;
    nodes[4].kind = RABBITIZER_PIC_NODE_CALL;
    nodes[4].delaySlotNode = 5; // the call owns its single delay slot node

    // Straight-line edges for nodes 0..3; the call edge goes to the continuation.
    for (i = 0; i < 4; i++) {
        edges[i].fromNodeIndex = i;
        edges[i].toNodeIndex = i + 1;
        nodes[i].firstEdge = i;
        nodes[i].edgeCount = 1;
    }
    edges[4].fromNodeIndex = 4;
    edges[4].toNodeIndex = 6;
    nodes[4].firstEdge = 4;
    nodes[4].edgeCount = 1;
    nodes[5].firstEdge = 5; // delay slot node has no edges
    nodes[5].edgeCount = 0;
    nodes[6].firstEdge = 5; // continuation has no successor in this snippet
    nodes[6].edgeCount = 0;

    relocs[0] = (RabbitizerPicRelocation){ 0, RABBITIZER_PIC_RELOC_GP_DISP_HI16, {7, 90}, 0,
                                           RABBITIZER_PIC_NO_NODE };
    relocs[1] = (RabbitizerPicRelocation){ 1, RABBITIZER_PIC_RELOC_GP_DISP_LO16, {7, 90}, 0, 0 };
    relocs[2] = (RabbitizerPicRelocation){ 3, RABBITIZER_PIC_RELOC_CALL16, {7, 5}, 0,
                                           RABBITIZER_PIC_NO_NODE };
    slots[0] = (RabbitizerPicGotSlot){ 7, -32752, {7, 5}, RABBITIZER_PIC_GOT_SOURCE_CALL16 };

    graph = (RabbitizerPicGraph){ nodes, NODE_COUNT, edges, EDGE_COUNT, 0, 0x00401000u };

    RabbitizerPicAnalysisStatus status =
        RabbitizerPicAnalysis_analyze(&graph, relocs, 3, slots, 1, &config, &result);
    if (status != RABBITIZER_PIC_ANALYSIS_OK) {
        return 1;
    }
    /* result.records[0].callee.kind == RABBITIZER_PIC_VALUE_GOT, symbolIndex == 5 */
    return (int)result.recordCount == 1 ? 0 : 1;
}
```

Build against the static library:

```sh
cc -std=c11 -Iinclude -Itables example.c build/librabbitizer.a -o example
```

## 6. Status codes

| Status | Meaning |
| --- | --- |
| `RABBITIZER_PIC_ANALYSIS_OK` | success |
| `..._ERROR_NULL` | a required array/pointer is missing or the result has no capacity |
| `..._ERROR_ENTRY` | graph missing, or `entryNodeIndex` out of range |
| `..._ERROR_NODE` / `..._ERROR_EDGE` | malformed node or edge reference |
| `..._ERROR_RELOC` | a relocation references an unknown node or pair |
| `..._ERROR_CAPACITY` | more distinct call records than the provided result capacity |

## 7. Scope and limitations

- Only the GPR operations needed for O32 PIC cpload/GOT/call recovery are
  modeled; other GPR writes become `UNKNOWN` rather than guessed values.
- Floating-point, HI/LO accumulator and privileged state are not tracked.
- Callees are never followed; only the static target of a direct `jal` is
  recorded.
- The analysis assumes the caller's graph is sound; it validates index ranges
  but does not reconstruct missing edges or delay slots.
