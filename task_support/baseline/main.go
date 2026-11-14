package main
import (
 "bytes"
 "context"
 "fmt"
 "io"
 "os"
 "time"
 srt "github.com/datarhei/gosrt"
)
func run() error {
 cfg:=srt.DefaultConfig()
 cfg.ReceiverLatency=20*time.Millisecond
 cfg.PeerLatency=20*time.Millisecond
 cfg.ConnectionTimeout=2*time.Second
 ln,err:=srt.Listen("srt","127.0.0.1:0",cfg);if err!=nil{return err};defer ln.Close()
 type accepted struct {c srt.Conn;err error}
 ready:=make(chan accepted,1)
 go func(){r,e:=ln.Accept2();if e!=nil{ready<-accepted{err:e};return};c,e:=r.Accept();ready<-accepted{c,e}}()
 ctx,cancel:=context.WithTimeout(context.Background(),3*time.Second);defer cancel()
 client,err:=srt.DialWithContext(ctx,"srt",ln.Addr().String(),cfg);if err!=nil{return err};defer client.Close()
 var server srt.Conn
 select {case a:=<-ready:if a.err!=nil{return a.err};server=a.c;case <-ctx.Done():return ctx.Err()};defer server.Close()
 // Two small audio transport payloads exercise independent sender/receiver queues.
 a:=[]byte{0xd5,0x55,0x7f,0xff};b:=[]byte{0x11,0x22,0x33,0x44}
 transfer:=func(from,to srt.Conn,want []byte)error{
  if err:=to.SetReadDeadline(time.Now().Add(2*time.Second));err!=nil{return err}
  if n,e:=from.Write(want);e!=nil||n!=len(want){return fmt.Errorf("write %d: %v",n,e)}
  got:=make([]byte,len(want));if _,e:=io.ReadFull(to,got);e!=nil{return e};if !bytes.Equal(want,got){return fmt.Errorf("payload %x",got)};return nil
 }
 if err=transfer(client,server,a);err!=nil{return err};if err=transfer(server,client,b);err!=nil{return err}
 fmt.Printf("BASELINE caller_listener_bidirectional=%x/%x socket_ids_distinct=%t rendezvous=not_implemented\n",a,b,client.SocketId()!=server.SocketId())
 if client.SocketId()==server.SocketId(){return fmt.Errorf("socket identity collision")};return nil
}
func main(){if err:=run();err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}}
