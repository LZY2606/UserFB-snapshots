package main
import (
 "fmt"
 "os"
 d "example.org/offline-dwarf"
 "example.org/offline-dwarf/task_support/fixture"
)
func open(s fixture.Sections)*d.Data { x,e:=d.New(s.Abbrev,nil,nil,s.Info,nil,nil,nil,nil);if e!=nil{panic(e)};return x }
func main(){
 l:=fixture.Legacy();ld:=open(l);t,e:=ld.Type(d.Offset(l.TypeOffset));if e!=nil||t.Size()!=4 {fmt.Fprintln(os.Stderr,"legacy failed",e);os.Exit(1)}
 fmt.Printf("LEGACY_TYPE name=%s size=%d\n",t.Common().Name,t.Size())
 s:=fixture.V5();x:=open(s);r:=x.Reader();r.Seek(d.Offset(s.AliasOffset));en,e:=r.Next();if e!=nil||en==nil{panic(e)}
 f:=en.AttrField(d.AttrType);fmt.Printf("V5_REF class=%s signature=%x\n",f.Class,f.Val)
 raw,e:=x.Type(d.Offset(s.TypeOffset));if e!=nil||raw.Size()!=4{panic(fmt.Sprintf("direct TU read: %v",e))};fmt.Printf("V5_DIRECT_TYPE name=%s size=%d\n",raw.Common().Name,raw.Size())
 typ,e:=x.Type(d.Offset(s.AliasOffset));if e!=nil {fmt.Printf("V5_SIGNATURE_UNAVAILABLE %s\n",e)}else{fmt.Printf("V5_SIGNATURE_TYPE %s size=%d\n",typ,typ.Size())}
}
