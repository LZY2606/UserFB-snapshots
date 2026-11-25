package memanalysis

import (
	"encoding/json"
	"os"
	"path/filepath"

	"github.com/tetratelabs/wazero/internal/leb128"
)

// wasmBuilder assembles the restricted, i32-only modules used by the contract
// tests. It mirrors the hand-rolled encoding in samples/generate rather than
// pulling in another dependency.
type wasmBuilder struct {
	funcTypes  [][]wasmValType
	funcTypeOf []int
	exports    []wasmExport
	bodies     [][]byte
	writeSites []Target
}

type wasmValType byte

type wasmExport struct {
	name string
	kind byte
	idx  uint32
}

func lebU(v uint32) []byte { return leb128.EncodeUint32(v) }

func lebS(v int32) []byte { return leb128.EncodeInt32(v) }

func sec(id byte, payload []byte) []byte {
	return append(append([]byte{id}, lebU(uint32(len(payload)))...), payload...)
}

func name(s string) []byte {
	return append(lebU(uint32(len(s))), []byte(s)...)
}

func memarg(align, offset uint32) []byte {
	return append(lebU(align), lebU(offset)...)
}

func newWasmBuilder() *wasmBuilder { return &wasmBuilder{} }

// addFunction registers an empty-result i32 function with the given number of
// i32 parameters and returns its local function index.
func (b *wasmBuilder) addFunction(params int, export string) int {
	idx := len(b.funcTypeOf)
	b.funcTypes = append(b.funcTypes, make([]wasmValType, params))
	b.funcTypeOf = append(b.funcTypeOf, idx)
	b.bodies = append(b.bodies, nil)
	if export != "" {
		b.exports = append(b.exports, wasmExport{name: export, kind: 0x00, idx: uint32(idx)})
	}
	return idx
}

// emit appends bytes to a function body.
func (b *wasmBuilder) emit(fn int, bytes ...byte) { b.bodies[fn] = append(b.bodies[fn], bytes...) }

// emitU appends an unsigned LEB128 immediate to a function body.
func (b *wasmBuilder) emitU(fn int, v uint32) { b.bodies[fn] = append(b.bodies[fn], lebU(v)...) }

// markStore records a manifest target for a write instruction at the current
// body offset and returns that offset.
func (b *wasmBuilder) markStore(fn int, object string) uint32 {
	off := uint32(len(b.bodies[fn]))
	b.writeSites = append(b.writeSites, Target{Function: uint32(fn), Offset: off, Object: object})
	return off
}

func (b *wasmBuilder) build() []byte {
	out := []byte{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}

	// Type section: all () -> () or (i32^n) -> () functions.
	var types []byte
	types = append(types, lebU(uint32(len(b.funcTypes)))...)
	for _, params := range b.funcTypes {
		types = append(types, 0x60)
		types = append(types, lebU(uint32(len(params)))...)
		for range params {
			types = append(types, 0x7f)
		}
		types = append(types, 0) // no results
	}
	out = append(out, sec(0x01, types)...)

	// Function section.
	funcs := append(lebU(uint32(len(b.funcTypeOf))), func(builders []int) []byte {
		var x []byte
		for _, t := range builders {
			x = append(x, byte(t))
		}
		return x
	}(b.funcTypeOf)...)
	out = append(out, sec(0x03, funcs)...)

	// Memory section: one non-shared memory, min 1, explicit max 2 pages.
	out = append(out, sec(0x05, []byte{0x01, 0x01, 0x01, 0x02})...)

	// Export section.
	var exps []byte
	exps = append(exps, lebU(uint32(len(b.exports)))...)
	for _, e := range b.exports {
		exps = append(exps, name(e.name)...)
		exps = append(exps, e.kind, byte(e.idx))
	}
	out = append(out, sec(0x07, exps)...)

	// Code section.
	var codes []byte
	codes = append(codes, lebU(uint32(len(b.bodies)))...)
	for _, body := range b.bodies {
		full := append([]byte{0x00}, body...) // zero local declarations
		full = append(full, 0x0b)             // end
		codes = append(codes, append(lebU(uint32(len(full))), full...)...)
	}
	out = append(out, sec(0x0a, codes)...)
	return out
}

// loadCrosscallSample reads the public controlled sample and its manifest.
func loadCrosscallSample() ([]byte, Plan, error) {
	root := findRepoRoot()
	bin, err := os.ReadFile(filepath.Join(root, "samples", "crosscall.wasm"))
	if err != nil {
		return nil, Plan{}, err
	}
	planBytes, err := os.ReadFile(filepath.Join(root, "samples", "crosscall.json"))
	if err != nil {
		return nil, Plan{}, err
	}
	var plan Plan
	if err := json.Unmarshal(planBytes, &plan); err != nil {
		return nil, Plan{}, err
	}
	return bin, plan, nil
}

func findRepoRoot() string {
	dir, err := os.Getwd()
	if err != nil {
		panic(err)
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			panic("repository root not found")
		}
		dir = parent
	}
}

func findEvent(events []Event, fn, off uint32) *Event {
	for i := range events {
		if events[i].Function == fn && events[i].Offset == off {
			return &events[i]
		}
	}
	return nil
}
