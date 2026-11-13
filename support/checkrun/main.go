// Checks the Go testing event stream outside the test process.
package main
import("bufio";"encoding/json";"fmt";"os";"strings")
type event struct { Action, Package, Test, Output string }
func main(){
 roots:=[]string{"TestChainPCM","TestChainSeek","TestChainPages","TestChainStreaming"}
 run:=map[string]bool{};pass:=map[string]bool{};children:=map[string]int{};packages:=0
 scan:=bufio.NewScanner(os.Stdin);scan.Buffer(make([]byte,4096),1024*1024)
 for scan.Scan(){var e event;if err:=json.Unmarshal(scan.Bytes(),&e);err!=nil{panic(err)}
  if e.Output!="" {fmt.Print(e.Output)}
  if e.Action=="skip" || e.Action=="fail" {fmt.Fprintln(os.Stderr,"target skipped or failed:",e.Test);os.Exit(1)}
  if e.Test=="" {if e.Action=="pass" {packages++};continue}
  if e.Action=="run" {run[e.Test]=true};if e.Action=="pass" {pass[e.Test]=true}
 }
 if scan.Err()!=nil{panic(scan.Err())}
 for _,root:=range roots {for name:=range pass {if strings.HasPrefix(name,root+"/") && run[name] {children[root]++}}
  if !run[root] || !pass[root] || children[root]==0 {fmt.Fprintln(os.Stderr,"missing completed test group:",root);os.Exit(1)}
 }
 if packages!=1 {fmt.Fprintln(os.Stderr,"missing package completion");os.Exit(1)}
 fmt.Println("CHAIN_TARGET_GROUPS_COMPLETE")
}
