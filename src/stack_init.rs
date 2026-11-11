// SPDX-License-Identifier: Apache-2.0 OR MIT

//! Restricted, flow-sensitive check that every byte read from the eBPF stack
//! has been written on every path leading to the read.
//!
//! This is *not* a replacement for the Linux kernel verifier. It only models a
//! single function with one 512-byte stack frame, tracks the stack pointer
//! (`r10`) and its derived copies, and performs a definite-initialization
//! analysis over the control-flow graph. Anything outside of its model
//! (reachable helper calls, local function calls, atomic memory operations,
//! arithmetic that would destroy the stack pointer meaning, …) is rejected
//! explicitly instead of being silently ignored.

use crate::ebpf;
use crate::lib::*;

const STACK_BYTES: usize = ebpf::STACK_SIZE;
const NREGS: usize = 11;

// Concrete offsets are kept only while they stay inside the frame or at its
// upper edge (r10 itself, offset 0). A pointer moved outside this window can
// no longer be related to a definite frame position, so it collapses to the
// ambiguous "top" value; this keeps the abstract lattice finite (at most
// STACK_BYTES concrete offsets plus top) so loops are guaranteed to converge
// quickly.
const MIN_FRAME_OFFSET: i32 = -(STACK_BYTES as i32);
const MAX_FRAME_OFFSET: i32 = 0;

fn reject<T, S: AsRef<str>>(msg: S) -> Result<T, Error> {
    Err(Error::other(format!("[Verifier] Error: {}", msg.as_ref())))
}

/// What a register is known to hold.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
enum RegValue {
    /// Unknown scalar; known not to hold a stack-derived value.
    #[default]
    Scalar,
    /// Candidate stack pointers, expressed as signed offsets from `r10`
    /// (which points one byte past the end of the 512-byte stack). An empty
    /// vector is the ambiguous top value: stack-derived on some path, but its
    /// offset cannot be relied upon.
    Ptr(Vec<i32>),
}

/// What an eight-byte stack slot is known to contain.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
enum SlotValue {
    /// Ordinary data: no stack pointer bytes are known to live here.
    #[default]
    Scalar,
    /// Holds a stack pointer covering all eight bytes; an empty vector is the
    /// ambiguous top value.
    Ptr(Vec<i32>),
    /// Pointer bytes are present but the slot is not a fully covered pointer
    /// value. Such a slot can never yield a usable pointer or a clean scalar
    /// again.
    Poisoned,
}

impl SlotValue {
    /// Lattice merge: joining two slot states coming from different paths.
    fn merge(self, other: SlotValue) -> SlotValue {
        use SlotValue::*;
        match (self, other) {
            (Poisoned, _) | (_, Poisoned) => Poisoned,
            (Scalar, Scalar) => Scalar,
            (Ptr(mut a), Ptr(b)) => {
                if a.is_empty() || b.is_empty() {
                    Ptr(Vec::new())
                } else {
                    merge_offsets(&mut a, &b);
                    Ptr(a)
                }
            }
            // A pointer slot joined with a scalar slot cannot be relied upon:
            // the slot may be a plain value, so the pointer provenance is
            // lost (and pointer bytes may still be there).
            (Ptr(_), Scalar) | (Scalar, Ptr(_)) => Poisoned,
        }
    }
}

fn merge_offsets(into: &mut Vec<i32>, other: &[i32]) {
    for off in other {
        if !into.contains(off) {
            into.push(*off);
        }
    }
    into.sort_unstable();
}

/// Abstract state at an instruction boundary.
#[derive(Clone, Debug, PartialEq, Eq)]
struct State {
    /// Per-register abstract value.
    regs: [RegValue; NREGS],
    /// Per-byte definite initialization bit.
    init: [bool; STACK_BYTES],
    /// One entry per eight-byte slot (aligned to eight bytes from the frame
    /// base); tracks stack pointer provenance stored on the stack.
    slots: [SlotValue; STACK_BYTES / 8],
}

impl State {
    fn entry() -> State {
        let mut regs: [RegValue; NREGS] = core::array::from_fn(|_| RegValue::Scalar);
        // r10 points one past the end of the stack: offset 0 relative to r10.
        regs[10] = RegValue::Ptr(vec![0]);
        State {
            regs,
            init: [false; STACK_BYTES],
            slots: core::array::from_fn(|_| SlotValue::Scalar),
        }
    }

    fn merge(&mut self, other: &State) {
        for (r, o) in self.regs.iter_mut().zip(other.regs.iter()) {
            *r = merge_reg(core::mem::take(r), o.clone());
        }
        // A byte is definitely initialized only when initialized on every
        // incoming path: join takes the intersection.
        for (b, o) in self.init.iter_mut().zip(other.init.iter()) {
            *b &= *o;
        }
        for (s, o) in self.slots.iter_mut().zip(other.slots.iter()) {
            *s = core::mem::take(s).merge(o.clone());
        }
    }
}

fn merge_reg(a: RegValue, b: RegValue) -> RegValue {
    use RegValue::*;
    match (a, b) {
        (Scalar, Scalar) => Scalar,
        (Ptr(mut x), Ptr(y)) => {
            if x.is_empty() || y.is_empty() {
                Ptr(Vec::new())
            } else {
                merge_offsets(&mut x, &y);
                Ptr(x)
            }
        }
        // A stack pointer on some path and a scalar on another: the register
        // cannot be used as a clean pointer nor washed into a clean scalar.
        (Ptr(_), Scalar) | (Scalar, Ptr(_)) => Ptr(Vec::new()),
    }
}

/// Translate a relative-to-r10 offset into a frame byte index (0..512).
fn frame_index(off: i32) -> Option<usize> {
    let idx = i64::from(off) + i64::try_from(STACK_BYTES).unwrap();
    usize::try_from(idx).ok().filter(|i| *i < STACK_BYTES)
}

/// Checked pointer arithmetic: the offset must not wrap the 64-bit space.
fn apply_delta(base: i32, delta: i64) -> Option<i32> {
    i64::from(base).checked_add(delta)?.try_into().ok()
}

/// True if the offset still names a position inside (or at the edge of) the
/// frame and can therefore be represented concretely.
fn offset_is_useful(off: i32) -> bool {
    (MIN_FRAME_OFFSET..=MAX_FRAME_OFFSET).contains(&off)
}

struct Analyzer<'a> {
    prog: &'a [u8],
    nb_insns: usize,
    states: Vec<Option<State>>,
    /// BFS worklist, implemented as a vector with a moving head so that error
    /// reporting follows program order and stays deterministic.
    worklist: Vec<usize>,
    head: usize,
}

impl<'a> Analyzer<'a> {
    fn new(prog: &'a [u8]) -> Self {
        let nb_insns = prog.len() / ebpf::INSN_SIZE;
        Analyzer {
            prog,
            nb_insns,
            states: (0..nb_insns).map(|_| None).collect(),
            worklist: Vec::new(),
            head: 0,
        }
    }

    fn insn(&self, idx: usize) -> ebpf::Insn {
        ebpf::get_insn(self.prog, idx)
    }

    /// Push `state` to node `target`, merging it with any state already there.
    fn propagate(&mut self, target: usize, state: State) {
        match self.states[target].as_mut() {
            Some(existing) => {
                let before = existing.clone();
                existing.merge(&state);
                if existing != &before {
                    self.worklist.push(target);
                }
            }
            None => {
                self.states[target] = Some(state);
                self.worklist.push(target);
            }
        }
    }

    fn pop_node(&mut self) -> Option<usize> {
        if self.head < self.worklist.len() {
            let pc = self.worklist[self.head];
            self.head += 1;
            Some(pc)
        } else {
            None
        }
    }

    fn run(mut self) -> Result<(), Error> {
        self.propagate(0, State::entry());
        while let Some(pc) = self.pop_node() {
            let state = self.states[pc].clone().unwrap();
            let insn = self.insn(pc);
            self.step(pc, &insn, state)?;
        }
        Ok(())
    }

    fn step(&mut self, pc: usize, insn: &ebpf::Insn, mut state: State) -> Result<(), Error> {
        let dst = insn.dst as usize;
        let src = insn.src as usize;
        let next = pc + 1;

        // Node reached by a relative jump (offset relative to the next
        // instruction), validated as a real instruction boundary.
        let jump_target = |off: i16| -> Result<usize, Error> {
            let target = i64::try_from(pc).unwrap() + 1 + i64::from(off);
            let target = usize::try_from(target).map_err(|_| {
                Error::other(format!("[Verifier] Error: jump out of code at insn #{pc}"))
            })?;
            if target >= self.nb_insns || self.insn(target).opc == 0 {
                return reject(format!(
                    "jump lands outside of an instruction boundary at #{target} (insn #{pc})"
                ));
            }
            Ok(target)
        };

        let fallthrough = |this: &mut Self, state: State| {
            if next < this.nb_insns {
                this.propagate(next, state);
            }
        };

        match insn.opc {
            // ---- Immediate loads -------------------------------------------------
            ebpf::LD_DW_IMM => {
                // The format verifier guarantees the following word exists and
                // holds opcode 0; the second half is not an analysis node.
                if next >= self.nb_insns || self.insn(next).opc != 0 {
                    return reject(format!("incomplete LD_DW instruction (insn #{pc})"));
                }
                state.regs[dst] = RegValue::Scalar;
                if next + 1 < self.nb_insns {
                    self.propagate(next + 1, state);
                }
            }

            // ---- Register loads --------------------------------------------------
            ebpf::LD_B_REG | ebpf::LD_H_REG | ebpf::LD_W_REG | ebpf::LD_DW_REG => {
                let size = load_size(insn.opc)?;
                self.check_read(pc, &state, src, insn.off, size)?;
                state.regs[dst] = self.load_value(pc, &state, src, insn.off, size)?;
                fallthrough(self, state);
            }

            // ---- Immediate stores ------------------------------------------------
            ebpf::ST_B_IMM | ebpf::ST_H_IMM | ebpf::ST_W_IMM | ebpf::ST_DW_IMM => {
                let size = store_size(insn.opc)?;
                self.check_write(pc, &state, dst, insn.off, size)?;
                self.mark_scalar_store(&mut state, dst, insn.off, size);
                fallthrough(self, state);
            }

            // ---- Register stores -------------------------------------------------
            ebpf::ST_B_REG | ebpf::ST_H_REG | ebpf::ST_W_REG | ebpf::ST_DW_REG => {
                let size = store_size(insn.opc)?;
                self.check_write(pc, &state, dst, insn.off, size)?;
                let stored = state.regs[src].clone();
                self.mark_reg_store(&mut state, dst, insn.off, size, &stored);
                fallthrough(self, state);
            }

            // ---- Atomic memory operations: explicitly unsupported ---------------
            ebpf::ST_W_XADD | ebpf::ST_DW_XADD => {
                return reject(format!(
                    "atomic memory operations are not supported by the stack initialization checker (insn #{pc})"
                ));
            }

            // ---- 32-bit ALU: truncation can never preserve provenance ------------
            ebpf::ADD32_IMM
            | ebpf::SUB32_IMM
            | ebpf::MUL32_IMM
            | ebpf::DIV32_IMM
            | ebpf::OR32_IMM
            | ebpf::AND32_IMM
            | ebpf::LSH32_IMM
            | ebpf::RSH32_IMM
            | ebpf::NEG32
            | ebpf::MOD32_IMM
            | ebpf::XOR32_IMM
            | ebpf::MOV32_IMM
            | ebpf::ARSH32_IMM => {
                if matches!(state.regs[dst], RegValue::Ptr(_)) {
                    return reject(format!(
                        "32-bit operation truncates a stack pointer (insn #{pc})"
                    ));
                }
                state.regs[dst] = RegValue::Scalar;
                fallthrough(self, state);
            }
            ebpf::ADD32_REG
            | ebpf::SUB32_REG
            | ebpf::MUL32_REG
            | ebpf::DIV32_REG
            | ebpf::OR32_REG
            | ebpf::AND32_REG
            | ebpf::LSH32_REG
            | ebpf::RSH32_REG
            | ebpf::MOD32_REG
            | ebpf::XOR32_REG
            | ebpf::MOV32_REG
            | ebpf::ARSH32_REG => {
                // A pointer on either side cannot be truncated or washed into
                // a plain scalar that could later be used for addressing.
                if matches!(state.regs[dst], RegValue::Ptr(_))
                    || matches!(state.regs[src], RegValue::Ptr(_))
                {
                    return reject(format!(
                        "32-bit operation truncates a stack pointer (insn #{pc})"
                    ));
                }
                state.regs[dst] = RegValue::Scalar;
                fallthrough(self, state);
            }
            ebpf::LE | ebpf::BE => {
                if matches!(state.regs[dst], RegValue::Ptr(_)) {
                    return reject(format!(
                        "narrow endian conversion truncates a stack pointer (insn #{pc})"
                    ));
                }
                state.regs[dst] = RegValue::Scalar;
                fallthrough(self, state);
            }

            // ---- 64-bit ALU ------------------------------------------------------
            ebpf::ADD64_IMM => {
                state.regs[dst] = self.add_imm(&state.regs[dst], i64::from(insn.imm), pc)?;
                fallthrough(self, state);
            }
            ebpf::SUB64_IMM => {
                state.regs[dst] = self.add_imm(&state.regs[dst], -i64::from(insn.imm), pc)?;
                fallthrough(self, state);
            }
            ebpf::MOV64_IMM => {
                state.regs[dst] = RegValue::Scalar;
                fallthrough(self, state);
            }
            ebpf::MOV64_REG => {
                state.regs[dst] = state.regs[src].clone();
                fallthrough(self, state);
            }
            ebpf::ADD64_REG | ebpf::SUB64_REG => {
                self.check_reg_arith(&state.regs[dst], &state.regs[src], pc)?;
                state.regs[dst] = RegValue::Scalar;
                fallthrough(self, state);
            }
            ebpf::MUL64_IMM
            | ebpf::DIV64_IMM
            | ebpf::OR64_IMM
            | ebpf::AND64_IMM
            | ebpf::LSH64_IMM
            | ebpf::RSH64_IMM
            | ebpf::NEG64
            | ebpf::MOD64_IMM
            | ebpf::XOR64_IMM
            | ebpf::ARSH64_IMM => {
                if matches!(state.regs[dst], RegValue::Ptr(_)) {
                    return reject(format!(
                        "arithmetic operation destroys stack pointer meaning (insn #{pc})"
                    ));
                }
                state.regs[dst] = RegValue::Scalar;
                fallthrough(self, state);
            }
            ebpf::MUL64_REG
            | ebpf::DIV64_REG
            | ebpf::OR64_REG
            | ebpf::AND64_REG
            | ebpf::LSH64_REG
            | ebpf::RSH64_REG
            | ebpf::MOD64_REG
            | ebpf::XOR64_REG
            | ebpf::ARSH64_REG => {
                // A pointer on either operand cannot survive these operations
                // nor be laundered into an ordinary scalar.
                if matches!(state.regs[dst], RegValue::Ptr(_))
                    || matches!(state.regs[src], RegValue::Ptr(_))
                {
                    return reject(format!(
                        "arithmetic operation destroys stack pointer meaning (insn #{pc})"
                    ));
                }
                state.regs[dst] = RegValue::Scalar;
                fallthrough(self, state);
            }

            // ---- Jumps: conditions are not interpreted, both edges are taken -----
            ebpf::JA => {
                let target = jump_target(insn.off)?;
                self.propagate(target, state);
            }
            ebpf::JEQ_IMM
            | ebpf::JGT_IMM
            | ebpf::JGE_IMM
            | ebpf::JLT_IMM
            | ebpf::JLE_IMM
            | ebpf::JSET_IMM
            | ebpf::JNE_IMM
            | ebpf::JSGT_IMM
            | ebpf::JSGE_IMM
            | ebpf::JSLT_IMM
            | ebpf::JSLE_IMM
            | ebpf::JEQ_IMM32
            | ebpf::JGT_IMM32
            | ebpf::JGE_IMM32
            | ebpf::JLT_IMM32
            | ebpf::JLE_IMM32
            | ebpf::JSET_IMM32
            | ebpf::JNE_IMM32
            | ebpf::JSGT_IMM32
            | ebpf::JSGE_IMM32
            | ebpf::JSLT_IMM32
            | ebpf::JSLE_IMM32 => {
                let target = jump_target(insn.off)?;
                self.propagate(target, state.clone());
                fallthrough(self, state);
            }
            ebpf::JEQ_REG
            | ebpf::JGT_REG
            | ebpf::JGE_REG
            | ebpf::JLT_REG
            | ebpf::JLE_REG
            | ebpf::JSET_REG
            | ebpf::JNE_REG
            | ebpf::JSGT_REG
            | ebpf::JSGE_REG
            | ebpf::JSLT_REG
            | ebpf::JSLE_REG
            | ebpf::JEQ_REG32
            | ebpf::JGT_REG32
            | ebpf::JGE_REG32
            | ebpf::JLT_REG32
            | ebpf::JLE_REG32
            | ebpf::JSET_REG32
            | ebpf::JNE_REG32
            | ebpf::JSGT_REG32
            | ebpf::JSGE_REG32
            | ebpf::JSLT_REG32
            | ebpf::JSLE_REG32 => {
                // Comparisons never modify registers, so pointer operands are
                // harmless: the condition is not inferred and both edges are
                // explored with the state unchanged.
                let target = jump_target(insn.off)?;
                self.propagate(target, state.clone());
                fallthrough(self, state);
            }

            // ---- Calls and exits -------------------------------------------------
            ebpf::CALL => {
                if insn.src == 1 {
                    return reject(format!(
                        "local function calls are not supported by the stack initialization checker (insn #{pc})"
                    ));
                }
                return reject(format!(
                    "helper calls are not supported by the stack initialization checker (insn #{pc})"
                ));
            }
            ebpf::TAIL_CALL => {
                return reject(format!(
                    "tail calls are not supported by the stack initialization checker (insn #{pc})"
                ));
            }
            ebpf::EXIT => {
                // Terminal node: no successor.
            }

            // ---- Packet-oriented loads: never touch the stack -------------------
            // LD_ABS takes only an immediate; LD_IND indexes from src, which
            // must therefore be an ordinary (non-stack) scalar.
            ebpf::LD_ABS_B | ebpf::LD_ABS_H | ebpf::LD_ABS_W | ebpf::LD_ABS_DW => {
                state.regs[0] = RegValue::Scalar;
                fallthrough(self, state);
            }
            ebpf::LD_IND_B | ebpf::LD_IND_H | ebpf::LD_IND_W | ebpf::LD_IND_DW => {
                if matches!(state.regs[src], RegValue::Ptr(_)) {
                    return reject(format!(
                        "packet access with a stack-derived register is not supported (insn #{pc})"
                    ));
                }
                state.regs[0] = RegValue::Scalar;
                fallthrough(self, state);
            }

            _ => {
                return reject(format!(
                    "unsupported eBPF opcode {:#x} in stack initialization checker (insn #{pc})",
                    insn.opc
                ));
            }
        }
        Ok(())
    }

    /// Register-register arithmetic is only accepted for plain scalars.
    fn check_reg_arith(&self, dst: &RegValue, src: &RegValue, pc: usize) -> Result<(), Error> {
        if matches!(dst, RegValue::Ptr(_)) || matches!(src, RegValue::Ptr(_)) {
            return reject(format!(
                "arithmetic mixes stack and non-stack sources (insn #{pc})"
            ));
        }
        Ok(())
    }

    fn add_imm(&self, reg: &RegValue, delta: i64, pc: usize) -> Result<RegValue, Error> {
        match reg {
            RegValue::Scalar => Ok(RegValue::Scalar),
            // Ambiguous top stays top.
            RegValue::Ptr(offsets) if offsets.is_empty() => Ok(RegValue::Ptr(Vec::new())),
            RegValue::Ptr(offsets) => {
                let mut new = Vec::with_capacity(offsets.len());
                for off in offsets {
                    let next = apply_delta(*off, delta).ok_or_else(|| {
                        Error::other(format!(
                            "[Verifier] Error: stack pointer offset wraps (insn #{pc})"
                        ))
                    })?;
                    if !offset_is_useful(next) {
                        // The pointer can never address the frame again; keep
                        // the ambiguity explicit instead of growing forever.
                        return Ok(RegValue::Ptr(Vec::new()));
                    }
                    new.push(next);
                }
                Ok(RegValue::Ptr(new))
            }
        }
    }

    /// Resolve every candidate stack range touched by a memory operand. An
    /// empty result means the base register is not stack-derived.
    fn resolve_ranges(
        &self,
        state: &State,
        base: usize,
        off: i16,
        size: usize,
        pc: usize,
    ) -> Result<Vec<(usize, usize)>, Error> {
        match &state.regs[base] {
            RegValue::Scalar => Ok(Vec::new()),
            RegValue::Ptr(candidates) if candidates.is_empty() => reject(format!(
                "stack-derived register with ambiguous provenance used for memory access (insn #{pc})"
            )),
            RegValue::Ptr(candidates) => {
                let mut ranges = Vec::with_capacity(candidates.len());
                for cand in candidates {
                    let start = apply_delta(*cand, i64::from(off)).ok_or_else(|| {
                        Error::other(format!(
                            "[Verifier] Error: stack address offset wraps (insn #{pc})"
                        ))
                    })?;
                    let start_idx = frame_index(start).ok_or_else(|| {
                        Error::other(format!(
                            "[Verifier] Error: stack access starts outside of the 512-byte stack (insn #{pc})"
                        ))
                    })?;
                    let last =
                        apply_delta(start, i64::try_from(size).unwrap() - 1).ok_or_else(|| {
                            Error::other(format!(
                                "[Verifier] Error: stack access offset wraps (insn #{pc})"
                            ))
                        })?;
                    frame_index(last).ok_or_else(|| {
                        Error::other(format!(
                            "[Verifier] Error: stack access ends outside of the 512-byte stack (insn #{pc})"
                        ))
                    })?;
                    let end_idx = frame_index(last).unwrap() + 1;
                    ranges.push((start_idx, end_idx));
                }
                Ok(ranges)
            }
        }
    }

    /// Every byte of every candidate range must be definitely initialized.
    fn check_read(
        &self,
        pc: usize,
        state: &State,
        base: usize,
        off: i16,
        size: usize,
    ) -> Result<(), Error> {
        let ranges = self.resolve_ranges(state, base, off, size, pc)?;
        for (start, end) in ranges {
            let mut bad: Option<(usize, usize)> = None;
            for idx in start..end {
                if !state.init[idx] {
                    bad = Some(match bad {
                        None => (idx, idx + 1),
                        Some((lo, hi)) => (lo.min(idx), hi.max(idx + 1)),
                    });
                }
            }
            if let Some((lo, hi)) = bad {
                let rel_lo = lo as i32 - STACK_BYTES as i32;
                let rel_hi = hi as i32 - STACK_BYTES as i32;
                return reject(format!(
                    "read of uninitialized stack byte(s) at offsets {rel_lo}..{rel_hi} (insn #{pc})"
                ));
            }
        }
        Ok(())
    }

    /// Writes are bounds-checked for every candidate pointer.
    fn check_write(
        &self,
        pc: usize,
        state: &State,
        base: usize,
        off: i16,
        size: usize,
    ) -> Result<(), Error> {
        // Resolution itself rejects ambiguous pointers and out-of-frame ranges.
        self.resolve_ranges(state, base, off, size, pc)?;
        Ok(())
    }

    /// Abstract value produced by a load.
    fn load_value(
        &self,
        pc: usize,
        state: &State,
        base: usize,
        off: i16,
        size: usize,
    ) -> Result<RegValue, Error> {
        let ranges = self.resolve_ranges(state, base, off, size, pc)?;
        if ranges.is_empty() {
            // Non-stack memory (packet, mbuff, allowed memory): plain scalar.
            return Ok(RegValue::Scalar);
        }

        // Pointer provenance can only survive an exact eight-byte reload of an
        // eight-byte-aligned, fully covered slot.
        let mut merged: Option<SlotValue> = None;
        for (start, end) in &ranges {
            let slot = start / 8;
            let aligned = *start == slot * 8 && *end == (slot + 1) * 8 && size == 8;
            let value = if !aligned {
                // A narrow or misaligned reload touching pointer bytes can
                // neither reconstruct the pointer nor wash it into a scalar.
                if region_touches_pointer(state, *start, *end) {
                    return reject(format!(
                        "narrow or misaligned load reads stack pointer bytes (insn #{pc})"
                    ));
                }
                SlotValue::Scalar
            } else {
                match &state.slots[slot] {
                    SlotValue::Scalar => SlotValue::Scalar,
                    SlotValue::Poisoned => {
                        return reject(format!(
                            "load from a stack slot with partial pointer coverage (insn #{pc})"
                        ));
                    }
                    SlotValue::Ptr(offsets) => SlotValue::Ptr(offsets.clone()),
                }
            };
            merged = Some(match merged.take() {
                None => value,
                Some(prev) => prev.merge(value),
            });
        }

        match merged.unwrap_or(SlotValue::Scalar) {
            SlotValue::Scalar => Ok(RegValue::Scalar),
            SlotValue::Ptr(offsets) if offsets.is_empty() => reject(format!(
                "stack slot holds a pointer only on some paths (insn #{pc})"
            )),
            SlotValue::Ptr(offsets) => Ok(RegValue::Ptr(offsets)),
            SlotValue::Poisoned => reject(format!(
                "overlapping or partial reads cannot reconstruct a stack pointer (insn #{pc})"
            )),
        }
    }

    /// Store an immediate, which never carries pointer provenance.
    fn mark_scalar_store(&self, state: &mut State, base: usize, off: i16, size: usize) {
        let Ok(ranges) = self.resolve_ranges(state, base, off, size, usize::MAX) else {
            return;
        };
        let Some((start, end)) = intersect_ranges(&ranges) else {
            return;
        };
        for idx in start..end {
            state.init[idx] = true;
        }
        self.update_slots_for_store(state, start, end, None);
    }

    /// Store a register, possibly carrying pointer provenance.
    fn mark_reg_store(
        &self,
        state: &mut State,
        base: usize,
        off: i16,
        size: usize,
        value: &RegValue,
    ) {
        let Ok(ranges) = self.resolve_ranges(state, base, off, size, usize::MAX) else {
            return;
        };
        let Some((start, end)) = intersect_ranges(&ranges) else {
            return;
        };
        let pointer = match value {
            RegValue::Scalar => None,
            RegValue::Ptr(offsets) => Some(offsets.clone()),
        };
        for idx in start..end {
            state.init[idx] = true;
        }
        self.update_slots_for_store(state, start, end, pointer);
    }

    /// Recompute slot provenance after a store covering frame bytes
    /// `start..end` (already intersected across every candidate pointer).
    /// `pointer` is `Some` only when the stored register carries stack pointer
    /// provenance.
    fn update_slots_for_store(
        &self,
        state: &mut State,
        start: usize,
        end: usize,
        pointer: Option<Vec<i32>>,
    ) {
        let first_slot = start / 8;
        let last_slot = (end - 1) / 8;
        for slot in first_slot..=last_slot {
            let slot_start = slot * 8;
            let slot_end = slot_start + 8;
            let covers_full_slot = start <= slot_start && end >= slot_end;
            state.slots[slot] = match (&pointer, covers_full_slot) {
                // Exact full-width pointer spill keeps provenance.
                (Some(offsets), true) => SlotValue::Ptr(offsets.clone()),
                // Any partial pointer store poisons every touched slot.
                (Some(_), false) => SlotValue::Poisoned,
                // A full scalar overwrite removes all previous pointer bytes.
                (None, true) => SlotValue::Scalar,
                // A partial scalar overwrite only poisons a slot that held a
                // pointer; ordinary slots are untouched.
                (None, false) => match &state.slots[slot] {
                    SlotValue::Ptr(_) => SlotValue::Poisoned,
                    _ => state.slots[slot].clone(),
                },
            };
        }
    }
}

/// Bytes covered by every candidate pointer of an ambiguous store: the
/// intersection. A write through one candidate must not count for the others.
fn intersect_ranges(ranges: &[(usize, usize)]) -> Option<(usize, usize)> {
    let start = ranges.iter().map(|(s, _)| *s).max()?;
    let end = ranges.iter().map(|(_, e)| *e).min()?;
    if start < end {
        Some((start, end))
    } else {
        None
    }
}

fn region_touches_pointer(state: &State, start: usize, end: usize) -> bool {
    let first_slot = start / 8;
    let last_slot = (end - 1) / 8;
    for slot in first_slot..=last_slot {
        if !matches!(state.slots[slot], SlotValue::Scalar) {
            return true;
        }
    }
    false
}

fn load_size(opc: u8) -> Result<usize, Error> {
    match opc {
        ebpf::LD_B_REG => Ok(1),
        ebpf::LD_H_REG => Ok(2),
        ebpf::LD_W_REG => Ok(4),
        ebpf::LD_DW_REG => Ok(8),
        _ => reject(format!("unknown load size for opcode {opc:#x}")),
    }
}

fn store_size(opc: u8) -> Result<usize, Error> {
    match opc {
        ebpf::ST_B_IMM | ebpf::ST_B_REG => Ok(1),
        ebpf::ST_H_IMM | ebpf::ST_H_REG => Ok(2),
        ebpf::ST_W_IMM | ebpf::ST_W_REG => Ok(4),
        ebpf::ST_DW_IMM | ebpf::ST_DW_REG => Ok(8),
        _ => reject(format!("unknown store size for opcode {opc:#x}")),
    }
}

/// Run the stack definite-initialization analysis on a program that already
/// passed the ordinary format checks.
pub(crate) fn analyze(prog: &[u8]) -> Result<(), Error> {
    Analyzer::new(prog).run()
}
