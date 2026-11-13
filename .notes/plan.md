# Plan: RabbitizerFlowGraph (delay-slot-aware CFG)

## Requirements (from task)
- New C API `RabbitizerFlowGraph_build`, header `include/analysis/RabbitizerFlowGraph.h`,
  impl `src/analysis/RabbitizerFlowGraph.c`, exported from `rabbitizer.h`.
- Inputs: address-ordered uint32 words, non-wrapping 32-bit start vram, entry vram array.
- CPU instructions only; no code execution; no heap allocation (caller-provided node/edge/diag arrays).
- Nodes = dispatch sites with context: NORMAL vs DELAY_SLOT; delay node carries the chosen successor.
- Conditional branch: two outcomes kept conservatively; normal branches execute slot on both paths;
  likely branches skip slot on not-taken (annul) path straight to PC+8, even if slot word invalid.
- Same instruction reachable as normal entry and as slot must be distinct contexts; normal node
  continues sequentially and never inherits pending jump.
- Loops deduped on finite contexts; entry order/duplicates must not change canonical graph.
- Branches: BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ + likely forms; J/JAL/JR/JALR.
- J target computed with real PC high nibble (PC+4); vram 0 is legal (no 0x80000000 default).
- Slot runs before target; slot writes to condition regs do not re-decide branch.
- JAL/JALR: link reg + PC+8 recorded before slot; JR $ra => return, other JR => unresolved;
  JALR rd=0 => no link; rs==rd => explicit diagnostic; call target/return-site recorded after slot;
  don't follow callees, don't assume return.
- REGIMM link branches, exception returns, other control transfers => unsupported diagnostic.
- Pseudo-instruction config must not affect graph.
- Diagnostics (classified, with branch vram + current vram): missing reachable slot, invalid
  instruction, control transfer inside slot, misaligned/out-of-region target; stop affected path only.
- Stable sorted nodes/edges/diagnostics; nodes copy raw word + decoded identity.
- Tests: tests/c/delay_graph_cases.c registered via support/check.h (task_cases/task_case_count),
  TASK_CHECK per listed nodes/edges. Focus: likely both paths, slot with normal entry,
  slot overwrites indirect target reg, missing slot but annul path kept, back edge, J across
  region boundary. run.sh target links production sources directly.
- Also a caller-buffer C example. C stdlib only. Keep old APIs unchanged.

## Design
- Work item: (index, kind NORMAL/DELAY, pending struct). Pending = branch vram + outcome (taken/not)
  + target vram + kind (conditional outcome / unconditional) + link info for calls.
- Node identity: (vram, kind, branchVram, targetVram). Canonicalize via sorted array + linear scan
  (bounded sizes, no malloc). Nodes sorted: vram, kind, branchVram, targetVram.
- Edges: (fromNodeId, toNodeId, kind enum). Sorted: from, kind, to. Edge kinds:
  SEQUENTIAL, DELAY_ENTER (normal dispatch -> its delay-context node), SLOT_TAKEN, SLOT_NOT_TAKEN,
  LIKELY_ANNUL, JUMP (unconditional dispatch), RETURN (jr ra terminal), UNRESOLVED (jr non-ra terminal),
  CALL (jal/jalr edge into slot's resolved target? see below).
- Terminals: JR/JALR have unresolved target => no target node; edge RETURN/UNRESOLVED with
  toNodeId = RAB_FLOWGRAPH_NODE_ID_NONE. Call info attached on the CALL/SLOT edge metadata or node.
- Diagnostics enum: MISSING_SLOT, INVALID_INSTRUCTION, CONTROL_IN_SLOT, TARGET_MISALIGNED,
  TARGET_OUT_OF_REGION, UNSUPPORTED_CONTROL, JALR_RS_EQUALS_RD. Fields: kind, branchVram, currentVram.
- Region: [startVram, startVram + 4*wordCount); non-wrapping checked up front.
- Decode once per index into local RabbitizerInstruction (stack), with pseudos disabled around
  decode and restored after (save/restore RabbitizerConfig_Cfg.pseudos.enablePseudos) so graph
  is pseudo-independent. Validity via RabbitizerInstruction_isValid.
- J target: (instr_index<<2) | ((pc+4) & 0xF0000000) — own helper since vram=0 special case in lib.
- JALR: rd field; link reg = rd (unless 0); rs==rd diagnostic. JAL link reg = 31.
- Capacity failure => RAB_FLOWGRAPH_ERROR_CAPACITY, graph marked incomplete (complete=false).

## Files
1. include/analysis/RabbitizerFlowGraph.h — types + build entry
2. src/analysis/RabbitizerFlowGraph.c — implementation
3. include/rabbitizer.h — export new header
4. tests/c/delay_graph_cases.c — TaskCase regression suite
5. examples/c/flowgraph_example.c — caller-allocated buffer example
6. .notes/plan.md — this plan

## Verification
- sh support/run.sh baseline
- sh support/run.sh target
- cc -Wall -Wextra -Wpedantic -Werror build of new sources
