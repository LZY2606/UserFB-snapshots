package main
import (
 "context"
 "encoding/json"
 "fmt"
 "io"
 "net"
 "os"
 "sync"
 "time"
 "github.com/mafredri/cdp"
 "github.com/mafredri/cdp/protocol/runtime"
 "github.com/mafredri/cdp/protocol/target"
 "github.com/mafredri/cdp/rpcc"
 "github.com/mafredri/cdp/session"
)
func must(err error) {if err!=nil {fmt.Fprintln(os.Stderr,err);os.Exit(1)}}
func main(){
 ctx,cancel:=context.WithTimeout(context.Background(),5*time.Second);defer cancel()
 client,server:=net.Pipe(); var wg sync.WaitGroup;wg.Add(1)
 seen:=make(chan string,16)
 go func(){defer wg.Done();defer server.Close();d:=json.NewDecoder(server);e:=json.NewEncoder(server)
  for {var req struct{ID int `json:"id"`;Method string `json:"method"`;Params json.RawMessage `json:"params"`};if d.Decode(&req)!=nil{return};seen<-req.Method
   reply:=map[string]any{"id":req.ID,"result":map[string]any{}}
   switch req.Method {
   case "Target.attachToTarget": reply["result"]=map[string]any{"sessionId":"s1"}
   case "Target.sendMessageToTarget":
    var args struct{SessionID string `json:"sessionId"`;Message string `json:"message"`};if json.Unmarshal(req.Params,&args)!=nil{return}
    var inner struct{ID int `json:"id"`;Method string `json:"method"`};if json.Unmarshal([]byte(args.Message),&inner)!=nil{return}
    if args.SessionID!="s1" || inner.Method!="Runtime.evaluate" {return};seen<-inner.Method
    if e.Encode(reply)!=nil{return}
    message,_:=json.Marshal(map[string]any{"id":inner.ID,"result":map[string]any{"result":map[string]any{"type":"number","value":7}}})
    if e.Encode(map[string]any{"method":"Target.receivedMessageFromTarget","params":map[string]any{"sessionId":"s1","message":string(message)}})!=nil{return};continue
   }
   if e.Encode(reply)!=nil{return}
  }
 }()
 conn,err:=rpcc.DialContext(ctx,"",rpcc.WithDialer(func(context.Context,string)(io.ReadWriteCloser,error){return client,nil}));must(err)
 manager,err:=session.NewManager(cdp.NewClient(conn));must(err)
 sub,err:=manager.Dial(ctx,target.ID("page1"));must(err)
 result,err:=cdp.NewClient(sub).Runtime.Evaluate(ctx,runtime.NewEvaluateArgs("3+4"));must(err)
 if string(result.Result.Value)!="7" {must(fmt.Errorf("unexpected value %s",result.Result.Value))}
 must(manager.Close());must(conn.Close());wg.Wait();close(seen)
 counts:=map[string]int{};for m:=range seen{counts[m]++}
 if counts["Target.attachToTarget"]!=1 || counts["Runtime.evaluate"]!=1 || counts["Target.detachFromTarget"]!=1 || counts["Target.setAutoAttach"]!=0 {must(fmt.Errorf("unexpected calls: %v",counts))}
 fmt.Println("BASELINE Manager.Dial attach=1 evaluate=7 detach=1 autoAttach=0")
 fmt.Println("BASELINE real generated Runtime client over existing session tunnel; autoattach startup is absent")
}
