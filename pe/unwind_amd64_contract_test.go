// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
	"testing"
)

// This test synthesizes complete PE32+ images and sparse snapshot
// memory in pure Go. Every expected register value and every expected
// read address is derived by hand below; no Windows API is involved.

const (
	testImageBase = 0x7ffe_0000_0000 // actual load base, deliberately != header ImageBase
	vaText        = 0x1000
	vaPData       = 0x2000
	vaXData       = 0x3000
)

// ---- tiny PE32+ builder -------------------------------------------------

type builtImage struct {
	disk []byte // ordinary on-disk layout
	mem  []byte // loader-mapped layout (section data at its virtual address)
}

type testSection struct {
	name string
	va   uint32
	data []byte
}

func buildPE(sections []testSection, exceptionSize uint32) *builtImage {
	const (
		fileAlign  = 0x200
		sectAlign  = 0x1000
		headerSize = 0x200
		peOff      = 0x80
	)
	// Lay sections out on disk.
	type placed struct {
		testSection
		raw uint32
	}
	pl := make([]placed, len(sections))
	raw := uint32(headerSize)
	for i, s := range sections {
		pl[i] = placed{testSection: s, raw: raw}
		raw += align4(uint32(len(s.data)), fileAlign)
	}
	sizeOfImage := align4(sections[len(sections)-1].va+uint32(len(sections[len(sections)-1].data)), sectAlign)

	disk := make([]byte, raw)
	// DOS header.
	binary.LittleEndian.PutUint16(disk[0:2], 0x5a4d)
	binary.LittleEndian.PutUint32(disk[0x3c:0x40], peOff)
	// PE signature.
	copy(disk[peOff:peOff+4], []byte{'P', 'E', 0, 0})
	// COFF header.
	coff := FileHeader{
		Machine:              IMAGE_FILE_MACHINE_AMD64,
		NumberOfSections:     uint16(len(sections)),
		SizeOfOptionalHeader: uint16(binary.Size(OptionalHeader64{})),
		Characteristics:      0x0022, // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
	}
	var buf bytes.Buffer
	if err := binary.Write(&buf, binary.LittleEndian, &coff); err != nil {
		panic(err)
	}
	copy(disk[peOff+4:], buf.Bytes())
	// Optional header (PE32+).
	oh := OptionalHeader64{
		Magic:               0x20b,
		MajorLinkerVersion:  14,
		SectionAlignment:    sectAlign,
		FileAlignment:       fileAlign,
		ImageBase:           0x140000000, // intentionally different from testImageBase
		SizeOfImage:         sizeOfImage,
		SizeOfHeaders:       headerSize,
		NumberOfRvaAndSizes: 16,
	}
	if exceptionSize > 0 {
		oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = DataDirectory{VirtualAddress: vaPData, Size: exceptionSize}
	}
	buf.Reset()
	if err := binary.Write(&buf, binary.LittleEndian, &oh); err != nil {
		panic(err)
	}
	optOff := peOff + 4 + 20
	copy(disk[optOff:], buf.Bytes())
	// Section headers.
	shOff := optOff + binary.Size(OptionalHeader64{})
	for i, p := range pl {
		sh := SectionHeader32{}
		copy(sh.Name[:], p.name)
		sh.VirtualSize = uint32(len(p.data))
		sh.VirtualAddress = p.va
		sh.SizeOfRawData = align4(uint32(len(p.data)), fileAlign)
		sh.PointerToRawData = p.raw
		sh.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE
		base := shOff + i*40
		buf.Reset()
		if err := binary.Write(&buf, binary.LittleEndian, &sh); err != nil {
			panic(err)
		}
		copy(disk[base:], buf.Bytes())
		copy(disk[p.raw:], p.data)
	}

	// Memory-mapped variant: headers stay at 0, section bytes move to VAs.
	mem := make([]byte, sizeOfImage)
	copy(mem[:headerSize], disk[:headerSize])
	for _, p := range pl {
		copy(mem[p.va:], p.data)
	}
	return &builtImage{disk: disk, mem: mem}
}

func align4(v, a uint32) uint32 { return (v + a - 1) &^ (a - 1) }

// ---- UNWIND_INFO / RUNTIME_FUNCTION encoders ----------------------------

func ucode(codeOffset, info byte) [2]byte { return [2]byte{codeOffset, info} }

func unwindInfo(flags, prologSize, frameReg, frameOff byte, codes [][2]byte, tail []byte) []byte {
	// Version occupies bits 0..2, flags bits 3..7.
	out := []byte{1 | (flags << 3), prologSize, (frameReg & 0x0f) | (frameOff << 4), byte(len(codes))}
	for _, c := range diskCodes(codes) {
		out = append(out, c[0], c[1])
	}
	if len(out)%4 != 0 {
		out = append(out, 0, 0)
	}
	out = append(out, tail...)
	return out
}

// diskCodes reorders logical unwind codes (written in prolog, i.e.
// ascending CodeOffset order) into the on-disk layout: groups sorted by
// descending CodeOffset, with each multi-slot operation's extension
// slots kept immediately after its primary slot.
func diskCodes(codes [][2]byte) [][2]byte {
	type group struct {
		start int
		width int
		off   byte
	}
	var groups []group
	for i := 0; i < len(codes); {
		op := codes[i][1] >> 4
		info := codes[i][1] & 0x0f
		width := 1
		switch op {
		case uwopAllocLarge:
			if info == 0 {
				width = 2
			} else {
				width = 3
			}
		case uwopSaveNonvol:
			width = 2
		case uwopSaveNonvolFar:
			width = 3
		}
		// Clamp on deliberately truncated inputs; the parser is what
		// reports the missing extension slot as an unwind error.
		if i+width > len(codes) {
			width = len(codes) - i
		}
		groups = append(groups, group{start: i, width: width, off: codes[i][0]})
		i += width
	}
	// Stable sort on descending CodeOffset.
	for i := 1; i < len(groups); i++ {
		for j := i; j > 0 && groups[j-1].off < groups[j].off; j-- {
			groups[j-1], groups[j] = groups[j], groups[j-1]
		}
	}
	out := make([][2]byte, 0, len(codes))
	for _, g := range groups {
		out = append(out, codes[g.start:g.start+g.width]...)
	}
	return out
}

func runtimeFunction(begin, end, unwind uint32) []byte {
	b := make([]byte, 12)
	binary.LittleEndian.PutUint32(b[0:4], begin)
	binary.LittleEndian.PutUint32(b[4:8], end)
	binary.LittleEndian.PutUint32(b[8:12], unwind)
	return b
}

func dword(v uint32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, v)
	return b
}

// ---- sparse snapshot memory ---------------------------------------------

type memChunk struct {
	base uint64
	data []byte
}

type sparseMem struct {
	chunks  []memChunk
	failVA  uint64 // when nonzero, reads intersecting this address fail
	shortVA uint64 // when nonzero, 8-byte reads at this address return 4 bytes
	calls   []AMD64ReadSource
}

func (m *sparseMem) addChunk(base uint64, data []byte) {
	m.chunks = append(m.chunks, memChunk{base, data})
}

func (m *sparseMem) addStack(base uint64, vals ...uint64) {
	m.chunks = append(m.chunks, stackChunk(base, vals...))
}

func stackChunk(base uint64, vals ...uint64) memChunk {
	b := make([]byte, 8*len(vals))
	for i, v := range vals {
		binary.LittleEndian.PutUint64(b[i*8:], v)
	}
	return memChunk{base: base, data: b}
}

func (m *sparseMem) ReadAtVA(p []byte, va uint64) (int, error) {
	m.calls = append(m.calls, AMD64ReadSource{VA: va, Size: len(p)})
	if m.failVA != 0 && va == m.failVA {
		return 0, errors.New("synthetic memory callback failure")
	}
	if m.shortVA != 0 && va == m.shortVA {
		copy(p, p[:0])
		return 4, nil
	}
	for _, c := range m.chunks {
		if va >= c.base && va+uint64(len(p)) <= c.base+uint64(len(c.data)) {
			copy(p, c.data[va-c.base:])
			return len(p), nil
		}
	}
	return 0, errors.New("unmapped snapshot address")
}

// ---- snapshot helpers ----------------------------------------------------

func distinctiveRegs() AMD64RegisterSet {
	var r AMD64RegisterSet
	for i := range r {
		r[i] = 0xa000 + uint64(i)
	}
	return r
}

func openBoth(t *testing.T, img *builtImage) (diskFile, memFile *File) {
	t.Helper()
	f1, err := NewFile(bytes.NewReader(img.disk))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	f2, err := NewFileFromMemory(bytes.NewReader(img.mem))
	if err != nil {
		t.Fatalf("NewFileFromMemory: %v", err)
	}
	return f1, f2
}

func sameFrame(t *testing.T, tag string, got, want AMD64Snapshot) {
	t.Helper()
	if got.RIP != want.RIP || got.RSP != want.RSP || got.Reg != want.Reg {
		t.Errorf("%s: frame mismatch\n got RIP=%#x RSP=%#x regs=%#x\nwant RIP=%#x RSP=%#x regs=%#x",
			tag, got.RIP, got.RSP, got.Reg, want.RIP, want.RSP, want.Reg)
	}
}

func mustUnwind(t *testing.T, f *File, snap AMD64Snapshot, mem *sparseMem) *AMD64UnwindResult {
	t.Helper()
	res, err := f.UnwindAMD64(testImageBase, snap, mem)
	if err != nil {
		t.Fatalf("UnwindAMD64 failed: %v", err)
	}
	return res
}

func regSnapshot(rip, rsp uint64, regs AMD64RegisterSet) AMD64Snapshot {
	return AMD64Snapshot{RIP: rip, RSP: rsp, Reg: regs}
}

// va maps an image RVA to a virtual address under the test load base.
func va(rva uint32) uint64 { return testImageBase + uint64(rva) }

// ---- the contract --------------------------------------------------------

func TestAMD64UnwindContract(t *testing.T) {
	t.Run("leaf", testLeaf)
	t.Run("prolog", testProlog)
	t.Run("frame_base", testFrameBase)
	t.Run("large_alloc", testLargeAlloc)
	t.Run("chain", testChain)
	t.Run("machine_frame", testMachineFrame)
	t.Run("errors", testErrors)
}

func testLeaf(t *testing.T) {
	// Image A: no exception directory at all.
	imgNoDir := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x100)},
	}, 0)
	// Image B: valid directory whose single entry does not cover the PC.
	pdata := runtimeFunction(vaText+0x100, vaText+0x200, vaXData)
	ui := unwindInfo(0, 1, 0, 0, [][2]byte{ucode(0, uwopPushNonvol<<4|AMD64RegRDI)}, nil)
	imgCovered := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x300)},
		{".pdata", vaPData, pdata},
		{".xdata", vaXData, ui},
	}, 12)

	for _, img := range []*builtImage{imgNoDir, imgCovered} {
		disk, mem := openBoth(t, img)
		for _, f := range []*File{disk, mem} {
			const s = uint64(0x100000)
			sm := &sparseMem{}
			sc := stackChunk(s, 0xdeadbeef) // [S] = return address
			sm.addChunk(sc.base, sc.data)
			snap := regSnapshot(va(vaText+(0x10)), s, distinctiveRegs())
			res := mustUnwind(t, f, snap, sm)
			if !res.Leaf || res.Function != nil {
				t.Fatalf("expected leaf unwind, got leaf=%v func=%v", res.Leaf, res.Function)
			}
			want := snap
			want.RIP, want.RSP = 0xdeadbeef, s+8
			sameFrame(t, "leaf", res.Frame, want)
			if len(res.Reads) != 1 || res.Reads[0].VA != s || res.Reads[0].Size != 8 {
				t.Fatalf("leaf reads = %+v, want single 8-byte read at %#x", res.Reads, s)
			}
		}
	}
}

func testProlog(t *testing.T) {
	// Prolog:
	//   off 1: push rdi            (UWOP_PUSH_NONVOL, RDI)
	//   off 4: sub  rsp, 32        (UWOP_ALLOC_SMALL info=3)
	//   off 7: mov  [rsp], rbx     (UWOP_SAVE_NONVOL RBX, scaled off 0)
	// PrologSize 10. Entry RSP = S.
	codes := [][2]byte{
		ucode(1, uwopPushNonvol<<4|AMD64RegRDI),
		ucode(4, uwopAllocSmall<<4|3),
		ucode(7, uwopSaveNonvol<<4|AMD64RegRBX),
		ucode(0, 0), // SAVE_NONVOL extension slot: scaled offset 0
	}
	pdata := runtimeFunction(vaText, vaText+0x100, vaXData)
	ui := unwindInfo(0, 10, 0, 0, codes, nil)
	// The on-disk layout must be in descending CodeOffset order: save@7
	// (with its extension slot), then alloc@4, then push@1. The parser is
	// responsible for normalizing it back to prolog order.
	if ui[4] != 7 || ui[8] != 4 || ui[10] != 1 {
		t.Fatalf("unwind codes are not stored in descending order: % x", ui[4:12])
	}
	img := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x100)},
		{".pdata", vaPData, pdata},
		{".xdata", vaXData, ui},
	}, 12)

	const (
		s       = uint64(0x200000)
		rdiOld  = uint64(0x1111_1111_1111_1111)
		rbxOld  = uint64(0x2222_2222_2222_2222)
		retAddr = uint64(0x3333_3333_3333_3333)
	)
	baseRegs := distinctiveRegs()

	type stop struct {
		name      string
		relPC     uint32
		rsp       uint64
		stack     []memChunk
		applied   int  // number of ops expected to be reversed
		keepRBX   bool // true while the SAVE_NONVOL has not executed
		wantReads int
	}
	stops := []stop{
		// PC at function entry: no prolog instruction has executed.
		{"at_entry", 0, s, []memChunk{stackChunk(s, retAddr)}, 0, true, 1},
		// Exact boundary: the PUSH at offset 1 has just executed.
		{"at_push_boundary", 1, s - 8, []memChunk{stackChunk(s-8, rdiOld, retAddr)}, 1, true, 2},
		// Between push and alloc: still only the push applies.
		{"after_push", 3, s - 8, []memChunk{stackChunk(s-8, rdiOld, retAddr)}, 1, true, 2},
		// Exact boundary: the ALLOC at offset 4 has just executed.
		{"at_alloc_boundary", 4, s - 40, []memChunk{stackChunk(s-8, rdiOld, retAddr)}, 2, true, 2},
		// The two-slot SAVE_NONVOL at offset 7 has not run yet. It must
		// be skipped as one operation; its extension slot is not walked
		// as a separate op.
		{"before_save", 6, s - 40, []memChunk{stackChunk(s-8, rdiOld, retAddr)}, 2, true, 2},
		// Exact boundary: the save has just executed.
		{"at_save_boundary", 7, s - 40, []memChunk{stackChunk(s-40, rbxOld, 0, 0, 0, rdiOld, retAddr)}, 3, false, 3},
		// Last byte of the prolog region; every op remains applicable.
		{"end_of_prolog", 9, s - 40, []memChunk{stackChunk(s-40, rbxOld, 0, 0, 0, rdiOld, retAddr)}, 3, false, 3},
		// PC in the function body, prolog treated as complete.
		{"in_body", 0x50, s - 40, []memChunk{stackChunk(s-40, rbxOld, 0, 0, 0, rdiOld, retAddr)}, 3, false, 3},
	}

	disk, memFile := openBoth(t, img)
	for _, f := range []*File{disk, memFile} {
		for _, st := range stops {
			t.Run(st.name, func(t *testing.T) {
				sm := &sparseMem{}
				for _, c := range st.stack {
					sm.addChunk(c.base, c.data)
				}
				regs := baseRegs
				snap := regSnapshot(va(vaText+(st.relPC)), st.rsp, regs)
				res := mustUnwind(t, f, snap, sm)
				if res.Leaf {
					t.Fatal("prolog case must not be leaf")
				}
				if res.Function == nil || res.Function.BeginRVA != vaText {
					t.Fatalf("selected function = %+v", res.Function)
				}
				if len(res.Applied) != st.applied {
					t.Fatalf("applied %d ops, want %d: %+v", len(res.Applied), st.applied, res.Applied)
				}
				want := snap
				want.RIP, want.RSP = retAddr, s+8
				if st.applied >= 1 {
					want.Reg[AMD64RegRDI] = rdiOld
				}
				if !st.keepRBX {
					want.Reg[AMD64RegRBX] = rbxOld
				}
				sameFrame(t, st.name, res.Frame, want)
				// Input snapshot must be untouched.
				if snap.Reg != regs || snap.RSP != st.rsp || snap.RIP != va(vaText+(st.relPC)) {
					t.Fatal("UnwindAMD64 modified the input snapshot")
				}
				// Every recorded read must be an 8-byte aligned stack slot.
				for _, r := range res.Reads {
					if r.Size != 8 {
						t.Fatalf("read size = %d, want 8", r.Size)
					}
				}
				if len(res.Reads) != st.wantReads {
					t.Fatalf("%s: %d reads, want %d (%+v)", st.name, len(res.Reads), st.wantReads, res.Reads)
				}
			})
		}
	}
}

func testFrameBase(t *testing.T) {
	// Prolog:
	//   off 1: push rbp
	//   off 4: sub  rsp, 40        (ALLOC_SMALL info=4)
	//   off 8: lea  rbp, [rsp+32]  (SET_FPREG, scaled frame offset 2)
	//   off 11: mov  [rbp-24], rsi (SAVE_NONVOL RSI at fixed base +8;
	//                                  rbp = fixed base +32)
	// Entry RSP = S. After the prolog RSP = S-48 and RBP = S-16.
	codes := [][2]byte{
		ucode(1, uwopPushNonvol<<4|AMD64RegRBP),
		ucode(4, uwopAllocSmall<<4|4),
		ucode(8, uwopSetFPReg<<4),
		ucode(11, uwopSaveNonvol<<4|AMD64RegRSI),
		ucode(1, 0), // extension slot: scaled offset 1 -> fixed base + 8
	}
	pdata := runtimeFunction(vaText, vaText+0x100, vaXData)
	ui := unwindInfo(0, 14, AMD64RegRBP, 2, codes, nil)
	img := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x100)},
		{".pdata", vaPData, pdata},
		{".xdata", vaXData, ui},
	}, 12)

	const (
		s       = uint64(0x300000)
		rbpOld  = uint64(0x4444_4444_4444_4444)
		rsiOld  = uint64(0x5555_5555_5555_5555)
		retAddr = uint64(0x6666_6666_6666_6666)
		dyn     = uint64(0x60) // dynamic (alloca-style) stack growth after the prolog
	)

	t.Run("dynamic_rsp", func(t *testing.T) {
		disk, memFile := openBoth(t, img)
		for _, f := range []*File{disk, memFile} {
			sm := &sparseMem{}
			// fixed base S-48: RSI save at +8 = S-40; RBP push at S-8; ret at S.
			sm.addStack(s-48, 0, rsiOld, 0, 0, 0, rbpOld, retAddr)
			regs := distinctiveRegs()
			regs[AMD64RegRBP] = s - 16 // frame register established at SET_FPREG
			snap := regSnapshot(va(vaText+(0x40)), s-48-dyn, regs)
			res := mustUnwind(t, f, snap, sm)
			want := snap
			want.RIP, want.RSP = retAddr, s+8
			want.Reg[AMD64RegRBP] = rbpOld
			want.Reg[AMD64RegRSI] = rsiOld
			sameFrame(t, "dynamic_rsp", res.Frame, want)
			// Reads must be pinned at fixed-area slots, never drifted by RSP.
			wantVAs := map[uint64]bool{s - 40: true, s - 8: true, s: true}
			for _, r := range res.Reads {
				if !wantVAs[r.VA] {
					t.Fatalf("unexpected read at %#x (reads %+v)", r.VA, res.Reads)
				}
			}
			if len(res.Reads) != 3 {
				t.Fatalf("want 3 reads, got %+v", res.Reads)
			}
		}
	})

	t.Run("before_setfpreg", func(t *testing.T) {
		// PC between ALLOC and SET_FPREG: the frame register is not yet
		// usable, so RBP must be ignored and only the first two ops apply.
		disk, _ := openBoth(t, img)
		sm := &sparseMem{}
		sm.addStack(s-8, rbpOld, retAddr)
		regs := distinctiveRegs()
		regs[AMD64RegRBP] = 0xdead // stale value that must not be consulted
		snap := regSnapshot(va(vaText+(5)), s-48, regs)
		res := mustUnwind(t, disk, snap, sm)
		if len(res.Applied) != 2 {
			t.Fatalf("want 2 applied ops before SET_FPREG, got %+v", res.Applied)
		}
		want := snap
		want.RIP, want.RSP = retAddr, s+8
		want.Reg[AMD64RegRBP] = rbpOld
		sameFrame(t, "before_setfpreg", res.Frame, want)
	})
}

func testLargeAlloc(t *testing.T) {
	// Two functions: one per ALLOC_LARGE encoding, each also pushing RBX.
	// H1 at 0x1000: push rbx; ALLOC_LARGE info=0 ext=5 -> 40 bytes.
	// H2 at 0x1200: push rbx; ALLOC_LARGE info=1 size=0x1000.
	h1Codes := [][2]byte{
		ucode(1, uwopPushNonvol<<4|AMD64RegRBX),
		ucode(4, uwopAllocLarge<<4|0),
		ucode(0, 5), // extension slot: 5 * 8 = 40
	}
	h2Codes := [][2]byte{
		ucode(1, uwopPushNonvol<<4|AMD64RegRBX),
		ucode(4, uwopAllocLarge<<4|1),
		ucode(0, 0), ucode(0, 0), // 32-bit extension, value 0x1000
	}
	binary.LittleEndian.PutUint16(h2Codes[2][:], 0x1000)
	// The first record carries a language handler RVA. It is recorded in
	// the result but must never be invoked by the unwinder.
	const fakeHandlerRVA = uint32(0x5678)
	ui1 := unwindInfo(1 /* UNW_FLAG_EHANDLER */, 8, 0, 0, h1Codes, dword(fakeHandlerRVA))
	ui2 := unwindInfo(0, 8, 0, 0, h2Codes, nil)
	xdata := append(ui1, ui2...)
	ui2RVA := vaXData + uint32(len(ui1))
	pdata := append(runtimeFunction(vaText, vaText+0x100, vaXData),
		runtimeFunction(vaText+0x200, vaText+0x300, ui2RVA)...)
	img := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x400)},
		{".pdata", vaPData, pdata},
		{".xdata", vaXData, xdata},
	}, 24)

	cases := []struct {
		name  string
		relPC uint32
		alloc uint64
		slots int
	}{
		{"alloc_large_scaled", 0x50, 40, 2},
		{"alloc_large_u32", 0x250, 0x1000, 3},
	}
	t.Run("recorded_handler_and_opcodes", func(t *testing.T) {
		disk, _ := openBoth(t, img)
		res := mustUnwind(t, disk, regSnapshot(va(vaText+0x50), 0x400000-8-40, distinctiveRegs()),
			func() *sparseMem {
				sm := &sparseMem{}
				sm.addStack(0x400000-8, 0x7777, 0x8888)
				return sm
			}())
		if res.Records[0].HandlerRVA != fakeHandlerRVA {
			t.Fatalf("handler rva = %#x, want %#x", res.Records[0].HandlerRVA, fakeHandlerRVA)
		}
		large := res.Records[0].Ops[1]
		if large.Op != AMD64OpAllocLarge || large.Slots != 2 {
			t.Fatalf("op decode = %+v", large)
		}
	})

	disk, memFile := openBoth(t, img)
	for _, f := range []*File{disk, memFile} {
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				const s = uint64(0x400000)
				rbxOld := uint64(0x7777_7777_7777_7777)
				ret := uint64(0x8888_8888_8888_8888)
				low := s - 8 - tc.alloc
				sm := &sparseMem{}
				sm.addStack(s-8, rbxOld, ret) // push slot and return address
				regs := distinctiveRegs()
				snap := regSnapshot(va(vaText+(tc.relPC)), low, regs)
				res := mustUnwind(t, f, snap, sm)
				if len(res.Records) != 1 || len(res.Records[0].Ops) != 2 {
					t.Fatalf("decoded ops = %+v", res.Records)
				}
				var large *AMD64UnwindOp
				for i := range res.Records[0].Ops {
					if res.Records[0].Ops[i].Op == AMD64OpAllocLarge {
						large = &res.Records[0].Ops[i]
					}
				}
				if large == nil || large.AllocSize != uint32(tc.alloc) || large.Slots != tc.slots {
					t.Fatalf("large alloc decode = %+v, want size %#x slots %d", large, tc.alloc, tc.slots)
				}
				want := snap
				want.RIP, want.RSP = ret, s+8
				want.Reg[AMD64RegRBX] = rbxOld
				sameFrame(t, tc.name, res.Frame, want)
			})
		}
	}
}

func testChain(t *testing.T) {
	// Child C  [0x1000,0x1080): SAVE_NONVOL R12 at fixed base, CHAIN_INFO -> parent P.
	// Parent P [0x1200,0x1300): push rdi; ALLOC_SMALL 32; CHAIN_INFO -> grandparent GP.
	// GP      [0x1400,0x1500): ALLOC_LARGE 0x1000 (info=1), terminal.
	cCodes := [][2]byte{
		ucode(2, uwopSaveNonvol<<4|AMD64RegR12),
		ucode(0, 0), // SAVE_NONVOL extension slot: scaled offset 0
		ucode(0, uwopChainInfo<<4),
	}
	pCodes := [][2]byte{
		ucode(1, uwopPushNonvol<<4|AMD64RegRDI),
		ucode(4, uwopAllocSmall<<4|3),
		ucode(0, uwopChainInfo<<4),
	}
	gpCodes := [][2]byte{
		ucode(1, uwopAllocLarge<<4|1),
		ucode(0, 0), ucode(0, 0),
	}
	binary.LittleEndian.PutUint16(gpCodes[1][:], 0x1000)

	rtP := runtimeFunction(vaText+0x200, vaText+0x300, 0) // parent UnwindRVA patched later
	rtGP := runtimeFunction(vaText+0x400, vaText+0x500, 0)
	uiC := unwindInfo(0, 5, 0, 0, cCodes, rtP)
	uiP := unwindInfo(0, 8, 0, 0, pCodes, rtGP)
	uiGP := unwindInfo(0, 4, 0, 0, gpCodes, nil)
	uiPRVA := vaXData + uint32(len(uiC))
	uiGPRVA := uiPRVA + uint32(len(uiP))
	binary.LittleEndian.PutUint32(rtP[8:12], uiPRVA)
	binary.LittleEndian.PutUint32(rtGP[8:12], uiGPRVA)
	// Rebuild with the patched chain tails embedded.
	uiC = unwindInfo(0, 5, 0, 0, cCodes, rtP)
	uiP = unwindInfo(0, 8, 0, 0, pCodes, rtGP)
	xdata := append(append(uiC, uiP...), uiGP...)
	pdata := runtimeFunction(vaText, vaText+0x80, vaXData)
	img := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x600)},
		{".pdata", vaPData, pdata},
		{".xdata", vaXData, xdata},
	}, 12)

	// Concrete stack geometry.
	const sChild = uint64(0x500000) // child entry RSP; R12 is saved here
	const tParent = sChild + 40     // parent entry RSP (push 8 + alloc 32)
	gpEntry := tParent + 0x1000     // grandparent entry RSP (its alloc is 0x1000)
	r12Old := uint64(0x9999_9999_9999_9999)
	rdiOld := uint64(0xaaaa_aaaa_aaaa_aaaa)
	gpRet := uint64(0xbbbb_bbbb_bbbb_bbbb)

	disk, memFile := openBoth(t, img)
	for _, f := range []*File{disk, memFile} {
		sm := &sparseMem{}
		sm.addStack(sChild, r12Old)    // child R12 save
		sm.addStack(sChild+32, rdiOld) // parent push slot (tParent-8)
		sm.addStack(gpEntry, gpRet)    // grandparent return address
		regs := distinctiveRegs()
		snap := regSnapshot(va(vaText+(0x20)), sChild, regs)
		res := mustUnwind(t, f, snap, sm)
		if len(res.Records) != 3 || res.Records[0].Parent == nil || res.Records[1].Parent == nil {
			t.Fatalf("chain records = %+v", res.Records)
		}
		if res.Records[2].Parent != nil {
			t.Fatal("terminal record must not carry a parent")
		}
		want := snap
		want.RIP, want.RSP = gpRet, gpEntry+8
		want.Reg[AMD64RegR12] = r12Old
		want.Reg[AMD64RegRDI] = rdiOld
		sameFrame(t, "chain", res.Frame, want)
		// Exactly three reads: two local saves plus one, and only one,
		// ordinary return-address read.
		if len(res.Reads) != 3 {
			t.Fatalf("chain reads = %+v, want exactly 3", res.Reads)
		}
		if res.Reads[2].VA != gpEntry {
			t.Fatalf("sole return-address read at %#x, want %#x", res.Reads[2].VA, gpEntry)
		}
		// Applied ops combine child save, parent allocation/push and the
		// grandparent allocation, one record at a time; within each
		// record operations are reversed prolog order.
		gotOps := []int{}
		for _, op := range res.Applied {
			gotOps = append(gotOps, op.Op)
		}
		wantOps := []int{
			AMD64OpSaveNonvol,                    // child
			AMD64OpAllocSmall, AMD64OpPushNonvol, // parent (reversed)
			AMD64OpAllocLarge, // grandparent
		}
		if len(gotOps) != len(wantOps) {
			t.Fatalf("applied ops = %v, want %v", gotOps, wantOps)
		}
		for i := range wantOps {
			if gotOps[i] != wantOps[i] {
				t.Fatalf("applied ops = %v, want %v", gotOps, wantOps)
			}
		}
	}
}

func testMachineFrame(t *testing.T) {
	// M0: bare PUSH_MACHFRAME without error code (32-byte trap frame).
	// M1: bare PUSH_MACHFRAME with error code (40-byte trap frame).
	// In the ABI a machine frame is always the first prolog operation,
	// so neither record has other codes.
	m0Codes := [][2]byte{ucode(1, uwopPushMachframe<<4|0)}
	m1Codes := [][2]byte{ucode(1, uwopPushMachframe<<4|1)}
	ui0 := unwindInfo(0, 4, 0, 0, m0Codes, nil)
	ui1 := unwindInfo(0, 4, 0, 0, m1Codes, nil)
	ui1RVA := vaXData + uint32(len(ui0))
	xdata := append(ui0, ui1...)
	pdata := append(runtimeFunction(vaText, vaText+0x100, vaXData),
		runtimeFunction(vaText+0x200, vaText+0x300, ui1RVA)...)
	img := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x400)},
		{".pdata", vaPData, pdata},
		{".xdata", vaXData, xdata},
	}, 24)

	disk, memFile := openBoth(t, img)
	for _, f := range []*File{disk, memFile} {
		t.Run("no_error_code", func(t *testing.T) {
			const s = uint64(0x600000)
			origRSP := uint64(0xcccc_cccc_cccc_cccc)
			trapRIP := uint64(0xdddd_dddd_dddd_dddd)
			sm := &sparseMem{}
			// Trap frame at S-32: +0x00 orig RSP, +0x08 RFLAGS, +0x10 CS, +0x18 RIP.
			sm.addStack(s-32, origRSP, 0, 0, trapRIP)
			snap := regSnapshot(va(vaText+(0x10)), s-32, distinctiveRegs())
			res := mustUnwind(t, f, snap, sm)
			want := snap
			want.RIP, want.RSP = trapRIP, origRSP
			sameFrame(t, "machframe32", res.Frame, want)
			if len(res.Reads) != 2 || res.Reads[0].VA != s-32 || res.Reads[1].VA != s-32+0x18 {
				t.Fatalf("machframe reads = %+v", res.Reads)
			}
		})
		t.Run("with_error_code", func(t *testing.T) {
			const s = uint64(0x700000)
			origRSP := uint64(0xeeee_eeee_eeee_eeee)
			trapRIP := uint64(0xffff_ffff_ffff_ffff)
			sm := &sparseMem{}
			// The 40-byte error-code trap frame starts at S-40; the
			// trailing error code slot (+0x20) is never read.
			sm.addStack(s-40, origRSP, 0, 0, trapRIP, 0xc0de)
			snap := regSnapshot(va(vaText+(0x210)), s-40, distinctiveRegs())
			res := mustUnwind(t, f, snap, sm)
			want := snap
			want.RIP, want.RSP = trapRIP, origRSP
			sameFrame(t, "machframe40", res.Frame, want)
			// Two trap-frame reads only; no extra return-address pop.
			if len(res.Reads) != 2 {
				t.Fatalf("machframe40 reads = %+v, want 2", res.Reads)
			}
			for _, r := range res.Reads {
				if r.VA >= s {
					t.Fatalf("machine frame read %#x escaped the trap frame", r.VA)
				}
			}
		})
	}
}

func testErrors(t *testing.T) {
	// A legal image reused at the end to prove read-only state survives.
	goodCodes := [][2]byte{ucode(1, uwopPushNonvol<<4|AMD64RegRDI)}
	goodUI := unwindInfo(0, 2, 0, 0, goodCodes, nil)
	goodPDATA := runtimeFunction(vaText, vaText+0x100, vaXData)
	goodImg := buildPE([]testSection{
		{".text", vaText, make([]byte, 0x100)},
		{".pdata", vaPData, goodPDATA},
		{".xdata", vaXData, goodUI},
	}, 12)
	goodFile, err := NewFile(bytes.NewReader(goodImg.disk))
	if err != nil {
		t.Fatal(err)
	}

	checkFail := func(t *testing.T, name string, f *File, snap AMD64Snapshot, sm *sparseMem, wantSubstring string) {
		t.Helper()
		before := snap
		res, err := f.UnwindAMD64(testImageBase, snap, sm)
		if err == nil {
			t.Fatalf("%s: expected error containing %q, got result %+v", name, wantSubstring, res)
		}
		if !strings.Contains(err.Error(), wantSubstring) {
			t.Fatalf("%s: error %q does not contain %q", name, err.Error(), wantSubstring)
		}
		if res != nil {
			t.Fatalf("%s: no partial result may be published, got %+v", name, res)
		}
		if snap != before {
			t.Fatalf("%s: input snapshot mutated", name)
		}
	}

	t.Run("corrupt_directory_size", func(t *testing.T) {
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, make([]byte, 10)}, // present, wrong size
		}, 10)
		f, _ := openBoth(t, img)
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		checkFail(t, "corrupt dir", f, snap, &sparseMem{}, "multiple of")
	})

	t.Run("unmapped_xdata", func(t *testing.T) {
		pdata := runtimeFunction(vaText, vaText+0x100, 0x9999) // RVA outside every section
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, pdata},
		}, 12)
		f, _ := openBoth(t, img)
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		checkFail(t, "unmapped xdata", f, snap, &sparseMem{}, "not mapped")
	})

	t.Run("illegal_slot_count", func(t *testing.T) {
		// countCodes claims 255 slots but the xdata section ends after 4 bytes.
		trunc := []byte{1, 2, 0, 255}
		pdata := runtimeFunction(vaText, vaText+0x100, vaXData)
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, pdata},
			{".xdata", vaXData, trunc},
		}, 12)
		f, _ := openBoth(t, img)
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		checkFail(t, "illegal slots", f, snap, &sparseMem{}, "exceeds section bounds")
	})

	t.Run("missing_extension_slot", func(t *testing.T) {
		bad := unwindInfo(0, 4, 0, 0, [][2]byte{ucode(1, uwopAllocLarge<<4|0)}, nil)
		pdata := runtimeFunction(vaText, vaText+0x100, vaXData)
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, pdata},
			{".xdata", vaXData, bad},
		}, 12)
		f, _ := openBoth(t, img)
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		checkFail(t, "missing extension", f, snap, &sparseMem{}, "extension slot")
	})

	t.Run("overlapping_ranges", func(t *testing.T) {
		pdata := append(runtimeFunction(vaText, vaText+0x100, vaXData),
			runtimeFunction(vaText+0x80, vaText+0x180, vaXData)...)
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x200)},
			{".pdata", vaPData, pdata},
			{".xdata", vaXData, goodUI},
		}, 24)
		f, _ := openBoth(t, img)
		snap := regSnapshot(va(vaText+(0x90)), 0x100000, distinctiveRegs())
		checkFail(t, "overlap", f, snap, &sparseMem{}, "overlapping")
	})

	t.Run("chain_cycle", func(t *testing.T) {
		// A chains to B whose UNWIND_INFO chains straight back to A.
		rtB := runtimeFunction(vaText+0x200, vaText+0x300, 0)
		uiA := unwindInfo(0, 2, 0, 0, [][2]byte{
			ucode(1, uwopSaveNonvol<<4|AMD64RegR12),
			ucode(0, 0), // SAVE_NONVOL extension slot
			ucode(0, uwopChainInfo<<4),
		}, rtB)
		rtA := runtimeFunction(vaText, vaText+0x100, vaXData)
		binary.LittleEndian.PutUint32(rtB[8:12], vaXData+uint32(len(uiA)))
		uiA = unwindInfo(0, 2, 0, 0, [][2]byte{
			ucode(1, uwopSaveNonvol<<4|AMD64RegR12),
			ucode(0, 0), // SAVE_NONVOL extension slot
			ucode(0, uwopChainInfo<<4),
		}, rtB)
		uiB := unwindInfo(0, 2, 0, 0, [][2]byte{
			ucode(1, uwopPushNonvol<<4|AMD64RegRDI),
			ucode(0, uwopChainInfo<<4),
		}, rtA)
		xdata := append(uiA, uiB...)
		pdata := append(rtA, rtB...)
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x400)},
			{".pdata", vaPData, pdata},
			{".xdata", vaXData, xdata},
		}, 24)
		f, _ := openBoth(t, img)
		sm := &sparseMem{}
		sm.addStack(0x100000, 0x1, 0x2, 0x3, 0x4)
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		checkFail(t, "cycle", f, snap, sm, "cyclic")
	})

	t.Run("unknown_opcode", func(t *testing.T) {
		// Opcode 13 has no meaning in the x64 ABI.
		bad := unwindInfo(0, 4, 0, 0, [][2]byte{ucode(1, 13<<4)}, nil)
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, goodPDATA},
			{".xdata", vaXData, bad},
		}, 12)
		f, _ := openBoth(t, img)
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		_, err := f.UnwindAMD64(testImageBase, snap, &sparseMem{})
		if !errors.Is(err, ErrAMD64UnwindNotSupported) || !strings.Contains(err.Error(), "opcode 13") {
			t.Fatalf("unknown opcode err = %v", err)
		}
	})

	t.Run("arithmetic_wrap", func(t *testing.T) {
		// SAVE_NONVOL_FAR (3 slots) with a 16-byte save offset applied to
		// a saturated RSP: the save address wraps past uint64.
		bad := unwindInfo(0, 4, 0, 0, [][2]byte{
			ucode(1, uwopSaveNonvolFar<<4|AMD64RegRBX),
			ucode(0x10, 0),
			ucode(0, 0), // SaveOffset = 0x10
		}, nil)
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, goodPDATA},
			{".xdata", vaXData, bad},
		}, 12)
		f, _ := openBoth(t, img)
		snap := regSnapshot(va(vaText+(0x10)), ^uint64(0), distinctiveRegs())
		checkFail(t, "wrap", f, snap, &sparseMem{}, "wrapped")
	})

	t.Run("rip_outside_image", func(t *testing.T) {
		f, _ := openBoth(t, goodImg)
		snap := regSnapshot(testImageBase-8, 0x100000, distinctiveRegs())
		checkFail(t, "rip below base", f, snap, &sparseMem{}, "below image base")
	})

	t.Run("short_memory_read", func(t *testing.T) {
		f, _ := openBoth(t, goodImg)
		sm := &sparseMem{shortVA: 0x100000}
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		checkFail(t, "short read", f, snap, sm, "short memory read")

		sm2 := &sparseMem{failVA: 0x100000}
		checkFail(t, "callback error", f, snap, sm2, "memory read failed")
	})

	t.Run("declared_epilogue_and_unsupported", func(t *testing.T) {
		f, _ := openBoth(t, goodImg)
		snap := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		snap.InEpilogue = true
		if _, err := f.UnwindAMD64(testImageBase, snap, &sparseMem{}); !errors.Is(err, ErrAMD64UnwindNotSupported) {
			t.Fatalf("epilogue err = %v, want ErrAMD64UnwindNotSupported", err)
		}
		// XMM save op (UWOP_SAVE_XMM128 = 8) is explicitly unsupported.
		xmm := unwindInfo(0, 4, 0, 0, [][2]byte{ucode(1, uwopSaveXMM128<<4|6)}, nil)
		img := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, goodPDATA},
			{".xdata", vaXData, xmm},
		}, 12)
		fx, _ := openBoth(t, img)
		snap2 := regSnapshot(va(vaText+(0x10)), 0x100000, distinctiveRegs())
		_, err := fx.UnwindAMD64(testImageBase, snap2, &sparseMem{})
		if !errors.Is(err, ErrAMD64UnwindNotSupported) {
			t.Fatalf("xmm err = %v, want ErrAMD64UnwindNotSupported", err)
		}
		// UNWIND_INFO version 2 is unsupported as well.
		v2 := unwindInfo(0, 4, 0, 0, goodCodes, nil)
		v2[0] = 2 // version nibble = 2, flags = 0
		img2 := buildPE([]testSection{
			{".text", vaText, make([]byte, 0x100)},
			{".pdata", vaPData, goodPDATA},
			{".xdata", vaXData, v2},
		}, 12)
		f2, _ := openBoth(t, img2)
		_, err = f2.UnwindAMD64(testImageBase, snap2, &sparseMem{})
		if !errors.Is(err, ErrAMD64UnwindNotSupported) {
			t.Fatalf("version err = %v, want ErrAMD64UnwindNotSupported", err)
		}
	})

	// After every failure path, the same File must still unwind a legal
	// snapshot: nothing caches half-parsed state. The failure calls here
	// are deliberately driven through goodFile itself before the legal
	// unwind succeeds on that very same instance.
	t.Run("state_not_polluted", func(t *testing.T) {
		const s = uint64(0x800000)
		rdiOld := uint64(0x1234_5678_9abc_def0)
		ret := uint64(0x0fed_cba9_8765_4321)
		regs := distinctiveRegs()
		snap := regSnapshot(va(vaText+(0x10)), s-8, regs)

		// A short read, a hard callback failure and a declared epilogue
		// must all fail without leaving state behind on the File.
		if _, err := goodFile.UnwindAMD64(testImageBase, snap, &sparseMem{shortVA: s - 8}); err == nil {
			t.Fatal("short read unexpectedly succeeded")
		}
		if _, err := goodFile.UnwindAMD64(testImageBase, snap, &sparseMem{failVA: s - 8}); err == nil {
			t.Fatal("callback failure unexpectedly succeeded")
		}
		epi := snap
		epi.InEpilogue = true
		if _, err := goodFile.UnwindAMD64(testImageBase, epi, &sparseMem{}); !errors.Is(err, ErrAMD64UnwindNotSupported) {
			t.Fatalf("epilogue err = %v", err)
		}

		sm := &sparseMem{}
		sm.addStack(s-8, rdiOld, ret) // push slot at s-8, return address at s
		res := mustUnwind(t, goodFile, snap, sm)
		want := snap
		want.RIP, want.RSP = ret, s+8
		want.Reg[AMD64RegRDI] = rdiOld
		sameFrame(t, "reuse", res.Frame, want)

		// A second legal unwind on the same File must be independent of
		// the first one (no shared result or record state).
		res2 := mustUnwind(t, goodFile, snap, sm)
		sameFrame(t, "reuse-again", res2.Frame, want)
	})
}
