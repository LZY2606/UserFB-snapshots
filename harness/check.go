package main
import("bufio";"encoding/json";"fmt";"os";"os/exec";"strings")
func main(){
 names:=[]string{"TestMemoFiles","TestMemoStream","TestMemoZip","TestMemoFailures","TestMemoOwnership"}
 cmd:=exec.Command("go","test","-vet=off","-count=1","-v","-json","-run","^("+strings.Join(names,"|")+")$",".")
 out,err:=cmd.StdoutPipe();if err!=nil{panic(err)};cmd.Stderr=os.Stderr;if err=cmd.Start();err!=nil{panic(err)}
 pass:=map[string]bool{};checks:=map[string]int{};skip:=false
 scan:=bufio.NewScanner(out);scan.Buffer(make([]byte,4096),8*1024*1024)
 for scan.Scan(){
  var e struct{Action,Test,Output string};if json.Unmarshal(scan.Bytes(),&e)!=nil{panic("bad test JSON")}
  fmt.Print(e.Output)
  if e.Action=="pass" && e.Test!=""{pass[e.Test]=true}
  if e.Action=="skip"{skip=true}
  for _,n:=range names{if (e.Test==n || strings.HasPrefix(e.Test,n+"/")) && strings.Contains(e.Output,"CHECK_CONTENT_CHECK"){checks[n]++}}
 }
 err=cmd.Wait();if scan.Err()!=nil || err!=nil || skip{fmt.Fprintln(os.Stderr,"target failed or skipped");os.Exit(1)}
 for _,n:=range names{if !pass[n] || checks[n]==0{fmt.Fprintln(os.Stderr,"missing executed content assertions:",n);os.Exit(1)}}
 fmt.Println("TARGET_MEMO_OK cases=5")
}
