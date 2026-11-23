package main
import (
 "encoding/json"
 "fmt"
 "io"
 "net"
 "os"
 "strconv"
 m "github.com/njasm/marionette_client"
)
func frame(c net.Conn, s string) { _,_ = fmt.Fprintf(c,"%d:%s",len(s),s) }
func receive(c net.Conn) ([]byte,error) {
 var n []byte; b:=make([]byte,1)
 for {if _,e:=io.ReadFull(c,b);e!=nil{return nil,e};if b[0]==':'{break};n=append(n,b[0])}
 size,e:=strconv.Atoi(string(n));if e!=nil{return nil,e};out:=make([]byte,size);_,e=io.ReadFull(c,out);return out,e
}
func probe(wrong bool) error {
 l,e:=net.Listen("tcp","127.0.0.1:0");if e!=nil{return e};defer l.Close()
 done:=make(chan error,1)
 go func(){c,e:=l.Accept();if e!=nil{done<-e;return};defer c.Close();frame(c,`{"applicationType":"gecko","marionetteProtocol":3}`)
 b,e:=receive(c);if e!=nil{done<-e;return};var a []json.RawMessage;if e=json.Unmarshal(b,&a);e!=nil{done<-e;return};var id int;_ = json.Unmarshal(a[1],&id);if wrong{id+=10};frame(c,fmt.Sprintf(`[1,%d,null,{"value":"order-entry"}]`,id));done<-nil}()
 t:=&m.MarionetteTransport{};a:=l.Addr().(*net.TCPAddr);if e=t.Connect("127.0.0.1",a.Port);e!=nil{return e}
 client:=m.NewClient();client.Transport(t);title,e:=client.Title();_ = t.Close();if se:=<-done;se!=nil{return se}
 if wrong {if e==nil{fmt.Printf("WRONG_ID_ACCEPTED title=%s\n",title)}else{fmt.Printf("WRONG_ID_REJECTED error=%v\n",e)};return nil}
 if e!=nil{return e};if title!="order-entry"{return fmt.Errorf("unexpected title %q",title)};fmt.Println("BASELINE_TITLE_OK order-entry");return nil
}
func main(){if e:=probe(false);e!=nil{fmt.Fprintln(os.Stderr,e);os.Exit(1)};if e:=probe(true);e!=nil{fmt.Fprintln(os.Stderr,e);os.Exit(1)}}
