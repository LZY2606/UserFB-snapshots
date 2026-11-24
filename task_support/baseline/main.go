package main
import (
 "fmt"
 "os"
 "reflect"
 pe "github.com/mewmew/pe"
 "github.com/mewmew/pe/task_support/fixtures"
)
func main(){
 f,err:=pe.ParseBytes(fixtures.NB10());if err!=nil {panic(err)}
 d,ok:=f.DbgData[0].(*pe.DebugCodeView)
 if !ok || d.PDBPath!="offline-demo.pdb" || d.Age!=3 || d.Offset!=0 {panic("NB10 mismatch")}
 fmt.Printf("BASELINE_OK NB10 path=%s age=%d sections=%d\n",d.PDBPath,d.Age,len(f.SectHdrs))
 if len(os.Args)>1 && os.Args[1]=="probe" {
  e,err:=pe.ParseBytes(fixtures.Embedded("NB09"));if err!=nil {panic(err)}
  cv:=e.DbgData[0].(*pe.DebugCodeView)
  field:=reflect.ValueOf(cv).Elem().FieldByName("Embedded")
  present:=field.IsValid()
  fmt.Printf("FEATURE_PROBE embedded_field=%v signature=%08x offset=%x legacy_age=%d path=%q\n",present,cv.Signature,cv.Offset,cv.Age,cv.PDBPath)
 }
}
