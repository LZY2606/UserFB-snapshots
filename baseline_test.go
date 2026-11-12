package selenium_test
import (
 "encoding/json"
 "net/http"
 "net/http/httptest"
 "testing"
 "github.com/tebeka/selenium"
)
func TestActionBaseline(t *testing.T) {
 calls:=0
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  w.Header().Set("Content-Type","application/json")
  if r.URL.Path=="/session" {w.Write([]byte(`{"value":{"sessionId":"rpa","capabilities":{"browserName":"chrome"}}}`));return}
  if r.URL.Path!="/session/rpa/actions" {t.Errorf("path %s",r.URL.Path)}
  var p struct{Actions []struct{ID string `json:"id"`;Actions []map[string]interface{} `json:"actions"`} `json:"actions"`}
  if err:=json.NewDecoder(r.Body).Decode(&p);err!=nil {t.Error(err)}
  if len(p.Actions)!=1 || p.Actions[0].ID!="kbd" || len(p.Actions[0].Actions)!=2 {t.Error("wrong actions")}
  calls++;w.Write([]byte(`{"value":null}`))
 }));defer s.Close()
 wd,err:=selenium.NewRemote(selenium.Capabilities{"browserName":"chrome"},s.URL);if err!=nil {t.Fatal(err)}
 wd.StoreKeyActions("kbd",selenium.KeyDownAction("a"),selenium.KeyUpAction("a"))
 if err:=wd.PerformActions();err!=nil {t.Fatal(err)}
 if calls!=1 {t.Fatal(calls)}
 t.Log("RPA_ACTION_BASELINE_OK")
}
