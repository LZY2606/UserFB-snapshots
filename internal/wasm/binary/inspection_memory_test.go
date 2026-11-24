package binary

import (
	"os"
	"testing"

	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/internal/wasm"
	"github.com/tetratelabs/wazero/task_support/fixture"
	"github.com/tetratelabs/wazero/task_support/taskcheck"
)

// Inspection tests build their own binaries from raw bytes (standard library
// only) and cross-check the shipped task_support/fixtures/mixed.wasm. Expected
// body offsets are listed independently from the recorded MemoryUses: the
// tests first assert the opcode bytes at those raw Code.Body coordinates, and
// only then compare them with what the production validator recorded.

// uleb encodes v as an unsigned LEB128 of any width.
func uleb(v uint64) []byte {
	var b []byte
	for {
		x := byte(v & 0x7f)
		v >>= 7
		if v != 0 {
			x |= 0x80
		}
		b = append(b, x)
		if v == 0 {
			return b
		}
	}
}

func inspectSec(id byte, payload []byte) []byte {
	b := []byte{id}
	b = append(b, uleb(uint64(len(payload)))...)
	return append(b, payload...)
}

func inspectHead() []byte { return []byte{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00} }

// inspectEmptyFuncTypeSection emits one () -> () function type.
func inspectEmptyFuncTypeSection() []byte {
	return inspectSec(wasm.SectionIDType, []byte{0x01, 0x60, 0x00, 0x00})
}

// inspectFuncImportSection emits one imported function of type 0.
func inspectFuncImportSection(module, name string) []byte {
	p := []byte{0x01}
	p = append(p, uleb(uint64(len(module)))...)
	p = append(p, module...)
	p = append(p, uleb(uint64(len(name)))...)
	p = append(p, name...)
	p = append(p, wasm.ExternTypeFunc, 0x00)
	return inspectSec(wasm.SectionIDImport, p)
}

// inspectMemoryImportSection emits one imported memory with the raw limits.
func inspectMemoryImportSection(module, name string, flag byte, min, max uint64, hasMax bool) []byte {
	p := []byte{0x01}
	p = append(p, uleb(uint64(len(module)))...)
	p = append(p, module...)
	p = append(p, uleb(uint64(len(name)))...)
	p = append(p, name...)
	p = append(p, wasm.ExternTypeMemory, flag)
	p = append(p, uleb(min)...)
	if hasMax {
		p = append(p, uleb(max)...)
	}
	return inspectSec(wasm.SectionIDImport, p)
}

// inspectMemorySection emits a memory section from raw limits payloads.
func inspectMemorySection(limits ...[]byte) []byte {
	p := uleb(uint64(len(limits)))
	for _, l := range limits {
		p = append(p, l...)
	}
	return inspectSec(wasm.SectionIDMemory, p)
}

func inspectLimits(flag byte, min, max uint64, hasMax bool) []byte {
	p := []byte{flag}
	p = append(p, uleb(min)...)
	if hasMax {
		p = append(p, uleb(max)...)
	}
	return p
}

// inspectFunctionSection emits n function definitions all using type 0.
func inspectFunctionSection(n int) []byte {
	p := uleb(uint64(n))
	for i := 0; i < n; i++ {
		p = append(p, 0x00)
	}
	return inspectSec(wasm.SectionIDFunction, p)
}

func inspectCodeSection(body []byte) []byte {
	code := append([]byte{0x00}, body...) // zero local declarations
	p := []byte{0x01}
	p = append(p, uleb(uint64(len(code)))...)
	p = append(p, code...)
	return inspectSec(wasm.SectionIDCode, p)
}

func inspectDataCountSection(n uint32) []byte {
	return inspectSec(wasm.SectionIDDataCount, uleb(uint64(n)))
}

// inspectActiveDataSection emits one prefix-2 active segment.
func inspectActiveDataSection(memIndex uint32, offsetExpr, init []byte) []byte {
	p := []byte{0x01, 0x02}
	p = append(p, uleb(uint64(memIndex))...)
	p = append(p, offsetExpr...)
	p = append(p, uleb(uint64(len(init)))...)
	p = append(p, init...)
	return inspectSec(wasm.SectionIDData, p)
}

// inspectPassiveDataSection emits one passive segment.
func inspectPassiveDataSection(init []byte) []byte {
	p := []byte{0x01, 0x01}
	p = append(p, uleb(uint64(len(init)))...)
	p = append(p, init...)
	return inspectSec(wasm.SectionIDData, p)
}

func inspectExportMemorySection(name string, memIndex uint32) []byte {
	p := []byte{0x01}
	p = append(p, uleb(uint64(len(name)))...)
	p = append(p, name...)
	p = append(p, wasm.ExternTypeMemory)
	p = append(p, uleb(uint64(memIndex))...)
	return inspectSec(wasm.SectionIDExport, p)
}

func inspectBuild(sections ...[]byte) []byte {
	b := inspectHead()
	for _, s := range sections {
		b = append(b, s...)
	}
	return b
}

func inspectDecode(t *testing.T, raw []byte) *wasm.Module {
	t.Helper()
	m, err := DecodeModuleForInspection(raw, api.CoreFeaturesV2)
	if err != nil {
		t.Fatalf("DecodeModuleForInspection: %v", err)
	}
	return m
}

func TestInspectionMemoryIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The shipped fixture must be byte-identical to the Go fixture builder.
		raw, err := os.ReadFile("../../../task_support/fixtures/mixed.wasm")
		if err != nil {
			t.Fatalf("read mixed.wasm: %v", err)
		}
		c.Check(string(raw) == string(fixture.Mixed()), "mixed.wasm must be rebuildable from the Go fixture")

		m := inspectDecode(t, raw)
		err = m.Validate(api.CoreFeaturesV2)
		c.Check(err == nil, "mixed.wasm must validate in inspection mode: %v", err)

		decls := m.MemoryDeclarations()
		c.Check(len(decls) == 3, "three combined memory declarations, got %d", len(decls))
		c.Check(decls[0].Index == 0 && decls[0].Imported && decls[0].ImportModule == "env" &&
			decls[0].ImportName == "ram" && !decls[0].Is64 && decls[0].Min == 1 &&
			decls[0].HasMax && decls[0].Max == 2, "memory 0 is imported env.ram, 32-bit, min 1 max 2: %+v", decls[0])
		c.Check(decls[1].Index == 1 && !decls[1].Imported && decls[1].Is64 &&
			decls[1].Min == 4294967296 && decls[1].HasMax && decls[1].Max == 4294967297,
			"memory 1 is local memory64 min 2^32 max 2^32+1: %+v", decls[1])
		c.Check(decls[2].Index == 2 && !decls[2].Imported && decls[2].Is64 &&
			decls[2].Min == 0 && decls[2].HasMax && decls[2].Max == 2,
			"memory 2 is local memory64 min 0 max 2: %+v", decls[2])

		// Imported function prefix puts the single local body at function index 1.
		c.Check(m.ImportFunctionCount == 1, "one imported function prefix")
		c.Check(len(m.CodeSection) == 1, "one local function body")

		// Data segment 0 is active, explicitly bound to memory 1 with an i64 offset.
		c.Check(len(m.DataSection) == 1, "one data segment")
		d := m.DataSection[0]
		c.Check(!d.Passive && d.MemoryIndex == 1, "active data segment targets memory 1")
		c.Check(d.OffsetExpression.Opcode == wasm.OpcodeI64Const, "i64.const offset expression")
		c.Check(string(d.Init) == string([]byte{0x11, 0x22, 0x33}), "init bytes 112233")

		uses := m.MemoryUses()
		c.Check(len(uses) == 4, "four memory references, got %d", len(uses))

		// Different import prefixes: two imported memories then a local one.
		twoImports := inspectBuild(
			inspectTwoMemoryImportsSection(),
			inspectMemorySection(inspectLimits(0x04, 7, 0, false)),
		)
		m2 := inspectDecode(t, twoImports)
		d2 := m2.MemoryDeclarations()
		c.Check(len(d2) == 3, "two imported + one local memory")
		c.Check(d2[0].Imported && d2[0].ImportModule == "mod1" && d2[0].Index == 0, "first import identity")
		c.Check(d2[1].Imported && d2[1].ImportModule == "mod2" && d2[1].Index == 1, "second import identity")
		c.Check(!d2[2].Imported && d2[2].Is64 && d2[2].Min == 7 && d2[2].Index == 2, "local memory re-indexed to 2")

		// A limits entry without max keeps HasMax false and Max zero.
		noMax := inspectBuild(inspectMemorySection(inspectLimits(0x04, 9, 0, false)))
		d3 := inspectDecode(t, noMax).MemoryDeclarations()
		c.Check(len(d3) == 1 && !d3[0].HasMax && d3[0].Max == 0 && d3[0].Min == 9,
			"missing max must not be reported as an implicit limit: %+v", d3[0])

		// Re-validating the same object must not append duplicate records.
		before := len(m.MemoryUses())
		c.Check(m.Validate(api.CoreFeaturesV2) == nil, "second validation succeeds")
		c.Check(len(m.MemoryUses()) == before, "second validation does not duplicate uses")

		// A failed validation leaves no seemingly complete relationship.
		badBody := []byte{
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeI32Load, 0x42, 0x09, 0x00, // memidx 9 does not exist
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		bad := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(badBody),
		)
		badMod := inspectDecode(t, bad)
		c.Check(badMod.Validate(api.CoreFeaturesV2) != nil, "unknown memory index rejected")
		c.Check(len(badMod.MemoryUses()) == 0, "failed validation leaves no uses")

		// Re-reading after the error works on a fresh decode.
		goodMod := inspectDecode(t, raw)
		c.Check(goodMod.Validate(api.CoreFeaturesV2) == nil, "fresh decode after error validates")
		c.Check(len(goodMod.MemoryUses()) == 4, "fresh decode records the four references again")

		// The ordinary entry point keeps its historical support boundary.
		legacyRaw, rerr := os.ReadFile("../../../task_support/fixtures/legacy.wasm")
		if rerr != nil {
			t.Fatalf("read legacy.wasm: %v", rerr)
		}
		legacyMod, lerr := DecodeModule(legacyRaw, api.CoreFeaturesV2, 65536, false, false, false)
		c.Check(lerr == nil && legacyMod.Validate(api.CoreFeaturesV2) == nil, "ordinary DecodeModule still handles legacy.wasm")
		c.Check(legacyMod.MemoryDeclarations() == nil && legacyMod.MemoryUses() == nil,
			"ordinary modules expose no inspection records")
		_, mixedErr := DecodeModule(raw, api.CoreFeaturesV2, 65536, false, false, false)
		c.Check(mixedErr != nil, "ordinary DecodeModule still rejects multi-memory mixed.wasm")
		inspectLegacy := inspectDecode(t, legacyRaw)
		c.Check(len(inspectLegacy.MemoryDeclarations()) == 1, "inspection decode also covers single-memory modules")

		// Functions that are never called, exported or started are still
		// validated, and their memory references are still recorded.
		uncalledBody := []byte{
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeI32Load, 0x02, 0x00,
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		uncalled := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(uncalledBody),
		)
		mUncalled := inspectDecode(t, uncalled)
		rawCopy := append([]byte(nil), uncalled...)
		bodyBefore := append([]byte(nil), mUncalled.CodeSection[0].Body...)
		c.Check(mUncalled.Validate(api.CoreFeaturesV2) == nil, "uncalled function validates")
		c.Check(len(mUncalled.MemoryUses()) == 1 && mUncalled.MemoryUses()[0].Opcode == "i32.load",
			"uncalled function memory reference still recorded")
		c.Check(string(mUncalled.CodeSection[0].Body) == string(bodyBefore), "Code.Body bytes unchanged by validation")
		c.Check(string(uncalled) == string(rawCopy), "input bytes unchanged by decode and validation")
	})
}

func TestInspectionMemoryTyping(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// mem32: i32 address is accepted, i64 address is a type mismatch.
		body32OK := []byte{
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeI32Load, 0x42, 0x00, 0x00, // align bit6, memidx 0, align 2, offset 0
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		mod32 := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(body32OK),
		)
		m32 := inspectDecode(t, mod32)
		c.Check(m32.Validate(api.CoreFeaturesV2) == nil, "i32 address on memory32 accepted")
		c.Check(m32.MemoryUses()[0].MemoryIndices[0] == 0, "explicit zero index recorded")

		body32Bad := []byte{
			wasm.OpcodeI64Const, 0x00,
			wasm.OpcodeI32Load, 0x42, 0x00, 0x00,
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		mod32Bad := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(body32Bad),
		)
		c.Check(inspectDecode(t, mod32Bad).Validate(api.CoreFeaturesV2) != nil, "i64 address on memory32 rejected")

		// mem64: i64 address accepted; the same >4GiB offset that fails on
		// memory32 is legal here and must survive at full uint64 width.
		bigOffset := uleb(1 << 32)
		body64OK := []byte{wasm.OpcodeI64Const, 0x00, wasm.OpcodeI32Load, 0x42, 0x00}
		body64OK = append(body64OK, bigOffset...)
		body64OK = append(body64OK, wasm.OpcodeDrop, wasm.OpcodeEnd)
		mod64 := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x04, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(body64OK),
		)
		m64 := inspectDecode(t, mod64)
		c.Check(m64.Validate(api.CoreFeaturesV2) == nil, "i64 address and >4GiB offset on memory64 accepted")
		c.Check(m64.MemoryUses()[0].Offset == 1<<32, "offset kept at full uint64 width")

		body32Big := []byte{wasm.OpcodeI32Const, 0x00, wasm.OpcodeI32Load, 0x42, 0x00}
		body32Big = append(body32Big, bigOffset...)
		body32Big = append(body32Big, wasm.OpcodeDrop, wasm.OpcodeEnd)
		mod32Big := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(body32Big),
		)
		c.Check(inspectDecode(t, mod32Big).Validate(api.CoreFeaturesV2) != nil, ">4GiB offset rejected on memory32")

		// memarg without the align bit defaults to memory zero.
		bodyDefault := []byte{
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeI32Load, 0x02, 0x00, // plain align 2, no memory index
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		modDefault := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(bodyDefault),
		)
		mDefault := inspectDecode(t, modDefault)
		c.Check(mDefault.Validate(api.CoreFeaturesV2) == nil, "memarg without bit6 decodes")
		c.Check(mDefault.MemoryUses()[0].MemoryIndices[0] == 0, "missing memory index means memory zero")

		// Unknown memory index is a validation error.
		bodyUnknown := []byte{
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeI32Load, 0x42, 0x05, 0x00,
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		modUnknown := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(bodyUnknown),
		)
		c.Check(inspectDecode(t, modUnknown).Validate(api.CoreFeaturesV2) != nil, "unknown memory index rejected")

		// memory.copy: destination i64 (mem1), source i32 (mem0), length i32.
		twoMem := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(
				inspectLimits(0x00, 1, 0, false), // memory 0: 32-bit
				inspectLimits(0x04, 1, 0, false), // memory 1: 64-bit
			),
			inspectFunctionSection(1),
		)
		copyOK := append(twoMem, inspectCodeSection([]byte{
			wasm.OpcodeI64Const, 0x00, // destination address, mem1
			wasm.OpcodeI32Const, 0x00, // source address, mem0
			wasm.OpcodeI32Const, 0x00, // length, i32
			wasm.OpcodeMiscPrefix, wasm.OpcodeMiscMemoryCopy, 0x01, 0x00,
			wasm.OpcodeEnd,
		})...)
		mCopy := inspectDecode(t, copyOK)
		c.Check(mCopy.Validate(api.CoreFeaturesV2) == nil, "mixed-width memory.copy accepted")
		use := mCopy.MemoryUses()[0]
		c.Check(use.Opcode == "memory.copy" && len(use.MemoryIndices) == 2 &&
			use.MemoryIndices[0] == 1 && use.MemoryIndices[1] == 0, "copy records destination then source")

		copyBadLen := append(twoMem, inspectCodeSection([]byte{
			wasm.OpcodeI64Const, 0x00,
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeI64Const, 0x00, // length must stay i32 because the source is 32-bit
			wasm.OpcodeMiscPrefix, wasm.OpcodeMiscMemoryCopy, 0x01, 0x00,
			wasm.OpcodeEnd,
		})...)
		c.Check(inspectDecode(t, copyBadLen).Validate(api.CoreFeaturesV2) != nil, "i64 copy length rejected when a memory is 32-bit")

		// memory.fill: fill value i32, address and length follow the 64-bit target.
		fillOK := append(twoMem, inspectCodeSection([]byte{
			wasm.OpcodeI64Const, 0x00, // address
			wasm.OpcodeI32Const, 0x00, // fill value stays i32
			wasm.OpcodeI64Const, 0x00, // length i64
			wasm.OpcodeMiscPrefix, wasm.OpcodeMiscMemoryFill, 0x01,
			wasm.OpcodeEnd,
		})...)
		c.Check(inspectDecode(t, fillOK).Validate(api.CoreFeaturesV2) == nil, "memory.fill typing accepted")

		fillBad := append(twoMem, inspectCodeSection([]byte{
			wasm.OpcodeI64Const, 0x00,
			wasm.OpcodeI64Const, 0x00, // fill value must be i32
			wasm.OpcodeI64Const, 0x00,
			wasm.OpcodeMiscPrefix, wasm.OpcodeMiscMemoryFill, 0x01,
			wasm.OpcodeEnd,
		})...)
		c.Check(inspectDecode(t, fillBad).Validate(api.CoreFeaturesV2) != nil, "i64 fill value rejected")

		// memory.init: source offset and length remain i32, destination follows memory64.
		initMod := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x04, 1, 0, false)),
			inspectFunctionSection(1),
			inspectDataCountSection(1),
			inspectCodeSection([]byte{
				wasm.OpcodeI64Const, 0x00, // destination address i64
				wasm.OpcodeI32Const, 0x00, // source offset i32
				wasm.OpcodeI32Const, 0x00, // length i32
				wasm.OpcodeMiscPrefix, wasm.OpcodeMiscMemoryInit, 0x00, 0x00,
				wasm.OpcodeEnd,
			}),
			inspectPassiveDataSection([]byte{0xaa}),
		)
		c.Check(inspectDecode(t, initMod).Validate(api.CoreFeaturesV2) == nil, "memory.init typing accepted")

		// Active data segments: offset expression must match the target memory.
		i64Expr := []byte{wasm.OpcodeI64Const, 0x00, wasm.OpcodeEnd}
		i32Expr := []byte{wasm.OpcodeI32Const, 0x00, wasm.OpcodeEnd}
		segOK := inspectBuild(
			inspectMemorySection(inspectLimits(0x04, 1, 0, false)),
			inspectActiveDataSection(0, i64Expr, []byte{1, 2}),
		)
		c.Check(inspectDecode(t, segOK).Validate(api.CoreFeaturesV2) == nil, "i64 data offset on memory64 accepted")

		segBad := inspectBuild(
			inspectMemorySection(inspectLimits(0x04, 1, 0, false)),
			inspectActiveDataSection(0, i32Expr, []byte{1, 2}),
		)
		c.Check(inspectDecode(t, segBad).Validate(api.CoreFeaturesV2) != nil, "i32 data offset on memory64 rejected")

		segUnknown := inspectBuild(
			inspectMemorySection(inspectLimits(0x04, 1, 0, false)),
			inspectActiveDataSection(9, i64Expr, []byte{1}),
		)
		c.Check(inspectDecode(t, segUnknown).Validate(api.CoreFeaturesV2) != nil, "data segment on unknown memory rejected")

		passiveMod := inspectBuild(inspectPassiveDataSection([]byte{1, 2, 3}))
		pm := inspectDecode(t, passiveMod)
		c.Check(pm.Validate(api.CoreFeaturesV2) == nil, "passive segment validates without a memory")
		c.Check(pm.DataSection[0].Passive && pm.DataSection[0].MemoryIndex == 0, "passive segment stays unbound")

		// Exported memory indices are checked against the combined space.
		exportBad := inspectBuild(
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectExportMemorySection("m", 3),
		)
		c.Check(inspectDecode(t, exportBad).Validate(api.CoreFeaturesV2) != nil, "out-of-range memory export rejected")

		// Limits validation: width, page ceilings and min<=max. These fail at
		// decode time, so the decoder is called directly instead of inspectDecode.
		_, err32Ceil := DecodeModuleForInspection(
			inspectBuild(inspectMemorySection(inspectLimits(0x00, 1<<16+1, 0, false))), api.CoreFeaturesV2)
		c.Check(err32Ceil != nil, "memory32 page ceiling enforced")
		_, err64Ceil := DecodeModuleForInspection(
			inspectBuild(inspectMemorySection(inspectLimits(0x04, 1<<48+1, 0, false))), api.CoreFeaturesV2)
		c.Check(err64Ceil != nil, "memory64 page ceiling enforced")
		_, errMinMax := DecodeModuleForInspection(
			inspectBuild(inspectMemorySection(inspectLimits(0x04, 5, 4, true))), api.CoreFeaturesV2)
		c.Check(errMinMax != nil, "min greater than max rejected")

		// Shared memory is rejected on the inspection path.
		shared := inspectBuild(inspectMemoryImportSection("env", "s", 0x03, 1, 2, true))
		_, sharedErr := DecodeModuleForInspection(shared, api.CoreFeaturesV2)
		c.Check(sharedErr != nil, "shared memory rejected in inspection mode")

		// SIMD memory access is rejected even though V2 enables SIMD.
		simdBody := []byte{
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeVecPrefix, wasm.OpcodeVecV128Load, 0x02, 0x00,
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		simdMod := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(simdBody),
		)
		c.Check(inspectDecode(t, simdMod).Validate(api.CoreFeaturesV2) != nil, "SIMD load rejected in inspection mode")

		// Atomic access is rejected regardless of the threads feature.
		atomicBody := []byte{
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeI32Const, 0x00,
			wasm.OpcodeAtomicPrefix, 0x00, 0x02, 0x00, // atomic.notify
			wasm.OpcodeEnd,
		}
		atomicMod := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection(atomicBody),
		)
		c.Check(inspectDecode(t, atomicMod).Validate(api.CoreFeaturesV2) != nil, "atomic access rejected in inspection mode")

		// Unreachable code stays polymorphic but its memory index and immediates
		// are still decoded and checked.
		deadBad := inspectBuild(
			inspectMemorySection(inspectLimits(0x00, 1, 0, false)),
			inspectFunctionSection(1),
			inspectCodeSection([]byte{
				wasm.OpcodeUnreachable,
				wasm.OpcodeI32Const, 0x00,
				wasm.OpcodeI32Load, 0x42, 0x07, 0x00, // index 7 unknown even in dead code
				wasm.OpcodeDrop,
				wasm.OpcodeEnd,
			}),
		)
		c.Check(inspectDecode(t, deadBad).Validate(api.CoreFeaturesV2) != nil, "memory index checked inside unreachable code")

		deadOK := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(
				inspectLimits(0x00, 1, 0, false),
				inspectLimits(0x04, 1, 0, false),
			),
			inspectFunctionSection(1),
			inspectCodeSection([]byte{
				wasm.OpcodeUnreachable,
				wasm.OpcodeI64Const, 0x00,
				wasm.OpcodeI32Load, 0x42, 0x01, 0x00, // memory 1, reachable only via dead code
				wasm.OpcodeDrop,
				wasm.OpcodeEnd,
			}),
		)
		mDead := inspectDecode(t, deadOK)
		c.Check(mDead.Validate(api.CoreFeaturesV2) == nil, "polymorphic dead code still validates")
		c.Check(len(mDead.MemoryUses()) == 1 && mDead.MemoryUses()[0].MemoryIndices[0] == 1,
			"dead-code memory reference still recorded")
	})
}

func TestInspectionMemoryOffsets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Independently laid out body; offsets are computed from the raw bytes
		// themselves rather than derived from the recorded output.
		body := []byte{
			0x42, 0x00, // 0-1: i64.const 0
			0x28, 0x42, 0x01, 0x80, 0x80, 0x80, 0x80, 0x40, // 2-9: i32.load memidx 1, offset 2^34
			0x1a,       // 10: drop
			0x41, 0x00, // 11-12: i32.const 0 (proves cursor realignment)
			0x1a, // 13: drop
			0x0b, // 14: end
		}
		mod := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(
				inspectLimits(0x00, 1, 0, false),
				inspectLimits(0x04, 1, 0, false),
			),
			inspectFunctionSection(1),
			inspectCodeSection(body),
		)
		// Raw byte coordinates first.
		c.Check(body[2] == wasm.OpcodeI32Load, "raw body[2] is i32.load")
		c.Check(body[3] == 0x42, "raw body[3] carries the memory-index align bit")
		c.Check(body[10] == wasm.OpcodeDrop, "raw body[10] is the drop after the memarg")
		c.Check(body[13] == wasm.OpcodeDrop, "raw body[13] proves the multi-byte offset did not desync the cursor")

		m := inspectDecode(t, mod)
		c.Check(m.Validate(api.CoreFeaturesV2) == nil, "multi-byte offset module validates")
		c.Check(len(m.MemoryUses()) == 1, "one use")
		u := m.MemoryUses()[0]
		c.Check(u.BodyOffset == 2, "recorded offset matches the independent body coordinate")
		c.Check(u.MemoryIndices[0] == 1, "memory index 1 decoded after align bit6")
		c.Check(u.Offset == 1<<34, "full ULEB64 offset 2^34 decoded without truncation")

		// Multi-byte memory index: 129 memory64 declarations, reference #128.
		var manyLimits [][]byte
		for i := 0; i < 129; i++ {
			manyLimits = append(manyLimits, inspectLimits(0x04, 0, 0, false))
		}
		multiBody := []byte{
			wasm.OpcodeI64Const, 0x00,
			wasm.OpcodeI32Load, 0x42, 0x80, 0x01, 0x00, // memidx 128 = ULEB 0x80 0x01
			wasm.OpcodeDrop,
			wasm.OpcodeEnd,
		}
		multiMod := inspectBuild(
			inspectEmptyFuncTypeSection(),
			inspectMemorySection(manyLimits...),
			inspectFunctionSection(1),
			inspectCodeSection(multiBody),
		)
		c.Check(multiBody[4] == 0x80 && multiBody[5] == 0x01, "raw multi-byte memory index bytes")
		mMulti := inspectDecode(t, multiMod)
		c.Check(mMulti.Validate(api.CoreFeaturesV2) == nil, "module with 129 memories validates")
		c.Check(mMulti.MemoryUses()[0].MemoryIndices[0] == 128, "multi-byte memory index 128 decoded")

		// memory.copy ordering and offsets against raw body coordinates.
		copyBody := []byte{
			wasm.OpcodeI64Const, 0x00, // 0-1
			wasm.OpcodeI32Const, 0x00, // 2-3
			wasm.OpcodeI32Const, 0x00, // 4-5
			wasm.OpcodeMiscPrefix,                 // 6
			wasm.OpcodeMiscMemoryCopy, 0x01, 0x00, // 7,8,9: dst 1, src 0
			wasm.OpcodeEnd, // 10
		}
		copyMod := inspectBuild(inspectEmptyFuncTypeSection(),
			inspectMemorySection(
				inspectLimits(0x00, 1, 0, false),
				inspectLimits(0x04, 1, 0, false),
			),
			inspectFunctionSection(1),
			inspectCodeSection(copyBody))
		c.Check(copyBody[6] == wasm.OpcodeMiscPrefix && copyBody[7] == wasm.OpcodeMiscMemoryCopy,
			"raw copy opcode coordinates")
		mCopy := inspectDecode(t, copyMod)
		c.Check(mCopy.Validate(api.CoreFeaturesV2) == nil, "copy module validates")
		cu := mCopy.MemoryUses()[0]
		c.Check(cu.BodyOffset == 6, "copy body offset matches raw coordinate")
		c.Check(cu.MemoryIndices[0] == 1 && cu.MemoryIndices[1] == 0, "copy indices in destination, source order")
		c.Check(cu.Offset == 0, "non-memarg instruction reports zero offset")

		// mixed.wasm: every recorded offset points at the expected raw opcode.
		raw, err := os.ReadFile("../../../task_support/fixtures/mixed.wasm")
		if err != nil {
			t.Fatalf("read mixed.wasm: %v", err)
		}
		mixed := inspectDecode(t, raw)
		if err = mixed.Validate(api.CoreFeaturesV2); err != nil {
			t.Fatalf("mixed.wasm validate: %v", err)
		}
		body0 := mixed.CodeSection[0].Body
		expected := []struct {
			offset uint32
			op     byte
			name   string
		}{
			{2, wasm.OpcodeI32Load, "i32.load"},
			{17, wasm.OpcodeMiscPrefix, "memory.copy prefix"},
			{21, wasm.OpcodeMemorySize, "memory.size"},
			{26, wasm.OpcodeMemoryGrow, "memory.grow"},
		}
		for _, e := range expected {
			c.Check(body0[e.offset] == e.op, "raw mixed.wasm body[%d] is %s", e.offset, e.name)
		}
		uses := mixed.MemoryUses()
		for i, e := range expected {
			c.Check(uses[i].BodyOffset == e.offset && uses[i].FunctionIndex == 1,
				"recorded use %d at function 1 offset %d, got %d", i, e.offset, uses[i].BodyOffset)
		}
		c.Check(uses[0].Opcode == "i32.load" && uses[0].MemoryIndices[0] == 1 && uses[0].Offset == 5368709120,
			"load reference: memory 1, offset 5*2^30")
		c.Check(uses[1].Opcode == "memory.copy" && uses[1].MemoryIndices[0] == 1 && uses[1].MemoryIndices[1] == 0,
			"copy reference: destination 1, source 0")
		c.Check(uses[2].Opcode == "memory.size" && uses[2].MemoryIndices[0] == 2, "size reference: memory 2")
		c.Check(uses[3].Opcode == "memory.grow" && uses[3].MemoryIndices[0] == 2, "grow reference: memory 2")

		// The imported function prefix means the local body is function 1.
		c.Check(mixed.ImportFunctionCount == 1 && mixed.FunctionSection != nil,
			"function index includes the imported prefix")
	})
}

// inspectTwoMemoryImportsSection emits two imported 32-bit memories.
func inspectTwoMemoryImportsSection() []byte {
	build := func(module, name string) []byte {
		p := []byte{}
		p = append(p, uleb(uint64(len(module)))...)
		p = append(p, module...)
		p = append(p, uleb(uint64(len(name)))...)
		p = append(p, name...)
		p = append(p, wasm.ExternTypeMemory, 0x00, 0x01)
		return p
	}
	p := []byte{0x02}
	p = append(p, build("mod1", "a")...)
	p = append(p, build("mod2", "b")...)
	return inspectSec(wasm.SectionIDImport, p)
}
