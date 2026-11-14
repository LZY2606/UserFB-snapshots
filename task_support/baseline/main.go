package main

import (
	"bytes"
	"fmt"
	pe "github.com/saferwall/pe"
	"github.com/saferwall/pe/task_support/fixture"
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
	for _, bits := range []int{32, 64} {
		input := fixture.Disk(bits)
		original := append([]byte(nil), input...)
		f, err := pe.NewBytes(input, &pe.Options{})
		must(err == nil, "NewBytes")
		err = f.Parse()
		must(err == nil, fmt.Sprint("Parse: ", err))
		marker, err := f.GetData(0x1050, 4)
		must(err == nil && string(marker) == "DATA", "production GetData mismatch")
		var callback uint64
		if bits == 64 {
			v, ok := f.TLS.Callbacks.([]uint64)
			must(ok && len(v) == 1, "TLS64")
			callback = v[0]
			must(callback == 0x140001500, "callback64")
		} else {
			v, ok := f.TLS.Callbacks.([]uint32)
			must(ok && len(v) == 1, "TLS32")
			callback = uint64(v[0])
			must(callback == 0x401500, "callback32")
		}
		must(len(f.Debugs) == 1, "debug count")
		cv, ok := f.Debugs[0].Info.(pe.CVInfoPDB70)
		must(ok && cv.Age == 3 && cv.PDBFileName == "a.pdb", "CodeView")
		must(bytes.Equal(input, original), "input modified")
		fmt.Printf("BASELINE_OK PE%d marker=%s callback=%#x pdb=%s age=%d\n", bits, marker, callback, cv.PDBFileName, cv.Age)
	}
	_, present := reflect.TypeOf(pe.Options{}).FieldByName("MemoryLayout")
	fmt.Println("MEMORY_LAYOUT_AVAILABLE", present)
}
