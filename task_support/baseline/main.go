package main

import (
	"bytes"
	"fmt"
	"github.com/aclements/go-obj/obj"
	"github.com/aclements/go-obj/task_support/fixture"
)

func main() {
	for _, bits := range []int{32, 64} {
		raw := fixture.Image(bits)
		f, err := obj.Open(bytes.NewReader(raw))
		if err != nil {
			panic(err)
		}
		s := f.ResolveAddr(0x2000)
		if s == nil || s.Name != ".data" {
			panic("mapped data missing")
		}
		d, err := s.Data(s.Bounds())
		if err != nil {
			panic(err)
		}
		if len(d.R) != 2 || d.R[0].Addr != 0x2008 || d.R[0].Addend != -7 || d.R[1].Addr != 0x2018 || d.R[1].Addend != 11 {
			panic(fmt.Sprintf("relocations: %+v", d.R))
		}
		if f.NumSyms() != 6 || f.Sym(0).Name != "entry" || f.Sym(0).Value != 0x1000 {
			panic("symbol identity")
		}
		_, has := f.(interface {
			SetLoadBias(int64) error
			LoadBias() int64
		})
		fmt.Printf("BASELINE_ELF%d data=%#x relocs=%d addends=%d,%d symbol=%s bias_api=%t\n", bits, d.Addr, len(d.R), d.R[0].Addend, d.R[1].Addend, f.Sym(0).Name, has)
		f.Close()
	}
}
