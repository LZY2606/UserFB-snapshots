package agouti_test
import (
 "encoding/json"
 "fmt"
 "net/http"
 "net/http/httptest"
 "net/url"
 "strings"
 "testing"
 "github.com/sclevine/agouti"
)
func TestOriginalResetBaseline(t *testing.T) {
 current:="about:blank"
 dirty:=map[string]bool{}
 clears:=map[string]int{}
 writes:=0
 peer:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  w.Header().Set("Content-Type","application/json")
  origin:=func()string{u,_:=url.Parse(current);return u.Scheme+"://"+u.Host}
  value:=interface{}(nil)
  switch r.Method+" "+r.URL.Path {
  case "POST /session":fmt.Fprint(w,`{"sessionId":"flow","value":{}}`);return
  case "POST /session/flow/url":
   var b struct{URL string `json:"url"`};if err:=json.NewDecoder(r.Body).Decode(&b);err!=nil {t.Error(err)}
   current=b.URL;if current!="about:blank" {dirty[origin()]=true}
  case "GET /session/flow/url":value=current
  case "POST /session/flow/accept_alert":
  case "DELETE /session/flow/cookie":clears[origin()]++;dirty[origin()]=false
  case "DELETE /session/flow/local_storage","DELETE /session/flow/session_storage":clears[origin()]++
  case "POST /session/flow/elements":value=[]map[string]string{{"ELEMENT":"field"}}
  case "POST /session/flow/element":value=map[string]string{"ELEMENT":"field"}
  case "POST /session/flow/element/field/clear":
  case "POST /session/flow/element/field/value":
   var b map[string]interface{};json.NewDecoder(r.Body).Decode(&b)
   chars:=b["value"].([]interface{});var out strings.Builder;for _,c:=range chars{out.WriteString(c.(string))}
   if out.String()!="订单42" {t.Errorf("input=%s",out.String())};writes++
  case "DELETE /session/flow":
  default:t.Errorf("unexpected %s %s",r.Method,r.URL.Path);http.Error(w,`{"value":{"message":"unexpected endpoint"}}`,404);return
  }
  json.NewEncoder(w).Encode(map[string]interface{}{"value":value})
 }));defer peer.Close()
 page,err:=agouti.NewPage(peer.URL);if err!=nil{t.Fatal(err)}
 if err=page.Navigate("https://alpha.invalid/order");err!=nil{t.Fatal(err)}
 if err=page.Find("#order").Fill("订单42");err!=nil{t.Fatal(err)}
 if err=page.Navigate("https://beta.invalid/confirm");err!=nil{t.Fatal(err)}
 if err=page.Reset();err!=nil{t.Fatal(err)}
 if !dirty["https://alpha.invalid"]||dirty["https://beta.invalid"]||current!="about:blank"||writes!=1 {t.Fatal(dirty,current,writes)}
 if clears["https://alpha.invalid"]!=0||clears["https://beta.invalid"]!=3{t.Fatal(clears)}
 if err=page.Destroy();err!=nil{t.Fatal(err)}
 fmt.Printf("ORIGIN_RESET_BASELINE alpha_retained=%v beta_clears=%d form_writes=%d final=%s\n",dirty["https://alpha.invalid"],clears["https://beta.invalid"],writes,current)
}
