package main
import (
 "bytes"
 "fmt"
 "github.com/tetratelabs/wabin/binary"
 "github.com/tetratelabs/wabin/wasm"
)
func main() {
 input:=[]byte{0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,3,2,1,0,5,3,1,0,1,7,5,1,1,102,0,0,10,16,1,14,0,65,0,65,42,54,2,0,65,0,40,2,0,11}
 m,err:=binary.DecodeModule(input,wasm.CoreFeaturesV1);if err!=nil {panic(err)}
 if m.MemorySection.Min!=1 || len(m.CodeSection)!=1 || !bytes.Equal(m.CodeSection[0].Body,[]byte{65,0,65,42,54,2,0,65,0,40,2,0,11}) {panic("decoded body mismatch")}
 output:=binary.EncodeModule(m); if !bytes.Equal(input,output) {panic("roundtrip mismatch")}
 fmt.Println("WABIN_BASELINE_OK memory=1 body=13 roundtrip")
}
