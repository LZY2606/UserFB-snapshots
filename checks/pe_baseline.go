package main
import (
 "fmt"
 p "github.com/Binject/debug/pe"
)
func main() {
 f,e:=p.Open("pe/testdata/gcc-amd64-mingw-exec");if e!=nil {panic(e)};defer f.Close()
 h,ok:=f.OptionalHeader.(*p.OptionalHeader64);if !ok {panic("not PE32+")}
 s:=f.Section(".text");if s==nil {panic("missing text")}
 b,e:=s.Data();if e!=nil || len(b)==0 {panic("empty text")}
 if f.Machine!=p.IMAGE_FILE_MACHINE_AMD64 {panic("machine")}
 d:=h.DataDirectory[p.IMAGE_DIRECTORY_ENTRY_EXCEPTION]
 fmt.Printf("PE_BASELINE_OK amd64 sections=%d text_bytes=%d\n",len(f.Sections),len(b))
 fmt.Printf("EXCEPTION_DIRECTORY rva=%d size=%d\n",d.VirtualAddress,d.Size)
}
