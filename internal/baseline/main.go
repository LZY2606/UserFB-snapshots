package main

import (
	"fmt"
	secure "github.com/unrolled/secure"
	"net/http"
	"net/http/httptest"
	"strings"
)

func main() {
	outer := secure.New(secure.Options{ContentSecurityPolicy: "script-src $NONCE"})
	inner := secure.New(secure.Options{ContentSecurityPolicyReportOnly: "script-src $NONCE"})
	h := outer.Handler(inner.Handler(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, secure.CSPNonce(r.Context()))
	})))
	req := httptest.NewRequest("GET", "https://example.test/", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	body := rec.Body.String()
	enforced := rec.Header().Get("Content-Security-Policy")
	report := rec.Header().Get("Content-Security-Policy-Report-Only")
	if body == "" || !strings.Contains(report, body) || !strings.Contains(enforced, body) {
		panic("chained layers did not share one nonce")
	}
	if secure.CSPNonce(req.Context()) != "" {
		panic("source request mutated")
	}
	fmt.Println("baseline OK: enforced/report/body all share one nonce; original context empty")
}
