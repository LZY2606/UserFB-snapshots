package agouti_test

import (
 "encoding/json"
 "fmt"
 "net/http"
 "net/http/httptest"
 "strings"
 "sync"
 "testing"

 "github.com/sclevine/agouti"
)

func TestFrameBaseline(t *testing.T) {
 var mu sync.Mutex
 current:="root"
 values:=map[string]string{"root":"root-text","outer":"outer-text"}
 commands:=[]string{}
 server:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  mu.Lock();defer mu.Unlock()
  endpoint:=strings.TrimPrefix(r.URL.Path,"/session/s/")
  commands=append(commands,r.Method+" "+endpoint+" @"+current)
  var value any
  switch {
  case r.Method=="POST" && endpoint=="elements":
   var query struct {Using,Value string};_ = json.NewDecoder(r.Body).Decode(&query)
   id:=current+"-value";if query.Value=="#outer" {id="outer-frame"}
   value=[]map[string]string{{"ELEMENT":id}}
  case endpoint=="frame":
   var body struct{ID any};_ = json.NewDecoder(r.Body).Decode(&body)
   if body.ID==nil {current="root"} else {current="outer"}
  case strings.HasSuffix(endpoint,"/text"):
   value=values[current]
  case strings.HasSuffix(endpoint,"/clear"):
   values[current]=""
  case strings.HasSuffix(endpoint,"/value"):
   var body struct{Value []string};_ = json.NewDecoder(r.Body).Decode(&body)
   values[current]=strings.Join(body.Value,"")
  default:
   w.WriteHeader(400);_ = json.NewEncoder(w).Encode(map[string]any{"value":map[string]string{"message":"unexpected "+endpoint}});return
  }
  _ = json.NewEncoder(w).Encode(map[string]any{"value":value})
 }))
 defer server.Close()
 page:=agouti.JoinPage(server.URL+"/session/s")
 oldSelection:=page.Find("#value")
 before,err:=oldSelection.Text();if err!=nil{t.Fatal(err)}
 if err:=page.Find("#outer").SwitchToFrame();err!=nil{t.Fatal(err)}
 shifted,err:=oldSelection.Text();if err!=nil{t.Fatal(err)}
 if err:=page.SwitchToRootFrame();err!=nil{t.Fatal(err)}
 restored,err:=oldSelection.Text();if err!=nil{t.Fatal(err)}
 if before!="root-text" || shifted!="outer-text" || restored!="root-text" {t.Fatalf("texts=%q,%q,%q",before,shifted,restored)}
 if err:=oldSelection.Fill("Alice");err!=nil{t.Fatal(err)}
 filled,err:=oldSelection.Text();if err!=nil || filled!="Alice" {t.Fatalf("fill=%q %v",filled,err)}
 mu.Lock();defer mu.Unlock()
 joined:=strings.Join(commands,"|")
 if !strings.Contains(joined,"POST element/root-value/clear @root|POST element/root-value/value @root") {t.Fatalf("commands=%s",joined)}
 fmt.Println("FRAME_BASELINE before=root-text shifted=outer-text restored=root-text filled=Alice")
}
