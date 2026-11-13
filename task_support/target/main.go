package main
import("bufio";"encoding/json";"fmt";"os";"os/exec";"strings")
type event struct { Action, Test, Output string }
func main(){
 names:=[]string{"TestLLHLSPartLifecycle","TestLLHLSPartRoundTrip","TestLLHLSPartWindow","TestAppendSegmentToMediaPlaylist","TestSetDefaultMapForMediaPlaylist"}; wanted:=map[string]bool{}; for _,n:=range names {wanted[n]=false}
 args:=[]string{"test","-vet=off","-json","-count=1","-run","^("+strings.Join(names,"|")+")$","."}
 cmd:=exec.Command("go",args...);cmd.Stderr=os.Stderr;pipe,err:=cmd.StdoutPipe();if err!=nil{panic(err)};if err=cmd.Start();err!=nil{panic(err)}
 scanner:=bufio.NewScanner(pipe);scanner.Buffer(make([]byte,4096),8*1024*1024);bad:=false
 for scanner.Scan(){var e event;if err=json.Unmarshal(scanner.Bytes(),&e);err!=nil{fmt.Fprintln(os.Stderr,err);bad=true;continue};fmt.Print(e.Output);if _,ok:=wanted[e.Test];ok {if e.Action=="pass"{wanted[e.Test]=true};if e.Action=="skip"||e.Action=="fail"{bad=true}};if e.Action=="skip"{bad=true}}
 if scanner.Err()!=nil{bad=true};if cmd.Wait()!=nil{bad=true};for n,ok:=range wanted {if !ok{fmt.Fprintln(os.Stderr,"required test did not pass:",n);bad=true}}
 if bad{os.Exit(1)};fmt.Println("target tests executed:",len(names))
}
