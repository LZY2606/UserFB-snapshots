// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import (
	"encoding/binary"
	"fmt"
	"sort"
)

// unwindError tags errors with the image position that made the unwind
// metadata unusable.
type unwindError struct {
	pos string
	msg string
}

func (e *unwindError) Error() string {
	if e.pos == "" {
		return "pe: unwind: " + e.msg
	}
	return fmt.Sprintf("pe: unwind: %s: %s", e.pos, e.msg)
}

func unwindFailf(pos, format string, args ...interface{}) error {
	return &unwindError{pos: pos, msg: fmt.Sprintf(format, args...)}
}

// containingSection returns the section whose virtual range contains
// rva, or nil. Both disk and memory-mapped images are handled the same
// way, because Section.sr was constructed with the correct base by
// NewFile/NewFileFromMemory.
func (f *File) containingSection(rva uint32) *Section {
	for _, s := range f.Sections {
		base := s.VirtualAddress
		span := s.VirtualSize
		if span == 0 || s.Size > span {
			span = s.Size
		}
		if span == 0 {
			continue
		}
		end, ok := addU32(base, span)
		if !ok {
			continue
		}
		if rva >= base && rva < end {
			return s
		}
	}
	return nil
}

// readRVA reads n bytes at image rva exclusively through the section
// readers. Referenced data is never located by adding a file offset to
// the directory start, and the directory entry and its target do not
// need to live in the same section.
func (f *File) readRVA(rva uint32, n int) ([]byte, error) {
	if n < 0 || n > maxUnwindBytes {
		return nil, unwindFailf(fmt.Sprintf("rva 0x%x", rva), "illegal read size %d", n)
	}
	s := f.containingSection(rva)
	if s == nil || s.sr == nil {
		// Sections added through the write-out API carry no backing
		// reader until the file is re-opened; treat that as unmapped
		// metadata rather than dereferencing a nil reader.
		return nil, unwindFailf(fmt.Sprintf("rva 0x%x", rva), "rva is not mapped into any readable section")
	}
	off := int64(rva - s.VirtualAddress)
	// Raw data is file-aligned padding, but the meaningful virtual span
	// is VirtualSize. Bound the read by the smaller of the two so both
	// disk and mapped layouts reject references past the real content.
	limit := s.sr.Size()
	if s.VirtualSize > 0 && uint32(limit) > s.VirtualSize {
		limit = int64(s.VirtualSize)
	}
	if off > limit || int64(n) > limit-off {
		return nil, unwindFailf(
			fmt.Sprintf("rva 0x%x (section %q)", rva, s.Name),
			"read of %d bytes exceeds section bounds", n)
	}
	buf := make([]byte, n)
	got, err := s.ReadAt(buf, off)
	if err != nil || got != n {
		return nil, unwindFailf(
			fmt.Sprintf("rva 0x%x (section %q)", rva, s.Name),
			"short section read: got %d of %d bytes: %v", got, n, err)
	}
	return buf, nil
}

// exceptionDirectory returns the exception data directory, or an
// all-zero directory when the image has none.
func (f *File) exceptionDirectory() (DataDirectory, error) {
	var zero DataDirectory
	switch oh := f.OptionalHeader.(type) {
	case *OptionalHeader64:
		return oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION], nil
	case *OptionalHeader32:
		return zero, nil
	default:
		return zero, unwindFailf("", "missing optional header")
	}
}

// findRuntimeFunction scans the exception directory for the single
// RUNTIME_FUNCTION whose [BeginRVA, EndRVA) half-open range contains
// targetRVA. Overlapping ranges are rejected. A present but corrupt
// directory is reported as an error rather than silently degrading to
// leaf handling.
func (f *File) findRuntimeFunction(dir DataDirectory, targetRVA uint32) (*AMD64RuntimeFunction, bool, error) {
	if dir.VirtualAddress == 0 || dir.Size == 0 {
		return nil, false, nil
	}
	const rtSize = 12
	if dir.Size%rtSize != 0 {
		return nil, false, unwindFailf(
			fmt.Sprintf("exception directory rva 0x%x", dir.VirtualAddress),
			"directory size %d is not a multiple of %d", dir.Size, rtSize)
	}
	count := int(dir.Size / rtSize)
	if count > maxUnwindRecords*1024 {
		return nil, false, unwindFailf(
			fmt.Sprintf("exception directory rva 0x%x", dir.VirtualAddress),
			"directory entry count %d is implausibly large", count)
	}
	raw, err := f.readRVA(dir.VirtualAddress, int(dir.Size))
	if err != nil {
		return nil, false, err
	}
	entries := make([]*AMD64RuntimeFunction, 0, count)
	for i := 0; i < count; i++ {
		entry := raw[i*rtSize : i*rtSize+rtSize]
		rt := &AMD64RuntimeFunction{
			BeginRVA:  binary.LittleEndian.Uint32(entry[0:4]),
			EndRVA:    binary.LittleEndian.Uint32(entry[4:8]),
			UnwindRVA: binary.LittleEndian.Uint32(entry[8:12]),
			Source:    int64(dir.VirtualAddress) + int64(i*rtSize),
		}
		if rt.BeginRVA == 0 && rt.EndRVA == 0 && rt.UnwindRVA == 0 {
			continue // null padding entry
		}
		if rt.EndRVA <= rt.BeginRVA || rt.UnwindRVA == 0 {
			return nil, false, unwindFailf(
				fmt.Sprintf("exception directory entry #%d (rva 0x%x)", i, rt.Source),
				"illegal RUNTIME_FUNCTION begin=0x%x end=0x%x unwind=0x%x",
				rt.BeginRVA, rt.EndRVA, rt.UnwindRVA)
		}
		entries = append(entries, rt)
	}
	// Any pair of overlapping half-open ranges makes the directory
	// corrupt, regardless of whether it covers the target PC.
	sort.Slice(entries, func(i, j int) bool { return entries[i].BeginRVA < entries[j].BeginRVA })
	for i := 1; i < len(entries); i++ {
		if entries[i].BeginRVA < entries[i-1].EndRVA {
			return nil, false, unwindFailf(
				fmt.Sprintf("exception directory entry (rva 0x%x)", entries[i].Source),
				"overlapping RUNTIME_FUNCTION ranges 0x%x-0x%x and 0x%x-0x%x",
				entries[i-1].BeginRVA, entries[i-1].EndRVA,
				entries[i].BeginRVA, entries[i].EndRVA)
		}
	}
	for _, rt := range entries {
		if targetRVA >= rt.BeginRVA && targetRVA < rt.EndRVA {
			return rt, true, nil
		}
	}
	return nil, true, nil
}

// parseUnwindInfo decodes one UNWIND_INFO at rva, including the
// complete CHAIN_INFO parent RUNTIME_FUNCTION when present.
func (f *File) parseUnwindInfo(rva uint32) (*AMD64UnwindRecord, error) {
	// Fixed header is 4 bytes; one UNWIND_CODE slot is 2 bytes.
	head, err := f.readRVA(rva, 4)
	if err != nil {
		return nil, err
	}
	verFlags := head[0]
	rec := &AMD64UnwindRecord{
		Version:        int(verFlags & 0x07),
		Flags:          verFlags >> 3,
		PrologSize:     head[1],
		FrameReg:       int(head[2] & 0x0f),
		FrameRegOffset: head[2] >> 4,
		Source:         int64(rva),
	}
	if rec.FrameReg == 0 {
		// A zero FrameRegister field means the function uses no frame
		// register (RAX cannot be designated as one).
		rec.FrameReg = -1
	}
	countCodes := int(head[3])
	if rec.Version != 1 {
		return nil, unsupportedf(fmt.Sprintf("unwind info rva 0x%x", rva),
			"UNWIND_INFO version %d is not supported (only version 1)", rec.Version)
	}
	if countCodes > maxUnwindCodes {
		return nil, unwindFailf(fmt.Sprintf("unwind info rva 0x%x", rva),
			"illegal unwind code count %d", countCodes)
	}
	total := 4 + 2*countCodes
	body, err := f.readRVA(rva, total)
	if err != nil {
		return nil, err
	}
	pos := 4
	for i := 0; i < countCodes; i++ {
		slotOff := pos
		codeOffset := body[pos]
		info := body[pos+1]
		op := info >> 4
		opInfo := info & 0x0f
		pos += 2
		decoded := AMD64UnwindOp{CodeOffset: uint32(codeOffset), Slots: 1, Source: slotOff}
		srcPos := fmt.Sprintf("unwind info rva 0x%x slot %d", rva, i)
		switch op {
		case uwopPushNonvol:
			decoded.Op, decoded.Reg = AMD64OpPushNonvol, int(opInfo)
		case uwopAllocSmall:
			decoded.Op, decoded.AllocSize = AMD64OpAllocSmall, uint32(opInfo+1)*8
		case uwopAllocLarge:
			decoded.Op = AMD64OpAllocLarge
			switch opInfo {
			case 0:
				if i+1 >= countCodes {
					return nil, unwindFailf(srcPos, "ALLOC_LARGE is missing its extension slot")
				}
				ext := body[pos+1]
				pos += 2
				decoded.Slots = 2
				decoded.AllocSize = uint32(ext) * 8
				i++
			case 1:
				if i+2 >= countCodes {
					return nil, unwindFailf(srcPos, "ALLOC_LARGE is missing extension slots")
				}
				decoded.AllocSize = binary.LittleEndian.Uint32(body[pos : pos+4])
				pos += 4
				decoded.Slots = 3
				i += 2
			default:
				return nil, unwindFailf(srcPos, "illegal ALLOC_LARGE info %d", opInfo)
			}
			if decoded.AllocSize == 0 || decoded.AllocSize%8 != 0 {
				return nil, unwindFailf(srcPos, "illegal ALLOC_LARGE size %d", decoded.AllocSize)
			}
		case uwopSetFPReg:
			if rec.FrameReg < 0 {
				return nil, unwindFailf(srcPos, "SET_FPREG present but no frame register is declared")
			}
			if rec.setFPRegOp() != nil {
				return nil, unwindFailf(srcPos, "duplicate SET_FPREG operation")
			}
			decoded.Op, decoded.Reg, decoded.FPOffset =
				AMD64OpSetFPReg, rec.FrameReg, uint32(rec.FrameRegOffset)*16
		case uwopSaveNonvol, uwopSaveNonvolFar:
			decoded.Op = AMD64OpSaveNonvol
			decoded.Reg = int(opInfo)
			needSlots := 2
			if op == uwopSaveNonvolFar {
				needSlots = 3
			}
			if i+(needSlots-1) >= countCodes {
				return nil, unwindFailf(srcPos, "SAVE_NONVOL is missing extension slots")
			}
			if op == uwopSaveNonvol {
				decoded.SaveOffset = uint32(binary.LittleEndian.Uint16(body[pos:pos+2])) * 8
				pos += 2
			} else {
				decoded.SaveOffset = binary.LittleEndian.Uint32(body[pos : pos+4])
				pos += 4
			}
			decoded.Slots = needSlots
			i += needSlots - 1
		case uwopPushMachframe:
			if i != 0 {
				return nil, unwindFailf(srcPos, "PUSH_MACHFRAME must be the first unwind code")
			}
			if opInfo&^1 != 0 {
				return nil, unwindFailf(srcPos, "illegal PUSH_MACHFRAME info %d", opInfo)
			}
			decoded.Op, decoded.MachframeError = AMD64OpPushMachframe, opInfo&1 == 1
		case uwopChainInfo:
			if opInfo != 0 {
				return nil, unwindFailf(srcPos, "illegal CHAIN_INFO info %d", opInfo)
			}
			if i != countCodes-1 {
				return nil, unwindFailf(srcPos, "CHAIN_INFO must be the last unwind code")
			}
			decoded.Op = AMD64OpChain
			// Codes end here; the RUNTIME_FUNCTION tail is word aligned,
			// so an odd slot count is followed by two padding bytes.
			chainOff := total
			if chainOff%4 != 0 {
				chainOff += 2
			}
			tail, err := f.readRVA(rva+uint32(chainOff), 12)
			if err != nil {
				return nil, err
			}
			parent := &AMD64RuntimeFunction{
				BeginRVA:  binary.LittleEndian.Uint32(tail[0:4]),
				EndRVA:    binary.LittleEndian.Uint32(tail[4:8]),
				UnwindRVA: binary.LittleEndian.Uint32(tail[8:12]),
				Source:    int64(rva) + int64(chainOff),
			}
			if parent.EndRVA <= parent.BeginRVA || parent.UnwindRVA == 0 {
				return nil, unwindFailf(
					fmt.Sprintf("chain tail rva 0x%x", rva+uint32(chainOff)),
					"illegal parent RUNTIME_FUNCTION begin=0x%x end=0x%x unwind=0x%x",
					parent.BeginRVA, parent.EndRVA, parent.UnwindRVA)
			}
			normalizeOpOrder(rec)
			rec.Parent = parent
			rec.Size = chainOff + 12
			rec.Ops = append(rec.Ops, decoded)
			return rec, nil
		case uwopSaveXMM, uwopSaveXMMFar, uwopSaveXMM128, uwopSaveXMM128Far:
			return nil, unsupportedf(srcPos, "XMM save operations are not supported by this read-only unwinder")
		case uwopSetFPRegFar:
			return nil, unsupportedf(srcPos, "UWOP_SET_FPREG_LARGE is not supported")
		default:
			return nil, unsupportedf(srcPos, "unsupported unwind opcode %d", op)
		}
		rec.Ops = append(rec.Ops, decoded)
	}
	// On disk the unwind codes are ordered by descending CodeOffset.
	// Normalize to prolog (ascending) order so the engine can reverse the
	// slice literally; each decoded op already spans its extension slots.
	normalizeOpOrder(rec)
	// Optional handler data starts at the word-aligned end of the codes.
	handlerOff := total
	if handlerOff%4 != 0 {
		handlerOff += 2
	}
	rec.Size = handlerOff
	if rec.Flags != 0 {
		hdr, err := f.readRVA(rva+uint32(handlerOff), 4)
		if err != nil {
			return nil, err
		}
		rec.HandlerRVA = binary.LittleEndian.Uint32(hdr[0:4])
		rec.Size = handlerOff + 4
	}
	return rec, nil
}

// normalizeOpOrder puts decoded operations into ascending CodeOffset
// order, i.e. the order the corresponding instructions execute in the
// prolog. The sort is stable so operations sharing an offset keep their
// on-disk relative order.
func normalizeOpOrder(rec *AMD64UnwindRecord) {
	sort.SliceStable(rec.Ops, func(i, j int) bool {
		return rec.Ops[i].CodeOffset < rec.Ops[j].CodeOffset
	})
}
