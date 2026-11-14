package selenium_test
import (
 "encoding/json"
 "fmt"
 "net/http"
 "net/http/httptest"
 "testing"
 "github.com/tebeka/selenium"
)
func TestHTTPFormBaseline(t *testing.T) {
 var switchSeen bool
 var calls []string
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  calls=append(calls,r.Method+" "+r.URL.Path)
  w.Header().Set("Content-Type","application/json")
  switch r.Method+" "+r.URL.Path {
  case "POST /session":
   var body map[string]interface{}; if err:=json.NewDecoder(r.Body).Decode(&body);err!=nil {t.Error(err)}
   caps:=body["capabilities"].(map[string]interface{})["alwaysMatch"].(map[string]interface{})
   switchSeen=caps["webSocketUrl"]==true
   fmt.Fprint(w,`{"value":{"sessionId":"form","capabilities":{"browserVersion":"120.0","webSocketUrl":"ws://127.0.0.1:9/bidi"}}}`)
  case "POST /session/form/element": fmt.Fprint(w,`{"value":{"element-6066-11e4-a52e-4f735466cecf":"field"}}`)
  case "POST /session/form/element/field/value":
   var body map[string]interface{};json.NewDecoder(r.Body).Decode(&body)
   if body["text"]!="订单-42" {t.Errorf("wrong input: %v",body)}
   fmt.Fprint(w,`{"value":null}`)
  case "POST /session/form/execute/sync":fmt.Fprint(w,`{"value":{"submitted":true}}`)
  case "DELETE /session/form":fmt.Fprint(w,`{"value":null}`)
  default: t.Errorf("unexpected %s %s",r.Method,r.URL.Path);http.Error(w,`{"value":{"error":"unknown command"}}`,404)
  }
 }));defer s.Close()
 wd,err:=selenium.NewRemote(selenium.Capabilities{"browserName":"firefox","webSocketUrl":true},s.URL);if err!=nil {t.Fatal(err)}
 field,err:=wd.FindElement(selenium.ByCSSSelector,"input[name=order]");if err!=nil {t.Fatal(err)}
 if err=field.SendKeys("订单-42");err!=nil {t.Fatal(err)}
 result,err:=wd.ExecuteScript("return {submitted:true}",nil);if err!=nil {t.Fatal(err)}
 if result.(map[string]interface{})["submitted"]!=true {t.Fatal(result)}
 if err=wd.Quit();err!=nil {t.Fatal(err)}
 if len(calls)!=5 {t.Fatal(calls)}
 fmt.Printf("HTTP_FORM_OK calls=%d websocket_switch_sent=%v\n",len(calls),switchSeen)
}
