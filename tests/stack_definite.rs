// SPDX-License-Identifier: Apache-2.0 OR MIT

//! Integration tests for the restricted stack definite-initialization checker
//! exposed as `rbpf::check_stack_initialized`.
//!
//! The checker is a deliberately limited static analysis (single function,
//! 512-byte stack, `r10` as the frame-end pointer); it does not claim to
//! replace the Linux kernel eBPF verifier. These tests exercise byte-level
//! coverage, control-flow joins and loops, stack pointer spill/reload aliasing
//! and the `set_verifier` hook. Programs that are rejected are only inspected
//! statically and are never executed.

extern crate rbpf;

use rbpf::assembler::assemble;
use rbpf::{EbpfVmNoData, EbpfVmRaw, check_stack_initialized};

/// Assemble a program and assert that the initialization checker accepts it.
fn accept(src: &str) -> Vec<u8> {
    let prog = assemble(src).unwrap();
    check_stack_initialized(&prog)
        .unwrap_or_else(|e| panic!("expected the checker to accept:\n{src}\nerror: {e}"));
    prog
}

/// Assemble a program and assert that the checker rejects it. The error must
/// reference the offending instruction index.
fn reject(src: &str) -> String {
    let prog = assemble(src).unwrap();
    let err = check_stack_initialized(&prog)
        .expect_err("expected the checker to reject the program")
        .to_string();
    assert!(
        err.contains("insn #"),
        "error message should carry an instruction index: {err}"
    );
    err
}

/// Same as [`reject`] for programs built from raw bytes (the assembler has no
/// mnemonic for legacy XADD).
fn reject_bytes(prog: &[u8]) -> String {
    let err = check_stack_initialized(prog)
        .expect_err("expected the checker to reject the program")
        .to_string();
    assert!(
        err.contains("insn #"),
        "error message should carry an instruction index: {err}"
    );
    err
}

#[test]
fn byte_coverage() {
    // A plain write/read round trip is accepted and runs deterministically.
    let prog = accept("stdw [r10-8], 42\nldxdw r0, [r10-8]\nexit");
    let vm = EbpfVmNoData::new(Some(&prog)).unwrap();
    assert_eq!(vm.execute_program().unwrap(), 42);

    // Immediate 64-bit loads do not touch the stack and skip the LD_DW second
    // half-word, which is not an analysis node.
    let prog = accept("lddw r0, 43\nexit");
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        43
    );

    // Reading a stack slot that was never written is rejected. The program is
    // only inspected statically and must not be executed.
    let err = reject("ldxdw r0, [r10-8]\nexit");
    assert!(err.contains("offsets -8..0"), "unexpected message: {err}");

    // Two four-byte writes together satisfy one eight-byte read.
    let prog = accept("stw [r10-8], 1\nstw [r10-4], 2\nldxdw r0, [r10-8]\nexit");
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        (2u64 << 32) | 1
    );

    // Seven covered bytes do not satisfy an eight-byte read: the writes cover
    // -8..-1, leaving the byte at offset -1 uninitialized.
    let err = reject("stw [r10-8], 0\nstw [r10-5], 0\nldxdw r0, [r10-8]\nexit");
    assert!(err.contains("offsets -1..0"), "unexpected message: {err}");

    // Byte-sized accesses are tracked at byte granularity.
    let prog = accept("stb [r10-1], 7\nldxb r0, [r10-1]\nexit");
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        7
    );
    let err = reject("stb [r10-1], 7\nldxh r0, [r10-2]\nexit");
    assert!(err.contains("offsets -2..-1"), "unexpected message: {err}");

    // Half-word and word accesses follow the same coverage rule.
    let prog = accept("sth [r10-2], 0\nldxh r0, [r10-2]\nexit");
    EbpfVmNoData::new(Some(&prog))
        .unwrap()
        .execute_program()
        .unwrap();
    reject("sth [r10-2], 0\nldxw r0, [r10-4]\nexit");

    // The deepest valid slot is in range; one byte further down is not.
    accept("stb [r10-512], 1\nldxb r0, [r10-512]\nexit");
    reject("stb [r10-513], 1\nexit");
    // r10 points one byte past the end: accesses at offset 0 leave the frame.
    reject("stb [r10], 1\nexit");
    reject("ldxb r0, [r10]\nexit");

    // Unreachable instructions do not produce initialization errors.
    accept("ja +1\nldxdw r0, [r10-8]\nexit");

    // 32-bit truncation of a stack pointer is rejected rather than silently
    // turning the pointer into a scalar.
    reject("mov r1, r10\nmov32 r1, r10\nexit");
    reject("mov r1, r10\nadd32 r1, 0\nexit");

    // Register-register arithmetic that mixes stack and non-stack sources must
    // not degrade into an unchecked scalar, regardless of which operand holds
    // the pointer.
    reject("mov r1, r10\nmov r2, 0\nadd r1, r2\nexit");
    reject("mov r2, r10\nmov r1, 0\nor r1, r2\nexit");
    reject("mov r2, r10\nmov r1, 0\nxor r1, r2\nexit");
    // The same applies to 32-bit operations reading a pointer operand.
    reject("mov r2, r10\nmov32 r1, r2\nexit");
    reject("mov r2, r10\nadd32 r1, r2\nexit");
    // Pointer comparisons are accepted: conditions are not inferred, so both
    // edges are explored and the pointer remains tracked.
    accept("mov r1, r10\njeq r1, 0, +1\nmov r0, 0\nexit");

    // Packet memory accesses through ordinary (non-stack) registers keep
    // following the existing conventions and run through the interpreter.
    let prog = accept("ldxb r0, [r1+0]\nexit");
    let vm = EbpfVmRaw::new(Some(&prog)).unwrap();
    assert_eq!(vm.execute_program(&mut [77u8, 1, 2]).unwrap(), 77);

    // The original format checks still run first: an invalid destination
    // register is reported with the usual verifier message.
    let prog = assemble("mov r11, 1\nexit").unwrap();
    let err = check_stack_initialized(&prog).unwrap_err().to_string();
    assert!(
        err.contains("invalid destination register"),
        "unexpected: {err}"
    );
}

#[test]
fn joins_loops() {
    // Branch-derived pointers with different offsets: the write through the
    // ambiguous pointer only counts for bytes covered by every candidate, so
    // neither slot is guaranteed initialized at the join and the read fails.
    let err = reject(
        "mov r1, r10
         jgt r0, 0, +2
         add r1, -8
         ja +1
         add r1, -16
         stdw [r1], 9
         ldxdw r0, [r10-8]
         exit",
    );
    assert!(err.contains("offsets -8..0"), "unexpected message: {err}");

    // Same shape, but both branches pick the same offset: the slot is written
    // on every path and the read is accepted. r0 is zero on entry, so the
    // interpreter takes the fall-through edge and produces a deterministic 9.
    let prog = accept(
        "mov r1, r10
         jgt r0, 0, +2
         add r1, -8
         ja +1
         add r1, -8
         stdw [r1], 9
         ldxdw r0, [r10-8]
         exit",
    );
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        9
    );

    // A read through an ambiguous pointer must be initialized for every
    // candidate range. Both slots are written first, so it is accepted.
    let prog = accept(
        "stdw [r10-8], 1
         stdw [r10-16], 2
         mov r1, r10
         jgt r0, 0, +2
         add r1, -8
         ja +1
         add r1, -16
         ldxdw r0, [r1]
         exit",
    );
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        1
    );

    // A write through an ambiguous pointer only marks the intersection of the
    // candidate ranges: offsets -8 and -7 with a two-byte store overlap on
    // just byte -7. Reads of the other bytes still fail, while the common byte
    // is accepted.
    reject(
        "mov r1, r10
         jgt r0, 0, +2
         add r1, -8
         ja +1
         add r1, -7
         sth [r1], 0
         ldxh r0, [r10-8]
         exit",
    );
    accept(
        "mov r1, r10
         jgt r0, 0, +2
         add r1, -8
         ja +1
         add r1, -7
         sth [r1], 0
         ldxb r0, [r10-7]
         exit",
    );

    // Halves written in different branches cannot be combined into one fully
    // initialized eight-byte value.
    reject(
        "jgt r0, 0, +2
         stw [r10-8], 1
         ja +1
         stw [r10-4], 2
         ldxdw r0, [r10-8]
         exit",
    );

    // Writing the full slot in both branches does satisfy the joined read.
    let prog = accept(
        "jgt r0, 0, +2
         stdw [r10-8], 1
         ja +1
         stdw [r10-8], 2
         ldxdw r0, [r10-8]
         exit",
    );
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        1
    );

    // Loop first-iteration read: the slot is only written later in the body,
    // which cannot prove it initialized when the read is reached the first
    // time. Rejected statically, never executed.
    reject(
        "mov r1, 0
         jeq r1, 1, +4
         ldxdw r0, [r10-8]
         stdw [r10-8], 1
         mov r1, 1
         ja -5
         exit",
    );

    // A write dominating the loop, followed by a back edge carrying no new
    // information: the fixed point is reached and the analysis terminates.
    let prog = accept(
        "stdw [r10-8], 7
         mov r1, 0
         jeq r1, 1, +3
         ldxdw r0, [r10-8]
         mov r1, 1
         ja -4
         exit",
    );
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        7
    );

    // A pointer that walks to a fresh offset on every back edge reaches the
    // ambiguous top value after finitely many iterations; the following store
    // (and any later use) is then rejected instead of making the analysis
    // diverge. This also covers the "zero-iteration" requirement: the write on
    // the back edge cannot prove the pointer's target is initialized at entry.
    reject(
        "mov r1, r10
         mov r2, 0
         jeq r2, 1, +3
         add r1, -8
         mov r2, 1
         ja -4
         stdw [r1], 1
         exit",
    );

    // A path that skips the write entirely still reaches the read: the write
    // cannot be treated as guaranteed.
    reject(
        "jne r0, 0, +1
         ja +1
         stdw [r10-8], 1
         ldxdw r0, [r10-8]
         exit",
    );
}

#[test]
fn spill_alias() {
    // Full eight-byte spill of a stack pointer and a matching full reload keep
    // the pointer trackable, including a write through the reloaded copy.
    let prog = accept(
        "mov r1, r10
         add r1, -16
         stxdw [r10-8], r1
         ldxdw r2, [r10-8]
         stdw [r2], 5
         ldxdw r0, [r10-16]
         exit",
    );
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        5
    );

    // A single-byte overwrite after the spill destroys the pointer proof: a
    // full reload of the partially covered slot is rejected.
    reject(
        "mov r1, r10
         add r1, -16
         stxdw [r10-8], r1
         stb [r10-8], 1
         ldxdw r2, [r10-8]
         exit",
    );

    // A narrow load of the poisoned pointer slot must not launder the pointer
    // bytes into an ordinary scalar.
    reject(
        "mov r1, r10
         add r1, -16
         stxdw [r10-8], r1
         stb [r10-8], 1
         ldxb r2, [r10-8]
         exit",
    );

    // A narrow load directly from a fully covered pointer slot is rejected as
    // well: overlapping/partial reads cannot reconstruct or launder it.
    reject(
        "mov r1, r10
         add r1, -32
         stxdw [r10-16], r1
         ldxw r2, [r10-16]
         exit",
    );

    // Slot contents coming from different branches must be merged: a pointer
    // slot on one edge and a scalar slot on the other poisons the join.
    reject(
        "jgt r0, 0, +2
         stdw [r10-8], 1
         ja +1
         stxdw [r10-8], r10
         ldxdw r1, [r10-8]
         exit",
    );

    // A merged pointer slot with two candidate offsets reloads as an
    // ambiguous pointer; a write through it only covers the intersection
    // (empty here), so a later definite read fails.
    reject(
        "mov r1, r10
         jgt r0, 0, +3
         add r1, -8
         stxdw [r10-16], r1
         ja +2
         add r1, -24
         stxdw [r10-16], r1
         ldxdw r2, [r10-16]
         stdw [r2], 3
         ldxdw r0, [r10-8]
         exit",
    );

    // Spilling and reloading an ordinary scalar value keeps working.
    let prog = accept("stdw [r10-8], 123\nldxdw r0, [r10-8]\nexit");
    assert_eq!(
        EbpfVmNoData::new(Some(&prog))
            .unwrap()
            .execute_program()
            .unwrap(),
        123
    );
}

#[test]
fn verifier_hook() {
    // The new checker has the same signature as a Verifier and can be handed
    // to set_verifier; accepted programs still run with deterministic results.
    let good = accept("stdw [r10-8], 42\nldxdw r0, [r10-8]\nexit");
    let bad = assemble("ldxdw r0, [r10-8]\nexit").unwrap();
    let mut vm = EbpfVmNoData::new(Some(&good)).unwrap();
    vm.set_verifier(check_stack_initialized).unwrap();
    assert_eq!(vm.execute_program().unwrap(), 42);

    // Replacing the program with one that reads uninitialized stack bytes
    // fails, and the previously loaded program stays in place and executable.
    assert!(check_stack_initialized(&bad).is_err());
    let load_err = vm.set_program(&bad).unwrap_err().to_string();
    assert!(
        load_err.contains("uninitialized stack"),
        "unexpected: {load_err}"
    );
    assert_eq!(vm.execute_program().unwrap(), 42);

    // Attaching the checker to a VM whose currently loaded program is bad
    // immediately rejects the swap.
    let mut vm = EbpfVmNoData::new(Some(&bad)).unwrap();
    assert!(vm.set_verifier(check_stack_initialized).is_err());

    // The default internal verifier and VM behavior are unchanged: the bad
    // program still loads without the custom checker attached.
    EbpfVmNoData::new(Some(&bad)).unwrap();

    // Reachable helper calls, local function calls and atomic operations are
    // reported as unsupported, with the instruction index attached; they are
    // only analyzed statically and never executed.
    let unsupported = reject("call 0\nexit");
    assert!(
        unsupported.contains("helper calls are not supported"),
        "unexpected: {unsupported}"
    );
    let unsupported = reject("callx 0\nexit");
    assert!(
        unsupported.contains("local function calls are not supported"),
        "unexpected: {unsupported}"
    );
    #[rustfmt::skip]
    let xadd = &[
        0xc3, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // stxxaddw [r1+0], r2 (legacy XADD)
        0x95, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // exit
    ];
    let unsupported = reject_bytes(xadd);
    assert!(
        unsupported.contains("atomic memory operations are not supported"),
        "unexpected: {unsupported}"
    );

    // The same constructs are tolerated when they cannot be reached: the
    // analysis only reports errors on reachable instruction nodes.
    accept("ja +1\ncall 0\nexit");
    accept("ja +1\ncallx 0\nexit");

    // The input bytes are never rewritten.
    let mut snapshot = bad.clone();
    let before = snapshot.clone();
    let _ = check_stack_initialized(&snapshot);
    assert_eq!(snapshot, before);
    let _ = &mut snapshot;
}
