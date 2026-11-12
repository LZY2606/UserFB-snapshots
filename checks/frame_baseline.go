package main
import (
 "bytes"
 "encoding/binary"
 "fmt"
 "github.com/go-delve/delve/pkg/dwarf/frame"
)
func image(o binary.ByteOrder) []byte {
 var b bytes.Buffer
 binary.Write(&b,o,uint32(12)); binary.Write(&b,o,uint32(0xffffffff))
 b.Write([]byte{3,0,1,0x78,16,0x0c,7,8})
 binary.Write(&b,o,uint32(23)); binary.Write(&b,o,uint32(0))
 binary.Write(&b,o,uint64(0x1000)); binary.Write(&b,o,uint64(0x20));b.Write([]byte{0x41,0x0e,0x10})
 return b.Bytes()
}
func main() {
 fs,e:=frame.Parse(image(binary.LittleEndian),binary.LittleEndian,0,8,0)
 if e!=nil || len(fs)!=1 {panic(fmt.Sprint(e,len(fs)))}
 c,e:=fs[0].EstablishFrame(0x1000)
 if e!=nil || c.CFA.Reg!=7 || c.CFA.Offset!=8 {panic("CFA")}
 fmt.Println("FRAME_BASELINE_OK begin=4096 cfa=r7+8")
 fs,e=frame.Parse(image(binary.BigEndian),binary.BigEndian,0,8,0)
 fmt.Printf("CURRENT_BIG_ENDIAN frames=%d error=%v\n",len(fs),e)
}
