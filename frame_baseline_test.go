package chromedp

import (
 "context"
 "encoding/json"
 "fmt"
 "io"
 "sync"
 "testing"
 "time"
 "github.com/chromedp/cdproto"
 "github.com/chromedp/cdproto/cdp"
 "github.com/chromedp/cdproto/page"
 "github.com/chromedp/cdproto/runtime"
)

// Channel transport is an offline browser protocol fixture, not a target registry.
// Tests may consume commands and send events/replies in any controlled order.
type frameWire struct {
 incoming chan *cdproto.Message
 commands chan *cdproto.Message
 closed chan struct{}
 once sync.Once
}
func newFrameWire()*frameWire{return &frameWire{incoming:make(chan *cdproto.Message,1024),commands:make(chan *cdproto.Message,1024),closed:make(chan struct{})}}
func(w *frameWire)Read(ctx context.Context,m *cdproto.Message)error{select{case x:=<-w.incoming:*m=*x;return nil;case <-ctx.Done():return ctx.Err();case <-w.closed:return io.EOF}}
func(w *frameWire)Write(ctx context.Context,m *cdproto.Message)error{x:=*m;select{case w.commands<-&x:return nil;case <-ctx.Done():return ctx.Err();case <-w.closed:return io.EOF}}
func(w *frameWire)Close()error{w.once.Do(func(){close(w.closed)});return nil}
func frameJSON(x any)[]byte{b,e:=json.Marshal(x);if e!=nil{panic(e)};return b}
func frameReply(m *cdproto.Message,result any)*cdproto.Message{return &cdproto.Message{ID:m.ID,SessionID:m.SessionID,Result:frameJSON(result)}}
func TestFrameBaseline(t *testing.T){
 ctx,cancel:=context.WithTimeout(context.Background(),10*time.Second);defer cancel()
 w:=newFrameWire();defer w.Close()
 b,e:=NewBrowserTransport(ctx,w);if e!=nil{t.Fatal(e)}
 go func(){for{select{
 case <-ctx.Done():return
 case m:=<-w.commands:
  var result any=map[string]any{}
  switch string(m.Method){
  case "DOM.getDocument":result=map[string]any{"root":map[string]any{"nodeId":1,"backendNodeId":1,"nodeType":9,"nodeName":"#document","localName":"","nodeValue":"","children":[]any{
    map[string]any{"nodeId":2,"backendNodeId":2,"nodeType":1,"nodeName":"IFRAME","localName":"iframe","nodeValue":"","frameId":"remote-frame"},
    map[string]any{"nodeId":3,"backendNodeId":3,"nodeType":1,"nodeName":"INPUT","localName":"input","nodeValue":""},
   }}}
  case "DOM.querySelector":var p struct{NodeID int64 `json:"nodeId"`;Selector string `json:"selector"`};json.Unmarshal(m.Params,&p);id:=0;if p.NodeID==1&&p.Selector=="#normal"{id=3};result=map[string]any{"nodeId":id}
  }
  select{case w.incoming<-frameReply(m,result):case <-ctx.Done():return}
 }}}()
 target,e:=b.newTarget(ctx,"main-target","main-session");if e!=nil{t.Fatal(e)};go target.run(ctx)
 target.pageEvent(&page.EventFrameNavigated{Frame:&cdp.Frame{ID:"main-frame",URL:"https://form.example/"}})
 target.runtimeEvent(&runtime.EventExecutionContextCreated{Context:&runtime.ExecutionContextDescription{ID:1,UniqueID:"main-context",AuxData:frameJSON(map[string]any{"frameId":"main-frame","isDefault":true})}})
 target.documentUpdated(ctx)
 normal,e:=Nodes(CSS("#normal"))(ctx,target);if e!=nil||len(normal)!=1||normal[0].NodeID!=3{t.Fatalf("normal=%v err=%v",normal,e)}
 target.frameMu.RLock();f:=target.frames[target.cur];target.frameMu.RUnlock();f.RLock();iframe:=f.Nodes[2];f.RUnlock()
 scoped,e:=Nodes(CSS("#inside"),FromNode(iframe),AtLeast(0))(ctx,target);if e!=nil||len(scoped)!=0{t.Fatalf("scoped=%v err=%v",scoped,e)}
 fmt.Println("BASELINE_FRAME normal=3 oopif_from_node=0")
}
