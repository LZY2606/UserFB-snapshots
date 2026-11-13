package main
import("encoding/json";"os";"fmt";"strings";"io")
type Event struct{Action,Test,Output string}
func main(){names:=os.Args[2:];required:=map[string]bool{};for _,n:=range names{required[n]=false};sub:=map[string]int{};asserts:=map[string]bool{};passed:=map[string]bool{}
 f,e:=os.Open(os.Args[1]);if e!=nil{panic(e)};defer f.Close();d:=json.NewDecoder(f)
 for{var ev Event;e=d.Decode(&ev);if e==io.EOF{break};if e!=nil{panic(e)};top:=strings.Split(ev.Test,"/")[0];if _,ok:=required[top];!ok{continue};if ev.Action=="skip"{panic("skipped target")};if ev.Action=="pass"{passed[ev.Test]=true;if top==ev.Test{required[top]=true}else{sub[top]++}};if ev.Action=="output" && strings.Contains(ev.Output,"UNIT_ASSERT "+ev.Test+" checks="){var name string;var n int;if _,e:=fmt.Sscanf(strings.TrimSpace(ev.Output),"UNIT_ASSERT %s checks=%d",&name,&n);e==nil&&name==ev.Test&&n>0{asserts[ev.Test]=true}}}
 for n,ok:=range required{if !ok||sub[n]==0{panic("missing target/subtest "+n)}};for n:=range passed{if strings.Contains(n,"/")&&!asserts[n]{panic("no content assertions "+n)}};fmt.Println("GO_TARGET_COMPLETE groups=3")
}
