package line

import (
 "bytes"
 "encoding/binary"
 "fmt"
 "testing"
)

// Legal, compiler-independent DWARF2 line contribution.
func TestBaseline(t *testing.T) {
 var pro bytes.Buffer
 pro.Write([]byte{1,1,251,14,13,0,1,1,1,1,0,0,0,1,0,0,1,0})
 pro.WriteString("unit.c\x00"); pro.Write([]byte{0,0,0,0})
 instr:=[]byte{0,9,2,0,16,0,0,0,0,0,0,1,2,4,0,1,1}
 var raw bytes.Buffer
 binary.Write(&raw,binary.LittleEndian,uint32(6+pro.Len()+len(instr)))
 binary.Write(&raw,binary.LittleEndian,uint16(2))
 binary.Write(&raw,binary.LittleEndian,uint32(pro.Len()))
 raw.Write(pro.Bytes());raw.Write(instr)
 info:=Parse("",&raw,nil,nil,0,false,8)
 f,n:=info.PCToLine(0,0x1001)
 if f!="unit.c" || n!=1 {t.Fatalf("%s:%d",f,n)}
 fmt.Printf("BASELINE pc=0x1001 file=%s line=%d\n",f,n)
}
