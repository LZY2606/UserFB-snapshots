package sitemap

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"reflect"
	"strings"
	"testing"
)

type resumeBaselineTransport struct {
	calls, closed int
	source        []byte
}
type resumeBaselineBody struct {
	io.Reader
	owner *resumeBaselineTransport
}

func (b *resumeBaselineBody) Close() error { b.owner.closed++; return nil }
func (tr *resumeBaselineTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	tr.calls++
	body := string(tr.source)
	if req.URL.Path == "/index" {
		body = `<sitemapindex><sitemap><loc>https://capture.invalid/map.xml</loc></sitemap></sitemapindex>`
	}
	return &http.Response{StatusCode: 200, Header: http.Header{"Etag": {`"capture-v1"`}, "Content-Type": {"application/xml"}}, ContentLength: int64(len(body)), Body: &resumeBaselineBody{strings.NewReader(body), tr}, Request: req}, nil
}
func TestResumeStartingPoint(t *testing.T) {
	source, err := os.ReadFile("testdata/resume-source.xml")
	if err != nil {
		t.Fatal(err)
	}
	tr := &resumeBaselineTransport{source: source}
	old := http.DefaultClient
	http.DefaultClient = &http.Client{Transport: tr}
	defer func() { http.DefaultClient = old }()
	var got []string
	err = ParseFromSite("https://capture.invalid/map.xml", func(e Entry) error {
		got = append(got, e.GetLocation())
		if e.GetLastModified() == nil {
			t.Fatal("date lost")
		}
		return nil
	})
	want := []string{"https://capture.invalid/猫?x=1&y=2", "https://capture.invalid/b?q=一&x=2", "https://capture.invalid/final"}
	if err != nil || !reflect.DeepEqual(got, want) || tr.calls != 1 || tr.closed != 1 {
		t.Fatalf("existing HTTP parse got=%v err=%v calls=%d close=%d", got, err, tr.calls, tr.closed)
	}
	fmt.Println("EXISTING_HTTP_XML_ROWS_OK")
	var index []string
	err = ParseIndexFromSite("https://capture.invalid/index", func(e IndexEntry) error { index = append(index, e.GetLocation()); return nil })
	if err != nil || len(index) != 1 || index[0] != "https://capture.invalid/map.xml" || tr.closed != 2 {
		t.Fatal("existing index HTTP path", err, index, tr.closed)
	}
	fmt.Println("EXISTING_INDEX_HTTP_CLOSE_OK")
	stop := errors.New("consumer stopped")
	count := 0
	err = Parse(strings.NewReader(string(source)), func(e Entry) error { count++; return stop })
	if !errors.Is(err, stop) || count != 1 {
		t.Fatal("existing consumer stop", err, count)
	}
	fmt.Println("EXISTING_CONSUMER_STOP_OK")
}
