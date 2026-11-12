// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// ErrAMD64UnwindNotSupported is returned, wrapped with a source
// position, when the snapshot hits a deliberately unsupported case:
// a PC declared to be in an epilogue, XMM save codes, UNWIND_INFO
// versions other than 1, or unwind opcodes this read-only unwinder does
// not implement. Use errors.Is to test for it.
var ErrAMD64UnwindNotSupported = errors.New("pe: amd64 unwind: unsupported")

func unsupportedf(pos, format string, args ...interface{}) error {
	return fmt.Errorf("%w: %s: %s", ErrAMD64UnwindNotSupported, pos, fmt.Sprintf(format, args...))
}

// UnwindAMD64 recovers the single previous stack frame of an AMD64
// PE32+ image from a crash snapshot, using the
// IMAGE_RUNTIME_FUNCTION_ENTRY / UNWIND_INFO metadata in the exception
// directory.
//
// The image is treated strictly as a data file: no machine code and no
// language-specific handlers are ever executed or invoked. Only
// UNWIND_INFO version 1 is understood, and snap must describe a PC that
// the caller has already confirmed is not in an epilogue. If
// snap.InEpilogue is set, ErrAMD64UnwindNotSupported is returned: an
// epilogue cannot be unwound from metadata alone, and instructions are
// never decoded or guessed at.
//
// imageBase is the actual virtual load base of the snapshot; the RIP in
// snap is converted to an RVA with that base. readMem supplies snapshot
// memory at virtual addresses, see MemReaderAt.
//
// On success the returned AMD64UnwindResult holds an independently
// allocated frame snapshot; snap is never modified. On error no partial
// frame is published and every error identifies the image position or
// virtual address involved.
//
// This is a restricted single-frame recovery routine, not an exception
// dispatcher: CHAIN_INFO parents are followed, but language handlers are
// only recorded in AMD64UnwindRecord.HandlerRVA and never called.
func (f *File) UnwindAMD64(imageBase uint64, snap AMD64Snapshot, readMem MemReaderAt) (*AMD64UnwindResult, error) {
	if f == nil {
		return nil, unwindFailf("", "nil pe.File")
	}
	if readMem == nil {
		return nil, unwindFailf("", "nil memory reader")
	}
	if f.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 {
		return nil, unsupportedf("", "image machine 0x%x is not AMD64", f.FileHeader.Machine)
	}
	if _, ok := f.OptionalHeader.(*OptionalHeader64); !ok {
		return nil, unsupportedf("", "image is not PE32+")
	}
	if snap.InEpilogue {
		return nil, unsupportedf(fmt.Sprintf("rip 0x%x", snap.RIP),
			"caller declared the PC to be inside an epilogue")
	}
	if snap.RIP < imageBase {
		return nil, unwindFailf("", "rip 0x%x is below image base 0x%x", snap.RIP, imageBase)
	}
	ripRVA64 := snap.RIP - imageBase
	if ripRVA64 > 0xffffffff {
		return nil, unwindFailf("", "rip 0x%x does not lie inside the image", snap.RIP)
	}
	ripRVA := uint32(ripRVA64)

	dir, err := f.exceptionDirectory()
	if err != nil {
		return nil, err
	}
	rt, _, err := f.findRuntimeFunction(dir, ripRVA)
	if err != nil {
		return nil, err
	}

	// Independent copy: snap is passed by value and never written to.
	res := &AMD64UnwindResult{Frame: snap}
	u := &amd64Unwinder{f: f, readMem: readMem, res: res}

	if rt == nil {
		// No exception directory, or a valid directory whose entries do
		// not cover this PC: leaf rules apply. A present but corrupt
		// directory has already returned an error above and never
		// degrades into this path.
		res.Leaf = true
		if err := u.leafReturn(); err != nil {
			return nil, err
		}
		return res, nil
	}

	res.Function = rt
	if err := u.unwindChain(rt, ripRVA, true); err != nil {
		return nil, err
	}
	return res, nil
}

// amd64Unwinder carries the mutable unwind state. The caller-visible
// result is only touched through the res field.
type amd64Unwinder struct {
	f       *File
	readMem MemReaderAt
	res     *AMD64UnwindResult
	seen    map[uint32]bool // UNWIND_INFO RVAs, for cycle detection
}

// leafReturn pops the return address from [RSP] and advances RSP by 8.
func (u *amd64Unwinder) leafReturn() error {
	ret, err := u.readPtr(u.res.Frame.RSP)
	if err != nil {
		return err
	}
	rsp, ok := addU64(u.res.Frame.RSP, 8)
	if !ok {
		return unwindFailf(fmt.Sprintf("va 0x%x", u.res.Frame.RSP),
			"rsp wrapped while popping the return address")
	}
	u.res.Frame.RIP = ret
	u.res.Frame.RSP = rsp
	return nil
}

// readPtr reads an 8-byte pointer through the memory callback and keeps
// a source record. Callback errors and short reads fail the whole
// unwind; no partial frame is produced.
func (u *amd64Unwinder) readPtr(va uint64) (uint64, error) {
	buf := make([]byte, 8)
	n, err := u.readMem.ReadAtVA(buf, va)
	u.res.Reads = append(u.res.Reads, AMD64ReadSource{VA: va, Size: 8})
	if err != nil {
		return 0, unwindFailf(fmt.Sprintf("va 0x%x", va), "memory read failed: %v", err)
	}
	if n != 8 {
		return 0, unwindFailf(fmt.Sprintf("va 0x%x", va),
			"short memory read: got %d of 8 bytes", n)
	}
	return binary.LittleEndian.Uint64(buf), nil
}

// unwindChain applies one RUNTIME_FUNCTION and, for CHAIN_INFO records,
// every complete parent record. The first record is filtered by pcRVA
// so prolog filtering applies to the function containing the snapshot
// PC; parent records are processed as if their prolog had completed.
func (u *amd64Unwinder) unwindChain(first *AMD64RuntimeFunction, pcRVA uint32, filterByPC bool) error {
	if u.seen == nil {
		u.seen = make(map[uint32]bool)
	}
	rt := first
	filtered := filterByPC
	depth := 0
	for {
		depth++
		if depth > maxUnwindRecords {
			return unwindFailf(fmt.Sprintf("unwind info rva 0x%x", rt.UnwindRVA),
				"chain depth exceeds %d records", maxUnwindRecords)
		}
		if u.seen[rt.UnwindRVA] {
			return unwindFailf(fmt.Sprintf("unwind info rva 0x%x", rt.UnwindRVA),
				"cyclic CHAIN_INFO reference")
		}
		u.seen[rt.UnwindRVA] = true
		rec, err := u.f.parseUnwindInfo(rt.UnwindRVA)
		if err != nil {
			return err
		}
		u.res.Records = append(u.res.Records, rec)

		relPC := uint32(0)
		useFilter := false
		if filtered {
			if pcRVA < rt.BeginRVA {
				return unwindFailf(fmt.Sprintf("runtime function rva 0x%x", rt.Source),
					"pc rva 0x%x precedes function begin 0x%x", pcRVA, rt.BeginRVA)
			}
			relPC = pcRVA - rt.BeginRVA
			useFilter = relPC < uint32(rec.PrologSize)
		}
		machframe, err := u.applyRecord(rec, relPC, useFilter)
		if err != nil {
			return err
		}
		if rec.Parent == nil {
			// Terminal record: take the ordinary return address exactly
			// once. A machine frame restores RIP and the original RSP
			// from fixed trap-frame slots, so it must not pop again.
			if !machframe {
				if err := u.leafReturn(); err != nil {
					return err
				}
			}
			return nil
		}
		rt = rec.Parent
		filtered = false
	}
}

// executed reports whether the prolog instruction of op has run by the
// snapshot PC. When filtering is off the whole prolog is treated as
// completed.
func opExecuted(op AMD64UnwindOp, relPC uint32, filter bool) bool {
	return !filter || op.CodeOffset <= relPC
}

// applyRecord reverses the unwind operations of one record in reverse
// prolog order. It reports whether the record contained a machine frame.
func (u *amd64Unwinder) applyRecord(rec *AMD64UnwindRecord, relPC uint32, filter bool) (bool, error) {
	frame := &u.res.Frame

	fpOp := u.findSetFPReg(rec)
	fpEstablished := fpOp != nil && opExecuted(*fpOp, relPC, filter)
	if fpEstablished {
		// The frame register was set to RSP_at_that_point + FPOffset, so
		// RSP at the SET_FPREG point is frameReg - FPOffset. Restoring
		// it here absorbs any dynamic (alloca-style) stack allocation
		// that metadata cannot describe. Before SET_FPREG has executed
		// the frame register must not be used.
		fpValue := frame.Reg[rec.FrameReg]
		if fpValue < uint64(fpOp.FPOffset) {
			return false, unwindFailf(opPos(rec, *fpOp),
				"frame register value 0x%x underflows offset %d", fpValue, fpOp.FPOffset)
		}
		frame.RSP = fpValue - uint64(fpOp.FPOffset)
	}

	// The base of the fixed allocation area: the lowest RSP reached by
	// the (executed prefix of the) prolog. SAVE_NONVOL addresses are
	// relative to this base, so they are computed once and never drift
	// while ALLOC/PUSH operations are being undone.
	fixedBase, err := u.computeFixedBase(rec, relPC, filter, fpOp, fpEstablished, frame.RSP)
	if err != nil {
		return false, err
	}

	machframe := false
	for i := len(rec.Ops) - 1; i >= 0; i-- {
		op := rec.Ops[i]
		if op.Op == AMD64OpChain {
			continue
		}
		if !opExecuted(op, relPC, filter) {
			// The prolog instruction has not run yet. The parser
			// already consumed multi-slot operations as one decoded op,
			// so skipping walks the record by the real slot width:
			// extension slots are never mistaken for new operations.
			continue
		}
		u.res.Applied = append(u.res.Applied, op)
		switch op.Op {
		case AMD64OpPushNonvol:
			val, err := u.readPtr(frame.RSP)
			if err != nil {
				return false, err
			}
			frame.Reg[op.Reg] = val
			rsp, ok := addU64(frame.RSP, 8)
			if !ok {
				return false, unwindFailf(opPos(rec, op), "rsp wrapped while popping register")
			}
			frame.RSP = rsp
		case AMD64OpAllocSmall, AMD64OpAllocLarge:
			rsp, ok := addU64(frame.RSP, uint64(op.AllocSize))
			if !ok {
				return false, unwindFailf(opPos(rec, op), "rsp wrapped while releasing allocation")
			}
			frame.RSP = rsp
		case AMD64OpSetFPReg:
			// RSP was corrected from the frame register before the
			// loop. The register itself is non-volatile and gets its
			// caller value back from its own PUSH/SAVE_NONVOL entry.
		case AMD64OpSaveNonvol:
			addr, ok := addU64(fixedBase, uint64(op.SaveOffset))
			if !ok {
				return false, unwindFailf(opPos(rec, op), "save address wrapped")
			}
			val, err := u.readPtr(addr)
			if err != nil {
				return false, err
			}
			frame.Reg[op.Reg] = val
		case AMD64OpPushMachframe:
			machframe = true
			if err := u.applyMachframe(rec, op, frame); err != nil {
				return false, err
			}
			// A machine frame is always the first prolog operation; the
			// canonical unwind contract stops processing the record here,
			// so earlier fixed allocations are not released separately:
			// RSP was already restored from the trap-frame slot.
			return true, nil
		default:
			return false, unsupportedf(opPos(rec, op), "unwind operation %d", op.Op)
		}
	}
	return machframe, nil
}

// computeFixedBase returns the lowest fixed-area RSP reached by the
// executed prolog prefix.
func (u *amd64Unwinder) computeFixedBase(rec *AMD64UnwindRecord, relPC uint32, filter bool,
	fpOp *AMD64UnwindOp, fpEstablished bool, currentRSP uint64) (uint64, error) {

	base := currentRSP
	if !fpEstablished {
		// Without a frame register dynamic stack allocation is not
		// possible in a conforming image, and RSP already sits at the
		// low water mark of the executed prefix.
		return base, nil
	}
	// The upfront RSP correction rewound RSP to the SET_FPREG point.
	// Executed allocations placed after SET_FPREG in the prolog lowered
	// RSP further and must be subtracted again to reach the fixed base.
	for _, op := range rec.Ops {
		if op.CodeOffset <= fpOp.CodeOffset || !opExecuted(op, relPC, filter) {
			continue
		}
		var size uint64
		switch op.Op {
		case AMD64OpPushNonvol:
			size = 8
		case AMD64OpAllocSmall, AMD64OpAllocLarge:
			size = uint64(op.AllocSize)
		case AMD64OpPushMachframe:
			if op.MachframeError {
				size = 40
			} else {
				size = 32
			}
		default:
			continue
		}
		if size > base {
			return 0, unwindFailf(opPos(rec, op), "fixed allocation base underflowed")
		}
		base -= size
	}
	return base, nil
}

// findSetFPReg returns the SET_FPREG operation, or nil.
func (u *amd64Unwinder) findSetFPReg(rec *AMD64UnwindRecord) *AMD64UnwindOp {
	return rec.setFPRegOp()
}

// setFPRegOp returns the SET_FPREG operation of the record, or nil.
func (rec *AMD64UnwindRecord) setFPRegOp() *AMD64UnwindOp {
	for i := range rec.Ops {
		if rec.Ops[i].Op == AMD64OpSetFPReg {
			return &rec.Ops[i]
		}
	}
	return nil
}

// applyMachframe reverses UWOP_PUSH_MACHFRAME. By the time the reverse
// loop reaches this first-prolog operation, every other executed
// allocation has been undone and frame.RSP addresses the trap frame.
// Its prescribed layout is:
//
//	+0x00 original RSP (before the machine frame)
//	+0x08 RFLAGS
//	+0x10 CS
//	+0x18 RIP
//	+0x20 error code, present only when the error-code flag is set
func (u *amd64Unwinder) applyMachframe(rec *AMD64UnwindRecord, op AMD64UnwindOp, frame *AMD64Snapshot) error {
	origRSP, err := u.readPtr(frame.RSP + 0x00)
	if err != nil {
		return err
	}
	rip, err := u.readPtr(frame.RSP + 0x18)
	if err != nil {
		return err
	}
	frame.RIP = rip
	frame.RSP = origRSP
	return nil
}

func opPos(rec *AMD64UnwindRecord, op AMD64UnwindOp) string {
	if rec != nil {
		return fmt.Sprintf("unwind info rva 0x%x code byte %d", rec.Source, op.Source)
	}
	return fmt.Sprintf("unwind code byte %d", op.Source)
}
