package secure

import (
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

// errNonceGenBoom is a sentinel error returned by a failing nonce generator.
//
//nolint:gochecknoglobals
var errNonceGenBoom = errors.New("nonce generator boom")

// countingNonceSource hands out deterministic nonces and records every call.
type countingNonceSource struct {
	mu    sync.Mutex
	calls int
	err   error
}

func (c *countingNonceSource) next(_ *http.Request) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.calls++

	if c.err != nil {
		return "", c.err
	}

	return fmt.Sprintf("nonce-%d", c.calls), nil
}

func (c *countingNonceSource) callCount() int {
	c.mu.Lock()
	defer c.mu.Unlock()

	return c.calls
}

// fixedNonceSource always returns the same value or error, while counting calls.
type fixedNonceSource struct {
	value string
	err   error
	calls int
}

func (f *fixedNonceSource) next(_ *http.Request) (string, error) {
	f.calls++

	if f.err != nil {
		return "", f.err
	}

	return f.value, nil
}

// extractNonce pulls the nonce out of a CSP header value of the form
// "... 'nonce-VALUE' ...".
func extractNonce(t *testing.T, header string) string {
	t.Helper()

	marker := "'nonce-"

	start := strings.Index(header, marker)
	if start < 0 {
		t.Fatalf("header %q contains no nonce placeholder expansion", header)
	}

	start += len(marker)

	end := strings.Index(header[start:], "'")
	if end < 0 {
		t.Fatalf("header %q has an unterminated nonce value", header)
	}

	return header[start : start+end]
}

// TestNonceScope is a regression group for request-scoped CSP nonces:
// chained Secure middlewares, a custom NonceSource and the failure paths of
// every public entry point must all agree on a single nonce per request.
func TestNonceScope(t *testing.T) {
	const policy = "script-src $NONCE"

	t.Run("preset nonce wins and is shared by both layers", func(t *testing.T) {
		gen := &countingNonceSource{}
		outer := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})
		inner := New(Options{ContentSecurityPolicyReportOnly: policy, NonceSource: gen.next})

		bizRan := false
		handler := outer.Handler(inner.Handler(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			bizRan = true
			fmt.Fprint(w, CSPNonce(r.Context()))
		})))

		req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		req = req.WithContext(WithCSPNonce(req.Context(), "trusted-nonce-123"))
		res := httptest.NewRecorder()
		handler.ServeHTTP(res, req)

		expect(t, res.Code, http.StatusOK)
		expect(t, gen.callCount(), 0)
		expect(t, bizRan, true)
		expect(t, res.Body.String(), "trusted-nonce-123")
		expect(t, res.Header().Get(cspHeader), "script-src 'nonce-trusted-nonce-123'")
		expect(t, res.Header().Get(cspReportOnlyHeader), "script-src 'nonce-trusted-nonce-123'")
		// The source request keeps the value it was given and is otherwise untouched.
		expect(t, CSPNonce(req.Context()), "trusted-nonce-123")
		// The generated value must never be stored back on the shared Options.
		if !strings.Contains(outer.opt.ContentSecurityPolicy, "$NONCE") {
			t.Errorf("shared Options policy was rewritten: %q", outer.opt.ContentSecurityPolicy)
		}
	})

	t.Run("preset nonce bypasses generator validation", func(t *testing.T) {
		// WithCSPNonce is a trusted caller contract: its value is inherited
		// verbatim even if it would fail the generator output rules.
		gen := &countingNonceSource{}
		s := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})

		req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		req = req.WithContext(WithCSPNonce(req.Context(), "trust ed!"))
		res := httptest.NewRecorder()
		s.Handler(cspHandler).ServeHTTP(res, req)

		expect(t, res.Code, http.StatusOK)
		expect(t, gen.callCount(), 0)
		expect(t, res.Body.String(), "trust ed!")
		expect(t, res.Header().Get(cspHeader), "script-src 'nonce-trust ed!'")
	})

	t.Run("empty context nonce is treated as missing", func(t *testing.T) {
		gen := &countingNonceSource{}
		s := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})

		req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		req = req.WithContext(WithCSPNonce(req.Context(), ""))
		res := httptest.NewRecorder()
		s.Handler(cspHandler).ServeHTTP(res, req)

		expect(t, gen.callCount(), 1)
		expect(t, res.Body.String(), "nonce-1")
		expect(t, res.Header().Get(cspHeader), "script-src 'nonce-nonce-1'")
		// An empty preset value must not survive on the derived request either.
		expect(t, CSPNonce(req.Context()), "")
	})

	t.Run("single layer expands every placeholder", func(t *testing.T) {
		gen := &countingNonceSource{}
		const multi = "default-src 'self' $NONCE; script-src 'strict-dynamic' $NONCE"
		s := New(Options{ContentSecurityPolicy: multi, NonceSource: gen.next})

		req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		res := httptest.NewRecorder()
		s.Handler(cspHandler).ServeHTTP(res, req)

		header := res.Header().Get(cspHeader)
		expect(t, gen.callCount(), 1)
		expect(t, strings.Count(header, "'nonce-nonce-1'"), 2)
		expect(t, res.Body.String(), extractNonce(t, header))
		expect(t, CSPNonce(req.Context()), "")
	})

	t.Run("double layer generates once and shares the nonce", func(t *testing.T) {
		gen := &countingNonceSource{}
		outer := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})
		inner := New(Options{ContentSecurityPolicyReportOnly: policy, NonceSource: gen.next})

		handler := outer.Handler(inner.Handler(cspHandler))

		req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		res := httptest.NewRecorder()
		handler.ServeHTTP(res, req)

		expect(t, res.Code, http.StatusOK)
		expect(t, gen.callCount(), 1)

		bodyNonce := res.Body.String()
		if bodyNonce == "" {
			t.Fatal("business handler received no nonce")
		}

		expect(t, extractNonce(t, res.Header().Get(cspHeader)), bodyNonce)
		expect(t, extractNonce(t, res.Header().Get(cspReportOnlyHeader)), bodyNonce)
		expect(t, CSPNonce(req.Context()), "")
	})

	t.Run("request-only handlers defer header writes", func(t *testing.T) {
		gen := &countingNonceSource{}
		outer := New(Options{
			ContentSecurityPolicy: policy,
			NonceSource:           gen.next,
			SecureContextKey:      "outer-header",
		})
		inner := New(Options{
			ContentSecurityPolicyReportOnly: policy,
			NonceSource:                     gen.next,
			SecureContextKey:                "inner-header",
		})

		headersWrittenByMiddleware := false
		handler := outer.HandlerForRequestOnly(inner.HandlerForRequestOnly(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if w.Header().Get(cspHeader) != "" || w.Header().Get(cspReportOnlyHeader) != "" {
				headersWrittenByMiddleware = true
			}

			outerHeaders, _ := r.Context().Value(outer.ctxSecureHeaderKey).(http.Header)
			innerHeaders, _ := r.Context().Value(inner.ctxSecureHeaderKey).(http.Header)
			addResponseHeaders(outerHeaders, w)
			addResponseHeaders(innerHeaders, w)
			fmt.Fprint(w, CSPNonce(r.Context()))
		})))

		req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		res := httptest.NewRecorder()
		handler.ServeHTTP(res, req)

		expect(t, headersWrittenByMiddleware, false)
		expect(t, gen.callCount(), 1)

		bodyNonce := res.Body.String()
		expect(t, extractNonce(t, res.Header().Get(cspHeader)), bodyNonce)
		expect(t, extractNonce(t, res.Header().Get(cspReportOnlyHeader)), bodyNonce)
		expect(t, CSPNonce(req.Context()), "")
	})

	t.Run("interleaved independent requests stay independent", func(t *testing.T) {
		gen := &countingNonceSource{}
		outer := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})
		inner := New(Options{ContentSecurityPolicyReportOnly: policy, NonceSource: gen.next})

		var resB *httptest.ResponseRecorder
		reqB := httptest.NewRequest(http.MethodGet, "https://example.test/b", nil)

		handler := outer.Handler(inner.Handler(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			// Halfway through request A, serve a complete independent request B.
			if CSPNonce(r.Context()) == "nonce-1" {
				resB = httptest.NewRecorder()
				outer.Handler(inner.Handler(cspHandler)).ServeHTTP(resB, reqB)
			}

			fmt.Fprint(w, CSPNonce(r.Context()))
		})))

		reqA := httptest.NewRequest(http.MethodGet, "https://example.test/a", nil)
		resA := httptest.NewRecorder()
		handler.ServeHTTP(resA, reqA)

		expect(t, gen.callCount(), 2)
		expect(t, resA.Body.String(), "nonce-1")
		expect(t, resB.Body.String(), "nonce-2")
		expect(t, extractNonce(t, resA.Header().Get(cspHeader)), "nonce-1")
		expect(t, extractNonce(t, resA.Header().Get(cspReportOnlyHeader)), "nonce-1")
		expect(t, extractNonce(t, resB.Header().Get(cspHeader)), "nonce-2")
		expect(t, extractNonce(t, resB.Header().Get(cspReportOnlyHeader)), "nonce-2")
		expect(t, CSPNonce(reqA.Context()), "")
		expect(t, CSPNonce(reqB.Context()), "")
	})

	t.Run("static policy neither generates nor rewrites context", func(t *testing.T) {
		gen := &countingNonceSource{}
		s := New(Options{
			ContentSecurityPolicy:           "default-src 'self'",
			ContentSecurityPolicyReportOnly: "default-src 'self'",
			NonceSource:                     gen.next,
		})

		plainReq := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		plainRes := httptest.NewRecorder()
		s.Handler(cspHandler).ServeHTTP(plainRes, plainReq)

		expect(t, gen.callCount(), 0)
		expect(t, plainRes.Body.String(), "")
		expect(t, plainRes.Header().Get(cspHeader), "default-src 'self'")

		presetReq := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		presetReq = presetReq.WithContext(WithCSPNonce(presetReq.Context(), "keep"))
		presetRes := httptest.NewRecorder()
		s.Handler(cspHandler).ServeHTTP(presetRes, presetReq)

		expect(t, gen.callCount(), 0)
		expect(t, presetRes.Body.String(), "keep")
		expect(t, CSPNonce(presetReq.Context()), "keep")
	})

	t.Run("nil source keeps the built-in random generator", func(t *testing.T) {
		s := New(Options{ContentSecurityPolicy: policy})
		handler := s.Handler(cspHandler)

		nonces := make([]string, 0, 2)

		for i := 0; i < 2; i++ {
			req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
			res := httptest.NewRecorder()
			handler.ServeHTTP(res, req)

			nonce := res.Body.String()
			if nonce == "" {
				t.Fatal("built-in generator produced an empty nonce")
			}

			if _, err := base64.RawStdEncoding.DecodeString(nonce); err != nil {
				t.Fatalf("built-in nonce %q is not base64: %v", nonce, err)
			}

			expect(t, extractNonce(t, res.Header().Get(cspHeader)), nonce)
			nonces = append(nonces, nonce)
		}

		if nonces[0] == nonces[1] {
			t.Fatal("built-in generator reused a nonce across requests")
		}
	})

	t.Run("generator failure ends every entry point safely", func(t *testing.T) {
		invalidValues := []string{
			"",              // empty
			"ctrl\x00nonce", // control character
			"tab\tnonce",    // control character
			"has space",     // outside the allowed character set
			"semi;colon:",   // outside the allowed character set
			"quote'nonce\"", // outside the allowed character set
		}

		for _, bad := range invalidValues {
			bad := bad
			t.Run("invalid/"+fmt.Sprintf("%q", bad), func(t *testing.T) {
				gen := &fixedNonceSource{value: bad}
				s := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})
				s.SetBadRequestHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
					http.Error(w, "custom bad request", http.StatusBadRequest)
				}))

				// Handler family: the bad-request handler ends the request.
				bizRan := false
				res := httptest.NewRecorder()
				req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
				s.Handler(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
					bizRan = true
				})).ServeHTTP(res, req)

				expect(t, res.Code, http.StatusBadRequest)
				expect(t, strings.Contains(res.Body.String(), "custom bad request"), true)
				expect(t, bizRan, false)
				expect(t, res.Header().Get(cspHeader), "")
				expect(t, CSPNonce(req.Context()), "")

				// Process family: a recognizable nonce error is returned.
				header, newReq, err := s.ProcessNoModifyRequest(httptest.NewRecorder(),
					httptest.NewRequest(http.MethodGet, "https://example.test/", nil))
				if err == nil || !errors.Is(err, ErrInvalidNonce) {
					t.Fatalf("expected ErrInvalidNonce, got %v", err)
				}

				if header != nil {
					t.Errorf("expected nil header on failure, got %v", header)
				}

				if newReq != nil {
					t.Errorf("expected nil request on failure, got %v", newReq)
				}

				if bad != "" && strings.Contains(err.Error(), bad) {
					t.Errorf("error message leaks the rejected nonce value: %v", err)
				}

				processRes := httptest.NewRecorder()
				if err := s.Process(processRes, httptest.NewRequest(http.MethodGet, "https://example.test/", nil)); !errors.Is(err, ErrInvalidNonce) {
					t.Fatalf("Process error = %v, want ErrInvalidNonce", err)
				}

				expect(t, processRes.Header().Get(cspHeader), "")

				nonce, err := s.ProcessAndReturnNonce(httptest.NewRecorder(),
					httptest.NewRequest(http.MethodGet, "https://example.test/", nil))
				if !errors.Is(err, ErrInvalidNonce) {
					t.Fatalf("ProcessAndReturnNonce error = %v, want ErrInvalidNonce", err)
				}

				expect(t, nonce, "")
				// One generator call per entry point, none of them panic.
				expect(t, gen.calls, 4)
			})
		}

		t.Run("generator error is preserved", func(t *testing.T) {
			gen := &fixedNonceSource{err: errNonceGenBoom}
			s := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})

			bizRan := false
			res := httptest.NewRecorder()
			req := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
			s.Handler(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
				bizRan = true
			})).ServeHTTP(res, req)

			expect(t, res.Code, http.StatusBadRequest)
			expect(t, bizRan, false)
			expect(t, res.Header().Get(cspHeader), "")

			_, _, err := s.ProcessNoModifyRequest(httptest.NewRecorder(),
				httptest.NewRequest(http.MethodGet, "https://example.test/", nil))
			if !errors.Is(err, errNonceGenBoom) {
				t.Fatalf("expected errors.Is(err, errNonceGenBoom), got %v", err)
			}

			if errors.Is(err, ErrInvalidNonce) {
				t.Fatalf("generator error must not be mistaken for ErrInvalidNonce: %v", err)
			}

			nonce, err := s.ProcessAndReturnNonce(httptest.NewRecorder(),
				httptest.NewRequest(http.MethodGet, "https://example.test/", nil))
			if !errors.Is(err, errNonceGenBoom) {
				t.Fatalf("expected errors.Is(err, errNonceGenBoom), got %v", err)
			}

			expect(t, nonce, "")
			expect(t, gen.calls, 3)
		})
	})

	t.Run("process entry points share the request nonce", func(t *testing.T) {
		gen := &countingNonceSource{}
		s := New(Options{
			ContentSecurityPolicy:           policy,
			ContentSecurityPolicyReportOnly: policy,
			NonceSource:                     gen.next,
		})

		processRes := httptest.NewRecorder()
		processReq := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		if err := s.Process(processRes, processReq); err != nil {
			t.Fatalf("Process returned error: %v", err)
		}

		expect(t, extractNonce(t, processRes.Header().Get(cspHeader)), "nonce-1")
		expect(t, extractNonce(t, processRes.Header().Get(cspReportOnlyHeader)), "nonce-1")
		expect(t, CSPNonce(processReq.Context()), "")

		nonce, err := s.ProcessAndReturnNonce(httptest.NewRecorder(),
			httptest.NewRequest(http.MethodGet, "https://example.test/", nil))
		if err != nil {
			t.Fatalf("ProcessAndReturnNonce returned error: %v", err)
		}

		expect(t, nonce, "nonce-2")

		noModifyRes := httptest.NewRecorder()
		noModifyReq := httptest.NewRequest(http.MethodGet, "https://example.test/", nil)
		header, newReq, err := s.ProcessNoModifyRequest(noModifyRes, noModifyReq)
		if err != nil {
			t.Fatalf("ProcessNoModifyRequest returned error: %v", err)
		}

		expect(t, CSPNonce(newReq.Context()), "nonce-3")
		expect(t, extractNonce(t, header.Get(cspHeader)), "nonce-3")
		expect(t, noModifyRes.Header().Get(cspHeader), "")
		expect(t, CSPNonce(noModifyReq.Context()), "")
		expect(t, gen.callCount(), 3)
	})

	t.Run("host rejection and https redirect keep working", func(t *testing.T) {
		gen := &countingNonceSource{}

		hostSecure := New(Options{
			AllowedHosts:          []string{"allowed.example"},
			ContentSecurityPolicy: policy,
			NonceSource:           gen.next,
		})
		hostRes := httptest.NewRecorder()
		hostReq := httptest.NewRequest(http.MethodGet, "http://bad.example/", nil)
		hostSecure.Handler(cspHandler).ServeHTTP(hostRes, hostReq)

		expect(t, hostRes.Code, http.StatusInternalServerError)
		expect(t, hostRes.Body.String(), "Bad Host\n")
		expect(t, hostRes.Header().Get(cspHeader), "")

		sslSecure := New(Options{
			SSLRedirect:           true,
			ContentSecurityPolicy: policy,
			NonceSource:           gen.next,
		})
		sslRes := httptest.NewRecorder()
		sslReq := httptest.NewRequest(http.MethodGet, "http://example.test/", nil)
		sslSecure.Handler(cspHandler).ServeHTTP(sslRes, sslReq)

		expect(t, sslRes.Code, http.StatusMovedPermanently)
		expect(t, sslRes.Header().Get("Location"), "https://example.test/")
		expect(t, sslRes.Header().Get(cspHeader), "")
	})

	// Sanity check that a background context without any nonce value works too.
	t.Run("background context", func(t *testing.T) {
		gen := &countingNonceSource{}
		s := New(Options{ContentSecurityPolicy: policy, NonceSource: gen.next})

		req, err := http.NewRequestWithContext(context.Background(), http.MethodGet, "https://example.test/", nil)
		if err != nil {
			t.Fatalf("request build failed: %v", err)
		}

		res := httptest.NewRecorder()
		s.Handler(cspHandler).ServeHTTP(res, req)

		expect(t, res.Code, http.StatusOK)
		expect(t, gen.callCount(), 1)
		expect(t, res.Body.String(), "nonce-1")
	})
}
