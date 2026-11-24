package main
import("fmt";"os";"path/filepath";"github.com/tetratelabs/wazero/api";bf "github.com/tetratelabs/wazero/internal/wasm/binary";"github.com/tetratelabs/wazero/task_support/fixture")
func main(){
 if len(os.Args)==2 && os.Args[1]=="--write" {for n,b:=range map[string][]byte{"legacy.wasm":fixture.Legacy(),"two32.wasm":fixture.Two32(),"memory64.wasm":fixture.One64(),"mixed.wasm":fixture.Mixed()} {if e:=os.WriteFile(filepath.Join("task_support/fixtures",n),b,0644);e!=nil{panic(e)}};return}
 raw,e:=os.ReadFile("task_support/fixtures/legacy.wasm");if e!=nil{panic(e)};m,e:=bf.DecodeModule(raw,api.CoreFeaturesV2,65536,false,false,false);if e!=nil{panic(e)};if e=m.Validate(api.CoreFeaturesV2);e!=nil{panic(e)};fmt.Printf("LEGACY_STATIC_OK functions=%d memory_min=%d body_bytes=%d\n",len(m.CodeSection),m.MemorySection.Min,len(m.CodeSection[0].Body))
 for _,x:=range []struct{file,label string}{{"two32.wasm","MULTI_CURRENT_ERROR"},{"memory64.wasm","MEMORY64_CURRENT_ERROR"}} {raw,e=os.ReadFile("task_support/fixtures/"+x.file);if e!=nil{panic(e)};_,e=bf.DecodeModule(raw,api.CoreFeaturesV2,65536,false,false,false);if e==nil{panic("legacy boundary unexpectedly widened")};fmt.Printf("%s=%v\n",x.label,e)}
 fmt.Println("BASELINE_OK")
}
