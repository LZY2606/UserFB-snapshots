package main
import (
 at "archive/tar"
 az "archive/zip"
 "bytes"
 "fmt"
 "io"
 st "github.com/google/safearchive/tar"
 sz "github.com/google/safearchive/zip"
)
func main(){
 var t bytes.Buffer;tw:=at.NewWriter(&t)
 if err:=tw.WriteHeader(&at.Header{Name:"file",Mode:0640,Size:4,Uid:1201,Gid:1202,Uname:"private-user",Gname:"private-group",Format:at.FormatPAX});err!=nil{panic(err)}
 tw.Write([]byte("data"));tw.Close()
 tr:=st.NewReader(bytes.NewReader(t.Bytes()));tr.SetSecurityMode(st.MaximumSecurityMode)
 h,err:=tr.Next();if err!=nil{panic(err)}
 p,_:=io.ReadAll(tr);if h.Uid!=1201 || h.Uname!="private-user" || string(p)!="data" {panic("unexpected tar state")}
 var z bytes.Buffer;zw:=az.NewWriter(&z)
 extra:=[]byte{0x75,0x78,7,0,1,2,0xb1,4,2,0xb2,4}
 f,err:=zw.CreateHeader(&az.FileHeader{Name:"file",Method:az.Store,Extra:extra});if err!=nil{panic(err)}
 f.Write([]byte("data"));zw.Close()
 zr,err:=sz.NewReader(bytes.NewReader(z.Bytes()),int64(z.Len()));if err!=nil{panic(err)}
 zr.SetSecurityMode(sz.MaximumSecurityMode)
 if !bytes.Contains(zr.File[0].Extra,extra){panic("unexpected zip state")}
 r,err:=zr.File[0].Open();if err!=nil{panic(err)};defer r.Close()
 p,_=io.ReadAll(r);if string(p)!="data"{panic("zip data changed")}
 fmt.Println("baseline OK: MaximumSecurityMode retains tar owner and ZIP Unix identity extra; both payloads readable")
}
