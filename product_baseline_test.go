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

const testProductBaselinePage = `<!doctype html><html><head>
<meta property="og:title" content="Legacy catalog"><meta property="og:image" content="/cover.png"><meta property="og:image:width" content="42">
<script type="application/ld+json">{"@graph":[{"@id":"#family","@type":"ProductGroup","variesBy":["color"],"color":"blue","hasVariant":{"@id":"#one"},"offers":{"@id":"#sale"}},{"@id":"#one","@type":"Product","sku":"ONE"},{"@id":"#sale","@type":"Offer","itemOffered":{"@id":"#one"},"price":19.90,"priceCurrency":"USD"}]}</script>
</head><body><p>Local product capture input.</p></body></html>`

type testProductBaselineTransport struct {
	calls, closed int
	final         string
}

type testProductBaselineBody struct {
	io.Reader
	transport *testProductBaselineTransport
}

func (b *testProductBaselineBody) Close() error { b.transport.closed++; return nil }

func (tr *testProductBaselineTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	tr.calls++
	if req.URL.Path == "/start" {
		return &http.Response{StatusCode: 302, Header: http.Header{"Location": {"/archive/page"}}, Body: &testProductBaselineBody{strings.NewReader(""), tr}, Request: req}, nil
	}
	tr.final = req.URL.String()
	return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": {"text/html; charset=utf-8"}}, Body: &testProductBaselineBody{strings.NewReader(testProductBaselinePage), tr}, Request: req}, nil
}

// Reused existing Fetch/Parse/Walk witness from an earlier scaffold at the identical upstream commit.
// Only the input fixture and private test names are adapted; no product extractor is supplied.
func TestProductStartingPoint(t *testing.T) {
	tr := &testProductBaselineTransport{}
	og := New("https://catalog.invalid/start")
	og.Intent.HTTPClient = &http.Client{Transport: tr}
	if err := og.Fetch(); err != nil {
		t.Fatal(err)
	}
	if og.Title != "Legacy catalog" || len(og.Image) != 1 || og.Image[0].Width != 42 || tr.calls != 2 || tr.closed != 2 || tr.final != "https://catalog.invalid/archive/page" {
		t.Fatalf("existing Fetch path: title=%q images=%v calls=%d closed=%d final=%q", og.Title, og.Image, tr.calls, tr.closed, tr.final)
	}
	parsed := New("https://catalog.invalid/start")
	if err := parsed.Parse(strings.NewReader(testProductBaselinePage)); err != nil {
		t.Fatal(err)
	}
	doc, err := html.Parse(strings.NewReader(testProductBaselinePage))
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

	var scripts int
	var scan func(*html.Node)
	scan = func(n *html.Node) {
		if n.Type == html.ElementNode && n.Data == "script" {
			scripts++
			if n.FirstChild == nil || !strings.Contains(n.FirstChild.Data, `"ProductGroup"`) {
				t.Fatal("script input lost")
			}
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			scan(c)
		}
	}
	scan(doc)
	if scripts != 1 {
		t.Fatal("fixture script count")
	}
	fmt.Println("PRODUCT_HTML_INPUT_OK")
}
