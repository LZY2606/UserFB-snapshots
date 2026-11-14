package geziyor_test

import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "reflect"
 "sync/atomic"
 "testing"

 "github.com/geziyor/geziyor"
 "github.com/geziyor/geziyor/client"
)

func TestBaselinePlan(t *testing.T) {
 var hits, parsed int32
 srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){atomic.AddInt32(&hits,1);fmt.Fprint(w,"page:"+r.URL.Path)}))
 defer srv.Close()
 g := geziyor.NewGeziyor(&geziyor.Options{
  RobotsTxtDisabled:true, ParseHTMLDisabled:true, LogDisabled:true, RetryTimes:-1,
  StartRequestsFunc:func(g *geziyor.Geziyor){
   g.Get(srv.URL+"/index",func(g *geziyor.Geziyor,r *client.Response){
    if string(r.Body)!="page:/index" {t.Error("bad index")}
    atomic.AddInt32(&parsed,1)
    g.Get(srv.URL+"/detail",func(g *geziyor.Geziyor,r *client.Response){atomic.AddInt32(&parsed,1)})
    g.Get(srv.URL+"/index",nil)
   })
  },
 })
 g.Start()
 if hits!=2 || parsed!=2 {t.Fatalf("hits=%d parsed=%d",hits,parsed)}
 _,exists:=reflect.TypeOf(g).MethodByName("StartPlan")
 fmt.Printf("BASELINE_OK requests=%d duplicate=1\nPLAN_API=%t\n",hits,exists)
}
