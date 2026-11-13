package grequests

import (
 "context"
 "fmt"
 "net/http"
 "net/http/httptest"
 "testing"
)

func TestBaseline(t *testing.T) {
 hits:=map[string]int{}
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  hits[r.URL.Path]++
  if r.URL.Path=="/start" {http.Redirect(w,r,"/data",http.StatusFound);return}
  fmt.Fprint(w,"records=2")
 }))
 defer srv.Close()
 s:=NewSession(nil)
 first:=&RequestOptions{RedirectLimit:-1}
 a,err:=s.Get(context.Background(),srv.URL+"/start",first)
 if err!=nil || a.StatusCode!=302 {t.Fatalf("first response: %v %v",a,err)}
 a.Close()
 second:=&RequestOptions{RedirectLimit:4}
 b,err:=s.Get(context.Background(),srv.URL+"/start",second)
 if err!=nil {t.Fatal(err)}
 defer b.Close()
 if b.StatusCode!=302 || hits["/data"]!=0 {t.Fatalf("old sticky policy changed: status=%d hits=%v",b.StatusCode,hits)}
 t.Logf("observed sticky redirect policy: second_status=%d final_hits=%d",b.StatusCode,hits["/data"])
 direct,err:=s.Get(context.Background(),srv.URL+"/data",nil)
 if err!=nil || direct.String()!="records=2" {t.Fatalf("normal fetch: %v",err)}
 t.Log("normal production Session.Get body=records=2")
}
