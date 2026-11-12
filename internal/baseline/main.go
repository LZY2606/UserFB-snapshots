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

 // default mode keeps the owner identity, the payload stays readable
 dtr:=st.NewReader(bytes.NewReader(t.Bytes()))
 dh,err:=dtr.Next();if err!=nil{panic(err)}
 dp,_:=io.ReadAll(dtr)
 if dh.Uid!=1201 || dh.Uname!="private-user" || string(dp)!="data" {panic("unexpected default tar state")}

 // maximum mode drops the owner identity (structured fields and PAX records), payload intact
 tr:=st.NewReader(bytes.NewReader(t.Bytes()));tr.SetSecurityMode(st.MaximumSecurityMode)
 h,err:=tr.Next();if err!=nil{panic(err)}
 p,_:=io.ReadAll(tr)
 if h.Uid!=0 || h.Gid!=0 || h.Uname!="" || h.Gname!="" || string(p)!="data" {panic("unexpected tar state")}
 for _,k:=range []string{"uid","gid","uname","gname"}{if _,ok:=h.PAXRecords[k];ok{panic("identity left in tar PAXRecords")}}

 var z bytes.Buffer;zw:=az.NewWriter(&z)
 extra:=[]byte{0x75,0x78,7,0,1,2,0xb1,4,2,0xb2,4}
 f,err:=zw.CreateHeader(&az.FileHeader{Name:"file",Method:az.Store,Extra:extra});if err!=nil{panic(err)}
 f.Write([]byte("data"));zw.Close()

 // default mode exposes the original extra bytes
 dzr,err:=sz.NewReader(bytes.NewReader(z.Bytes()),int64(z.Len()));if err!=nil{panic(err)}
 if !bytes.Equal(dzr.File[0].Extra,extra){panic("default zip extra changed")}

 // maximum mode strips the Unix identity extra, payload still served from the archived bytes
 zr,err:=sz.NewReader(bytes.NewReader(z.Bytes()),int64(z.Len()));if err!=nil{panic(err)}
 zr.SetSecurityMode(sz.MaximumSecurityMode)
 if bytes.Contains(zr.File[0].Extra,extra){panic("unexpected zip state")}
 if len(zr.File[0].Extra)!=0{panic("unix identity extra was not dropped")}
 r,err:=zr.File[0].Open();if err!=nil{panic(err)}
 p,_=io.ReadAll(r);if string(p)!="data"{panic("zip data changed")}
 fmt.Println("baseline OK: default mode keeps identity; MaximumSecurityMode drops tar owner and ZIP Unix identity extra; payloads readable")
}
