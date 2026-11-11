package requests_test

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"testing"

	"github.com/carlmjohnson/requests"
	"github.com/carlmjohnson/requests/internal/be"
)

// recordedHop is a single request seen by a redirectScript.
type recordedHop struct {
	url    string
	header http.Header
}

// redirectScript is an in-memory RoundTripper that records every request
// it sees and follows a scripted series of redirects.
// route returns the Location header to respond with for a request URL,
// or "" to respond 200 OK and end the chain.
type redirectScript struct {
	mu    sync.Mutex
	hops  []recordedHop
	route func(u *url.URL) string
}

func (rs *redirectScript) RoundTrip(req *http.Request) (*http.Response, error) {
	rs.mu.Lock()
	rs.hops = append(rs.hops, recordedHop{req.URL.String(), req.Header.Clone()})
	rs.mu.Unlock()

	res := &http.Response{
		StatusCode: http.StatusOK,
		Status:     "200 OK",
		Header:     make(http.Header),
		Body:       http.NoBody,
		Request:    req,
	}
	if loc := rs.route(req.URL); loc != "" {
		res.StatusCode = http.StatusFound
		res.Status = "302 Found"
		res.Header.Set("Location", loc)
	}
	return res, nil
}

func (rs *redirectScript) allHops() []recordedHop {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	return append([]recordedHop(nil), rs.hops...)
}

// headerValues returns the values of key for each recorded hop, in order.
func (rs *redirectScript) headerValues(key string) [][]string {
	hops := rs.allHops()
	values := make([][]string, len(hops))
	for i, h := range hops {
		values[i] = h.header.Values(key)
	}
	return values
}

func (rs *redirectScript) reset() {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	rs.hops = nil
}

type roundTripperFunc func(*http.Request) (*http.Response, error)

func (f roundTripperFunc) RoundTrip(req *http.Request) (*http.Response, error) {
	return f(req)
}

// scriptFor returns a redirectScript following the given path -> Location
// table. A path missing from the table responds 200 OK.
func scriptFor(table map[string]string) *redirectScript {
	return &redirectScript{route: func(u *url.URL) string {
		return table[u.Path]
	}}
}

func TestSensitiveRedirectSameOrigin(t *testing.T) {
	script := scriptFor(map[string]string{
		"/start": "/next", // relative redirect stays on the origin
		"/next":  "http://a.invalid/end",
	})
	err := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 3, len(got))
	for _, values := range got {
		be.AllEqual(t, []string{"fixture-token"}, values)
	}
}

func TestSensitiveRedirectCrossOriginChain(t *testing.T) {
	// Once the chain leaves the initial origin, the sensitive header
	// is not sent again, even when the chain returns to it.
	script := scriptFor(map[string]string{
		"/start":  "http://b.invalid/middle",
		"/middle": "http://a.invalid/end",
	})
	err := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 3, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, nil, got[1])
	be.AllEqual(t, nil, got[2])
}

func TestSensitiveRedirectCaseAndDefaultPort(t *testing.T) {
	script := scriptFor(map[string]string{
		"/start": "http://A.INVALID/next",     // DNS host case is ignored
		"/next":  "http://a.invalid:80/later", // explicit default port matches
		"/later": "http://a.invalid:8080/end", // different port: cross-origin
		"/end":   "http://a.invalid/back",     // still stripped after returning
	})
	err := requests.
		URL("http://a.invalid:80/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("x-SERVICE-token"). // declaration case does not matter
		Transport(script).
		Fetch(context.Background())
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 5, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, []string{"fixture-token"}, got[1])
	be.AllEqual(t, []string{"fixture-token"}, got[2])
	be.AllEqual(t, nil, got[3])
	be.AllEqual(t, nil, got[4])
}

func TestSensitiveRedirectHTTPSDowngrade(t *testing.T) {
	script := scriptFor(map[string]string{
		"/start": "https://a.invalid:443/next", // explicit default port matches
		"/next":  "http://a.invalid/end",       // downgrade: cross-origin
	})
	err := requests.
		URL("https://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 3, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, []string{"fixture-token"}, got[1])
	be.AllEqual(t, nil, got[2])
}

func TestSensitiveRedirectIPv6(t *testing.T) {
	script := scriptFor(map[string]string{
		// Same address value as [::1], different spelling.
		"/start": "http://[0:0:0:0:0:0:0:1]:8080/next",
		// Different address: cross-origin.
		"/next": "http://[::2]:8080/end",
	})
	err := requests.
		URL("http://[::1]:8080/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 3, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, []string{"fixture-token"}, got[1])
	be.AllEqual(t, nil, got[2])
}

func TestSensitiveRedirectTrailingDotAndSubdomain(t *testing.T) {
	script := scriptFor(map[string]string{
		"/start": "http://a.invalid./next", // trailing dot: cross-origin
	})
	err := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 2, len(got))
	be.AllEqual(t, nil, got[1])

	script = scriptFor(map[string]string{
		"/start": "http://sub.a.invalid/next", // subdomain: cross-origin
	})
	err = requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.NilErr(t, err)
	got = script.headerValues("X-Service-Token")
	be.Equal(t, 2, len(got))
	be.AllEqual(t, nil, got[1])
}

func TestSensitiveRedirectCheckRedirectError(t *testing.T) {
	stopErr := errors.New("stop following")
	cl := &http.Client{
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			return stopErr
		},
	}
	script := scriptFor(map[string]string{
		"/start": "http://a.invalid/next",
	})
	err := requests.
		URL("http://a.invalid/start").
		Client(cl).
		Transport(script).
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Fetch(context.Background())
	be.Nonzero(t, err)
	be.True(t, errors.Is(err, stopErr))
	// The existing policy ran and stopped the chain after one request.
	be.Equal(t, 1, len(script.allHops()))
}

func TestSensitiveRedirectUseLastResponse(t *testing.T) {
	cl := &http.Client{
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
	script := scriptFor(map[string]string{
		"/start": "http://b.invalid/next",
	})
	var code int
	err := requests.
		URL("http://a.invalid/start").
		Client(cl).
		Transport(script).
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		CheckStatus(http.StatusFound).
		Handle(func(res *http.Response) error {
			code = res.StatusCode
			return nil
		}).
		Fetch(context.Background())
	be.NilErr(t, err)
	be.Equal(t, http.StatusFound, code)
	be.Equal(t, 1, len(script.allHops()))
}

func TestSensitiveRedirectCallbackReAdd(t *testing.T) {
	// The callback observes the sanitized candidate request,
	// and cannot smuggle the sensitive header back onto the wire.
	var sawToken []bool
	cl := &http.Client{
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			sawToken = append(sawToken, req.Header.Get("X-Service-Token") != "")
			req.Header.Set("X-Service-Token", "fixture-token")
			return nil
		},
	}
	script := scriptFor(map[string]string{
		"/start":  "http://b.invalid/middle",
		"/middle": "http://b.invalid/end",
	})
	err := requests.
		URL("http://a.invalid/start").
		Client(cl).
		Transport(script).
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Fetch(context.Background())
	be.NilErr(t, err)
	be.AllEqual(t, []bool{false, false}, sawToken)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 3, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, nil, got[1])
	be.AllEqual(t, nil, got[2])
}

func TestSensitiveRedirectSharedClient(t *testing.T) {
	cl := &http.Client{}
	script := scriptFor(map[string]string{
		"/start": "http://b.invalid/next",
	})

	// A request that does not opt in keeps the standard behavior:
	// custom headers follow redirects across origins.
	err := requests.
		URL("http://a.invalid/start").
		Client(cl).
		Transport(script).
		Header("X-Service-Token", "fixture-token").
		Fetch(context.Background())
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 2, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, []string{"fixture-token"}, got[1])

	// The same shared client honors the policy for an opted-in request.
	script.reset()
	err = requests.
		URL("http://a.invalid/start").
		Client(cl).
		Transport(script).
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Fetch(context.Background())
	be.NilErr(t, err)
	got = script.headerValues("X-Service-Token")
	be.Equal(t, 2, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, nil, got[1])

	// The shared client itself was never modified.
	be.Zero(t, cl.CheckRedirect)
	be.Zero(t, cl.Transport)
}

func TestSensitiveRedirectClone(t *testing.T) {
	crossOrigin := func() *redirectScript {
		return scriptFor(map[string]string{
			"/start": "http://b.invalid/next",
		})
	}
	base := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		Header("X-Other-Secret", "fixture-other").
		SensitiveHeaders("X-Service-Token")
	clone := base.Clone().SensitiveHeaders("X-Other-Secret")

	// The base builder strips only its own declared header.
	s1 := crossOrigin()
	err := base.Transport(s1).Fetch(context.Background())
	be.NilErr(t, err)
	be.AllEqual(t, nil, s1.headerValues("X-Service-Token")[1])
	be.AllEqual(t, []string{"fixture-other"}, s1.headerValues("X-Other-Secret")[1])

	// The clone strips both headers, independently of the base.
	s2 := crossOrigin()
	err = clone.Transport(s2).Fetch(context.Background())
	be.NilErr(t, err)
	be.AllEqual(t, nil, s2.headerValues("X-Service-Token")[1])
	be.AllEqual(t, nil, s2.headerValues("X-Other-Secret")[1])
}

func TestSensitiveRedirectAccumulation(t *testing.T) {
	script := scriptFor(map[string]string{
		"/start": "http://b.invalid/next",
	})
	names := []string{"X-Token-A"}
	b := requests.
		URL("http://a.invalid/start").
		Header("X-Token-A", "fixture-a1", "fixture-a2").
		Header("X-Token-B", "fixture-b").
		Header("X-Normal-Header", "fixture-normal").
		SensitiveHeaders(names...).
		SensitiveHeaders("x-token-a", "X-Token-B"). // duplicates collapse
		SensitiveHeaders()                          // empty call is a no-op
	names[0] = "X-Not-Declared" // mutating the caller's slice has no effect

	err := b.Transport(script).Fetch(context.Background())
	be.NilErr(t, err)
	hops := script.allHops()
	be.Equal(t, 2, len(hops))
	// A multi-value header is sent in full on the initial origin.
	be.AllEqual(t, []string{"fixture-a1", "fixture-a2"}, hops[0].header.Values("X-Token-A"))
	// All declared headers are stripped cross-origin.
	be.AllEqual(t, nil, hops[1].header.Values("X-Token-A"))
	be.AllEqual(t, nil, hops[1].header.Values("X-Token-B"))
	// Ordinary headers still follow the standard redirect rules.
	be.AllEqual(t, []string{"fixture-normal"}, hops[1].header.Values("X-Normal-Header"))
}

func TestSensitiveRedirectDo(t *testing.T) {
	script := scriptFor(map[string]string{
		"/start": "http://b.invalid/next",
	})
	rb := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script)
	req, err := rb.Request(context.Background())
	be.NilErr(t, err)
	err = rb.Do(req)
	be.NilErr(t, err)
	got := script.headerValues("X-Service-Token")
	be.Equal(t, 2, len(got))
	be.AllEqual(t, []string{"fixture-token"}, got[0])
	be.AllEqual(t, nil, got[1])
	// The caller's request was not modified.
	be.AllEqual(t, []string{"fixture-token"}, req.Header.Values("X-Service-Token"))
}

func TestSensitiveRedirectBadRedirectTarget(t *testing.T) {
	// A redirect whose origin cannot be established fails before sending.
	script := scriptFor(map[string]string{
		"/start": "ftp://a.invalid/file",
	})
	err := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.Nonzero(t, err)
	be.True(t, errors.Is(err, requests.ErrTransport))
	be.Equal(t, 1, len(script.allHops()))
}

func TestSensitiveRedirectDefaultMaxRedirects(t *testing.T) {
	// With no CheckRedirect of its own, the standard library default
	// of at most 10 redirects still applies.
	script := scriptFor(map[string]string{
		"/loop": "/loop",
	})
	err := requests.
		URL("http://a.invalid/loop").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(script).
		Fetch(context.Background())
	be.Nonzero(t, err)
	be.Equal(t, 10, len(script.allHops()))
}

// barrier blocks n parties until all of them have arrived.
type barrier struct {
	arrived chan struct{}
	release chan struct{}
}

func newBarrier(n int) *barrier {
	b := &barrier{
		arrived: make(chan struct{}, n),
		release: make(chan struct{}),
	}
	go func() {
		for range n {
			<-b.arrived
		}
		close(b.release)
	}()
	return b
}

func (b *barrier) await() {
	b.arrived <- struct{}{}
	<-b.release
}

func TestSensitiveRedirectConcurrent(t *testing.T) {
	script := scriptFor(map[string]string{
		"/start":  "http://b.invalid/middle",
		"/middle": "http://a.invalid/end",
	})
	// Force the two chains to interleave:
	// neither initial request proceeds until both have arrived.
	gate := newBarrier(2)
	rt := roundTripperFunc(func(req *http.Request) (*http.Response, error) {
		if req.URL.Path == "/start" {
			gate.await()
		}
		return script.RoundTrip(req)
	})
	// One Builder, two concurrent Fetches.
	b := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(rt)

	var wg sync.WaitGroup
	errs := make(chan error, 2)
	for range 2 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			errs <- b.Fetch(context.Background())
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		be.NilErr(t, err)
	}
	// Each chain tracked its own "left the initial origin" state:
	// only the two initial requests carry the token.
	hops := script.allHops()
	be.Equal(t, 6, len(hops))
	for _, h := range hops {
		if strings.HasSuffix(h.url, "/start") {
			be.AllEqual(t, []string{"fixture-token"}, h.header.Values("X-Service-Token"))
		} else {
			be.AllEqual(t, nil, h.header.Values("X-Service-Token"))
		}
	}
}

func ExampleBuilder_SensitiveHeaders() {
	rt := roundTripperFunc(func(req *http.Request) (*http.Response, error) {
		fmt.Printf("%s token=%q\n", req.URL, req.Header.Get("X-Service-Token"))
		res := &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       http.NoBody,
			Request:    req,
		}
		if req.URL.Path == "/start" {
			res.StatusCode = http.StatusFound
			res.Header.Set("Location", "http://b.invalid/next")
		}
		return res, nil
	})
	err := requests.
		URL("http://a.invalid/start").
		Header("X-Service-Token", "fixture-token").
		SensitiveHeaders("X-Service-Token").
		Transport(rt).
		Fetch(context.Background())
	if err != nil {
		fmt.Println(err)
	}
	// Output:
	// http://a.invalid/start token="fixture-token"
	// http://b.invalid/next token=""
}
