package geziyor_test

import (
	"fmt"
	"github.com/geziyor/geziyor"
	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/export"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

type authorSink struct{ got []int }

func (s *authorSink) Export(ch chan interface{}) error {
	for x := range ch {
		s.got = append(s.got, x.(int))
	}
	return nil
}
func TestBaselineFanout(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "catalog") }))
	defer server.Close()
	a, b := &authorSink{}, &authorSink{}
	var body string
	g := geziyor.NewGeziyor(&geziyor.Options{StartURLs: []string{server.URL}, RobotsTxtDisabled: true, LogDisabled: true, Exporters: []export.Exporter{a, b}, ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
		body = string(r.Body)
		for _, x := range []int{3, 7, 11} {
			g.Exports <- x
		}
	}})
	g.Start()
	want := []int{3, 7, 11}
	if body != "catalog" || !reflect.DeepEqual(a.got, want) || !reflect.DeepEqual(b.got, want) {
		t.Fatalf("CONTENT fanout body=%q sink0=%v sink1=%v", body, a.got, b.got)
	}
	t.Log("BASE_FANOUT actual_http=1 callbacks=1 each_sink=[3 7 11]")
}
func TestBaselineFormats(t *testing.T) {
	d := t.TempDir()
	jp := filepath.Join(d, "rows.jsonl")
	cp := filepath.Join(d, "rows.csv")
	j := &export.JSONLine{FileName: jp}
	c := &export.CSV{FileName: cp}
	for _, e := range []export.Exporter{j, c} {
		ch := make(chan interface{}, 1)
		ch <- []string{"A", "B"}
		close(ch)
		if err := e.Export(ch); err != nil {
			t.Fatal(err)
		}
	}
	jb, _ := os.ReadFile(jp)
	cb, _ := os.ReadFile(cp)
	if string(jb) != "[\"A\",\"B\"]\n" || string(cb) != "A,B\n" {
		t.Fatalf("CONTENT formats json=%q csv=%q", jb, cb)
	}
	t.Log("BASE_FORMATS JSONLine and CSV bytes verified")
}
