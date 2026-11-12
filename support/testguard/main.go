package main
import("encoding/json";"fmt";"os";"os/exec";"bytes";"io")
func main(){
 if len(os.Args)<3 {panic("need regex and required top-level test names")}
 cmd:=exec.Command("go","test","-mod=vendor","-json","-count=1","-run",os.Args[1],"./unicode_regression")
 out,err:=cmd.CombinedOutput();os.Stdout.Write(out)
 if err!=nil {os.Exit(1)}
 dec:=json.NewDecoder(bytes.NewReader(out));passed:=map[string]bool{};ran:=map[string]bool{}
 for {var e struct{Action,Test string};err:=dec.Decode(&e);if err==io.EOF{break};if err!=nil{os.Exit(2)};if e.Test!=""{if e.Action=="skip"{fmt.Fprintln(os.Stderr,"target skip rejected");os.Exit(3)};if e.Action=="run"{ran[e.Test]=true};if e.Action=="pass"{passed[e.Test]=true}}}
 for _,name:=range os.Args[2:]{if !ran[name]||!passed[name]{fmt.Fprintln(os.Stderr,"missing executed PASS",name);os.Exit(4)}}
 fmt.Println("ZXCVBN_UNICODE_TARGET_TESTS_OK")
}
