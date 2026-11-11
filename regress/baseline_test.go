package regress_test

import (
	"context"
	"fmt"
	g "github.com/shurcooL/graphql"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestIncrementalBaseline(t *testing.T) {
	var seen int
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen++
		if r.Method != "POST" {
			t.Error("not POST")
		}
		if r.URL.Path == "/plain" {
			w.Header().Set("Content-Type", "application/json")
			fmt.Fprint(w, `{"data":{"user":{"name":"Ada"}}}`)
			return
		}
		w.Header().Set("Content-Type", `multipart/mixed; boundary="parts"; deferSpec=20220824`)
		fmt.Fprint(w, "--parts\r\nContent-Type: application/json\r\n\r\n"+`{"data":{"user":{"name":"Ada"}},"hasNext":true}`+"\r\n--parts\r\nContent-Type: application/json\r\n\r\n"+`{"hasNext":false}`+"\r\n--parts--\r\n")
	}))
	defer srv.Close()
	var q struct{ User struct{ Name g.String } }
	if err := g.NewClient(srv.URL+"/plain", srv.Client()).Query(context.Background(), &q, nil); err != nil || q.User.Name != "Ada" {
		t.Fatalf("normal: %v %+v", err, q)
	}
	err := g.NewClient(srv.URL+"/stream", srv.Client()).Query(context.Background(), &q, nil)
	if err == nil || !strings.Contains(err.Error(), "invalid character") || seen != 2 {
		t.Fatalf("stream=%v seen=%d", err, seen)
	}
	t.Log("GRAPHQL_BASELINE_OK plain=Ada multipart_rejected_before_decode")
}
