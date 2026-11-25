package memanalysis

import (
	"encoding/json"
	"reflect"
)

func contains(list []string, want string) bool {
	for _, v := range list {
		if v == want {
			return true
		}
	}
	return false
}

func clonePlan(p Plan) Plan {
	var out Plan
	out.Objects = append([]Object(nil), p.Objects...)
	out.Calls = append([]Call(nil), p.Calls...)
	out.Targets = append([]Target(nil), p.Targets...)
	if p.Sources != nil {
		out.Sources = make([]Source, len(p.Sources))
		for i, s := range p.Sources {
			out.Sources[i] = Source{ID: s.ID, Offset: s.Offset, Bytes: append([]byte(nil), s.Bytes...)}
		}
	}
	for i := range out.Calls {
		if p.Calls[i].Args != nil {
			out.Calls[i].Args = append([]uint32(nil), p.Calls[i].Args...)
		}
	}
	return out
}

func plansEqual(a, b Plan) bool { return reflect.DeepEqual(a, b) }

func jsonArrayNotEmpty(r Report) bool {
	raw, err := json.Marshal(r)
	if err != nil {
		return false
	}
	s := string(raw)
	return containsSubstring(s, `"victims":[]`) && containsSubstring(s, `"results":[]`) &&
		containsSubstring(s, `"calls":[`) && containsSubstring(s, `"writes":[`)
}

func containsSubstring(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

// buildUnsupportedDeadCodeModule places an i64.const after an unreachable so
// the unsupported opcode is only reachable through dead code.
func buildUnsupportedDeadCodeModule() []byte {
	const (
		i32Const = 0x41
		i32Store = 0x36
		unreach  = 0x00
		i64Const = 0x42
	)
	b := newWasmBuilder()
	fn := b.addFunction(0, "dead")
	b.emit(fn, i32Const, 0, i32Const, 1)
	b.markStore(fn, "objA")
	b.emit(fn, i32Store, 2, 0)
	b.emit(fn, unreach, i64Const, 0) // i64.const 0: valid wasm, outside the analyzer subset
	return b.build()
}

// buildLoopModule returns a module whose only export spins in an infinite
// loop, used to exercise context cancellation.
func buildLoopModule() []byte {
	b := newWasmBuilder()
	fn := b.addFunction(0, "spin")
	// loop (blocktype void); br 0; end
	b.emit(fn, 0x03, 0x40, 0x0c, 0x00, 0x0b)
	return b.build()
}

// buildGlobalPersistenceModule returns a module with one mutable i32 global:
// "setg" loads an input byte into the global, "getg" stores the global into
// objA, exercising origin persistence across calls.
func buildGlobalPersistenceModule() []byte {
	section := func(id byte, payload []byte) []byte {
		return append(append([]byte{id}, lebU(uint32(len(payload)))...), payload...)
	}
	// setg: i32.const 32; i32.load8_u align=0 off=0; global.set 0
	setgBody := []byte{0x41, 0x20, 0x2d, 0x00, 0x00, 0x24, 0x00, 0x0b}
	// getg: i32.const 0; global.get 0; i32.store align=2 off=0
	getgBody := []byte{0x41, 0x00, 0x23, 0x00, 0x36, 0x02, 0x00, 0x0b}

	out := []byte{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}
	out = append(out, section(0x01, []byte{0x01, 0x60, 0x00, 0x00})...)
	out = append(out, section(0x03, []byte{0x02, 0x00, 0x00})...)
	out = append(out, section(0x05, []byte{0x01, 0x01, 0x01, 0x02})...)
	out = append(out, section(0x06, []byte{0x01, 0x7f, 0x01, 0x41, 0x00, 0x0b})...)
	out = append(out, section(0x07, []byte{
		0x02,
		0x04, 's', 'e', 't', 'g', 0x00, 0x00,
		0x04, 'g', 'e', 't', 'g', 0x00, 0x01,
	})...)
	// Each code entry is a local-declaration count (zero) followed by the body.
	setgCode := append([]byte{0x00}, setgBody...)
	getgCode := append([]byte{0x00}, getgBody...)
	codes := append([]byte{0x02}, append(append(lebU(uint32(len(setgCode))), setgCode...),
		append(lebU(uint32(len(getgCode))), getgCode...)...)...)
	out = append(out, section(0x0a, codes)...)
	return out
}

// globalGetgStoreOffset is the body offset of the i32.store in getg:
// i32.const(2 bytes), global.get(2 bytes), then the store opcode.
const globalGetgStoreOffset = 4
