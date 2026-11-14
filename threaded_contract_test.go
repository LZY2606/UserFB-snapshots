package macho_test

// Contract tests for legacy LC_DYLD_INFO(_ONLY) threaded bind/rebase chains on
// arm64e. The images are data-only: no machine code is present or run and no
// dependency library is loaded.

import (
	"bytes"
	"encoding/binary"
	"testing"

	macho "github.com/blacktop/go-macho"
	"github.com/blacktop/go-macho/task_support/fixture"
	"github.com/blacktop/go-macho/task_support/taskcheck"
	"github.com/blacktop/go-macho/types"
	"github.com/blacktop/go-macho/types/objc"
)

const (
	tBase     = uint64(0x100000000)
	dataOff   = uint64(0x1000)
	data2Off  = uint64(0x8000)
	imageSize = 0x9000
)

func uleb(v uint64) []byte {
	var out []byte
	for {
		b := byte(v & 0x7f)
		v >>= 7
		if v != 0 {
			out = append(out, b|0x80)
		} else {
			return append(out, b)
		}
	}
}

func sleb(v int64) []byte {
	var out []byte
	for {
		b := byte(v & 0x7f)
		next := v >> 7
		if next == 0 && b&0x40 == 0 || next == -1 && b&0x40 != 0 {
			return append(out, b)
		}
		out = append(out, b|0x80)
		v = next
	}
}

// Slot encoders mirror the legacy arm64e threaded chain bit layout.
func plainRebaseSlot(target, next uint64) uint64 {
	low := target & ((uint64(1) << 43) - 1)
	high8 := (target >> 56) & 0xff
	return low | high8<<43 | (next&0x7ff)<<51
}

func authRebaseSlot(off32, diversity, addrDiv, key, next uint64) uint64 {
	return off32&0xffffffff |
		diversity<<32 | (addrDiv&1)<<48 | (key&3)<<49 |
		(next&0x7ff)<<51 | uint64(1)<<63
}

func plainBindSlot(ordinal uint16, slotAddend int64, next uint64) uint64 {
	add := uint64(slotAddend) & 0x7ffff
	return uint64(ordinal) | add<<32 | (next&0x7ff)<<51 | uint64(1)<<62
}

func authBindSlot(ordinal uint16, diversity, addrDiv, key, next uint64) uint64 {
	return uint64(ordinal) |
		diversity<<32 | (addrDiv&1)<<48 | (key&3)<<49 |
		(next&0x7ff)<<51 | uint64(1)<<62 | uint64(1)<<63
}

type segSpec struct {
	name   string
	addr   uint64
	off    uint64
	filesz uint64
	sects  []types.Section64
}

// threadedSpec describes one data-only threaded test image.
type threadedSpec struct {
	segs       []segSpec
	bind       []byte
	rebase     []byte
	slots      map[uint64]uint64 // file offset -> raw slot value
	withSymtab bool
	extraCmds  int
}

func buildThreadedImage(t *testing.T, spec threadedSpec) []byte {
	t.Helper()
	var h bytes.Buffer
	write := func(v any) {
		if err := binary.Write(&h, binary.LittleEndian, v); err != nil {
			t.Fatal(err)
		}
	}
	segsize := binary.Size(types.Segment64{})
	secsize := binary.Size(types.Section64{})
	dysize := binary.Size(types.DyldInfoCmd{})
	libsize := 40
	symsize := binary.Size(types.SymtabCmd{})

	ncmds := uint32(len(spec.segs) + 2 + spec.extraCmds)
	if spec.withSymtab {
		ncmds++
	}
	segmentBytes := 0
	for _, s := range spec.segs {
		segmentBytes += segsize + len(s.sects)*secsize
	}
	cmdSize := uint32(segmentBytes + dysize + libsize)
	if spec.withSymtab {
		cmdSize += uint32(symsize)
	}
	if spec.withSymtab {
		cmdSize += uint32(symsize)
	}
	write(types.FileHeader{
		Magic:        types.Magic64,
		CPU:          types.CPUArm64,
		SubCPU:       types.CPUSubtypeArm64E,
		Type:         types.MH_DYLIB,
		NCommands:    ncmds,
		SizeCommands: cmdSize,
	})
	for _, s := range spec.segs {
		seg := types.Segment64{
			LoadCmd: types.LC_SEGMENT_64,
			Len:     uint32(segsize + len(s.sects)*secsize),
			Addr:    s.addr,
			Memsz:   s.filesz,
			Offset:  s.off,
			Filesz:  s.filesz,
			Nsect:   uint32(len(s.sects)),
		}
		copy(seg.Name[:], s.name)
		write(seg)
		for _, sec := range s.sects {
			write(sec)
		}
	}
	const (
		rebaseOff = uint32(0x3000) // free LINKEDIT-area gap between __DATA and __DATA2
		bindOff   = uint32(0x3080)
		symOff    = uint32(0x3200)
		strOff    = uint32(0x3210)
	)
	write(types.DyldInfoCmd{
		LoadCmd:    types.LC_DYLD_INFO_ONLY,
		Len:        uint32(dysize),
		RebaseOff:  rebaseOff,
		RebaseSize: uint32(len(spec.rebase)),
		BindOff:    bindOff,
		BindSize:   uint32(len(spec.bind)),
	})
	write(types.DylibCmd{LoadCmd: types.LC_LOAD_DYLIB, Len: uint32(libsize), NameOffset: 24})
	h.WriteString("/fixture/lib\x00")
	for h.Len()%8 != 0 {
		h.WriteByte(0)
	}

	if spec.withSymtab {
		write(types.SymtabCmd{
			LoadCmd: types.LC_SYMTAB,
			Len:     uint32(symsize),
			Symoff:  symOff,
			Nsyms:   1,
			Stroff:  strOff,
			Strsize: uint32(len("_my_local\x00")),
		})
	}

	data := make([]byte, imageSize)
	copy(data, h.Bytes())
	copy(data[rebaseOff:], spec.rebase)
	copy(data[bindOff:], spec.bind)
	for off, val := range spec.slots {
		binary.LittleEndian.PutUint64(data[off:], val)
	}
	if spec.withSymtab {
		nl := types.Nlist64{
			Nlist: types.Nlist{Name: 1, Type: types.N_SECT | types.N_EXT, Sect: 1},
			Value: tBase + 0x1600,
		}
		var nb bytes.Buffer
		write2 := func(v any) {
			if err := binary.Write(&nb, binary.LittleEndian, v); err != nil {
				t.Fatal(err)
			}
		}
		write2(nl)
		copy(data[symOff:], nb.Bytes())
		copy(data[strOff:], "\x00_my_local\x00")
	}
	return data
}

// richThreadedImage builds an image whose chain mixes plain/authenticated
// rebases and binds, reuses one ordinal in a second segment, and includes a
// self-image bind and an unchained ordinary pointer.
func richThreadedImage(t *testing.T) []byte {
	const (
		chainA = dataOff + 0x100 // __DATA + 0x100
		chainB = data2Off + 0x40 // __DATA2 + 0x40
	)
	bind := []byte{0xd0}
	bind = append(bind, uleb(3)...) // ordinal table size 3
	// table[0]: external _OBJC_CLASS_$_Host, template addend 10
	bind = append(bind, 0x11)
	bind = append(bind, 0x40)
	bind = append(bind, "_OBJC_CLASS_$_Host\x00"...)
	bind = append(bind, 0x60)
	bind = append(bind, sleb(10)...)
	bind = append(bind, 0x90)
	// table[1]: weak external authenticated symbol, template addend 7
	bind = append(bind, 0x11)
	bind = append(bind, 0x40|types.BIND_SYMBOL_FLAGS_WEAK_IMPORT)
	bind = append(bind, "_auth_sym\x00"...)
	bind = append(bind, 0x60)
	bind = append(bind, sleb(7)...)
	bind = append(bind, 0x90)
	// table[2]: self-image symbol, template addend 0
	bind = append(bind, 0x30) // SET_DYLIB_SPECIAL_IMM 0 (this image)
	bind = append(bind, 0x40)
	bind = append(bind, "_my_local\x00"...)
	bind = append(bind, 0x60, 0x00) // explicitly reset the addend state to 0
	bind = append(bind, 0x90)
	// APPLY chain A in __DATA (segment index 1)
	bind = append(bind, 0x71)
	bind = append(bind, uleb(0x100)...)
	bind = append(bind, 0xd1)
	// APPLY chain B in __DATA2 (segment index 2)
	bind = append(bind, 0x72)
	bind = append(bind, uleb(0x40)...)
	bind = append(bind, 0xd1)
	bind = append(bind, 0x00) // DONE

	slots := map[uint64]uint64{
		chainA + 0x00: plainRebaseSlot(tBase+0x1400, 1),
		chainA + 0x08: authRebaseSlot(0x300, 0x1234, 1, 2, 1),
		chainA + 0x10: plainBindSlot(0, 5, 1),
		chainA + 0x18: authBindSlot(1, 0xbeef, 0, 1, 1),
		chainA + 0x20: plainBindSlot(0, -3, 1),
		chainA + 0x28: plainBindSlot(2, 0, 0),
		chainB:        plainBindSlot(0, 2, 0),
		// An ordinary pointer that is not part of any chain.
		dataOff + 0x700: 0x00000abcdef00000,
	}

	dataConst := types.Section64{Addr: tBase + 0x1100, Size: 0x300, Offset: 0x1100}
	copy(dataConst.Name[:], "__objc_const")
	copy(dataConst.Seg[:], "__DATA")
	cstring := types.Section64{Addr: tBase + 0x1400, Size: 0x40, Offset: 0x1400}
	copy(cstring.Name[:], "__cstring")
	copy(cstring.Seg[:], "__DATA")
	data2Const := types.Section64{Addr: tBase + 0x8040, Size: 0x10, Offset: 0x8040}
	copy(data2Const.Name[:], "__objc_data2")
	copy(data2Const.Seg[:], "__DATA2")

	return buildThreadedImage(t, threadedSpec{
		segs: []segSpec{
			{name: "__TEXT", addr: tBase, off: 0, filesz: dataOff},
			{name: "__DATA", addr: tBase + dataOff, off: dataOff, filesz: 0x2000,
				sects: []types.Section64{dataConst, cstring}},
			{name: "__DATA2", addr: tBase + data2Off, off: data2Off, filesz: 0x1000,
				sects: []types.Section64{data2Const}},
		},
		bind:       bind,
		slots:      slots,
		withSymtab: true,
	})
}

func openRich(t *testing.T) *macho.File {
	t.Helper()
	f, err := macho.NewFile(bytes.NewReader(richThreadedImage(t)))
	if err != nil {
		t.Fatal(err)
	}
	return f
}

// TestThreadedRecords covers record decoding, authenticated slots, signed
// addends, ordinal reuse across segments and truncated input.
func TestThreadedRecords(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := openRich(t)

		rebases, err := f.GetRebaseInfo()
		if err != nil {
			t.Fatalf("GetRebaseInfo: %v", err)
		}
		binds, err := f.GetBindInfo()
		if err != nil {
			t.Fatalf("GetBindInfo: %v", err)
		}
		c.Check(len(rebases) == 2, "expected 2 threaded rebases, got %d", len(rebases))
		c.Check(len(binds) == 5, "expected 5 threaded binds (4 chain A + 1 chain B), got %d", len(binds))

		// Plain rebase: absolute preferred VM target, no fabricated auth data.
		plain := rebases[0]
		c.Check(plain.Threaded != nil && !plain.Threaded.Authenticated,
			"first rebase must be a non-authenticated threaded record")
		c.Check(plain.Threaded != nil && plain.Threaded.Target == tBase+0x1400,
			"plain rebase target = %#x", plain.Threaded.Target)
		c.Check(plain.Start+plain.Offset == tBase+0x1100,
			"plain rebase slot vmaddr = %#x", plain.Start+plain.Offset)
		c.Check(plain.Section == "__objc_const", "plain rebase section = %q", plain.Section)
		c.Check(plain.Value == plainRebaseSlot(tBase+0x1400, 1),
			"plain rebase keeps raw slot value")

		// Authenticated rebase: image-relative target and auth provenance.
		authR := rebases[1]
		c.Check(authR.Threaded != nil && authR.Threaded.Authenticated,
			"second rebase must be authenticated")
		c.Check(authR.Threaded != nil && authR.Threaded.Target == tBase+0x300,
			"auth rebase target relative to base = %#x", authR.Threaded.Target)
		c.Check(authR.Threaded != nil && authR.Threaded.Diversity == 0x1234,
			"auth rebase diversity = %#x", authR.Threaded.Diversity)
		c.Check(authR.Threaded != nil && authR.Threaded.Key == 2,
			"auth rebase key = %d", authR.Threaded.Key)
		c.Check(authR.Threaded != nil && authR.Threaded.AddressDiversity,
			"auth rebase address diversity must be set")

		// Chain A binds in slot order.
		posAdd, authB, negAdd, selfBind := binds[0], binds[1], binds[2], binds[3]
		c.Check(posAdd.Threaded != nil && posAdd.Threaded.Ordinal == 0,
			"positive addend bind ordinal = %d", ordinalOf(posAdd))
		c.Check(posAdd.Addend == 15, "plain bind adds template 10 + slot 5, got %d", posAdd.Addend)
		c.Check(posAdd.Name == "_OBJC_CLASS_$_Host", "bind symbol = %q", posAdd.Name)
		c.Check(posAdd.Start+posAdd.SegOffset == tBase+0x1110,
			"bind located at its own slot %#x", posAdd.Start+posAdd.SegOffset)

		c.Check(authB.Threaded != nil && authB.Threaded.Authenticated,
			"second bind must be authenticated")
		c.Check(authB.Threaded != nil && authB.Threaded.Ordinal == 1,
			"auth bind ordinal = %d", ordinalOf(authB))
		c.Check(authB.Addend == 7, "auth bind keeps template addend only, got %d", authB.Addend)
		c.Check(authB.Threaded != nil && authB.Threaded.Diversity == 0xbeef,
			"auth bind diversity = %#x", authB.Threaded.Diversity)
		c.Check(authB.Threaded != nil && authB.Threaded.Key == 1,
			"auth bind key = %d", authB.Threaded.Key)
		c.Check(authB.Flags == types.BIND_SYMBOL_FLAGS_WEAK_IMPORT,
			"auth bind keeps template weak flag %#x", authB.Flags)

		c.Check(negAdd.Addend == 7, "negative slot addend: 10 + (-3) = 7, got %d", negAdd.Addend)
		c.Check(negAdd.Threaded != nil && !negAdd.Threaded.Authenticated,
			"negative addend bind is not authenticated")

		c.Check(selfBind.Dylib == "this-image", "self bind dylib = %q", selfBind.Dylib)
		c.Check(selfBind.Threaded != nil && selfBind.Threaded.Ordinal == 2,
			"self bind ordinal = %d", ordinalOf(selfBind))

		// Chain B reuses ordinal 0 but is located by its own segment's slot.
		reused := binds[4]
		c.Check(reused.Segment == "__DATA2", "reused ordinal located in %q segment", reused.Segment)
		c.Check(reused.Start+reused.SegOffset == tBase+0x8040,
			"reused ordinal slot vmaddr = %#x", reused.Start+reused.SegOffset)
		c.Check(reused.Name == "_OBJC_CLASS_$_Host", "reused ordinal symbol = %q", reused.Name)
		c.Check(reused.Addend == 12, "reused ordinal addend 10 + 2 = 12, got %d", reused.Addend)
		c.Check(reused.Section == "__objc_data2", "reused ordinal section = %q", reused.Section)

		// Repeated reads must not add entries.
		rebases2, err := f.GetRebaseInfo()
		if err != nil {
			t.Fatal(err)
		}
		binds2, err := f.GetBindInfo()
		if err != nil {
			t.Fatal(err)
		}
		c.Check(len(rebases2) == len(rebases) && len(binds2) == len(binds),
			"repeated Get*Info calls must not duplicate records")

		// Calling the pointer reader first on a fresh image yields the same counts.
		first := openRich(t)
		if _, err := first.GetSlidPointerAtAddress(tBase + 0x1100); err != nil {
			t.Fatalf("GetSlidPointerAtAddress first: %v", err)
		}
		rb, err := first.GetRebaseInfo()
		if err != nil {
			t.Fatal(err)
		}
		bb, err := first.GetBindInfo()
		if err != nil {
			t.Fatal(err)
		}
		c.Check(len(rb) == 2 && len(bb) == 5,
			"pointer-first ordering still yields complete records (rebases=%d binds=%d)",
			len(rb), len(bb))

		// A truncated bind stream (declared size cuts the bytecode short) must
		// return an error, not panic.
		truncated := richThreadedImage(t)
		dyldCmdOff := -1
		for off := 0; off+binary.Size(types.DyldInfoCmd{}) <= 0x400; off += 4 {
			if binary.LittleEndian.Uint32(truncated[off:]) == uint32(types.LC_DYLD_INFO_ONLY) {
				dyldCmdOff = off
				break
			}
		}
		c.Check(dyldCmdOff >= 0, "located LC_DYLD_INFO_ONLY command")
		if dyldCmdOff >= 0 {
			// BindSize is the uint32 at command offset 20; shrink it to cut the
			// stream inside the first symbol string.
			binary.LittleEndian.PutUint32(truncated[dyldCmdOff+20:], 5)
			tf, err := macho.NewFile(bytes.NewReader(truncated))
			if err != nil {
				t.Fatal(err)
			}
			if _, err := tf.GetBindInfo(); err == nil {
				c.Check(false, "truncated threaded bytecode must return an error")
			} else {
				c.Check(true, "truncated threaded bytecode returned error: %v", err)
			}
		}

		// A chain slot outside file data must return an error.
		badImg := imageWithApplyOffset(t, 0x1f00)
		bf, err := macho.NewFile(bytes.NewReader(badImg))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := bf.GetRebaseInfo(); err == nil {
			c.Check(false, "out-of-bounds APPLY chain must return an error")
		} else {
			c.Check(true, "out-of-bounds APPLY returned error: %v", err)
		}

		// A bind ordinal larger than the table must return an error.
		badOrdImg := imageWithSlot(t, plainBindSlot(9, 0, 0))
		of, err := macho.NewFile(bytes.NewReader(badOrdImg))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := of.GetBindInfo(); err == nil {
			c.Check(false, "out-of-range bind ordinal must return an error")
		} else {
			c.Check(true, "out-of-range ordinal returned error: %v", err)
		}
	})
}

func ordinalOf(b types.Bind) uint16 {
	if b.Threaded == nil {
		return 0xffff
	}
	return b.Threaded.Ordinal
}

// imageWithApplyOffset builds a minimal threaded image whose single APPLY
// starts at the given __DATA segment offset.
func imageWithApplyOffset(t *testing.T, applyOff uint64) []byte {
	bind := []byte{0xd0}
	bind = append(bind, uleb(1)...)
	bind = append(bind, 0x11, 0x40)
	bind = append(bind, "_x\x00"...)
	bind = append(bind, 0x90)
	bind = append(bind, 0x71)
	bind = append(bind, uleb(applyOff)...)
	bind = append(bind, 0xd1, 0x00)
	sec := types.Section64{Addr: tBase + 0x1100, Size: 0x100, Offset: 0x1100}
	copy(sec.Name[:], "__objc_const")
	copy(sec.Seg[:], "__DATA")
	return buildThreadedImage(t, threadedSpec{
		segs: []segSpec{
			{name: "__TEXT", addr: tBase, off: 0, filesz: dataOff},
			{name: "__DATA", addr: tBase + dataOff, off: dataOff, filesz: 0x1000, sects: []types.Section64{sec}},
		},
		bind: bind,
	})
}

// imageWithSlot builds a minimal threaded image whose single chain slot holds
// the supplied raw value at __DATA + 0x100.
func imageWithSlot(t *testing.T, slot uint64) []byte {
	bind := []byte{0xd0}
	bind = append(bind, uleb(1)...)
	bind = append(bind, 0x11, 0x40)
	bind = append(bind, "_x\x00"...)
	bind = append(bind, 0x90)
	bind = append(bind, 0x71)
	bind = append(bind, uleb(0x100)...)
	bind = append(bind, 0xd1, 0x00)
	sec := types.Section64{Addr: tBase + 0x1100, Size: 0x100, Offset: 0x1100}
	copy(sec.Name[:], "__objc_const")
	copy(sec.Seg[:], "__DATA")
	return buildThreadedImage(t, threadedSpec{
		segs: []segSpec{
			{name: "__TEXT", addr: tBase, off: 0, filesz: dataOff},
			{name: "__DATA", addr: tBase + dataOff, off: dataOff, filesz: 0x1000, sects: []types.Section64{sec}},
		},
		bind:  bind,
		slots: map[uint64]uint64{dataOff + 0x100: slot},
	})
}

// TestThreadedConsumers covers pointer reads, self/external bind semantics and
// Objective-C category association.
func TestThreadedConsumers(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := openRich(t)

		// Rebase reads by address return the decoded target.
		got, err := f.GetSlidPointerAtAddress(tBase + 0x1100)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(got == tBase+0x1400, "plain rebase read by address = %#x", got)
		got, err = f.GetSlidPointerAtAddress(tBase + 0x1108)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(got == tBase+0x300, "auth rebase read by address = %#x", got)

		// External bind keeps its raw slot value; self bind resolves locally.
		ext, err := f.GetSlidPointerAtAddress(tBase + 0x1110)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(ext == plainBindSlot(0, 5, 1), "external bind keeps raw value, got %#x", ext)
		self, err := f.GetSlidPointerAtAddress(tBase + 0x1128)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(self == tBase+0x1600, "self bind resolves to local symbol %#x", self)

		// Unregistered ordinary pointers are read unchanged.
		plain, err := f.GetSlidPointerAtAddress(tBase + 0x1700)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(plain == 0x00000abcdef00000, "ordinary unchained pointer read as-is, got %#x", plain)

		// GetBindName queries legacy streams by slot virtual address.
		name, err := f.GetBindName(tBase + 0x1110)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(name == "_OBJC_CLASS_$_Host", "GetBindName by slot address = %q", name)
		if _, err := f.GetBindName(tBase + 0x1100); err == nil {
			c.Check(false, "rebase slot must not report a bind name")
		} else {
			c.Check(true, "rebase slot has no bind name: %v", err)
		}

		// Objective-C category: local category name plus external host class.
		fix, err := macho.NewFile(bytes.NewReader(fixture.Image(true)))
		if err != nil {
			t.Fatal(err)
		}
		cats, err := fix.GetObjCCategories()
		if err != nil {
			t.Fatalf("GetObjCCategories: %v", err)
		}
		c.Check(len(cats) == 1, "expected one category, got %d", len(cats))
		if len(cats) == 1 {
			cat := cats[0]
			c.Check(cat.Name == "Extras", "local category name = %q", cat.Name)
			c.Check(cat.Class != nil && cat.Class.Name == "Host",
				"external class associated as %q", className(cat))
			// The encoded class field must not be mistaken for an address.
			c.Check(cat.ClsVMAddr == uint64(1)<<62,
				"category Class field retains encoded value %#x", cat.ClsVMAddr)
			c.Check(cat.NameVMAddr == tBase+0x1400,
				"category name field decodes to cstring %#x", cat.NameVMAddr)
		}

		// Non-lazy categories follow the same source-location handling.
		nlImage := nonLazyCategoryImage(t)
		nl, err := macho.NewFile(bytes.NewReader(nlImage))
		if err != nil {
			t.Fatal(err)
		}
		nlCats, err := nl.GetObjCNonLazyCategories()
		if err != nil {
			t.Fatalf("GetObjCNonLazyCategories: %v", err)
		}
		c.Check(len(nlCats) == 1, "expected one non-lazy category, got %d", len(nlCats))
		if len(nlCats) == 1 {
			c.Check(nlCats[0].Name == "Extras", "non-lazy category name = %q", nlCats[0].Name)
			c.Check(nlCats[0].Class != nil && nlCats[0].Class.Name == "Host",
				"non-lazy external class = %q", className(nlCats[0]))
		}
	})
}

func className(cat objc.Category) string {
	if cat.Class == nil {
		return ""
	}
	return cat.Class.Name
}

// nonLazyCategoryImage mirrors the fixture but exposes __objc_nlcatlist.
func nonLazyCategoryImage(t *testing.T) []byte {
	const (
		catAddr  = tBase + 0x1200
		cstrAddr = tBase + 0x1400
		nlAddr   = tBase + 0x1600
		catOff   = uint64(0x1200)
		cstrOff  = uint64(0x1400)
		nlOff    = uint64(0x1600)
	)
	bind := []byte{0xd0}
	bind = append(bind, uleb(1)...)
	bind = append(bind, 0x11, 0x40)
	bind = append(bind, "_OBJC_CLASS_$_Host\x00"...)
	bind = append(bind, 0x90)
	// APPLY chain 1: the __objc_nlcatlist slot, a self-contained rebase.
	bind = append(bind, 0x71)
	bind = append(bind, uleb(0x600)...)
	bind = append(bind, 0xd1)
	// APPLY chain 2: category name rebase followed by external class bind.
	bind = append(bind, 0x71)
	bind = append(bind, uleb(0x200)...)
	bind = append(bind, 0xd1, 0x00)

	secConst := types.Section64{Addr: tBase + 0x1200, Size: 0x200, Offset: 0x1200}
	copy(secConst.Name[:], "__objc_const")
	copy(secConst.Seg[:], "__DATA")
	secNl := types.Section64{Addr: nlAddr, Size: 8, Offset: uint32(nlOff)}
	copy(secNl.Name[:], "__objc_nlcatlist")
	copy(secNl.Seg[:], "__DATA")
	secStr := types.Section64{Addr: cstrAddr, Size: 0x40, Offset: uint32(cstrOff)}
	copy(secStr.Name[:], "__cstring")
	copy(secStr.Seg[:], "__DATA")

	slots := map[uint64]uint64{
		nlOff:      plainRebaseSlot(catAddr, 0), // chain 1 ends here
		catOff:     plainRebaseSlot(cstrAddr, 1),
		catOff + 8: uint64(1) << 62, // external class bind, ordinal 0, next 0
	}
	img := buildThreadedImage(t, threadedSpec{
		segs: []segSpec{
			{name: "__TEXT", addr: tBase, off: 0, filesz: dataOff},
			{name: "__DATA", addr: tBase + dataOff, off: dataOff, filesz: 0x2000,
				sects: []types.Section64{secConst, secNl, secStr}},
		},
		bind:  bind,
		slots: slots,
	})
	copy(img[cstrOff:], "Extras\x00")
	return img
}

// TestThreadedCompatibility covers call ordering, classic dyld streams, chained
// fixups and the custom converter path.
func TestThreadedCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The plain, non-threaded fixture still uses the classic rebase stream.
		plain, err := macho.NewFile(bytes.NewReader(fixture.Image(false)))
		if err != nil {
			t.Fatal(err)
		}
		rs, err := plain.GetRebaseInfo()
		if err != nil {
			t.Fatal(err)
		}
		c.Check(len(rs) == 1, "classic rebase count = %d", len(rs))
		c.Check(len(rs) == 1 && rs[0].Threaded == nil,
			"classic rebase must not carry threaded info")
		c.Check(len(rs) == 1 && rs[0].Start+rs[0].Offset == tBase+0x1100,
			"classic rebase slot = %#x", rs[0].Start+rs[0].Offset)

		// Threaded image: rebase first, then bind, then pointer reads agree.
		tf, err := macho.NewFile(bytes.NewReader(fixture.Image(true)))
		if err != nil {
			t.Fatal(err)
		}
		rs, err = tf.GetRebaseInfo()
		if err != nil {
			t.Fatal(err)
		}
		bs, err := tf.GetBindInfo()
		if err != nil {
			t.Fatal(err)
		}
		c.Check(len(rs) == 2 && len(bs) == 1,
			"threaded fixture rebases=%d binds=%d", len(rs), len(bs))
		got, err := tf.GetSlidPointerAtAddress(tBase + 0x1100)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(got == tBase+0x1200, "threaded rebase target after rebase-first = %#x", got)
		// Third call stays stable.
		rs2, _ := tf.GetRebaseInfo()
		bs2, _ := tf.GetBindInfo()
		c.Check(len(rs2) == 2 && len(bs2) == 1, "third read keeps counts stable")

		// Bind-first ordering on a fresh image produces the same records.
		bf, err := macho.NewFile(bytes.NewReader(fixture.Image(true)))
		if err != nil {
			t.Fatal(err)
		}
		bbs, err := bf.GetBindInfo()
		if err != nil {
			t.Fatal(err)
		}
		brs, err := bf.GetRebaseInfo()
		if err != nil {
			t.Fatal(err)
		}
		c.Check(len(bbs) == 1 && len(brs) == 2,
			"bind-first ordering rebases=%d binds=%d", len(brs), len(bbs))

		// A configured custom converter bypasses fixup decoding.
		cfg := macho.FileConfig{VMAddrConverter: types.VMAddrConverter{
			Converter: func(v uint64) uint64 { return v + 0x1000 },
		}}
		cf, err := macho.NewFile(bytes.NewReader(fixture.Image(true)), cfg)
		if err != nil {
			t.Fatal(err)
		}
		v, err := cf.GetSlidPointerAtAddress(tBase + 0x1100)
		if err != nil {
			t.Fatal(err)
		}
		raw := binary.LittleEndian.Uint64(fixture.Image(true)[0x1100:])
		c.Check(v == raw+0x1000, "custom converter overrides threaded decoding, got %#x", v)
	})
}
