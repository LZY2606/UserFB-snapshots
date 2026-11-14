package selenium_test

import (
 "encoding/json"
 "net/http"
 "net/http/httptest"
 "testing"
 "github.com/tebeka/selenium"
)

func TestExportClickBaseline(t *testing.T) {
 calls:=[]string{}
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  calls=append(calls,r.Method+" "+r.URL.Path)
  w.Header().Set("Content-Type","application/json")
  switch r.URL.Path {
  case "/session":
   var req map[string]interface{}
   if err:=json.NewDecoder(r.Body).Decode(&req);err!=nil { t.Error(err) }
   caps,_:=req["desiredCapabilities"].(map[string]interface{})
   if caps["se:downloadsEnabled"]!=true {t.Error("managed download capability lost")}
   w.Write([]byte(`{"value":{"sessionId":"exports","capabilities":{"browserName":"chrome"}}}`))
  case "/session/exports/element":
   w.Write([]byte(`{"value":{"element-6066-11e4-a52e-4f735466cecf":"export-button"}}`))
  case "/session/exports/element/export-button/click":
   w.Write([]byte(`{"value":null}`))
  default:
   t.Errorf("unexpected request %s",r.URL.Path);w.WriteHeader(500);w.Write([]byte(`{"value":{"error":"unknown command","message":"unexpected"}}`))
  }
 }));defer s.Close()
 wd,err:=selenium.NewRemote(selenium.Capabilities{"browserName":"chrome","se:downloadsEnabled":true},s.URL)
 if err!=nil {t.Fatal(err)}
 e,err:=wd.FindElement(selenium.ByCSSSelector,"#export");if err!=nil {t.Fatal(err)}
 if err=e.Click();err!=nil {t.Fatal(err)}
 if len(calls)!=3 || calls[2]!="POST /session/exports/element/export-button/click" {t.Fatal(calls)}
 t.Log("EXPORT_CLICK_BASELINE_OK")
}
