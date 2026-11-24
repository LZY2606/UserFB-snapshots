package main
import("bytes";"debug/elf";"debug/dwarf";"fmt";"os";"github.com/go-delve/delve/pkg/dwarf/godwarf";"github.com/go-delve/delve/pkg/dwarf/op";"github.com/go-delve/delve/task_support/fixture")
func main(){
 if len(os.Args)==2 && os.Args[1]=="--write" {if err:=os.WriteFile("task_support/fixtures/typed.elf",fixture.ELF(),0644);err!=nil{panic(err)};return}
 raw,err:=os.ReadFile("task_support/fixtures/typed.elf");if err!=nil{panic(err)};f,err:=elf.NewFile(bytes.NewReader(raw));if err!=nil{panic(err)};dw,err:=f.DWARF();if err!=nil{panic(err)}
 _,_,units:=fixture.Sections();for i,u:=range units {t,err:=godwarf.ReadType(dw,0, dwarf.Offset(u.Start+uint32(u.Byte)),make(map[dwarf.Offset]godwarf.Type));if err!=nil{panic(err)};fmt.Printf("CU=%d BASE=%s SIZE=%d\n",i,t.String(),t.Size())}
 v,p,e:=op.ExecuteStackProgram(op.DwarfRegisters{},[]byte{0x37,0x39,0x22},8,nil);if e!=nil||v!=16||p!=nil{panic(fmt.Sprint(v,p,e))};fmt.Printf("LEGACY_VALUE=%d\n",v)
 if len(os.Args)<2 || os.Args[1]!="target" { _,_,e=op.ExecuteStackProgram(op.DwarfRegisters{},[]byte{0xa4,byte(units[0].Byte),1,0xfe,0x9f},8,nil);if e==nil{panic("typed unexpectedly supported")};fmt.Printf("TYPED_CURRENT_ERROR=%s\n",e) }
 fmt.Println("BASELINE_OK")
}
