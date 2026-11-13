package sling

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"reflect"
	"strings"
	"testing"
)

// batchItem is a JSON success target for batch tests.
type batchItem struct {
	ID int `json:"id"`
}

// batchFailure is a JSON failure target for batch tests.
type batchFailure struct {
	Error string `json:"error"`
}

// batchCloseBody is a response body counting Close calls.
type batchCloseBody struct {
	io.Reader
	closed *int
}

func (b batchCloseBody) Close() error { *b.closed++; return nil }

// batchDoer records the outgoing request and returns a canned response.
type batchDoer struct {
	calls    int
	lastReq  *http.Request
	lastBody []byte
	resp     *http.Response
	respFn   func() *http.Response
	err      error
}

func (d *batchDoer) Do(r *http.Request) (*http.Response, error) {
	d.calls++
	d.lastReq = r
	if r.Body != nil {
		d.lastBody, _ = io.ReadAll(r.Body)
	}
	if d.respFn != nil {
		return d.respFn(), d.err
	}
	return d.resp, d.err
}

// batchHTTPResponse builds an outer HTTP response with a close-counting body.
func batchHTTPResponse(status int, contentType, body string, closed *int) *http.Response {
	return &http.Response{
		StatusCode:    status,
		Status:        fmt.Sprintf("%d status", status),
		Proto:         "HTTP/1.1",
		ProtoMajor:    1,
		ProtoMinor:    1,
		Header:        http.Header{"Content-Type": []string{contentType}},
		Body:          batchCloseBody{strings.NewReader(body), closed},
		ContentLength: int64(len(body)),
	}
}

// batchMIMEPart formats one application/http MIME part carrying msg.
func batchMIMEPart(id, msg string) string {
	return "--b\r\nContent-Type: application/http\r\nContent-ID: <" + id + ">\r\n\r\n" + msg + "\r\n"
}

const batchBoundary = "multipart/mixed; boundary=b"

func TestBatchCollectionRequestShape(t *testing.T) {
	closed := 0
	msgA := "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 8\r\n\r\n{\"id\":1}"
	msgB := "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 8\r\n\r\n" // HEAD: no body follows
	msgC := "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n{\"id\"\r\n3\r\n:3}\r\n0\r\n\r\n"
	msgD := "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 14\r\n\r\n{\"error\":\"no\"}"
	// sub-responses intentionally out of order with mixed statuses
	body := batchMIMEPart("c", msgC) + batchMIMEPart("d", msgD) + batchMIMEPart("a", msgA) + batchMIMEPart("b", msgB) + "--b--\r\n"

	doer := &batchDoer{resp: batchHTTPResponse(200, batchBoundary, body, &closed)}
	sling := New().Doer(doer).
		Base("https://api.example.com/").Path("batch").
		Set("Authorization", "Bearer tok").
		QueryStruct(&struct {
			Verbose string `url:"verbose"`
		}{Verbose: "1"})

	reqA, _ := http.NewRequest("GET", "https://api.example.com/items/1?verbose=1", nil)
	reqA.Header.Set("X-Sub", "a")
	reqB, _ := http.NewRequest("HEAD", "https://api.example.com/items/2", nil)
	reqC, _ := http.NewRequest("GET", "https://api.example.com/items/3", nil)
	reqD, _ := http.NewRequest("GET", "https://api.example.com/items/4", nil)

	var a, b, c batchItem
	var dFailure batchFailure
	result, err := sling.ReceiveBatch([]BatchRequest{
		{ID: "a", Request: reqA, Success: &a},
		{ID: "b", Request: reqB, Success: &b},
		{ID: "c", Request: reqC, Success: &c},
		{ID: "d", Request: reqD, Failure: &dFailure},
	})
	if err != nil {
		t.Fatalf("ReceiveBatch error: %v", err)
	}
	if result == nil || result.Response == nil || result.Response.StatusCode != 200 {
		t.Fatalf("unexpected result: %+v", result)
	}

	// the outer request is a POST to the Sling URL with its query and headers
	if doer.calls != 1 {
		t.Fatalf("Doer calls = %d, want 1", doer.calls)
	}
	sent := doer.lastReq
	if sent.Method != "POST" {
		t.Errorf("outer method = %q, want POST", sent.Method)
	}
	if got := sent.URL.String(); got != "https://api.example.com/batch?verbose=1" {
		t.Errorf("outer URL = %q", got)
	}
	if got := sent.Header.Get("Authorization"); got != "Bearer tok" {
		t.Errorf("outer Authorization = %q", got)
	}
	mediaType, params, err := mime.ParseMediaType(sent.Header.Get("Content-Type"))
	if err != nil || mediaType != "multipart/mixed" || params["boundary"] == "" {
		t.Fatalf("outer Content-Type = %q", sent.Header.Get("Content-Type"))
	}

	// every MIME part is an application/http sub-request in origin-form
	reader := multipart.NewReader(bytes.NewReader(doer.lastBody), params["boundary"])
	sentParts := map[string]*http.Request{}
	for {
		part, err := reader.NextPart()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("parsing sent MIME: %v", err)
		}
		if ct := part.Header.Get("Content-Type"); ct != "application/http" {
			t.Errorf("part Content-Type = %q", ct)
		}
		id := strings.TrimSuffix(strings.TrimPrefix(part.Header.Get("Content-ID"), "<"), ">")
		sub, err := http.ReadRequest(bufio.NewReader(part))
		if err != nil {
			t.Fatalf("parsing sent sub-request: %v", err)
		}
		sentParts[id] = sub
	}
	if len(sentParts) != 4 {
		t.Fatalf("sent %d MIME parts, want 4", len(sentParts))
	}
	if got := sentParts["a"].Method + " " + sentParts["a"].URL.RequestURI(); got != "GET /items/1?verbose=1" {
		t.Errorf("sub-request a = %q", got)
	}
	if got := sentParts["a"].Header.Get("X-Sub"); got != "a" {
		t.Errorf("sub-request a X-Sub = %q", got)
	}
	if got := sentParts["b"].Method; got != "HEAD" {
		t.Errorf("sub-request b method = %q", got)
	}
	for id, sub := range sentParts {
		if got := sub.Header.Get("Authorization"); got != "" {
			t.Errorf("sub-request %s leaked outer Authorization %q", id, got)
		}
	}

	// results come back in input order, each in its own slot
	if len(result.Parts) != 4 {
		t.Fatalf("got %d parts, want 4", len(result.Parts))
	}
	for i, id := range []string{"a", "b", "c", "d"} {
		if result.Parts[i].ID != id {
			t.Fatalf("Parts[%d].ID = %q, want %q", i, result.Parts[i].ID, id)
		}
		if result.Parts[i].Err != nil {
			t.Errorf("Parts[%s].Err = %v", id, result.Parts[i].Err)
		}
	}
	if a.ID != 1 {
		t.Errorf("a = %+v, want id 1", a)
	}
	if b.ID != 0 {
		t.Errorf("HEAD part b decoded into %+v", b)
	}
	if c.ID != 3 {
		t.Errorf("chunked part c = %+v, want id 3", c)
	}
	if dFailure.Error != "no" {
		t.Errorf("d failure = %+v", dFailure)
	}
	// sub-responses expose status, headers, and their request
	if got := result.Parts[3].Response.StatusCode; got != 404 {
		t.Errorf("part d status = %d", got)
	}
	if got := result.Parts[0].Response.Header.Get("Content-Type"); got != "application/json" {
		t.Errorf("part a Content-Type = %q", got)
	}
	if result.Parts[0].Response.Request != reqA || result.Parts[1].Response.Request != reqB {
		t.Errorf("sub-response Request does not point at the input request")
	}
	if closed != 1 {
		t.Errorf("outer body closed %d times, want 1", closed)
	}
}

// countingBatchDecoder counts Decode calls and fails for selected part IDs.
type countingBatchDecoder struct {
	calls   int
	failIDs map[string]bool
}

var errBatchDecode = errors.New("batch decode boom")

func (d *countingBatchDecoder) Decode(resp *http.Response, v interface{}) error {
	d.calls++
	if d.failIDs[resp.Header.Get("X-Part-ID")] {
		return errBatchDecode
	}
	return json.NewDecoder(resp.Body).Decode(v)
}

func TestBatchCollectionDecodeErrors(t *testing.T) {
	msg := func(id, status, body string) string {
		return batchMIMEPart(id, "HTTP/1.1 "+status+"\r\nContent-Type: application/json\r\nX-Part-ID: "+id+"\r\nContent-Length: "+fmt.Sprint(len(body))+"\r\n\r\n"+body)
	}
	closed := 0
	body := msg("p1", "200 OK", `{"id":1}`) +
		msg("p2", "200 OK", `{"id":2}`) +
		msg("p3", "404 Not Found", `{"error":"no"}`) +
		batchMIMEPart("p4", "HTTP/1.1 204 No Content\r\n\r\n") +
		batchMIMEPart("p5", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") +
		msg("p6", "200 OK", `{"id":6}`) +
		"--b--\r\n"

	decoder := &countingBatchDecoder{failIDs: map[string]bool{"p2": true}}
	doer := &batchDoer{resp: batchHTTPResponse(200, batchBoundary, body, &closed)}
	sling := New().Doer(doer).ResponseDecoder(decoder).Base("https://api.example.com/batch")

	get := func(path string) *http.Request {
		r, _ := http.NewRequest("GET", "https://api.example.com/"+path, nil)
		return r
	}
	var p1, p2 batchItem
	var p3Failure batchFailure
	result, err := sling.ReceiveBatch([]BatchRequest{
		{ID: "p1", Request: get("i1"), Success: &p1},
		{ID: "p2", Request: get("i2"), Success: &p2},
		{ID: "p3", Request: get("i3"), Failure: &p3Failure},
		{ID: "p4", Request: get("i4"), Success: &batchItem{}},
		{ID: "p5", Request: get("i5"), Success: &batchItem{}},
		{ID: "p6", Request: get("i6")}, // nil targets skip decoding
	})
	if err != nil {
		t.Fatalf("ReceiveBatch error: %v", err)
	}
	if result == nil {
		t.Fatal("result is nil")
	}
	// only p1, p2, p3 are decoded: 204, Content-Length 0, and nil targets skip
	if decoder.calls != 3 {
		t.Errorf("decoder calls = %d, want 3", decoder.calls)
	}
	if p1.ID != 1 {
		t.Errorf("p1 = %+v", p1)
	}
	if !errors.Is(result.Parts[1].Err, errBatchDecode) {
		t.Errorf("p2 Err = %v, want errBatchDecode", result.Parts[1].Err)
	}
	if p3Failure.Error != "no" {
		t.Errorf("p3 failure = %+v", p3Failure)
	}
	for _, i := range []int{0, 2, 3, 4, 5} {
		if result.Parts[i].Err != nil {
			t.Errorf("Parts[%d].Err = %v", i, result.Parts[i].Err)
		}
	}
	if closed != 1 {
		t.Errorf("outer body closed %d times, want 1", closed)
	}
}

func TestBatchCollectionProtocolErrors(t *testing.T) {
	get := func(path string) *http.Request {
		r, _ := http.NewRequest("GET", "https://api.example.com/"+path, nil)
		return r
	}
	okMsg := batchMIMEPart("p1", "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n{\"id\":1}")

	cases := []struct {
		name   string
		status int
		ctype  string
		body   string
		parts  []BatchRequest
	}{
		{"outer status 500", 500, "application/json", `{"error":"down"}`, nil},
		{"outer not multipart", 200, "application/json", `{"id":1}`, nil},
		{"missing boundary", 200, "multipart/mixed", okMsg + "--b--\r\n", nil},
		{"missing closing boundary", 200, batchBoundary, okMsg, nil},
		{"missing part", 200, batchBoundary, okMsg + "--b--\r\n",
			[]BatchRequest{{ID: "p1", Request: get("i1")}, {ID: "p2", Request: get("i2")}}},
		{"duplicate part", 200, batchBoundary, okMsg + okMsg + "--b--\r\n", nil},
		{"unknown part id", 200, batchBoundary, batchMIMEPart("zzz", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") + okMsg + "--b--\r\n", nil},
		{"wrong part type", 200, batchBoundary, "--b\r\nContent-Type: text/plain\r\nContent-ID: <p1>\r\n\r\nhi\r\n--b--\r\n", nil},
		{"nested multipart part", 200, batchBoundary, "--b\r\nContent-Type: multipart/mixed; boundary=x\r\nContent-ID: <p1>\r\n\r\n--x--\r\n--b--\r\n", nil},
		{"truncated part body", 200, batchBoundary, batchMIMEPart("p1", "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n{\"i") + "--b--\r\n", nil},
		{"garbage http message", 200, batchBoundary, batchMIMEPart("p1", "not an http response") + "--b--\r\n", nil},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			parts := tc.parts
			if parts == nil {
				parts = []BatchRequest{{ID: "p1", Request: get("i1"), Success: &batchItem{}, Failure: &batchFailure{}}}
			}
			decoder := &countingBatchDecoder{}
			closed := 0
			doer := &batchDoer{resp: batchHTTPResponse(tc.status, tc.ctype, tc.body, &closed)}
			sling := New().Doer(doer).ResponseDecoder(decoder).Base("https://api.example.com/batch")
			result, err := sling.ReceiveBatch(parts)
			if err == nil {
				t.Errorf("expected error, got result %+v", result)
			}
			if result != nil {
				t.Errorf("result = %+v, want nil", result)
			}
			if decoder.calls != 0 {
				t.Errorf("decoder called %d times on a protocol error", decoder.calls)
			}
			for _, p := range parts {
				if p.Success != nil && *p.Success.(*batchItem) != (batchItem{}) {
					t.Errorf("success target modified: %+v", p.Success)
				}
			}
			if closed != 1 {
				t.Errorf("outer body closed %d times, want 1", closed)
			}
		})
	}

	// transport failure: no result, no decode attempt
	decoder := &countingBatchDecoder{}
	doer := &batchDoer{err: errors.New("connection refused")}
	sling := New().Doer(doer).ResponseDecoder(decoder).Base("https://api.example.com/batch")
	result, err := sling.ReceiveBatch([]BatchRequest{{ID: "p1", Request: get("i1"), Success: &batchItem{}}})
	if err == nil || result != nil {
		t.Errorf("transport failure: result=%+v err=%v", result, err)
	}
	if decoder.calls != 0 {
		t.Errorf("decoder called %d times on transport failure", decoder.calls)
	}
}

func TestBatchCollectionValidation(t *testing.T) {
	get := func(path string) *http.Request {
		r, _ := http.NewRequest("GET", "https://api.example.com/"+path, nil)
		return r
	}
	withBody, _ := http.NewRequest("GET", "https://api.example.com/i1", strings.NewReader("x"))
	validPart := func() BatchRequest {
		return BatchRequest{ID: "p1", Request: get("i1"), Success: &batchItem{}}
	}

	cases := []struct {
		name      string
		configure func(s *Sling) *Sling
		parts     []BatchRequest
	}{
		{"body provider", func(s *Sling) *Sling { return s.BodyJSON(&batchItem{}) }, []BatchRequest{validPart()}},
		{"no parts", nil, nil},
		{"empty id", nil, []BatchRequest{{ID: "", Request: get("i1")}}},
		{"id with space", nil, []BatchRequest{{ID: "a b", Request: get("i1")}}},
		{"id with slash", nil, []BatchRequest{{ID: "a/b", Request: get("i1")}}},
		{"id with bracket", nil, []BatchRequest{{ID: "<a>", Request: get("i1")}}},
		{"duplicate ids", nil, []BatchRequest{validPart(), validPart()}},
		{"nil request", nil, []BatchRequest{{ID: "p1"}}},
		{"post sub-request", nil, []BatchRequest{{ID: "p1", Request: func() *http.Request {
			r, _ := http.NewRequest("POST", "https://api.example.com/i1", nil)
			return r
		}()}}},
		{"sub-request with body", nil, []BatchRequest{{ID: "p1", Request: withBody}}},
		{"cross origin host", nil, []BatchRequest{{ID: "p1", Request: func() *http.Request {
			r, _ := http.NewRequest("GET", "https://other.example.com/i1", nil)
			return r
		}()}}},
		{"scheme mismatch", nil, []BatchRequest{{ID: "p1", Request: func() *http.Request {
			r, _ := http.NewRequest("GET", "http://api.example.com/i1", nil)
			return r
		}()}}},
		{"non http scheme", nil, []BatchRequest{{ID: "p1", Request: func() *http.Request {
			r, _ := http.NewRequest("GET", "ftp://api.example.com/i1", nil)
			return r
		}()}}},
		{"port mismatch", nil, []BatchRequest{{ID: "p1", Request: func() *http.Request {
			r, _ := http.NewRequest("GET", "https://api.example.com:8443/i1", nil)
			return r
		}()}}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			closed := 0
			doer := &batchDoer{resp: batchHTTPResponse(200, batchBoundary, "--b--\r\n", &closed)}
			s := New().Doer(doer).Base("https://api.example.com/batch")
			if tc.configure != nil {
				s = tc.configure(s)
			}
			result, err := s.ReceiveBatch(tc.parts)
			if err == nil {
				t.Errorf("expected validation error, got result %+v", result)
			}
			if result != nil {
				t.Errorf("result = %+v, want nil", result)
			}
			if doer.calls != 0 {
				t.Errorf("Doer called %d times despite invalid input", doer.calls)
			}
		})
	}

	// an explicit default port is the same origin
	closed := 0
	doer := &batchDoer{resp: batchHTTPResponse(200, batchBoundary,
		batchMIMEPart("p1", "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n{\"id\":1}")+"--b--\r\n", &closed)}
	r, _ := http.NewRequest("GET", "https://api.example.com:443/i1", nil)
	var item batchItem
	_, err := New().Doer(doer).Base("https://api.example.com/batch").ReceiveBatch([]BatchRequest{
		{ID: "p1", Request: r, Success: &item},
	})
	if err != nil || item.ID != 1 || doer.calls != 1 {
		t.Errorf("default port origin: err=%v item=%+v calls=%d", err, item, doer.calls)
	}
}

func TestBatchCollectionImmutabilityAndClosing(t *testing.T) {
	closed := 0
	doer := &batchDoer{respFn: func() *http.Response {
		return batchHTTPResponse(200, batchBoundary,
			batchMIMEPart("p1", "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n{\"id\":1}")+"--b--\r\n", &closed)
	}}
	sling := New().Doer(doer).
		Base("https://api.example.com/").Path("batch").
		Set("X-Outer", "1").
		QueryStruct(&struct {
			Verbose string `url:"verbose"`
		}{Verbose: "1"})
	rawBefore := sling.rawURL
	headerBefore := sling.header.Clone()

	req, _ := http.NewRequest("GET", "https://api.example.com/items/1?q=1", nil)
	req.Header.Set("X-Sub", "a")
	urlBefore := req.URL.String()
	subHeaderBefore := req.Header.Clone()

	// the same batch can be sent repeatedly
	for i := 0; i < 2; i++ {
		var item batchItem
		_, err := sling.ReceiveBatch([]BatchRequest{{ID: "p1", Request: req, Success: &item}})
		if err != nil || item.ID != 1 {
			t.Fatalf("call %d: err=%v item=%+v", i, err, item)
		}
	}
	if doer.calls != 2 {
		t.Errorf("Doer calls = %d, want 2", doer.calls)
	}
	if closed != 2 {
		t.Errorf("outer body closed %d times, want 2", closed)
	}

	// neither the Sling nor the sub-request was modified
	if sling.rawURL != rawBefore {
		t.Errorf("sling rawURL = %q, want %q", sling.rawURL, rawBefore)
	}
	if !reflect.DeepEqual(sling.header, headerBefore) {
		t.Errorf("sling header = %v, want %v", sling.header, headerBefore)
	}
	if got := req.URL.String(); got != urlBefore {
		t.Errorf("sub-request URL = %q, want %q", got, urlBefore)
	}
	if !reflect.DeepEqual(req.Header, subHeaderBefore) {
		t.Errorf("sub-request header = %v, want %v", req.Header, subHeaderBefore)
	}
	if got := doer.lastReq.URL.String(); got != "https://api.example.com/batch?verbose=1" {
		t.Errorf("outer URL = %q", got)
	}
}

// batchExampleDoer is a fake Doer returning a canned multipart/mixed
// batch response for the example.
type batchExampleDoer struct{}

func (batchExampleDoer) Do(req *http.Request) (*http.Response, error) {
	repo := `{"name":"sling","stars":1024}`
	issue := `{"message":"issue not found"}`
	body := "--b\r\n" +
		"Content-Type: application/http\r\nContent-ID: <repo>\r\n\r\n" +
		"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 29\r\n\r\n" + repo + "\r\n" +
		"--b\r\n" +
		"Content-Type: application/http\r\nContent-ID: <issue>\r\n\r\n" +
		"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 29\r\n\r\n" + issue + "\r\n" +
		"--b--\r\n"
	return &http.Response{
		StatusCode: 200,
		Status:     "200 OK",
		Header:     http.Header{"Content-Type": []string{"multipart/mixed; boundary=b"}},
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    req,
	}, nil
}

func ExampleSling_ReceiveBatch() {
	type Repo struct {
		Name  string `json:"name"`
		Stars int    `json:"stars"`
	}
	type RepoError struct {
		Message string `json:"message"`
	}

	repoReq, _ := http.NewRequest("GET", "https://api.example.com/repos/dghubble/sling", nil)
	issueReq, _ := http.NewRequest("GET", "https://api.example.com/repos/dghubble/sling/issues/9999", nil)

	repo, issue := new(Repo), new(Repo)
	repoErr, issueErr := new(RepoError), new(RepoError)

	result, err := New().Doer(batchExampleDoer{}).
		Base("https://api.example.com/").Path("batch").
		ReceiveBatch([]BatchRequest{
			{ID: "repo", Request: repoReq, Success: repo, Failure: repoErr},
			{ID: "issue", Request: issueReq, Success: issue, Failure: issueErr},
		})
	if err != nil {
		// protocol failure: transport error, non-2XX outer status, or
		// malformed multipart response; no target was modified
		panic(err)
	}
	for _, part := range result.Parts {
		fmt.Printf("%s: status=%d err=%v\n", part.ID, part.Response.StatusCode, part.Err)
	}
	// the 2XX part landed in Success, the 404 business failure in Failure
	fmt.Println(repo.Name, repo.Stars)
	fmt.Println(issueErr.Message)
	// Output:
	// repo: status=200 err=<nil>
	// issue: status=404 err=<nil>
	// sling 1024
	// issue not found
}
