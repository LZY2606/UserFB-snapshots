package agouti_test

import (
 "encoding/json"
 "net/http"
 "net/http/httptest"
 "reflect"
 "strings"
 "testing"
 "github.com/sclevine/agouti"
)

func TestFormLegacyBaseline(t *testing.T) {
 calls:=[]string{};checked:=false
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  w.Header().Set("Content-Type","application/json")
  path:=strings.TrimPrefix(r.URL.Path,"/session/forms/")
  value:=interface{}(nil)
  switch path {
  case "elements":
   var req struct{Value string};if err:=json.NewDecoder(r.Body).Decode(&req);err!=nil {t.Error(err)}
   id:="text";if req.Value=="#agree" {id="check"};if req.Value=="#country" {id="select"}
   value=[]map[string]string{{"ELEMENT":id}}
  case "element/text/clear":calls=append(calls,"clear:text")
  case "element/text/value":
   var req struct{Value []string};if err:=json.NewDecoder(r.Body).Decode(&req);err!=nil {t.Error(err)}
   calls=append(calls,"value:"+strings.Join(req.Value,""))
  case "element/check/attribute/type":value="checkbox"
  case "element/check/selected":value=checked
  case "element/check/click":checked=true;calls=append(calls,"click:check")
  case "element/select/elements":value=[]map[string]string{{"ELEMENT":"option"}}
  case "element/option/click":calls=append(calls,"click:option")
  default:t.Errorf("unexpected %s",path)
  }
  json.NewEncoder(w).Encode(map[string]interface{}{"value":value})
 }));defer s.Close()
 p:=agouti.JoinPage(s.URL+"/session/forms")
 if err:=p.Find("#name").Fill("東京");err!=nil {t.Fatal(err)}
 if err:=p.Find("#agree").Check();err!=nil {t.Fatal(err)}
 if err:=p.Find("#agree").Check();err!=nil {t.Fatal(err)}
 if err:=p.Find("#country").Select("Japan");err!=nil {t.Fatal(err)}
 want:=[]string{"clear:text","value:東京","click:check","click:option"}
 if !reflect.DeepEqual(calls,want) {t.Fatalf("calls=%q want=%q",calls,want)}
 t.Log("FORM_LEGACY_BASELINE_OK")
}

func TestFormLegacyRemountWitness(t *testing.T) {
 oldValue:=false
 s:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  w.Header().Set("Content-Type","application/json")
  switch {
  case strings.HasSuffix(r.URL.Path,"/elements"):
   w.Write([]byte(`{"value":[{"ELEMENT":"old"}]}`))
  case strings.HasSuffix(r.URL.Path,"/clear"):
   w.Write([]byte(`{"value":null}`))
  case strings.HasSuffix(r.URL.Path,"/old/value"):
   oldValue=true;w.WriteHeader(404);w.Write([]byte(`{"value":{"message":"old control replaced"}}`))
  default:t.Errorf("unexpected %s",r.URL.Path)
  }
 }));defer s.Close()
 err:=agouti.JoinPage(s.URL+"/session/remount").Find("#name").Fill("new")
 if err==nil || !oldValue || !strings.Contains(err.Error(),"old control replaced") {t.Fatalf("err=%v oldValue=%v",err,oldValue)}
 t.Log("FORM_LEGACY_REMOUNT_WITNESS_OK old ID receives Value after Clear")
}
