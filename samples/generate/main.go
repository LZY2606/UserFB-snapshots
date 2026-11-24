// Generates controlled inputs and the sample author's object/site manifest.
// It does not classify writes or calculate dependency reports.
package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

type object struct {
	ID   string `json:"id"`
	Base uint32 `json:"base"`
	Size uint32 `json:"size"`
}
type source struct {
	ID     string `json:"id"`
	Offset uint32 `json:"offset"`
	Bytes  []byte `json:"bytes"`
}
type target struct {
	Function uint32 `json:"function"`
	Offset   uint32 `json:"offset"`
	Object   string `json:"object"`
}
type call struct {
	Export string   `json:"export"`
	Args   []uint32 `json:"args"`
}
type plan struct {
	Objects []object `json:"objects"`
	Sources []source `json:"sources"`
	Targets []target `json:"targets"`
	Calls   []call   `json:"calls"`
}

func u(v uint32) (b []byte) {
	for {
		x := byte(v & 127)
		v >>= 7
		if v != 0 {
			x |= 128
		}
		b = append(b, x)
		if v == 0 {
			return
		}
	}
}
func sec(id byte, b []byte) []byte { return append(append([]byte{id}, u(uint32(len(b)))...), b...) }
func str(s string) []byte          { return append(u(uint32(len(s))), []byte(s)...) }

type code struct {
	b       []byte
	fn      uint32
	targets *[]target
}

func (c *code) emit(b ...byte) { c.b = append(c.b, b...) }
func (c *code) write(obj string, b ...byte) {
	*c.targets = append(*c.targets, target{c.fn, uint32(len(c.b)), obj})
	c.emit(b...)
}
func must(err error) {
	if err != nil {
		panic(err)
	}
}
func main() {
	out := "samples"
	if len(os.Args) > 1 {
		out = os.Args[1]
	}
	must(os.MkdirAll(out, 0755))
	p := plan{Objects: []object{{"input", 0, 16}, {"buffer", 32, 8}, {"state", 40, 8}}, Sources: []source{{"pointer", 0, []byte{38}}, {"length", 4, []byte{4}}, {"data-a", 8, []byte{17, 34}}, {"data-b", 10, []byte{51, 68}}}}
	names := []string{"prime", "helper", "run", "narrow", "linear", "overlap", "fill"}
	codes := make([][]byte, len(names))
	for i := range names {
		c := code{fn: uint32(i), targets: &p.Targets}
		switch i {
		case 0:
			c.emit(0x41, 40, 0x41, 0, 0x2d, 0, 0)
			c.write("state", 0x36, 2, 0)
			c.emit(0x41, 44, 0x41, 4, 0x2d, 0, 0)
			c.write("state", 0x36, 2, 0)
		case 1:
			c.emit(0x41, 40, 0x28, 2, 0, 0x24, 0, 0x23, 0, 0x41, 8, 0x41, 44, 0x28, 2, 0)
			c.write("buffer", 0xfc, 10, 0, 0)
		case 2:
			c.emit(0x10, 1)
		case 3:
			c.emit(0x41, 40, 0x41, 32)
			c.write("state", 0x3a, 0, 0)
		case 4:
			c.emit(0x41, 32, 0x41, 9)
			c.write("buffer", 0x3a, 0, 0)
			c.emit(0x41, 0xff, 0xff, 3, 0x41, 7)
			c.write("buffer", 0x3b, 1, 0)
		case 5:
			c.emit(0x41, 33, 0x41, 32, 0x41, 4)
			c.write("buffer", 0xfc, 10, 0, 0)
		case 6:
			c.emit(0x41, 32, 0x41, 8, 0x2d, 0, 0, 0x41, 4, 0x2d, 0, 0)
			c.write("buffer", 0xfc, 11, 0)
		}
		c.emit(0x0b)
		body := append([]byte{0}, c.b...)
		codes[i] = append(u(uint32(len(body))), body...)
	}
	b := []byte{0, 97, 115, 109, 1, 0, 0, 0}
	b = append(b, sec(1, []byte{1, 0x60, 0, 0})...)
	funcs := []byte{byte(len(names))}
	for range names {
		funcs = append(funcs, 0)
	}
	b = append(b, sec(3, funcs)...)
	b = append(b, sec(5, []byte{1, 1, 1, 2})...)
	b = append(b, sec(6, []byte{1, 0x7f, 1, 0x41, 0, 0x0b})...)
	exports := []byte{byte(len(names))}
	for i, n := range names {
		exports = append(exports, str(n)...)
		exports = append(exports, 0, byte(i))
	}
	b = append(b, sec(7, exports)...)
	all := []byte{byte(len(codes))}
	for _, c := range codes {
		all = append(all, c...)
	}
	b = append(b, sec(10, all)...)
	for _, n := range []string{"prime", "run", "prime", "narrow", "run", "linear", "prime", "run"} {
		p.Calls = append(p.Calls, call{n, []uint32{}})
	}
	must(os.WriteFile(filepath.Join(out, "crosscall.wasm"), b, 0644))
	j, e := json.MarshalIndent(p, "", "  ")
	must(e)
	must(os.WriteFile(filepath.Join(out, "crosscall.json"), append(j, '\n'), 0644))
}
