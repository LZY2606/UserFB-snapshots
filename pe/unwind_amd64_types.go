// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import "math"

// AMD64 integer register numbers used by AMD64RegisterSet and the
// UNWIND_CODE save operations. They follow the Windows x64 calling
// convention ordering documented for UNWIND_CODE (the lower 4 bits of
// the operation-info byte):
//
//	0  RAX   1  RCX   2  RDX   3  RBX
//	4  RSP   5  RBP   6  RSI   7  RDI
//	8  R8    9  R9    10 R10   11 R11
//	12 R12   13 R13   14 R14   15 R15
const (
	AMD64RegRAX = iota
	AMD64RegRCX
	AMD64RegRDX
	AMD64RegRBX
	AMD64RegRSP
	AMD64RegRBP
	AMD64RegRSI
	AMD64RegRDI
	AMD64RegR8
	AMD64RegR9
	AMD64RegR10
	AMD64RegR11
	AMD64RegR12
	AMD64RegR13
	AMD64RegR14
	AMD64RegR15
)

// AMD64RegisterSet holds the sixteen integer registers of an x64
// snapshot, indexed by the AMD64Reg* register numbers.
type AMD64RegisterSet [16]uint64

// AMD64Snapshot is a read-only CPU snapshot for File.UnwindAMD64. The
// fields are never modified by the library; the returned frame is a
// separate copy.
type AMD64Snapshot struct {
	RIP uint64
	RSP uint64
	Reg AMD64RegisterSet
	// InEpilogue must be set by the caller when the PC has been
	// confirmed to lie inside an epilogue. Unwinding such a PC requires
	// decoding instructions, which this library never does, so setting
	// it makes UnwindAMD64 return ErrAMD64UnwindNotSupported instead of
	// guessing. Leave it false for PCs in a prolog or in the function
	// body.
	InEpilogue bool
}

// MemReaderAt reads snapshot memory. ReadAtVA must fill len(p) bytes
// starting at virtual address va and return the number of bytes read
// together with an error. A short read (n < len(p)) is treated as an
// unwind error and does not produce a partial frame.
type MemReaderAt interface {
	ReadAtVA(p []byte, va uint64) (n int, err error)
}

// MemReaderAtFunc adapts a function with the ReadAtVA signature to a
// MemReaderAt.
type MemReaderAtFunc func(p []byte, va uint64) (n int, err error)

// ReadAtVA calls f(p, va).
func (f MemReaderAtFunc) ReadAtVA(p []byte, va uint64) (n int, err error) {
	return f(p, va)
}

// AMD64 unwind opcodes (UNWIND_CODE.Code, high nibble of the first
// slot byte), per the Windows x64 ABI.
const (
	uwopPushNonvol    = 0  // UWOP_PUSH_NONVOL, 1 slot
	uwopAllocLarge    = 1  // UWOP_ALLOC_LARGE, 2 or 3 slots
	uwopAllocSmall    = 2  // UWOP_ALLOC_SMALL, 1 slot
	uwopSetFPReg      = 3  // UWOP_SET_FPREG, 1 slot
	uwopSaveNonvol    = 4  // UWOP_SAVE_NONVOL, 2 slots
	uwopSaveNonvolFar = 5  // UWOP_SAVE_NONVOL_FAR, 3 slots
	uwopSaveXMM       = 6  // UWOP_SAVE_XMM, unsupported
	uwopSaveXMMFar    = 7  // UWOP_SAVE_XMM_FAR, unsupported
	uwopSaveXMM128    = 8  // UWOP_SAVE_XMM128, unsupported
	uwopSaveXMM128Far = 9  // UWOP_SAVE_XMM128_FAR, unsupported
	uwopPushMachframe = 10 // UWOP_PUSH_MACHFRAME, 1 slot
	uwopSetFPRegFar   = 11 // UWOP_SET_FPREG_LARGE, unsupported
	uwopChainInfo     = 12 // UWOP_CHAIN_INFO, 1 slot plus word-aligned parent RUNTIME_FUNCTION
)

// Unwind operation codes as they appear in AMD64UnwindRecord.Op.
const (
	// AMD64OpPushNonvol reverses UWOP_PUSH_NONVOL: a non-volatile
	// register is popped from the stack.
	AMD64OpPushNonvol = iota + 1
	// AMD64OpAllocSmall reverses UWOP_ALLOC_SMALL.
	AMD64OpAllocSmall
	// AMD64OpAllocLarge reverses UWOP_ALLOC_LARGE (both encodings).
	AMD64OpAllocLarge
	// AMD64OpSetFPReg reverses UWOP_SET_FPREG.
	AMD64OpSetFPReg
	// AMD64OpSaveNonvol reverses UWOP_SAVE_NONVOL and
	// UWOP_SAVE_NONVOL_FAR.
	AMD64OpSaveNonvol
	// AMD64OpPushMachframe reverses UWOP_PUSH_MACHFRAME.
	AMD64OpPushMachframe
	// AMD64OpChain marks an UWOP_CHAIN_INFO record; it carries the
	// parent RUNTIME_FUNCTION in AMD64UnwindRecord.Parent and performs
	// no register change itself.
	AMD64OpChain
)

// AMD64UnwindOp is one decoded unwind operation.
type AMD64UnwindOp struct {
	// Op is one of the AMD64Op* constants.
	Op int
	// CodeOffset is the offset relative to the beginning of the
	// function at which the prolog instruction executes.
	CodeOffset uint32
	// Reg is the register number (AMD64Reg*) for PUSH_NONVOL,
	// SAVE_NONVOL and SET_FPREG; otherwise zero.
	Reg int
	// AllocSize is the stack allocation size in bytes for
	// ALLOC_SMALL/ALLOC_LARGE, zero otherwise.
	AllocSize uint32
	// SaveOffset is the non-volatile register save offset relative to
	// the fixed allocation area base (RSP at function entry), for
	// SAVE_NONVOL/SAVE_NONVOL_FAR.
	SaveOffset uint32
	// FPOffset is the scaled frame-pointer offset
	// (UNWIND_INFO FrameRegisterOffset * 16) recorded by SET_FPREG.
	FPOffset uint32
	// MachframeError is the error-code flag (UNWIND_CODE info bit 0)
	// for PUSH_MACHFRAME.
	MachframeError bool
	// Slots is the number of UNWIND_CODE slots the original record
	// occupies, including extension slots.
	Slots int
	// Source is the byte offset of the leading UNWIND_CODE slot
	// relative to the start of the containing UNWIND_INFO.
	Source int
}

// AMD64RuntimeFunction mirrors an IMAGE_RUNTIME_FUNCTION_ENTRY.
type AMD64RuntimeFunction struct {
	BeginRVA  uint32
	EndRVA    uint32
	UnwindRVA uint32
	// Source is the file offset of this entry inside the loaded image.
	Source int64
}

// AMD64UnwindRecord is a decoded UNWIND_INFO plus the decoded unwind
// operations relevant to the snapshot PC.
type AMD64UnwindRecord struct {
	// Version is the UNWIND_INFO version (supported value is 1).
	Version int
	// Flags is the UNWIND_INFO flags byte.
	Flags uint8
	// PrologSize is the prolog length in bytes.
	PrologSize uint8
	// FrameReg is the frame register number, or -1 when none is used.
	FrameReg int
	// FrameRegOffset is the scaled frame register offset
	// (UNWIND_INFO FrameRegisterOffset field; the real byte offset is
	// FrameRegOffset*16).
	FrameRegOffset uint8
	// HandlerRVA is the language-specific handler RVA, or zero. The
	// handler is recorded for inspection but never invoked.
	HandlerRVA uint32
	// Ops are the decoded unwind operations in original prolog order.
	Ops []AMD64UnwindOp
	// Parent is the chained parent RUNTIME_FUNCTION for a CHAIN_INFO
	// record, otherwise nil.
	Parent *AMD64RuntimeFunction
	// Source is the file offset of the UNWIND_INFO.
	Source int64
	// Size is the number of image bytes the whole record occupies,
	// including codes, alignment padding, handler data and chain data.
	Size int
}

// AMD64ReadSource records one memory callback access.
type AMD64ReadSource struct {
	// VA is the virtual address passed to the memory callback.
	VA uint64
	// Size is the number of bytes requested.
	Size int
}

// AMD64UnwindResult is the read-only result of File.UnwindAMD64.
type AMD64UnwindResult struct {
	// Frame is the independently allocated previous-frame snapshot.
	Frame AMD64Snapshot
	// Function is the selected RUNTIME_FUNCTION. It is nil when the PC
	// falls outside the exception directory and leaf rules were used.
	Function *AMD64RuntimeFunction
	// Records are the UNWIND_INFO records processed, in order: the
	// record selected by the PC first, then each CHAIN_INFO parent.
	Records []*AMD64UnwindRecord
	// Applied are the unwind operations actually reversed, in the
	// order they were applied (reverse prolog order). Chain markers and
	// operations skipped because the PC is still inside the prolog are
	// not included.
	Applied []AMD64UnwindOp
	// Reads are the memory callback accesses in chronological order.
	Reads []AMD64ReadSource
	// Leaf is true when no exception directory entry covered the PC and
	// the frame was recovered with leaf rules.
	Leaf bool
}

// max sane sizes, used to guard against arithmetic wrap and runaway loops.
const (
	maxUnwindCodes   = 256
	maxUnwindRecords = 64
	maxUnwindBytes   = 1 << 20
)

func addU32(a, b uint32) (uint32, bool) {
	if b > math.MaxUint32-a {
		return 0, false
	}
	return a + b, true
}

func addU64(a, b uint64) (uint64, bool) {
	if b > math.MaxUint64-a {
		return 0, false
	}
	return a + b, true
}
