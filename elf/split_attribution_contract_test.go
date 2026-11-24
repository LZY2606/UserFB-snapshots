// Copyright 2025 The Binject Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package elf

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/Binject/debug/task_support/taskcheck"
)

// Expected values below were derived independently from raw section dumps:
// section headers give file offsets, the skeleton headers give DWO ids and
// addr_base, and manual abbreviation/rnglist decoding gives the ranges. They
// are not computed from the production results under test.

const (
	dwoIngress uint64 = 0x45f708c196b27967
	dwoEgress  uint64 = 0xf04a3798ae8d23ac
	dwoBoot    uint64 = 0xb0394b2f89034478

	utCompileForTest byte = 0x01 // DW_UT_compile, only used to build corrupt inputs
)

// sampleDir resolves task_support/samples from the elf package directory.
func sampleDir(t *testing.T, name string) string {
	t.Helper()
	abs, err := filepath.Abs(filepath.Join("..", "task_support", "samples", name))
	if err != nil {
		t.Fatal(err)
	}
	return abs
}

func openSample(t *testing.T, name string) (image, packageFile *File) {
	t.Helper()
	dir := sampleDir(t, name)
	image, err := Open(filepath.Join(dir, "device.elf"))
	if err != nil {
		t.Fatalf("open image: %v", err)
	}
	packageFile, err = Open(filepath.Join(dir, "device.elf.dwp"))
	if err != nil {
		t.Fatalf("open dwp: %v", err)
	}
	return image, packageFile
}

// funcKey identifies a function independently of its slice position.
type funcKey struct {
	dwo  uint64
	name string
}

func indexFunctions(fns []SplitFunction) map[funcKey]SplitFunction {
	m := map[funcKey]SplitFunction{}
	for _, fn := range fns {
		m[funcKey{fn.DWOID, fn.Name}] = fn
	}
	return m
}

// ---------------------------------------------------------------------------
// Synthetic ELF/CU-index fixture construction
// ---------------------------------------------------------------------------

// synthSection is one named section blob for the in-memory ELF builder.
type synthSection struct {
	name string
	data []byte
}

// buildELF64 serializes a minimal little-endian ELF64 ET_EXEC file containing
// exactly the requested sections plus a section-header string table.
func buildELF64(t *testing.T, sections []synthSection) []byte {
	t.Helper()
	var shstr bytes.Buffer
	shstr.WriteByte(0)
	nameOff := map[string]uint32{}
	for _, s := range sections {
		nameOff[s.name] = uint32(shstr.Len())
		shstr.WriteString(s.name)
		shstr.WriteByte(0)
	}
	shstrName := ".shstrtab"
	shstrOff := uint32(shstr.Len())
	shstr.WriteString(shstrName)
	shstr.WriteByte(0)
	shstrBytes := shstr.Bytes()

	ehSize := 64
	off := ehSize
	type placed struct {
		synthSection
		offset uint64
	}
	var all []placed
	for _, s := range sections {
		all = append(all, placed{s, uint64(off)})
		off += len(s.data)
	}
	shstrPlaced := placed{synthSection{shstrName, shstrBytes}, uint64(off)}
	off += len(shstrBytes)
	shoff := uint64(off)

	total := shoff + uint64(64*(len(sections)+2))
	buf := make([]byte, total)
	bo := binary.LittleEndian
	// e_ident
	buf[0], buf[1], buf[2], buf[3] = 0x7f, 'E', 'L', 'F'
	buf[4] = byte(ELFCLASS64)
	buf[5] = byte(ELFDATA2LSB)
	buf[6] = byte(EV_CURRENT)
	bo.PutUint16(buf[16:], 2)                       // e_type ET_EXEC
	bo.PutUint16(buf[18:], 62)                      // e_machine x86-64
	bo.PutUint32(buf[20:], 1)                       // e_version
	bo.PutUint16(buf[52:], 64)                      // e_ehsize
	bo.PutUint16(buf[58:], 64)                      // e_shentsize
	bo.PutUint16(buf[60:], uint16(len(sections)+2)) // e_shnum
	bo.PutUint16(buf[62:], uint16(len(sections)+1)) // e_shstrndx
	bo.PutUint64(buf[40:], uint64(shoff))           // e_shoff

	for _, p := range all {
		copy(buf[p.offset:], p.data)
	}
	copy(buf[shstrPlaced.offset:], shstrBytes)

	writeShdr := func(idx int, name uint32, typ SectionType, offset, size uint64) {
		base := shoff + uint64(idx)*64
		bo.PutUint32(buf[base:], name)          // sh_name
		bo.PutUint32(buf[base+4:], uint32(typ)) // sh_type
		bo.PutUint64(buf[base+24:], offset)     // sh_offset
		bo.PutUint64(buf[base+32:], size)       // sh_size
		bo.PutUint64(buf[base+48:], 1)          // sh_addralign
	}
	// index 0 is the null section header (already zeroed).
	for i, p := range all {
		writeShdr(i+1, nameOff[p.name], SHT_PROGBITS, p.offset, uint64(len(p.data)))
	}
	writeShdr(len(sections)+1, shstrOff, SHT_STRTAB, shstrPlaced.offset, uint64(len(shstrBytes)))
	return buf
}

// cuIndexBuilder assembles raw .debug_cu_index version 5 bytes.
type cuIndexBuilder struct {
	buckets uint32
	columns []uint32
	sigs    []uint64 // bucket order, zero for empty
	rows    []uint32 // bucket order, zero for empty
	offsets [][]uint32
	sizes   [][]uint32
}

func (b cuIndexBuilder) bytes() []byte {
	units := uint32(len(b.offsets))
	need := 16 + 8*int(b.buckets) + 4*int(b.buckets) + 4*len(b.columns) +
		8*len(b.columns)*int(units)
	out := make([]byte, need)
	bo := binary.LittleEndian
	p := 0
	bo.PutUint32(out[p:], 5)
	p += 4
	bo.PutUint32(out[p:], uint32(len(b.columns)))
	p += 4
	bo.PutUint32(out[p:], units)
	p += 4
	bo.PutUint32(out[p:], b.buckets)
	p += 4
	for _, s := range b.sigs {
		bo.PutUint64(out[p:], s)
		p += 8
	}
	for _, r := range b.rows {
		bo.PutUint32(out[p:], r)
		p += 4
	}
	for _, c := range b.columns {
		bo.PutUint32(out[p:], c)
		p += 4
	}
	for _, row := range b.offsets {
		for _, v := range row {
			bo.PutUint32(out[p:], v)
			p += 4
		}
	}
	for _, row := range b.sizes {
		for _, v := range row {
			bo.PutUint32(out[p:], v)
			p += 4
		}
	}
	return out
}

// synthPackageSections are the package sections every synthetic index points
// into; dummy payloads are sized generously and never decoded as DIEs here.
func synthPackageSections(index []byte, infoSize, abbrevSize, strOffSize, rngSize int) []synthSection {
	pad := func(n int, fill byte) []byte {
		b := make([]byte, n)
		for i := range b {
			b[i] = fill
		}
		return b
	}
	return []synthSection{
		{".debug_cu_index", index},
		{".debug_info.dwo", pad(infoSize, 0)},
		{".debug_abbrev.dwo", pad(abbrevSize, 0)},
		{".debug_str_offsets.dwo", pad(strOffSize, 0)},
		{".debug_rnglists.dwo", pad(rngSize, 0)},
		{".debug_str.dwo", []byte("shared strings\x00")},
	}
}

// ---------------------------------------------------------------------------
// TestSplitDWPIndex: hash probing, rows, columns and error handling
// ---------------------------------------------------------------------------

func TestSplitDWPIndex(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Four-slot table, one occupied bucket: misses terminate at an empty
		// slot; the stored row is 1-based.
		sigA := uint64(0x1122334455667700) // home bucket 0
		b := cuIndexBuilder{
			buckets: 4, columns: []uint32{1, 3, 6, 8},
			sigs:    []uint64{sigA, 0, 0, 0},
			rows:    []uint32{1, 0, 0, 0},
			offsets: [][]uint32{{0, 0, 0, 0}},
			sizes:   [][]uint32{{0x20, 0x10, 0x10, 0}},
		}
		blob := buildELF64(t, synthPackageSections(b.bytes(), 0x40, 0x20, 0x20, 0x20))
		f, err := NewFile(bytes.NewReader(blob))
		c.Check(err == nil, "synthetic ELF opens: %v", err)
		pkg, err := f.DWARFPackage()
		c.Check(err == nil, "synthetic package parses: %v", err)

		hit := pkg.Lookup(sigA)
		c.Check(hit.Found(), "stored signature is found")
		c.Check(hit.Unit != nil && hit.Unit.IndexRow == 1, "row number is 1-based, got %+v", hit.Unit)
		c.Check(hit.Unit.Signature == sigA, "unit carries signature %#x", hit.Unit.Signature)
		c.Check(hit.Miss == nil, "hit carries no miss record")

		// Absent id: home bucket 0 is occupied, the odd stride 3 lands on the
		// empty bucket 3 on the second probe, where the walk terminates.
		absent := uint64(0x0fedcba9876543210)
		miss := pkg.Lookup(absent)
		c.Check(!miss.Found() && miss.Unit == nil, "absent id is a clean miss")
		c.Check(miss.Miss != nil && miss.Miss.Reason == DWPMissEmptySlot,
			"miss reason is empty slot, got %+v", miss.Miss)
		c.Check(miss.Miss.TerminatingBucket == 3 && miss.Miss.Probes == 2,
			"double-hash probe walk records bucket %d and %d probes", miss.Miss.TerminatingBucket, miss.Miss.Probes)

		// Two-slot table fully occupied: every miss probes both buckets and
		// terminates with table-full, never looping forever.
		sigEven1 := uint64(0x0000000000000002) // home 0
		sigEven2 := uint64(0x0000000000000004) // home 0 as well -> collision -> bucket 1
		full := cuIndexBuilder{
			buckets: 2, columns: []uint32{1, 3},
			sigs:    []uint64{sigEven1, sigEven2},
			rows:    []uint32{1, 2},
			offsets: [][]uint32{{0, 0}, {0x10, 0x10}},
			sizes:   [][]uint32{{0x10, 0x10}, {0x10, 0x10}},
		}
		blob2 := buildELF64(t, synthPackageSections(full.bytes(), 0x30, 0x30, 0x10, 0))
		f2, err := NewFile(bytes.NewReader(blob2))
		c.Check(err == nil, "collision ELF opens: %v", err)
		pkg2, err := f2.DWARFPackage()
		c.Check(err == nil, "collision package parses: %v", err)
		r1 := pkg2.Lookup(sigEven1)
		r2 := pkg2.Lookup(sigEven2)
		c.Check(r1.Found() && r1.Unit.IndexRow == 1, "colliding sig row 1 resolves")
		c.Check(r2.Found() && r2.Unit.IndexRow == 2, "legally colliding sig row 2 resolves")
		fullMiss := pkg2.Lookup(0x6) // home 0, both slots occupied
		c.Check(!fullMiss.Found() && fullMiss.Miss.Reason == DWPMissTableFull,
			"full table miss is distinguishable: %+v", fullMiss.Miss)
		c.Check(fullMiss.Miss.Probes == 2, "full table probe count equals bucket count")

		// A displaced entry reached with a stride other than one. In an 8-bucket
		// table, C homes to bucket 0 but its upper 32 bits give stride 3; with
		// bucket 0 already taken it lands in bucket 3, skipping buckets 1 and 2.
		collider := uint64(0x0000000300000000) // home 0, stride 3
		homeZero := uint64(0x8)                // home 0, stride 1, occupies home
		displaced := cuIndexBuilder{
			buckets: 8, columns: []uint32{1},
			// Final layout: row1 at 0, row2 at 1, row3 at 2, row4 (collider) at 3.
			sigs:    []uint64{homeZero, 1, 2, collider, 0, 0, 0, 0},
			rows:    []uint32{1, 2, 3, 4, 0, 0, 0, 0},
			offsets: [][]uint32{{0}, {0x10}, {0x20}, {0x30}},
			sizes:   [][]uint32{{0x10}, {0x10}, {0x10}, {0x10}},
		}
		blob4 := buildELF64(t, synthPackageSections(displaced.bytes(), 0x40, 0x10, 0x10, 0))
		f4, err := NewFile(bytes.NewReader(blob4))
		c.Check(err == nil, "displaced-entry ELF opens: %v", err)
		pkg4, err := f4.DWARFPackage()
		c.Check(err == nil, "displaced-entry package parses: %v", err)
		dh := pkg4.Lookup(collider)
		c.Check(dh.Found() && dh.Unit.IndexRow == 4,
			"non-unit stride reaches the displaced collider at row 4, got %+v", dh.Unit)
		c.Check(pkg4.Lookup(homeZero).Unit.IndexRow == 1 &&
			pkg4.Lookup(1).Unit.IndexRow == 2 && pkg4.Lookup(2).Unit.IndexRow == 3,
			"the three home-bucket entries still resolve")

		// Unknown column identifiers are retained with id and byte range.
		unk := cuIndexBuilder{
			buckets: 1, columns: []uint32{1, 99},
			sigs: []uint64{sigA}, rows: []uint32{1},
			offsets: [][]uint32{{0, 0x30}},
			sizes:   [][]uint32{{0x10, 0x08}},
		}
		blob3 := buildELF64(t, synthPackageSections(unk.bytes(), 0x20, 0x10, 0x10, 0))
		f3, err := NewFile(bytes.NewReader(blob3))
		c.Check(err == nil, "unknown-column ELF opens: %v", err)
		pkg3, err := f3.DWARFPackage()
		c.Check(err == nil, "unknown-column package parses: %v", err)
		u := pkg3.Lookup(sigA).Unit
		c.Check(len(u.UnknownColumns) == 1 && u.UnknownColumns[0].ID == 99,
			"unknown column id retained: %+v", u.UnknownColumns)
		c.Check(u.UnknownColumns[0].Offset == 0x30 && u.UnknownColumns[0].Size == 8,
			"unknown column range retained: %+v", u.UnknownColumns[0])
		c.Check(u.Contributions[dwpSecInfo].Size == 0x10, "known INFO contribution still decoded")

		// Contribution reaching past its section must be a contextual error.
		bad := cuIndexBuilder{
			buckets: 1, columns: []uint32{1},
			sigs: []uint64{sigA}, rows: []uint32{1},
			offsets: [][]uint32{{0x10}},
			sizes:   [][]uint32{{0x40}}, // section only has 0x20 bytes
		}
		blobBad := buildELF64(t, synthPackageSections(bad.bytes(), 0x20, 0x10, 0x10, 0))
		fBad, err := NewFile(bytes.NewReader(blobBad))
		c.Check(err == nil, "out-of-bounds ELF opens: %v", err)
		_, err = fBad.DWARFPackage()
		var se *SplitError
		c.Check(errors.As(err, &se), "out-of-bounds contribution returns SplitError, got %T", err)
		if se != nil {
			c.Check(se.Object == "dwp" && se.Section == ".debug_info.dwo",
				"error names object and section: %+v", se)
		}

		// Duplicate signature in the hash table must fail.
		dup := cuIndexBuilder{
			buckets: 2, columns: []uint32{1},
			sigs: []uint64{sigA, sigA}, rows: []uint32{1, 1},
			offsets: [][]uint32{{0}}, sizes: [][]uint32{{0x10}},
		}
		blobDup := buildELF64(t, synthPackageSections(dup.bytes(), 0x20, 0x10, 0x10, 0))
		fDup, err := NewFile(bytes.NewReader(blobDup))
		c.Check(err == nil, "duplicate ELF opens: %v", err)
		_, err = fDup.DWARFPackage()
		c.Check(err != nil, "duplicate DWO identity rejected: %v", err)

		// Version 2 indexes are explicitly unsupported.
		v2 := cuIndexBuilder{
			buckets: 1, columns: []uint32{1},
			sigs: []uint64{sigA}, rows: []uint32{1},
			offsets: [][]uint32{{0}}, sizes: [][]uint32{{0x10}},
		}.bytes()
		binary.LittleEndian.PutUint32(v2, 2)
		blobV2 := buildELF64(t, synthPackageSections(v2, 0x20, 0x10, 0x10, 0))
		fV2, err := NewFile(bytes.NewReader(blobV2))
		c.Check(err == nil, "v2 ELF opens: %v", err)
		_, err = fV2.DWARFPackage()
		c.Check(err != nil, "GNU/version 2 index rejected: %v", err)

		// A TU index section makes the package unsupported.
		withTU := append(synthPackageSections(cuIndexBuilder{
			buckets: 1, columns: []uint32{1},
			sigs: []uint64{sigA}, rows: []uint32{1},
			offsets: [][]uint32{{0}}, sizes: [][]uint32{{0x10}},
		}.bytes(), 0x20, 0x10, 0x10, 0), synthSection{".debug_tu_index", make([]byte, 16)})
		fTU, err := NewFile(bytes.NewReader(buildELF64(t, withTU)))
		c.Check(err == nil, "TU ELF opens: %v", err)
		_, err = fTU.DWARFPackage()
		c.Check(err != nil, "TU index rejected: %v", err)
	})
}

// ---------------------------------------------------------------------------
// TestSplitFunctions: real recovery from the supplied ELF and DWP
// ---------------------------------------------------------------------------

func TestSplitFunctions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// ---- primary image, independently derived expectations ----
		img, dwp := openSample(t, "primary")
		pkg, err := dwp.DWARFPackage()
		c.Check(err == nil, "primary package opens: %v", err)
		fns, err := img.SplitFunctions(pkg)
		c.Check(err == nil, "primary recovery succeeds: %v", err)
		c.Check(len(fns) == 5, "primary yields five code-bearing functions, got %d", len(fns))

		idx := indexFunctions(fns)
		type expect struct {
			dwo          uint64
			row          uint32
			cu, compDir  string
			low, high    uint64
			skelHdr      uint64
			skelRoot     uint64
			dieLocal     uint64
			usesRng      bool
			addrBase     uint64
			addrEntrySec uint64
		}
		want := []expect{
			{dwoIngress, 1, "../sample-src/ingress.c", "/lab", 0x401000, 0x401027, 0x00, 0x14, 0x1a, false, 0x08, 0x08},
			{dwoIngress, 1, "../sample-src/ingress.c", "/lab", 0x401030, 0x40104a, 0x00, 0x14, 0x40, true, 0, 0},
			{dwoEgress, 2, "../sample-src/egress.c", "/lab", 0x401050, 0x401078, 0x28, 0x3c, 0x1e, false, 0x18, 0x18},
			{dwoEgress, 2, "../sample-src/egress.c", "/lab", 0x401080, 0x40109b, 0x28, 0x3c, 0x44, true, 0, 0},
			{dwoBoot, 3, "../sample-src/boot.c", "/lab", 0x4010a0, 0x4010ce, 0x50, 0x64, 0x1a, false, 0x28, 0x28},
		}
		names := map[uint64][]string{
			dwoIngress: {"ingress", "transform"},
			dwoEgress:  {"egress", "transform"},
			dwoBoot:    {"_start"},
		}
		// Skeleton DW_AT_low_pc per DWO: the base for every rnglist pair.
		skelLowPC := map[uint64]uint64{
			dwoIngress: 0x401000,
			dwoEgress:  0x401050,
			dwoBoot:    0x4010a0,
		}
		for _, w := range want {
			key := funcKey{w.dwo, names[w.dwo][0]}
			if w.usesRng {
				key.name = "transform"
			}
			fn, ok := idx[key]
			c.Check(ok, "function %+v recovered", key)
			if !ok {
				continue
			}
			c.Check(fn.DWOID == w.dwo && fn.IndexRow == w.row,
				"%s identity row=%d dwo=%#x", key.name, fn.IndexRow, fn.DWOID)
			c.Check(fn.CompileUnitName == w.cu && fn.SkeletonCompDir == w.compDir,
				"%s provenance cu=%q comp_dir=%q", key.name, fn.CompileUnitName, fn.SkeletonCompDir)
			c.Check(len(fn.Ranges) == 1 && fn.Ranges[0].Low == w.low && fn.Ranges[0].High == w.high,
				"%s range got %+v want [%#x,%#x)", key.name, fn.Ranges, w.low, w.high)
			c.Check(fn.SkeletonHeader.SectionOffset == w.skelHdr &&
				fn.SkeletonRootDIE.SectionOffset == w.skelRoot &&
				fn.SkeletonHeader.Object == "image" && fn.SkeletonRootDIE.Object == "image",
				"%s skeleton positions %+v", key.name, fn.SkeletonHeader)
			c.Check(fn.FunctionDIELocal.SectionOffset == w.dieLocal &&
				fn.FunctionDIESection.SectionOffset == fn.FunctionDIELocal.SectionOffset+
					fn.Contributions[dwpSecInfo].Offset &&
				fn.FunctionDIEFile.Object == "dwp",
				"%s DIE coordinate chain local=%#x section=%#x", key.name,
				fn.FunctionDIELocal.SectionOffset, fn.FunctionDIESection.SectionOffset)
			for _, col := range []uint32{dwpSecInfo, dwpSecAbbrev, dwpSecStrOffsets, dwpSecRnglists} {
				_, present := fn.Contributions[col]
				c.Check(present, "%s carries contribution column %d", key.name, col)
			}
			if w.usesRng {
				c.Check(fn.Rnglist != nil && fn.Addr == nil, "%s uses rnglistx only", key.name)
				c.Check(fn.Rnglist.Index == 0 && fn.Rnglist.BasePC == skelLowPC[w.dwo],
					"%s rnglist evidence base=%#x want %#x", key.name, fn.Rnglist.BasePC, skelLowPC[w.dwo])
				c.Check(fn.Rnglist.OffsetTableEntry.Object == "dwp" &&
					fn.Rnglist.List.Object == "dwp" &&
					fn.Rnglist.OffsetTableEntry.FileOffset != fn.Rnglist.List.FileOffset,
					"%s rnglist table/list positions distinct: %+v", key.name, fn.Rnglist)
				c.Check(fn.Contributions[dwpSecRnglists].Size > 0, "%s rnglists contribution non-empty", key.name)
			} else {
				c.Check(fn.Addr != nil && fn.Rnglist == nil, "%s uses addrx only", key.name)
				c.Check(fn.Addr.Index == 0 && fn.Addr.Base == w.addrBase,
					"%s addrx evidence base=%#x", key.name, fn.Addr.Base)
				c.Check(fn.Addr.TableEntry.Object == "image" &&
					fn.Addr.TableEntry.SectionOffset == w.addrEntrySec,
					"%s addr entry position %+v", key.name, fn.Addr.TableEntry)
				c.Check(fn.Addr.Value == w.low, "%s addr entry value %#x", key.name, fn.Addr.Value)
			}
		}
		// Same-named local functions stay distinct records.
		t1 := idx[funcKey{dwoIngress, "transform"}]
		t2 := idx[funcKey{dwoEgress, "transform"}]
		c.Check(t1.DWOID != t2.DWOID, "transform functions keep distinct DWO identities")
		c.Check(t1.FunctionDIESection.SectionOffset != t2.FunctionDIESection.SectionOffset,
			"transform functions keep distinct DIE positions")
		c.Check(t1.Ranges[0].Low != t2.Ranges[0].Low, "transform functions keep distinct ranges")
		// Both share the same raw index numbers (addrx 0 / rnglistx 0) yet
		// resolve through different tables.
		c.Check(t1.Rnglist.Index == t2.Rnglist.Index && t1.Rnglist.Index == 0,
			"same rnglistx index number reused across units")
		// Boot unit has no rnglists contribution.
		start := idx[funcKey{dwoBoot, "_start"}]
		c.Check(start.Contributions[dwpSecRnglists].Size == 0,
			"_start records an empty rnglists contribution")

		// ---- alternate image: different bases, same DWO identities ----
		imgA, dwpA := openSample(t, "alternate")
		pkgA, err := dwpA.DWARFPackage()
		c.Check(err == nil, "alternate package opens: %v", err)
		fnsA, err := imgA.SplitFunctions(pkgA)
		c.Check(err == nil, "alternate recovery succeeds: %v", err)
		c.Check(len(fnsA) == 5, "alternate yields five functions")
		idxA := indexFunctions(fnsA)
		altWant := map[funcKey][2]uint64{
			{dwoEgress, "egress"}:     {0x503000, 0x503028},
			{dwoEgress, "transform"}:  {0x503030, 0x50304b},
			{dwoBoot, "_start"}:       {0x503050, 0x50307e},
			{dwoIngress, "ingress"}:   {0x503080, 0x5030a7},
			{dwoIngress, "transform"}: {0x5030b0, 0x5030ca},
		}
		for k, rg := range altWant {
			fn, ok := idxA[k]
			c.Check(ok, "alternate function %+v present", k)
			if ok {
				c.Check(fn.Ranges[0].Low == rg[0] && fn.Ranges[0].High == rg[1],
					"alternate %v range %+v", k, fn.Ranges)
			}
		}
		// Package order differs: ingress is row 3 in the alternate package.
		c.Check(idxA[funcKey{dwoIngress, "ingress"}].IndexRow == 3,
			"alternate package row order differs")
		c.Check(idxA[funcKey{dwoBoot, "_start"}].IndexRow == 1,
			"alternate boot row is 1")

		// Reuse one package across both images, then return to the first:
		// address bases must never leak between analyses.
		reusedPrimary, err := img.SplitFunctions(pkg)
		c.Check(err == nil, "primary re-analysis with primary package: %v", err)
		reusedAlt, err := imgA.SplitFunctions(pkg)
		c.Check(err == nil, "alternate analysis with foreign package: %v", err)
		again, err := img.SplitFunctions(pkg)
		c.Check(err == nil, "primary analysis again: %v", err)
		c.Check(indexFunctions(reusedAlt)[funcKey{dwoEgress, "egress"}].Ranges[0].Low == 0x503000,
			"alternate keeps its own link base")
		c.Check(indexFunctions(again)[funcKey{dwoIngress, "ingress"}].Ranges[0].Low == 0x401000,
			"primary base restored on re-analysis, got %+v", indexFunctions(again)[funcKey{dwoIngress, "ingress"}].Ranges)
		c.Check(len(reusedPrimary) == 5 && len(reusedAlt) == 5 && len(again) == 5,
			"all reused analyses complete")

		// ---- input buffers are owned, not referenced ----
		rawDWP, err := os.ReadFile(filepath.Join(sampleDir(t, "primary"), "device.elf.dwp"))
		c.Check(err == nil, "read raw dwp bytes: %v", err)
		snapshot := append([]byte(nil), rawDWP...)
		fMem, err := NewFile(bytes.NewReader(rawDWP))
		c.Check(err == nil, "in-memory dwp opens: %v", err)
		pkgMem, err := fMem.DWARFPackage()
		c.Check(err == nil, "in-memory package builds: %v", err)
		for i := range rawDWP {
			rawDWP[i] = 0xff
		}
		got := pkgMem.Lookup(dwoIngress)
		c.Check(got.Found() && got.Unit.IndexRow == 1,
			"overwriting the input buffer does not change later Lookup")
		// Every byte the package retained came from the pre-mutation snapshot.
		c.Check(bytes.Count(pkgMem.sectionData[".debug_info.dwo"], []byte{0xff}) == 0,
			"package keeps private section copies after input overwrite")
		strSec := pkgMem.sections[".debug_str.dwo"]
		c.Check(bytes.Equal(pkgMem.sectionData[".debug_str.dwo"],
			snapshot[strSec.Offset:strSec.Offset+strSec.Size]),
			"owned string section still matches its original snapshot bytes")

		rawImg, err := os.ReadFile(filepath.Join(sampleDir(t, "primary"), "device.elf"))
		c.Check(err == nil, "read raw image bytes: %v", err)
		imgMem, err := NewFile(bytes.NewReader(rawImg))
		c.Check(err == nil, "in-memory image opens: %v", err)
		fnsMem, err := imgMem.SplitFunctions(pkgMem)
		c.Check(err == nil, "in-memory recovery: %v", err)
		savedFirst := fnsMem[0].Ranges[0]
		savedName := fnsMem[0].Name
		for i := range rawImg {
			rawImg[i] = 0
		}
		// The already returned records carry their own ranges and strings, so
		// overwriting the caller-owned input buffer cannot change them.
		c.Check(fnsMem[0].Ranges[0] == savedFirst && fnsMem[0].Name == savedName,
			"returned function results survive input buffer overwrite")
		c.Check(fnsMem[4].SkeletonHeader.FileOffset != 0,
			"returned evidence positions remain intact after overwrite")
		c.Check(imgMem.Close() == nil && fMem.Close() == nil, "in-memory files close")

		// Returned contribution copies are independent per record.
		infoContrib := again[1].Contributions[dwpSecInfo]
		again[0].Contributions[dwpSecInfo] = DWPContribution{
			Section: infoContrib.Section, SectionName: infoContrib.SectionName, Size: 1,
		}
		c.Check(again[1].Contributions[dwpSecInfo].Size != 1,
			"editing one returned record cannot touch another record")

		// Closing the backing ELF must not invalidate the package.
		c.Check(img.Close() == nil && dwp.Close() == nil, "sample files close")
		c.Check(pkg.Lookup(dwoBoot).Found(), "package survives DWP file close")
		c.Check(imgA.Close() == nil && dwpA.Close() == nil, "alternate files close")

		// ---- failure leaves no half result, then normal sample still works ----
		corrupt := func(transform func(b []byte)) []byte {
			raw, err := os.ReadFile(filepath.Join(sampleDir(t, "primary"), "device.elf"))
			c.Check(err == nil, "read image for corruption: %v", err)
			transform(raw)
			return raw
		}
		// Flip the first skeleton unit type to DW_UT_compile.
		badType := corrupt(func(b []byte) { b[0x1113+6] = utCompileForTest })
		badImg, err := NewFile(bytes.NewReader(badType))
		c.Check(err == nil, "corrupt image opens: %v", err)
		_, err = badImg.SplitFunctions(pkg)
		var se *SplitError
		c.Check(errors.As(err, &se), "wrong unit type returns SplitError: %v", err)

		// Flip the first skeleton DWO id so the package lookup misses.
		badID := corrupt(func(b []byte) { b[0x1113+12] ^= 0x01 })
		badIDImg, err := NewFile(bytes.NewReader(badID))
		c.Check(err == nil, "mismatched-id image opens: %v", err)
		_, err = badIDImg.SplitFunctions(pkg)
		c.Check(err != nil, "missing package unit rejected: %v", err)
		c.Check(errors.As(err, &se), "missing unit error is contextual: %v", err)

		// Corrupt the split unit type inside a DWP copy.
		rawDWP2, err := os.ReadFile(filepath.Join(sampleDir(t, "primary"), "device.elf.dwp"))
		c.Check(err == nil, "read dwp for corruption: %v", err)
		infoSecOff := pkg.sections[".debug_info.dwo"].Offset
		rawDWP2[infoSecOff+6] = utCompileForTest
		badDWP, err := NewFile(bytes.NewReader(rawDWP2))
		c.Check(err == nil, "corrupt dwp opens: %v", err)
		badPkg, err := badDWP.DWARFPackage()
		c.Check(err == nil, "corrupt dwp index still parses: %v", err)
		goodImg, _ := openSample(t, "primary")
		_, err = goodImg.SplitFunctions(badPkg)
		c.Check(err != nil, "unsupported split unit type rejected: %v", err)
		c.Check(goodImg.Close() == nil, "good image closes")

		// The untouched sample recovers normally after every failure.
		recoverImg, recoverDWP := openSample(t, "primary")
		recoverPkg, err := recoverDWP.DWARFPackage()
		c.Check(err == nil, "recovery package opens: %v", err)
		recoverFns, err := recoverImg.SplitFunctions(recoverPkg)
		c.Check(err == nil && len(recoverFns) == 5,
			"normal sample recovers after failures: %v", err)
		c.Check(recoverImg.Close() == nil && recoverDWP.Close() == nil, "recovery files close")
	})
}

// ---------------------------------------------------------------------------
// TestSplitAttribution: query semantics, boundaries and rnglist decoding
// ---------------------------------------------------------------------------

func TestSplitAttribution(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		img, dwp := openSample(t, "primary")
		pkg, err := dwp.DWARFPackage()
		c.Check(err == nil, "package opens: %v", err)
		fns, err := img.SplitFunctions(pkg)
		c.Check(err == nil, "functions recover: %v", err)

		// Hand-derived PC -> expected (name, CU name) sets for every query in
		// primary/queries.json; empty slices are deliberate misses.
		primaryExpect := map[uint64][][2]string{
			4198400: {{"ingress", "../sample-src/ingress.c"}},
			4198438: {{"ingress", "../sample-src/ingress.c"}},
			4198439: {},
			4198448: {{"transform", "../sample-src/ingress.c"}},
			4198473: {{"transform", "../sample-src/ingress.c"}},
			4198474: {},
			4198480: {{"egress", "../sample-src/egress.c"}},
			4198519: {{"egress", "../sample-src/egress.c"}},
			4198520: {},
			4198528: {{"transform", "../sample-src/egress.c"}},
			4198554: {{"transform", "../sample-src/egress.c"}},
			4198555: {},
			4198560: {{"_start", "../sample-src/boot.c"}},
			4198605: {{"_start", "../sample-src/boot.c"}},
			4198606: {},
		}
		queryBytes, err := os.ReadFile(filepath.Join(sampleDir(t, "primary"), "queries.json"))
		c.Check(err == nil, "read primary queries: %v", err)
		var primaryPCs []uint64
		c.Check(json.Unmarshal(queryBytes, &primaryPCs) == nil, "queries JSON parses")
		c.Check(len(primaryPCs) == len(primaryExpect), "all queries have expectations")
		for i, pc := range primaryPCs {
			matches := SelectSplitFunctions(fns, pc)
			c.Check(matches != nil, "miss keeps a non-nil empty slice at query %d", i)
			want := primaryExpect[pc]
			c.Check(len(matches) == len(want), "query %d pc=%#x match count got %d want %d",
				i, pc, len(matches), len(want))
			for j, w := range want {
				c.Check(matches[j].Name == w[0] && matches[j].CompileUnitName == w[1],
					"query %d match %d identity", i, j)
			}
		}

		// Explicit half-open boundary probes.
		c.Check(len(SelectSplitFunctions(fns, 0x401000)) == 1, "low bound is included")
		c.Check(len(SelectSplitFunctions(fns, 0x401027)) == 0, "high bound is excluded")
		c.Check(len(SelectSplitFunctions(fns, 0x401030)) == 1, "rnglist low bound included")
		c.Check(len(SelectSplitFunctions(fns, 0x40104a)) == 0, "rnglist high bound excluded")
		c.Check(len(SelectSplitFunctions(fns, 0)) == 0, "zero PC misses")
		c.Check(len(SelectSplitFunctions(fns, 0xffffffffffffffff)) == 0, "huge PC misses")
		c.Check(SelectSplitFunctions(fns, 0x40109b) != nil, "miss result is non-nil")

		// Query order does not influence attribution: shuffle the PCs.
		shuffled := []uint64{primaryPCs[14], primaryPCs[0], primaryPCs[7], primaryPCs[3]}
		wantLens := []int{0, 1, 1, 1}
		for i, pc := range shuffled {
			c.Check(len(SelectSplitFunctions(fns, pc)) == wantLens[i],
				"shuffled query %d pc=%#x", i, pc)
		}

		// Same index numbers across units resolve to different evidence.
		idx := indexFunctions(fns)
		ti := idx[funcKey{dwoIngress, "transform"}]
		te := idx[funcKey{dwoEgress, "transform"}]
		c.Check(ti.Rnglist.Index == 0 && te.Rnglist.Index == 0, "both rnglistx indexes are 0")
		c.Check(ti.Rnglist.OffsetTableEntry.FileOffset != te.Rnglist.OffsetTableEntry.FileOffset,
			"index 0 points at distinct table entries per unit")
		c.Check(ti.Rnglist.BasePC != te.Rnglist.BasePC, "paired skeleton low PCs differ")
		ig := idx[funcKey{dwoIngress, "ingress"}]
		eg := idx[funcKey{dwoEgress, "egress"}]
		c.Check(ig.Addr.Index == 0 && eg.Addr.Index == 0, "both addrx indexes are 0")
		c.Check(ig.Addr.TableEntry.FileOffset != eg.Addr.TableEntry.FileOffset,
			"addrx 0 reads distinct .debug_addr entries")

		// ---- alternate queries, including its different link base ----
		imgA, dwpA := openSample(t, "alternate")
		pkgA, err := dwpA.DWARFPackage()
		c.Check(err == nil, "alternate package opens: %v", err)
		fnsA, err := imgA.SplitFunctions(pkgA)
		c.Check(err == nil, "alternate functions recover: %v", err)
		altExpect := map[uint64][2]string{
			5255168: {"egress", "../sample-src/egress.c"},
			5255207: {"egress", "../sample-src/egress.c"},
			5255208: {},
			5255216: {"transform", "../sample-src/egress.c"},
			5255242: {"transform", "../sample-src/egress.c"},
			5255243: {},
			5255248: {"_start", "../sample-src/boot.c"},
			5255293: {"_start", "../sample-src/boot.c"},
			5255294: {},
			5255296: {"ingress", "../sample-src/ingress.c"},
			5255334: {"ingress", "../sample-src/ingress.c"},
			5255335: {},
			5255344: {"transform", "../sample-src/ingress.c"},
			5255369: {"transform", "../sample-src/ingress.c"},
			5255370: {},
		}
		qb, err := os.ReadFile(filepath.Join(sampleDir(t, "alternate"), "queries.json"))
		c.Check(err == nil, "read alternate queries: %v", err)
		var altPCs []uint64
		c.Check(json.Unmarshal(qb, &altPCs) == nil, "alternate queries parse")
		altHits := 0
		for _, pc := range altPCs {
			matches := SelectSplitFunctions(fnsA, pc)
			want, known := altExpect[pc]
			c.Check(known, "alternate pc %d has an expectation", pc)
			if len(want[0]) == 0 {
				c.Check(len(matches) == 0, "alternate pc %d misses", pc)
			} else {
				altHits++
				c.Check(len(matches) == 1 && matches[0].Name == want[0],
					"alternate pc %d matches %s", pc, want[0])
			}
		}
		c.Check(altHits == 10, "alternate has ten hit queries and five misses")
		c.Check(img.Close() == nil && dwp.Close() == nil && imgA.Close() == nil && dwpA.Close() == nil,
			"all files close")

		// ---- rnglist decoder: multiple pairs stay one record; bad opcodes fail ----
		skel := &skeletonUnit{lowPC: 0x1000, addrBase: 8,
			addr: addrBlock{base: 8, end: 16}}
		contrib := &DWPContribution{Section: dwpSecRnglists, SectionName: ".debug_rnglists.dwo",
			Offset: 0, Size: 0x20, FileOffset: 0x100}
		rng := make([]byte, 0x20)
		bo := binary.LittleEndian
		bo.PutUint32(rng[0:], 0x1c) // unit length
		bo.PutUint16(rng[4:], 5)
		rng[6] = 8
		bo.PutUint32(rng[8:], 1)  // one offset table entry
		bo.PutUint32(rng[12:], 4) // entry value 4 -> list at 12+4 = 16
		rng[16] = 4               // offset_pair [0x10, 0x20)
		rng[17] = 0x10
		rng[18] = 0x20
		rng[19] = 4 // second offset_pair [0x30, 0x40)
		rng[20] = 0x30
		rng[21] = 0x40
		rng[22] = 0 // end_of_list
		// Minimal info/abbrev fixture describing one subprogram with
		// rnglistx(0) at DIE offset 0 (abbrev code 1).
		abbrev := []byte{
			1, 0x2e, 0x00, // subprogram, no children
			0x55, 0x23, // DW_AT_ranges, DW_FORM_rnglistx
			0, 0,
			0,
		}
		info := []byte{1, 0} // abbrev 1, rnglistx index 0
		tables, perr := parseAbbrevTables(abbrev)
		c.Check(perr == nil, "synthetic abbrev parses: %v", perr)
		_, idxPos, listPos, ranges, derr := decodeRnglistx(info, 0, tables, rng, contrib, skel)
		c.Check(derr == nil, "two-pair rnglist decodes: %v", derr)
		c.Check(len(ranges) == 2, "multiple ranges stay in one record, got %d", len(ranges))
		c.Check(ranges[0].Low == 0x1010 && ranges[0].High == 0x1020 &&
			ranges[1].Low == 0x1030 && ranges[1].High == 0x1040,
			"offset pairs resolve against skeleton low_pc: %+v", ranges)
		c.Check(idxPos.FileOffset == 0x10c && listPos.FileOffset == 0x110,
			"rnglist evidence positions map back to file offsets")

		// An unsupported opcode (DW_RLE_base_addressx = 1) must fail loudly.
		rngBad := make([]byte, 0x20)
		copy(rngBad, rng)
		rngBad[16] = 1
		_, _, _, _, err = decodeRnglistx(info, 0, tables, rngBad, contrib, skel)
		c.Check(err != nil, "unsupported rnglist opcode rejected: %v", err)
		var se *SplitError
		c.Check(errors.As(err, &se) && se.Section == ".debug_rnglists.dwo",
			"rnglist error is contextual")

		// rnglistx index outside the offset table must fail.
		infoOOB := []byte{1, 5} // rnglistx index 5, table has one entry
		_, _, _, _, err = decodeRnglistx(infoOOB, 0, tables, rng, contrib, skel)
		c.Check(err != nil, "out-of-bounds rnglistx rejected: %v", err)

		// addrx outside the skeleton contribution must fail.
		addr := make([]byte, 24)
		bo.PutUint64(addr[8:], 0x401000)
		addrSec := &Section{SectionHeader: SectionHeader{Offset: 0x1000, Size: 24}}
		_, _, _, err = resolveAddrx(addr, addrSec, skel, 2) // only one entry
		c.Check(err != nil, "out-of-bounds addrx rejected: %v", err)
		secOff, fileOff, val, err := resolveAddrx(addr, addrSec, skel, 0)
		c.Check(err == nil && val == 0x401000 && secOff == 8 && fileOff == 0x1008,
			"in-bounds addrx maps value and positions: %#x %#x %#x err=%v", val, secOff, fileOff, err)
	})
}
