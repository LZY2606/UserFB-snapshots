// This checks transport shape only. Expected function names and ranges are not bundled.
package main
import("encoding/json";"fmt";"os")
func must(e error){if e!=nil{panic(e)}}
func main(){
 if len(os.Args)!=3{panic("queries.json report.json")}
 q,e:=os.ReadFile(os.Args[1]);must(e);var expected []uint64;must(json.Unmarshal(q,&expected))
 raw,e:=os.ReadFile(os.Args[2]);must(e)
 var report struct{Functions []map[string]json.RawMessage `json:"functions"`;Queries []struct{PC uint64 `json:"pc"`;Matches []map[string]json.RawMessage `json:"matches"`} `json:"queries"`}
 must(json.Unmarshal(raw,&report));if len(report.Functions)==0||len(report.Queries)!=len(expected){panic("empty functions or missing queries")}
 for _,f:=range report.Functions{if len(f)==0{panic("empty function record")}}
 matched:=0
 for i,r:=range report.Queries{if r.PC!=expected[i]||r.Matches==nil{panic("query order, pc or matches array")};for _,m:=range r.Matches{if len(m)==0{panic("empty match record")};matched++}}
 if matched==0{panic("all queries empty")};fmt.Printf("REPORT_SHAPE_OK queries=%d functions=%d\n",len(expected),len(report.Functions))
}
