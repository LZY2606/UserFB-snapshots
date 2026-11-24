//go:build taskbaseline
// +build taskbaseline

package rtsp

import (
 "bufio"
 "bytes"
 "fmt"
 "io"
 "net"
 "strings"
 "testing"
 "time"
)

func TestBaseline(t *testing.T) {
 listener, err := net.Listen("tcp", "127.0.0.1:0")
 if err != nil { t.Fatal(err) }
 defer listener.Close()
 done := make(chan error, 1)
 block := []byte{'$',0,0,16,0x80,8,0,7,0,0,0,160,0,0,0,1,1,2,3,4}
 go func() {
  c, e := listener.Accept(); if e != nil {done<-e;return}; defer c.Close()
  c.SetDeadline(time.Now().Add(5*time.Second))
  r := bufio.NewReader(c)
  for i,method := range []string{"OPTIONS", "DESCRIBE"} {
   request := ""
   for { line,e := r.ReadString('\n'); if e != nil {done<-e;return};request += line; if line=="\r\n" {break} }
   if !strings.HasPrefix(request,method+" rtsp://") || !strings.Contains(request,fmt.Sprintf("CSeq: %d\r\n",i+1)) {done<-fmt.Errorf("unexpected request %q",request);return}
   body := ""
   if method == "DESCRIBE" { body = "v=0\r\nm=audio 0 RTP/AVP 8\r\na=rtpmap:8 PCMA/8000/1\r\na=control:track0\r\n" }
   reply := []byte(fmt.Sprintf("RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Length: %d\r\n\r\n%s",i+1,len(body),body))
   if method == "DESCRIBE" {reply = append(reply,block...)}
   if _,e=io.Copy(c, bytes.NewReader(reply));e!=nil {done<-e;return}
  }
  done<-nil
 }()
 client,err := DialTimeout("rtsp://"+listener.Addr().String()+"/live",time.Second)
 if err != nil {t.Fatal(err)}
 defer client.Close()
 client.RtspTimeout=time.Second
 if err=client.Options();err!=nil {t.Fatal(err)}
 if _,err=client.Describe();err!=nil {t.Fatal(err)}
 res,err := client.poll()
 if err!=nil {t.Fatal(err)}
 if !bytes.Equal(res.Block,block) {t.Fatalf("block %x",res.Block)}
 if err=<-done;err!=nil {t.Fatal(err)}
 fmt.Println("BASELINE_RTSP_CONTROL_AND_INTERLEAVED_OK status=200 channel=0 payload=01020304")
}
