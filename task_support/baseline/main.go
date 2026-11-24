package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"github.com/cilium/ebpf/btf"
	"github.com/cilium/ebpf/task_support/fixture"
	"os"
	"reflect"
)

func must(ok bool, msg string) {
	if !ok {
		fmt.Fprintln(os.Stderr, msg)
		os.Exit(1)
	}
}
func main() {
	for _, o := range []struct {
		name string
		bo   binary.ByteOrder
	}{{"LE", binary.LittleEndian}, {"BE", binary.BigEndian}} {
		baseBytes, splitBytes := fixture.Original(o.bo)
		orig := append([]byte(nil), splitBytes...)
		base, e := btf.LoadSpecFromReader(bytes.NewReader(baseBytes))
		must(e == nil, fmt.Sprint(e))
		spec, e := btf.LoadSplitSpecFromReader(bytes.NewReader(splitBytes), base)
		must(e == nil, fmt.Sprint(e))
		var s *btf.Struct
		e = spec.TypeByName("leaf", &s)
		must(e == nil && len(s.Members) == 2, "local structure")
		id, e := spec.TypeID(s)
		must(e == nil && id == 3, "local type id")
		p, ok := s.Members[0].Type.(*btf.Pointer)
		must(ok && p.Target == s, "recursive reference")
		i, e := base.TypeByID(1)
		must(e == nil && s.Members[1].Type == i, "base object identity")
		must(s.Members[0].Name == "member" && len(s.Members[0].Tags) == 1 && s.Members[0].Tags[0] == "mark", "inherited string and declaration tag")
		_, e = spec.AnyTypeByName("int")
		must(e != nil, "split names leaked base")
		count := 0
		for _, e := range spec.All() {
			must(e == nil, "All error")
			count++
		}
		must(count == 2, "All includes tag/base")
		cp := spec.Copy()
		ct, e := cp.TypeByID(3)
		must(e == nil && ct != s, "independent copy")
		cs, ok := ct.(*btf.Struct)
		must(ok && cs.Members[0].Type.(*btf.Pointer).Target == cs, "copy cycle")
		must(bytes.Equal(orig, splitBytes), "input changed")
		fmt.Printf("BTF_BASELINE_OK %s local=%d member=%s int=%s tags=%s all=%d\n", o.name, id, s.Members[0].Name, i.TypeName(), s.Members[0].Tags[0], count)
	}
	_, present := reflect.TypeOf((*btf.Spec)(nil)).MethodByName("Rebase")
	fmt.Println("REBASE_AVAILABLE", present)
}
