package main
import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "strings"
 secure "github.com/unrolled/secure"
)
func main() {
 outer:=secure.New(secure.Options{ContentSecurityPolicy:"script-src $NONCE"})
 inner:=secure.New(secure.Options{ContentSecurityPolicyReportOnly:"script-src $NONCE"})
 h:=outer.Handler(inner.Handler(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  fmt.Fprint(w,secure.CSPNonce(r.Context()))
 })))
 req:=httptest.NewRequest("GET","https://example.test/",nil)
 rec:=httptest.NewRecorder();h.ServeHTTP(rec,req)
 body:=rec.Body.String()
 enforced:=rec.Header().Get("Content-Security-Policy")
 report:=rec.Header().Get("Content-Security-Policy-Report-Only")
 if body=="" || !strings.Contains(report,body) || strings.Contains(enforced,body) { panic("unexpected starting state") }
 if secure.CSPNonce(req.Context())!="" {panic("source request mutated")}
 fmt.Println("baseline OK: report/body match; enforced/body mismatch reproduced; original context empty")
}
