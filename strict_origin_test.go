package nosurf

import (
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
)

const (
	strictTestTarget       = "https://app.invalid/action"
	strictTestExemptTarget = "https://app.invalid/exempt"
)

// strictTestRawToken is a fixed, unmasked token used to build fictional
// cookies and masked tokens for the strict origin tests.
var strictTestRawToken = []byte("0123456789abcdef0123456789abcdef")

func strictTestCookie() *http.Cookie {
	return &http.Cookie{Name: CookieName, Value: b64encode(strictTestRawToken)}
}

func strictTestMaskedToken() string {
	return b64encode(maskToken(strictTestRawToken))
}

type strictOriginHarness struct {
	handler      *CSRFHandler
	successCalls int
	failureCalls int
	allowCalls   int
	reason       error
}

// newStrictOriginHarness builds a handler whose success handler returns 204
// and whose failure handler returns 403 after recording the failure reason.
// If allow is non-nil, it is wrapped so the tests can observe whether the
// allow-origin callback was invoked.
func newStrictOriginHarness(strict bool, allow func(*url.URL) bool) *strictOriginHarness {
	h := &strictOriginHarness{}
	h.handler = New(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h.successCalls++
		w.WriteHeader(http.StatusNoContent)
	}))
	h.handler.SetFailureHandler(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h.failureCalls++
		h.reason = Reason(r)
		w.WriteHeader(http.StatusForbidden)
	}))
	h.handler.SetStrictOriginCheck(strict)
	if allow != nil {
		h.handler.SetIsAllowedOriginFunc(func(origin *url.URL) bool {
			h.allowCalls++
			return allow(origin)
		})
	}
	return h
}

func (h *strictOriginHarness) serve(req *http.Request) *httptest.ResponseRecorder {
	rec := httptest.NewRecorder()
	h.handler.ServeHTTP(rec, req)
	return rec
}

func (h *strictOriginHarness) assert(t *testing.T, rec *httptest.ResponseRecorder,
	wantStatus int, wantReason error, wantSuccess, wantFailure, wantAllow int) {
	t.Helper()

	if rec.Code != wantStatus {
		t.Errorf("status code: got %d, want %d", rec.Code, wantStatus)
	}
	if h.reason != wantReason {
		t.Errorf("failure reason: got %v, want %v", h.reason, wantReason)
	}
	if h.successCalls != wantSuccess {
		t.Errorf("business handler calls: got %d, want %d", h.successCalls, wantSuccess)
	}
	if h.failureCalls != wantFailure {
		t.Errorf("failure handler calls: got %d, want %d", h.failureCalls, wantFailure)
	}
	if h.allowCalls != wantAllow {
		t.Errorf("allow-origin callback calls: got %d, want %d", h.allowCalls, wantAllow)
	}
}

func addStrictTestToken(req *http.Request, maskedToken string) {
	req.AddCookie(strictTestCookie())
	req.Header.Set(HeaderName, maskedToken)
}

func TestStrictOriginExplicitOrigin(t *testing.T) {
	allowAll := func(*url.URL) bool { return true }

	tests := []struct {
		name         string
		origin       string
		secFetchSite string
		allow        func(*url.URL) bool
		wantStatus   int
		wantReason   error
		wantAllow    int
	}{
		{
			name:       "matching Origin passes without consulting the allow callback",
			origin:     "https://app.invalid",
			allow:      allowAll,
			wantStatus: http.StatusNoContent,
		},
		{
			name:       "surrounding HTTP OWS is allowed",
			origin:     " \thttps://app.invalid \t",
			wantStatus: http.StatusNoContent,
		},
		{
			name:       "null is rejected without consulting the allow callback",
			origin:     "null",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "empty value is rejected without consulting the allow callback",
			origin:     "",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "userinfo is rejected",
			origin:     "https://user:pass@app.invalid",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "path consisting of a lone slash is rejected",
			origin:     "https://app.invalid/",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "path is rejected",
			origin:     "https://app.invalid/action",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "query is rejected",
			origin:     "https://app.invalid?x=1",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "empty query is rejected",
			origin:     "https://app.invalid?",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "fragment is rejected",
			origin:     "https://app.invalid#frag",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "empty fragment is rejected",
			origin:     "https://app.invalid#",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "comma-separated origins are rejected",
			origin:     "https://app.invalid,https://other.invalid",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "space-separated origins are rejected",
			origin:     "https://app.invalid https://other.invalid",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "tab-separated origins are rejected",
			origin:     "https://app.invalid\thttps://other.invalid",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "non-HTTP(S) scheme is rejected",
			origin:     "ftp://app.invalid",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "missing host is rejected",
			origin:     "https://",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "missing scheme is rejected",
			origin:     "app.invalid",
			allow:      allowAll,
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:       "different Origin without callback is rejected",
			origin:     "https://other.invalid",
			wantStatus: http.StatusForbidden,
			wantReason: ErrBadOrigin,
		},
		{
			name:         "Sec-Fetch-Site same-origin does not override a disallowed Origin",
			origin:       "https://other.invalid",
			secFetchSite: "same-origin",
			wantStatus:   http.StatusForbidden,
			wantReason:   ErrBadOrigin,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			h := newStrictOriginHarness(true, test.allow)
			req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
			addStrictTestToken(req, strictTestMaskedToken())
			req.Header.Add("Origin", test.origin)
			if test.secFetchSite != "" {
				req.Header.Set("Sec-Fetch-Site", test.secFetchSite)
			}

			rec := h.serve(req)

			wantSuccess := 0
			wantFailure := 0
			if test.wantStatus == http.StatusNoContent {
				wantSuccess = 1
			} else {
				wantFailure = 1
			}
			h.assert(t, rec, test.wantStatus, test.wantReason, wantSuccess, wantFailure, test.wantAllow)
		})
	}
}

func TestStrictOriginCrossOriginDecision(t *testing.T) {
	t.Run("valid cross origin without a callback is rejected", func(t *testing.T) {
		h := newStrictOriginHarness(true, nil)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "https://other.invalid")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadOrigin, 0, 1, 0)
	})

	t.Run("allow callback denial is reported as bad origin", func(t *testing.T) {
		h := newStrictOriginHarness(true, func(*url.URL) bool { return false })
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "https://other.invalid")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadOrigin, 0, 1, 1)
	})

	t.Run("allow callback approval passes the origin check", func(t *testing.T) {
		var gotOrigin *url.URL
		h := newStrictOriginHarness(true, func(origin *url.URL) bool {
			gotOrigin = origin
			return true
		})
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "https://other.invalid")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 1)
		if gotOrigin == nil {
			t.Fatal("allow-origin callback did not receive the parsed Origin")
		}
		if gotOrigin.Scheme != "https" || gotOrigin.Host != "other.invalid" {
			t.Errorf("allow-origin callback received %q://%q, want https://other.invalid",
				gotOrigin.Scheme, gotOrigin.Host)
		}
	})

	t.Run("allow callback approval still applies with Sec-Fetch-Site same-origin", func(t *testing.T) {
		h := newStrictOriginHarness(true, func(*url.URL) bool { return true })
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "https://other.invalid")
		req.Header.Set("Sec-Fetch-Site", "same-origin")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 1)
	})
}

func TestStrictOriginTokenStillVerified(t *testing.T) {
	badToken := b64encode(make([]byte, tokenLength))

	t.Run("same-origin request with a bad token fails token verification", func(t *testing.T) {
		h := newStrictOriginHarness(true, nil)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		req.AddCookie(strictTestCookie())
		req.Header.Set(HeaderName, badToken)
		req.Header.Set("Origin", "https://app.invalid")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadToken, 0, 1, 0)
	})

	t.Run("allowed cross-origin request with a bad token fails token verification", func(t *testing.T) {
		h := newStrictOriginHarness(true, func(*url.URL) bool { return true })
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		req.AddCookie(strictTestCookie())
		req.Header.Set(HeaderName, badToken)
		req.Header.Set("Origin", "https://other.invalid")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadToken, 0, 1, 1)
	})
}

func TestStrictOriginMissingOriginFallbacks(t *testing.T) {
	t.Run("Sec-Fetch-Site same-origin fast path is retained", func(t *testing.T) {
		h := newStrictOriginHarness(true, func(*url.URL) bool {
			t.Error("allow-origin callback must not run for the Sec-Fetch-Site fast path")
			return false
		})
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Sec-Fetch-Site", "same-origin")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})

	t.Run("same-origin Referer fallback is retained", func(t *testing.T) {
		h := newStrictOriginHarness(true, func(*url.URL) bool {
			t.Error("allow-origin callback must not run for a same-origin Referer")
			return false
		})
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Referer", "https://app.invalid/page")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})

	t.Run("cross-origin Referer fallback is still rejected", func(t *testing.T) {
		h := newStrictOriginHarness(true, nil)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Referer", "https://other.invalid/page")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadReferer, 0, 1, 0)
	})
}

func TestStrictOriginHeaderPresenceAndDuplicates(t *testing.T) {
	allowAll := func(*url.URL) bool { return true }

	t.Run("differently-cased Origin field is detected", func(t *testing.T) {
		h := newStrictOriginHarness(true, nil)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header["origin"] = []string{"https://app.invalid"}

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})

	t.Run("null in a differently-cased field does not fall back to Referer", func(t *testing.T) {
		h := newStrictOriginHarness(true, allowAll)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header["origin"] = []string{"null"}
		req.Header.Set("Referer", "https://app.invalid/page")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadOrigin, 0, 1, 0)
	})

	t.Run("repeated values under one field name are rejected", func(t *testing.T) {
		h := newStrictOriginHarness(true, allowAll)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Add("Origin", "https://app.invalid")
		req.Header.Add("Origin", "https://app.invalid")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadOrigin, 0, 1, 0)
	})

	t.Run("duplicate field names differing only in casing are rejected", func(t *testing.T) {
		h := newStrictOriginHarness(true, allowAll)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header["Origin"] = []string{"https://app.invalid"}
		req.Header["origin"] = []string{"https://app.invalid"}

		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadOrigin, 0, 1, 0)
	})
}

func TestStrictOriginDisabledKeepsLegacyBehavior(t *testing.T) {
	t.Run("Sec-Fetch-Site same-origin still bypasses a cross-origin Origin", func(t *testing.T) {
		h := newStrictOriginHarness(false, nil)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "https://other.invalid")
		req.Header.Set("Sec-Fetch-Site", "same-origin")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})

	t.Run("Origin null still falls back to a same-origin Referer", func(t *testing.T) {
		h := newStrictOriginHarness(false, nil)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "null")
		req.Header.Set("Referer", "https://app.invalid/page")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})

	t.Run("path-bearing Origin still passes when only scheme and host are compared", func(t *testing.T) {
		h := newStrictOriginHarness(false, nil)
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "https://app.invalid/page")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})
}

func TestStrictOriginSafeAndExemptBypass(t *testing.T) {
	t.Run("safe methods bypass strict origin checking", func(t *testing.T) {
		h := newStrictOriginHarness(true, nil)
		req := httptest.NewRequest(http.MethodGet, strictTestTarget, nil)
		req.Header.Set("Origin", "null")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})

	t.Run("exempt paths bypass strict origin checking", func(t *testing.T) {
		h := newStrictOriginHarness(true, nil)
		h.handler.ExemptPath("/exempt")
		req := httptest.NewRequest(http.MethodPost, strictTestExemptTarget, nil)
		addStrictTestToken(req, strictTestMaskedToken())
		req.Header.Set("Origin", "null")

		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
	})
}

type readCountingBody struct {
	reader io.Reader
	reads  int
}

func (b *readCountingBody) Read(p []byte) (int, error) {
	b.reads++
	return b.reader.Read(p)
}

func (b *readCountingBody) Close() error {
	return nil
}

func TestStrictOriginDoesNotReadBodyOnRejection(t *testing.T) {
	form := url.Values{FormFieldName: []string{strictTestMaskedToken()}}

	t.Run("rejected origin leaves the request body unread", func(t *testing.T) {
		body := &readCountingBody{reader: strings.NewReader(form.Encode())}
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, body)
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		req.AddCookie(strictTestCookie())
		// No token header: the token would have to be parsed from the body.
		req.Header.Set("Origin", "null")
		// This weak signal must not rescue the rejected explicit Origin.
		req.Header.Set("Sec-Fetch-Site", "same-origin")

		h := newStrictOriginHarness(true, func(*url.URL) bool { return true })
		rec := h.serve(req)

		h.assert(t, rec, http.StatusForbidden, ErrBadOrigin, 0, 1, 0)
		if body.reads != 0 {
			t.Errorf("request body was read %d time(s) before the origin was rejected, want 0", body.reads)
		}
	})

	t.Run("token extraction from the body reads it (control)", func(t *testing.T) {
		body := &readCountingBody{reader: strings.NewReader(form.Encode())}
		req := httptest.NewRequest(http.MethodPost, strictTestTarget, body)
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		req.AddCookie(strictTestCookie())
		req.Header.Set("Origin", "https://app.invalid")

		h := newStrictOriginHarness(true, nil)
		rec := h.serve(req)

		h.assert(t, rec, http.StatusNoContent, nil, 1, 0, 0)
		if body.reads == 0 {
			t.Error("control request body was not read; the read counter is not exercising token extraction")
		}
	})
}
