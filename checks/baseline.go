package main
import("fmt";"reflect";"github.com/elastic/go-libaudit/v2/auparse";"github.com/elastic/go-libaudit/v2/aucoalesce")
func read(s string)*auparse.AuditMessage{m,e:=auparse.ParseLogLine(s);if e!=nil{panic(e)};return m}
func main(){
 syscall:=`type=SYSCALL msg=audit(1700000000.001:42): arch=c000003e syscall=59 success=yes exit=0 pid=12 ppid=1 uid=1000 gid=1000 comm="echo" exe="/bin/echo"`
 one:=[]*auparse.AuditMessage{read(syscall),read(`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a1="alpha" a2="beta"`)}
 e,err:=aucoalesce.CoalesceMessages(one);if err!=nil||!reflect.DeepEqual(e.Process.Args,[]string{"echo","alpha","beta"}){panic(fmt.Sprint(e,err))}
 fmt.Println("AUDIT_SINGLE_OK",e.Process.Args)
 split:=[]*auparse.AuditMessage{read(syscall),read(`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a1="alpha"`),read(`type=EXECVE msg=audit(1700000000.001:42): a2="beta"`)}
 e,err=aucoalesce.CoalesceMessages(split);if err!=nil{panic(err)}
 fmt.Printf("AUDIT_SPLIT args=%v warnings=%d\n",e.Process.Args,len(e.Warnings))
 if len(e.Process.Args)!=0||len(e.Warnings)<2{panic("startpoint changed")}
 fmt.Println("AUDIT_BASELINE_REPRODUCED")
}
