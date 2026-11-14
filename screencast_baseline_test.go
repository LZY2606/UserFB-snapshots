package godet

import (
 "encoding/json"
 "fmt"
 "net/http"
 "net/http/httptest"
 "strings"
 "sync"
 "testing"
 "time"
 "github.com/gorilla/websocket"
)

// Protocol fixture only: the driver still uses its real HTTP discovery and socket.
type screenPeer struct {
 server *httptest.Server
 socket *websocket.Conn
 writes sync.Mutex
 connected chan struct{}
 commands chan Params
}
func newScreenPeer(t *testing.T) *screenPeer {
 t.Helper()
 p:=&screenPeer{connected:make(chan struct{}),commands:make(chan Params,1024)}
 mux:=http.NewServeMux()
 mux.HandleFunc("/json/list",func(w http.ResponseWriter,r *http.Request){json.NewEncoder(w).Encode([]Params{{"id":"form","type":"page","webSocketDebuggerUrl":"ws"+strings.TrimPrefix(p.server.URL,"http")+"/ws"}})})
 mux.HandleFunc("/ws",func(w http.ResponseWriter,r *http.Request){
  c,e:=(&websocket.Upgrader{CheckOrigin:func(*http.Request)bool{return true}}).Upgrade(w,r,nil)
  if e!=nil{return};p.socket=c;close(p.connected)
  for {var m Params;if c.ReadJSON(&m)!=nil{return};p.commands<-m}
 })
 p.server=httptest.NewServer(mux)
 t.Cleanup(func(){select{case <-p.connected:p.socket.Close();default:};p.server.Close()})
 return p
}
func(p *screenPeer) write(t *testing.T,m Params){t.Helper();<-p.connected;p.writes.Lock();defer p.writes.Unlock();if e:=p.socket.WriteJSON(m);e!=nil{t.Fatal(e)}}
func(p *screenPeer) next(t *testing.T)Params{t.Helper();select{case m:=<-p.commands:return m;case <-time.After(5*time.Second):t.Fatal("peer: missing command");return nil}}
func(p *screenPeer) reply(t *testing.T,m Params,result Params){p.write(t,Params{"id":m["id"],"result":result})}
func(p *screenPeer) event(t *testing.T,method string,params Params){p.write(t,Params{"method":method,"params":params})}
func TestScreencastBaseline(t *testing.T){
 p:=newScreenPeer(t)
 d,e:=Connect(strings.TrimPrefix(p.server.URL,"http://"),false);if e!=nil{t.Fatal(e)};defer d.Close()
 seen:=make(chan Params,1);d.CallbackEvent("Page.screencastFrame",func(v Params){seen<-v})
 done:=make(chan error,1)
 go func(){_,e:=d.SendRequest("Page.startScreencast",Params{"format":"png"});done<-e}()
 m:=p.next(t);if m["method"]!="Page.startScreencast"{t.Fatal(m)};p.reply(t,m,Params{})
 if e=<-done;e!=nil{t.Fatal(e)}
 p.event(t,"Page.screencastFrame",Params{"sessionId":7,"data":"ZnJhbWU=","metadata":Params{"pageScaleFactor":1}})
 select{case f:=<-seen:if f.Int("sessionId")!=7{t.Fatal(f)};case <-time.After(5*time.Second):t.Fatal("no frame")}
 go func(){_,e:=d.SendRequest("Page.screencastFrameAck",Params{"sessionId":7});done<-e}()
 m=p.next(t);if m["method"]!="Page.screencastFrameAck"{t.Fatal(m)};p.reply(t,m,Params{});if e=<-done;e!=nil{t.Fatal(e)}
 go func(){_,e:=d.SendRequest("Page.stopScreencast",nil);done<-e}()
 m=p.next(t);if m["method"]!="Page.stopScreencast"{t.Fatal(m)};p.reply(t,m,Params{});if e=<-done;e!=nil{t.Fatal(e)}
 fmt.Println("BASELINE_RAW_SCREENCAST frame=7 explicit_ack=1")
}
