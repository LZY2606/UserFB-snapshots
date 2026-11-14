package main

import (
 "fmt"
 "io"
 "net/http"
 "os"
 "strings"
 "time"
 sse "github.com/tmaxmax/go-sse"
)
type transport struct { calls int; seen []string; closed int }
type body struct { io.Reader; owner *transport }
func (b *body) Close() error { b.owner.closed++; return nil }
func (t *transport) RoundTrip(r *http.Request) (*http.Response,error) {
 t.calls++; t.seen=append(t.seen,r.Header.Get("Last-Event-ID"))
 status,text:=200,"id: alpha\ndata: one\n\nid: beta\ndata: two\n\n"
 if t.calls>1 { status=204; text="" }
 return &http.Response{StatusCode:status,Header:http.Header{"Content-Type":{"text/event-stream"}},Body:&body{strings.NewReader(text),t},Request:r},nil
}
func main(){
 tr:=&transport{}
 cl:=sse.Client{HTTPClient:&http.Client{Transport:tr},Backoff:sse.Backoff{InitialInterval:time.Nanosecond}}
 req,err:=http.NewRequest("GET","http://fixture.invalid/events",nil); if err!=nil {panic(err)}
 cn:=cl.NewConnection(req)
 var delivered []string
 cn.SubscribeToAll(func(e sse.Event){delivered=append(delivered,e.LastEventID+":"+e.Data)})
 err=cn.Connect()
 fmt.Printf("delivered=%q requests=%d retry_last_event_id=%q closed=%d\n",delivered,tr.calls,tr.seen[len(tr.seen)-1],tr.closed)
 if err==nil || len(delivered)!=2 || tr.calls!=2 || tr.seen[1]!="beta" || tr.closed!=2 {fmt.Fprintln(os.Stderr,"unexpected baseline observation",err); os.Exit(1)}
 fmt.Println("BASELINE_OK legacy cursor advances on callback delivery; explicit acknowledgement API absent")
}
