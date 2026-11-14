package godet

import (
 "encoding/json"
 "fmt"
 "net/http"
 "net/http/httptest"
 "strings"
 "testing"
 "github.com/gorilla/websocket"
)

// The fake endpoint only acknowledges existing production commands. It never
// emits a lifecycle event, so this is a baseline of navigation acknowledgement.
func TestNavigationBaseline(t *testing.T) {
 var endpoint string
 seen := make(chan Params, 4)
 upgrader := websocket.Upgrader{}
 server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  if r.URL.Path == "/json/list" {
   _ = json.NewEncoder(w).Encode([]map[string]string{{"id":"page-1","type":"page","webSocketDebuggerUrl":endpoint}})
   return
  }
  ws,err:=upgrader.Upgrade(w,r,nil); if err!=nil{return};defer ws.Close()
  for {
   var command Params
   if ws.ReadJSON(&command)!=nil{return}
   seen<-command
   var result any
   switch command.String("method") {
   case "Page.navigate":result=Params{"frameId":"main","loaderId":"new-document"}
   case "Runtime.evaluate":result=Params{"result":Params{"type":"string","value":"ready"}}
   default:result=Params{}
   }
   if ws.WriteJSON(Params{"id":command["id"],"result":result})!=nil{return}
  }
 }))
 defer server.Close()
 endpoint="ws"+strings.TrimPrefix(server.URL,"http")+"/devtools/page/page-1"
 remote,err:=Connect(strings.TrimPrefix(server.URL,"http://"),false)
 if err!=nil{t.Fatal(err)}
 defer remote.Close()
 frame,err:=remote.NavigateTransition("https://forms.invalid/next",Reload)
 if err!=nil || frame!="main"{t.Fatalf("navigate=%q, %v",frame,err)}
 command:=<-seen
 if command.String("method")!="Page.navigate" || command.Map("params")["transitionType"]!="reload"{t.Fatalf("command=%v",command)}
 value,err:=remote.Evaluate("document.title")
 if err!=nil || value!="ready"{t.Fatalf("evaluate=%v, %v",value,err)}
 fmt.Println("NAV_BASELINE frame=main returned_before_lifecycle=true evaluate=ready transition=reload")
}
