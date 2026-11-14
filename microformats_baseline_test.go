package opengraph

import (
	"fmt"
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"

	"golang.org/x/net/html"
)

const testMFPage = `<!doctype html><html><head>
<meta property="og:title" content="Legacy catalog">
<meta property="og:image" content="/cover.png">
<meta property="og:image:width" content="42">
<base href="../people/">
</head><body><article class="h-entry">
<a class="p-author u-author h-card" href="alice"><span class="p-name">A &amp; B</span><img src="a.png" alt="Portrait"></a>
<span class="p-name">Article</span></article></body></html>`

type testMFTransport struct {
	calls, closed int
	final         string
}

type testMFBody struct {
	io.Reader
	transport *testMFTransport
}

func (b *testMFBody) Close() error { b.transport.closed++; return nil }

func (tr *testMFTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	tr.calls++
	if req.URL.Path == "/start" {
		return &http.Response{StatusCode: 302, Header: http.Header{"Location": {"/archive/page"}}, Body: &testMFBody{strings.NewReader(""), tr}, Request: req}, nil
	}
	tr.final = req.URL.String()
	return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": {"text/html; charset=utf-8"}}, Body: &testMFBody{strings.NewReader(testMFPage), tr}, Request: req}, nil
}

// Only existing OpenGraph collection and the supplied HTML DOM are exercised.
// No Microformats properties, implied values, or future target tests are supplied.
func TestMicroformatsStartingPoint(t *testing.T) {
	tr := &testMFTransport{}
	og := New("https://catalog.invalid/start")
	og.Intent.HTTPClient = &http.Client{Transport: tr}
	if err := og.Fetch(); err != nil {
		t.Fatal(err)
	}
	if og.Title != "Legacy catalog" || len(og.Image) != 1 || og.Image[0].Width != 42 || tr.calls != 2 || tr.closed != 2 || tr.final != "https://catalog.invalid/archive/page" {
		t.Fatalf("existing Fetch path: title=%q images=%v calls=%d closed=%d final=%q", og.Title, og.Image, tr.calls, tr.closed, tr.final)
	}
	parsed := New("https://catalog.invalid/start")
	if err := parsed.Parse(strings.NewReader(testMFPage)); err != nil {
		t.Fatal(err)
	}
	doc, err := html.Parse(strings.NewReader(testMFPage))
	if err != nil {
		t.Fatal(err)
	}
	walked := New("https://catalog.invalid/start")
	if err := walked.Walk(doc); err != nil {
		t.Fatal(err)
	}
	if parsed.Title != og.Title || walked.Title != og.Title || !reflect.DeepEqual(parsed.Image, og.Image) || !reflect.DeepEqual(walked.Image, og.Image) {
		t.Fatal("public entry mismatch")
	}
	fmt.Println("EXISTING_FETCH_PARSE_WALK_OK")
	fmt.Println("REDIRECT_RESPONSE_AND_BODY_CLOSE_OK")
	var author *html.Node
	var visit func(*html.Node)
	visit = func(n *html.Node) {
		if n.Type == html.ElementNode {
			for _, a := range n.Attr {
				if a.Key == "class" && a.Val == "p-author u-author h-card" {
					author = n
				}
			}
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			visit(c)
		}
	}
	visit(doc)
	if author == nil || author.Data != "a" || author.Parent.Data != "article" || author.FirstChild.Data != "span" || author.FirstChild.FirstChild.Data != "A & B" {
		t.Fatal("HTML DOM fixture changed")
	}
	fmt.Println("HTML_NESTED_FIELD_INPUT_OK")
}
