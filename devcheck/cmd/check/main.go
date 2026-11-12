package main
import("encoding/json";"fmt";"io";"os")
func main(){
 f,e:=os.Open(os.Args[1]);if e!=nil{panic(e)};defer f.Close()
 required:=map[string]bool{"TestFilterDelta":false,"TestFilterX86":false,"TestFilterChainBlocks":false,"TestFilterReject":false}
 runs:=map[string]bool{};dec:=json.NewDecoder(f)
 for {var v struct{Action,Test string};e=dec.Decode(&v);if e==io.EOF{break};if e!=nil{panic(e)}
 if _,ok:=required[v.Test];!ok{continue};if v.Action=="run"{runs[v.Test]=true};if v.Action=="pass"{required[v.Test]=true};if v.Action=="skip"||v.Action=="fail"{panic("target skipped/failed: "+v.Test)}}
 for name,pass:=range required{if !pass||!runs[name]{panic("target did not run and pass: "+name)}}
 fmt.Println("XZ_TARGET_TESTS_EXECUTED count=4")
}
