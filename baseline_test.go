//go:build baselinecheck

package sling

import (
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
)

type testBody struct {
	io.Reader
	closed *int
}

func (b testBody) Close() error { *b.closed++; return nil }

type testDoer struct {
	raw, kind     string
	calls, closed int
}

func (d *testDoer) Do(r *http.Request) (*http.Response, error) {
	d.calls++
	return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": []string{d.kind}}, Body: testBody{strings.NewReader(d.raw), &d.closed}, ContentLength: -1, Request: r}, nil
}
func TestBaseline(t *testing.T) {
	d := &testDoer{raw: `{"text":"ordinary"}`, kind: "application/json"}
	s := New().Doer(d).Get("https://api.invalid/item")
	var v struct{ Text string }
	res, e := s.ReceiveSuccess(&v)
	if e != nil || v.Text != "ordinary" || d.calls != 1 || d.closed != 1 {
		t.Fatal(v, e, d)
	}
	fmt.Printf("baseline status=%d text=%s\n", res.StatusCode, v.Text)
	d = &testDoer{raw: "--b\r\nContent-Type: application/http\r\nContent-ID: <x>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n{\"x\":1}\r\n--b--\r\n", kind: "multipart/mixed; boundary=b"}
	_, e = New().Doer(d).Get("https://api.invalid/batch").ReceiveSuccess(&v)
	if e == nil || d.calls != 1 || d.closed != 1 {
		t.Fatal(e, d)
	}
	fmt.Printf("baseline batch-rejected=%t calls=%d closed=%d error=%s\n", e != nil, d.calls, d.closed, e)
}
