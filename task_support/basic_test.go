package task_support
import (
 "testing"
 d "example.org/offline-dwarf"
 "example.org/offline-dwarf/task_support/fixture"
)
func TestLegacyProduction(t *testing.T){s:=fixture.Legacy();x,e:=d.New(s.Abbrev,nil,nil,s.Info,nil,nil,nil,nil);if e!=nil{t.Fatal(e)};v,e:=x.Type(d.Offset(s.TypeOffset));if e!=nil||v.Size()!=4||v.Common().Name!="int"{t.Fatalf("legacy %v %v",v,e)}}
