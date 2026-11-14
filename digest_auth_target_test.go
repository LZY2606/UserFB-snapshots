package colly

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/gocolly/colly/v2/task_support/taskcheck"
)

// digestTestTransport is a deterministic in-memory transport. It records
// requests and lets a handler synthesize responses without any network or
// local listener, so the tests also run in restricted sandboxes.
type digestTestTransport struct {
	mu     sync.Mutex
	sends  []digestSend
	handle func(*digestTestTransport, *http.Request) (*http.Response, error)
}

type digestSend struct {
	url    string
	method string
	header http.Header
	body   []byte
	host   string
}

func (t *digestTestTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	var body []byte
	if r.Body != nil && r.Body != http.NoBody {
		b, err := io.ReadAll(r.Body)
		if err != nil {
			return nil, err
		}
		body = b
		if err := r.Body.Close(); err != nil {
			return nil, err
		}
	}
	t.mu.Lock()
	t.sends = append(t.sends, digestSend{
		url: r.URL.String(), method: r.Method, header: r.Header.Clone(), body: body, host: r.Host,
	})
	t.mu.Unlock()
	return t.handle(t, r)
}

func (t *digestTestTransport) snapshot() []digestSend {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]digestSend, len(t.sends))
	copy(out, t.sends)
	return out
}

// digestTestBody counts Close calls.
type digestTestBody struct {
	r      io.Reader
	closes *atomic.Int32
}

func (b *digestTestBody) Read(p []byte) (int, error) { return b.r.Read(p) }
func (b *digestTestBody) Close() error {
	b.closes.Add(1)
	return nil
}

func digestH(s string) string {
	sum := sha256.Sum256([]byte(s))
	return hex.EncodeToString(sum[:])
}

// digestTestParams parses a Digest Authorization header into a map and keeps
// the raw header for ordering/quoting checks.
func digestTestParams(t *testing.T, header string) (map[string]string, bool) {
	t.Helper()
	if !strings.HasPrefix(header, "Digest ") {
		return nil, false
	}
	out := map[string]string{}
	rest := strings.TrimPrefix(header, "Digest ")
	for len(rest) > 0 {
		rest = strings.TrimLeft(rest, " \t,")
		if rest == "" {
			break
		}
		eq := strings.IndexByte(rest, '=')
		if eq < 0 {
			break
		}
		name := strings.ToLower(strings.TrimSpace(rest[:eq]))
		rest = rest[eq+1:]
		var value string
		if strings.HasPrefix(rest, `"`) {
			var b strings.Builder
			i := 1
			for i < len(rest) {
				if rest[i] == '\\' && i+1 < len(rest) {
					b.WriteByte(rest[i+1])
					i += 2
					continue
				}
				if rest[i] == '"' {
					i++
					break
				}
				b.WriteByte(rest[i])
				i++
			}
			value = b.String()
			rest = rest[i:]
		} else {
			end := strings.IndexByte(rest, ',')
			if end < 0 {
				value = strings.TrimSpace(rest)
				rest = ""
			} else {
				value = strings.TrimSpace(rest[:end])
				rest = rest[end:]
			}
		}
		out[name] = value
	}
	return out, true
}

// digestTestChallenge builds a WWW-Authenticate challenge string.
func digestTestChallenge(realm, nonce, algorithm, qop string, extra ...string) string {
	parts := []string{
		`realm="` + realm + `"`,
		`nonce="` + nonce + `"`,
		`algorithm=` + algorithm,
		`qop="` + qop + `"`,
	}
	parts = append(parts, extra...)
	return "Digest " + strings.Join(parts, ", ")
}

// digestServerVerify recomputes the expected response server-side for the
// supported profile and returns the parsed parameters along with whether the
// header verified.
func digestServerVerify(t *testing.T, header, username, password, method, uri string, entity []byte, algIsSess bool) (map[string]string, bool) {
	t.Helper()
	params, ok := digestTestParams(t, header)
	if !ok {
		return nil, false
	}
	if params["username"] != username {
		return params, false
	}
	entityHash := digestH(string(entity))
	ha2 := digestH(method + ":" + uri + ":" + entityHash)
	if params["qop"] != "auth-int" {
		ha2 = digestH(method + ":" + uri)
	}
	ha1 := digestH(username + ":" + params["realm"] + ":" + password)
	if algIsSess || strings.EqualFold(params["algorithm"], "SHA-256-sess") {
		ha1 = digestH(ha1 + ":" + params["nonce"] + ":" + params["cnonce"])
	}
	expected := digestH(ha1 + ":" + params["nonce"] + ":" + params["nc"] + ":" + params["cnonce"] + ":" + params["qop"] + ":" + ha2)
	return params, params["response"] == expected
}

func TestDigestChallenge(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Origin validation.
		if _, err := normalizeDigestOrigin("http://example.com"); err != nil {
			c.Check(false, "plain origin rejected: %v", err)
		}
		if _, err := normalizeDigestOrigin("HTTP://Example.COM:80"); err != nil {
			c.Check(false, "case-insensitive default-port origin rejected: %v", err)
		}
		if _, err := normalizeDigestOrigin("http://[::1]:9000"); err != nil {
			c.Check(false, "IPv6 origin rejected: %v", err)
		}
		ipv6, err := normalizeDigestOrigin("http://[::1]:9000")
		c.Check(err == nil && ipv6 == "http://[::1]:9000", "IPv6 origin canonical form wrong: %q", ipv6)
		plain, err := normalizeDigestOrigin("HTTPS://Example.COM:443/")
		c.Check(err == nil && plain == "https://example.com:443", "origin canonicalization wrong: %q", plain)
		p80, err := normalizeDigestOrigin("http://example.com:80")
		pDef, err2 := normalizeDigestOrigin("http://example.com")
		c.Check(err == nil && err2 == nil && p80 == pDef, "explicit/implicit default port must match: %q vs %q", p80, pDef)
		for _, bad := range []string{
			"example.com",
			"ftp://example.com",
			"http://user:pass@example.com",
			"http://example.com/reports",
			"http://example.com?x=1",
			"http://example.com#frag",
			"",
		} {
			if _, err := normalizeDigestOrigin(bad); err == nil {
				c.Check(false, "origin %q should have been rejected", bad)
			}
		}

		// Challenge selection and parsing.
		headers := http.Header{
			"Www-Authenticate": []string{
				`Basic realm="x"`,
				`Digest realm="one, realm \"quoted\"", nonce="n1", algorithm=SHA-256, qop="auth, auth-int"`,
			},
		}
		ch := selectDigestChallenge(headers)
		if ch == nil {
			c.Check(false, "supported challenge not selected")
		} else {
			c.Check(ch.qop == "auth-int", "auth-int not preferred, got %q", ch.qop)
			c.Check(ch.nonce == "n1", "nonce not preserved: %q", ch.nonce)
			c.Check(ch.realm == `one, realm "quoted"`, "realm not unescaped: %q", ch.realm)
		}

		// Duplicate recognized parameter invalidates the challenge; a later
		// valid one is still selected.
		dup := http.Header{
			"Www-Authenticate": []string{
				`Digest realm="r", realm="r2", nonce="n", algorithm=SHA-256, qop="auth"`,
				`Digest realm="ok", nonce="n2", algorithm=SHA-256, qop="auth"`,
			},
		}
		if ch := selectDigestChallenge(dup); ch == nil {
			c.Check(false, "no fallback challenge after duplicate")
		} else {
			c.Check(ch.realm == "ok", "fallback realm wrong: %q", ch.realm)
		}

		// Unsupported variants fall through to the normal 401 path.
		for _, bad := range []string{
			`Digest realm="r", nonce="n", algorithm=MD5, qop="auth"`,
			`Digest realm="r", nonce="n", qop="auth"`,
			`Digest realm="r", nonce="", algorithm=SHA-256, qop="auth"`,
			`Digest realm="r", nonce="n", algorithm=SHA-256, qop="unknown"`,
			`Digest realm="r", nonce="n", algorithm=SHA-256, qop="auth", userhash=true`,
			`Digest realm="r", nonce="n", algorithm=SHA-256, qop="auth", charset=ISO-8859-1`,
			`Basic realm="r"`,
			`Bearer token`,
		} {
			if ch := selectDigestChallenge(http.Header{"Www-Authenticate": []string{bad}}); ch != nil {
				c.Check(false, "unsupported challenge %q was accepted", bad)
			}
		}

		// Mixed schemes inside a single field, picked in order.
		mixed := http.Header{
			"Www-Authenticate": []string{
				`Basic realm="basic", Digest realm="a", nonce="na", algorithm=SHA-256, qop="auth", Digest realm="b", nonce="nb", algorithm=SHA-256-sess, qop="auth-int"`,
			},
		}
		if ch := selectDigestChallenge(mixed); ch == nil {
			c.Check(false, "mixed field challenge not selected")
		} else {
			c.Check(ch.realm == "a", "first mixed challenge should win, got %q", ch.realm)
		}

		// ASCII credentials only.
		tmp := NewCollector()
		c.Check(tmp.SetDigestAuth("http://example.com", "üser", "pass") != nil, "non-ASCII username must be rejected")
		c.Check(tmp.SetDigestAuth("http://example.com", "user", "pässword") != nil, "non-ASCII password must be rejected")
		c.Check(tmp.SetDigestAuth("http://example.com", "", "") == nil, "empty ASCII credentials are allowed")

		// Header quoting for the Authorization value.
		c.Check(quoteDigest(`a"b\c`) == `"a\"b\\c"`, "quoted-string escaping wrong: %q", quoteDigest(`a"b\c`))

		// Backslash-escaped values survive parsing, and building an
		// Authorization round-trips them without corrupting quoting.
		esc := selectDigestChallenge(http.Header{"Www-Authenticate": {`Digest realm="r\\x", nonce="n\"y", algorithm=SHA-256, qop="auth", opaque="o\\p"`}})
		if esc == nil {
			c.Check(false, "escaped challenge not parsed")
		} else {
			c.Check(esc.realm == `r\x`, "escaped realm wrong: %q", esc.realm)
			c.Check(esc.nonce == `n"y`, "escaped nonce wrong: %q", esc.nonce)
			c.Check(esc.opaque == `o\p`, "escaped opaque wrong: %q", esc.opaque)
			sess := &digestSession{cnonce: `c"\\`, nc: 1}
			creds := digestCredentials{origin: "http://digest.test:80", username: `u"`, password: "p"}
			u, _ := url.Parse("http://digest.test/a%2Fb?q=1")
			hdr := buildDigestAuthorization(esc, creds, "GET", u, sess, "")
			got, ok := digestTestParams(t, hdr)
			c.Check(ok, "generated Authorization not parseable: %q", hdr)
			c.Check(got["username"] == `u"`, "username not round-tripped: %q", got["username"])
			c.Check(got["realm"] == `r\x`, "realm not round-tripped: %q", got["realm"])
			c.Check(got["nonce"] == `n"y`, "nonce not round-tripped: %q", got["nonce"])
			c.Check(got["opaque"] == `o\p`, "opaque not round-tripped: %q", got["opaque"])
			c.Check(got["cnonce"] == `c"\\`, "cnonce not round-tripped: %q", got["cnonce"])
			c.Check(got["uri"] == "/a%2Fb?q=1", "uri wrong: %q", got["uri"])
			c.Check(!strings.Contains(hdr, `algorithm="`), "algorithm must not be quoted: %q", hdr)
			c.Check(strings.Contains(hdr, "nc=00000001"), "nc must be unquoted 8 hex: %q", hdr)
			c.Check(strings.Contains(hdr, "qop=auth"), "qop must be unquoted: %q", hdr)
		}
		noOpaque := &digestChallenge{realm: "r", nonce: "n", algorithm: "SHA-256", qop: "auth"}
		u, _ := url.Parse("http://digest.test/x")
		hdr := buildDigestAuthorization(noOpaque, digestCredentials{username: "u", password: "p"}, "GET", u, &digestSession{cnonce: "c", nc: 1}, "")
		c.Check(!strings.Contains(hdr, "opaque="), "opaque must be omitted when absent: %q", hdr)

		// Parameter names and scheme names are case-insensitive; values are not.
		lower := selectDigestChallenge(http.Header{"Www-Authenticate": {`digest REALM="MiXeD", Nonce="NONCE", ALGORITHM=sha-256-SESS, QoP="AUTH-INT", STALE=TRUE`}})
		if lower == nil {
			c.Check(false, "case-insensitive challenge not parsed")
		} else {
			c.Check(lower.realm == "MiXeD", "realm value must keep case: %q", lower.realm)
			c.Check(lower.nonce == "NONCE", "nonce value must keep case: %q", lower.nonce)
			c.Check(lower.algorithm == "SHA-256-sess", "algorithm not normalized: %q", lower.algorithm)
			c.Check(lower.qop == "auth-int", "qop selection wrong: %q", lower.qop)
			c.Check(lower.stale, "stale=true not recognized case-insensitively")
		}
	})
}

// digestFixture wires a real Collector to the in-memory transport.
type digestFixture struct {
	t         *testing.T
	c         *Collector
	transport *digestTestTransport
}

func newDigestFixture(t *testing.T, handler func(*digestTestTransport, *http.Request) (*http.Response, error)) *digestFixture {
	t.Helper()
	tr := &digestTestTransport{handle: handler}
	c := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
	c.WithTransport(tr)
	return &digestFixture{t: t, c: c, transport: tr}
}

func digestResponse(req *http.Request, status int, headers http.Header, body []byte, closes *atomic.Int32) *http.Response {
	if headers == nil {
		headers = http.Header{}
	}
	if closes == nil {
		closes = &atomic.Int32{}
	}
	return &http.Response{
		StatusCode: status,
		Header:     headers,
		Body:       &digestTestBody{r: strings.NewReader(string(body)), closes: closes},
		Request:    req,
	}
}

func TestDigestReplay(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const realm = "reports"
		const user = "collector"
		const pass = "s3cret"
		const nonce = "nonce-a"
		entity := []byte{0, 'r', 'e', 'p', 'o', 'r', 't', '=', 0xff}
		var unauthorizedCloses atomic.Int32
		var successCloses atomic.Int32
		var challenges atomic.Int32

		fx := newDigestFixture(t, func(tr *digestTestTransport, r *http.Request) (*http.Response, error) {
			challenges.Add(1)
			if r.Header.Get("Authorization") == "" {
				return digestResponse(r, 401, http.Header{
					"Content-Type":     {"text/plain"},
					"Www-Authenticate": {digestTestChallenge(realm, nonce, "SHA-256", "auth-int", `opaque="opaque, with \"quote\""`)},
					"Set-Cookie":       {"session=from-401; Path=/"},
				}, []byte("challenge"), &unauthorizedCloses), nil
			}
			return digestResponse(r, 200, http.Header{"Content-Type": {"text/plain"}}, []byte("report-ready"), &successCloses), nil
		})
		if err := fx.c.SetDigestAuth("http://digest.test", user, pass); err != nil {
			c.Check(false, "SetDigestAuth failed: %v", err)
			return
		}

		var responses, headersSeen, errorsSeen, scraped atomic.Int32
		fx.c.OnResponseHeaders(func(r *Response) { headersSeen.Add(1) })
		fx.c.OnResponse(func(r *Response) {
			c.Check(string(r.Body) == "report-ready", "final body wrong: %q", string(r.Body))
			responses.Add(1)
		})
		fx.c.OnError(func(r *Response, err error) { errorsSeen.Add(1) })
		fx.c.OnScraped(func(r *Response) { scraped.Add(1) })

		err := fx.c.Request("POST", "http://digest.test/reports?month=11", bytesReader(entity), NewContext(), nil)
		c.Check(err == nil, "request returned error: %v", err)
		sends := fx.transport.snapshot()
		c.Check(len(sends) == 2, "expected exactly two sends, got %d", len(sends))
		c.Check(responses.Load() == 1 && scraped.Load() == 1, "final response/scraped not delivered once: %d/%d", responses.Load(), scraped.Load())
		c.Check(headersSeen.Load() == 1, "intermediate 401 leaked OnResponseHeaders: %d", headersSeen.Load())
		c.Check(errorsSeen.Load() == 0, "intermediate 401 leaked OnError: %d", errorsSeen.Load())
		c.Check(unauthorizedCloses.Load() == 1, "401 body not closed exactly once: %d", unauthorizedCloses.Load())
		c.Check(successCloses.Load() == 1, "final body not closed exactly once: %d", successCloses.Load())

		if len(sends) == 2 {
			authed := sends[1]
			params, ok := digestServerVerify(t, authed.header.Get("Authorization"), user, pass, "POST", "/reports?month=11", entity, false)
			c.Check(ok, "server-side digest verification failed")
			c.Check(params["uri"] == "/reports?month=11", "uri must be escaped path + raw query, got %q", params["uri"])
			c.Check(params["opaque"] == `opaque, with "quote"`, "opaque not echoed: %q", params["opaque"])
			c.Check(params["nc"] == "00000001", "first nc wrong: %q", params["nc"])
			c.Check(params["qop"] == "auth-int", "qop wrong: %q", params["qop"])
			c.Check(bytesEqual(authed.body, entity), "replayed entity differs: %v", authed.body)
			c.Check(sends[0].header.Get("Cookie") == "", "first send must not carry a caller cookie: %q", sends[0].header.Get("Cookie"))
			c.Check(strings.Contains(authed.header.Get("Cookie"), "session=from-401"), "retry must get the jar cookie for the current URL: %q", authed.header.Get("Cookie"))

			// The 401 Set-Cookie must land in the jar and be re-queried on
			// the retry (the jar supplies it on the actual request). Since the
			// transport sees the header the client wrote, verify via the jar.
			jarCookies := fx.c.Cookies("http://digest.test/reports")
			found := false
			for _, ck := range jarCookies {
				if ck.Name == "session" && ck.Value == "from-401" {
					found = true
				}
			}
			c.Check(found, "401 Set-Cookie missing from jar: %v", jarCookies)
		}

		// auth-int with an empty entity uses the empty-string digest.
		{
			var entityHashSeen string
			tr := &digestTestTransport{}
			tr.handle = func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, nonce, "SHA-256", "auth-int")}}, []byte("c"), nil), nil
				}
				params, ok := digestServerVerify(t, r.Header.Get("Authorization"), user, pass, "POST", "/empty", nil, false)
				if ok {
					entityHashSeen = digestH("")
				}
				_ = params
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			}
			empty := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			empty.WithTransport(tr)
			c.Check(empty.SetDigestAuth("http://digest.test", user, pass) == nil, "SetDigestAuth failed")
			c.Check(empty.PostRaw("http://digest.test/empty", nil) == nil, "empty auth-int POST failed")
			c.Check(entityHashSeen == digestH(""), "empty entity digest mismatch")
		}

		// A retry with the same nonce starts a new session only for new
		// nonces; a fresh request keeps counting from the same session.
		err = fx.c.Request("POST", "http://digest.test/reports?month=11", bytesReader(entity), NewContext(), nil)
		c.Check(err == nil, "second request failed: %v", err)
		sends = fx.transport.snapshot()
		if len(sends) == 4 {
			p2, ok := digestServerVerify(t, sends[3].header.Get("Authorization"), user, pass, "POST", "/reports?month=11", entity, false)
			c.Check(ok, "second request digest failed verification")
			c.Check(p2["nc"] == "00000002", "second request nc not incremented: %q", p2["nc"])
			p1, _ := digestTestParams(t, sends[1].header.Get("Authorization"))
			c.Check(p1["cnonce"] == p2["cnonce"], "same nonce must reuse cnonce")
		} else {
			c.Check(false, "second request produced %d sends, want 4", len(sends))
		}
	})
}

func bytesReader(b []byte) *bytes.Reader { return bytes.NewReader(b) }

func digestTestCountFiles(t *testing.T, dir string) int {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0
	}
	count := 0
	for _, e := range entries {
		if !e.IsDir() {
			count++
			continue
		}
		if sub, err := os.ReadDir(filepath.Join(dir, e.Name())); err == nil {
			count += len(sub)
		}
	}
	return count
}

func bytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func TestDigestSessions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- stale handling and the retry budget ---
		{
			const realm, user, pass = "reports", "collector", "s3cret"
			var unauthorizedCloses atomic.Int32
			var successCloses atomic.Int32
			var nonceSeq atomic.Int32
			fx := newDigestFixture(t, func(tr *digestTestTransport, r *http.Request) (*http.Response, error) {
				auth := r.Header.Get("Authorization")
				if auth == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, "nonce-1", "SHA-256", "auth", `stale=false`)}}, []byte("challenge"), &unauthorizedCloses), nil
				}
				params, _ := digestTestParams(t, auth)
				// First authenticated send gets a stale 401 with a *new*
				// nonce; the second succeeds.
				if nonceSeq.Add(1) == 1 {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, "nonce-2", "SHA-256", "auth", `stale=true`)}}, []byte("stale"), &unauthorizedCloses), nil
				}
				if params["nonce"] != "nonce-2" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, "nonce-3", "SHA-256", "auth", `stale=true`)}}, []byte("wrong"), &unauthorizedCloses), nil
				}
				return digestResponse(r, 200, nil, []byte("ok"), &successCloses), nil
			})
			if err := fx.c.SetDigestAuth("http://digest.test", user, pass); err != nil {
				c.Check(false, "SetDigestAuth: %v", err)
				return
			}
			var responses, errorsSeen atomic.Int32
			fx.c.OnResponse(func(r *Response) { responses.Add(1) })
			fx.c.OnError(func(r *Response, err error) { errorsSeen.Add(1) })
			err := fx.c.Visit("http://digest.test/report")
			c.Check(err == nil, "stale retry failed: %v", err)
			sends := fx.transport.snapshot()
			c.Check(len(sends) == 3, "expected 3 sends (initial + 2 auth), got %d", len(sends))
			c.Check(responses.Load() == 1 && errorsSeen.Load() == 0, "final callbacks wrong: %d/%d", responses.Load(), errorsSeen.Load())
			c.Check(unauthorizedCloses.Load() == 2, "each 401 body closed once, got %d", unauthorizedCloses.Load())
			c.Check(successCloses.Load() == 1, "final body closed once, got %d", successCloses.Load())

			// Same-nonce stale challenge must not trigger another send.
			var sameNonceSends atomic.Int32
			fx2 := newDigestFixture(t, func(tr *digestTestTransport, r *http.Request) (*http.Response, error) {
				sameNonceSends.Add(1)
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, "n", "SHA-256", "auth")}}, []byte("c1"), nil), nil
				}
				return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, "n", "SHA-256", "auth", `stale=true`)}}, []byte("c2"), nil), nil
			})
			if err := fx2.c.SetDigestAuth("http://digest.test", user, pass); err != nil {
				c.Check(false, "SetDigestAuth: %v", err)
				return
			}
			var err2 error
			fx2.c.OnError(func(r *Response, err error) { err2 = err })
			c.Check(fx2.c.Visit("http://digest.test/report") != nil, "same-nonce stale 401 should surface")
			c.Check(sameNonceSends.Load() == 2, "same-nonce stale should stop after one auth send, got %d", sameNonceSends.Load())
			c.Check(err2 != nil, "expected the 401 to surface as an error")

			// Even with a fresh stale nonce on every 401, the budget is one
			// unauthenticated request plus two authenticated sends per hop.
			var budgetSends atomic.Int32
			var staleCount atomic.Int32
			fx3 := newDigestFixture(t, func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				budgetSends.Add(1)
				n := staleCount.Add(1)
				return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, fmt.Sprintf("loop-%d", n), "SHA-256", "auth", "stale=true")}}, []byte("c"), nil), nil
			})
			if err := fx3.c.SetDigestAuth("http://digest.test", user, pass); err != nil {
				c.Check(false, "SetDigestAuth: %v", err)
				return
			}
			var err3 error
			fx3.c.OnError(func(r *Response, err error) { err3 = err })
			c.Check(fx3.c.Visit("http://digest.test/report") != nil, "endless stale challenges should eventually surface as an error")
			c.Check(budgetSends.Load() == 3, "retry budget should stop at 3 sends, got %d", budgetSends.Load())
			c.Check(err3 != nil, "final 401 should reach OnError")
		}

		// --- Clone sessions share counters and never reuse nc concurrently ---
		{
			const realm, user, pass = "reports", "collector", "s3cret"
			var mu sync.Mutex
			ncSeen := map[string]int{}
			fx := newDigestFixture(t, func(tr *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, "clone-nonce", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				params, _ := digestTestParams(t, r.Header.Get("Authorization"))
				mu.Lock()
				ncSeen[params["nc"]]++
				mu.Unlock()
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			})
			if err := fx.c.SetDigestAuth("http://digest.test", user, pass); err != nil {
				c.Check(false, "SetDigestAuth: %v", err)
				return
			}
			fx.c.Async = true
			clone := fx.c.Clone()
			var wg sync.WaitGroup
			const n = 8
			var failures atomic.Int32
			for i := 0; i < n; i++ {
				wg.Add(1)
				go func(i int) {
					defer wg.Done()
					target := fmt.Sprintf("http://digest.test/report/%d", i)
					if err := clone.Visit(target); err != nil {
						failures.Add(1)
					}
				}(i)
			}
			wg.Wait()
			fx.c.Wait()
			clone.Wait()
			c.Check(failures.Load() == 0, "clone visits failed: %d", failures.Load())
			mu.Lock()
			distinct := len(ncSeen)
			dup := false
			for _, v := range ncSeen {
				if v > 1 {
					dup = true
				}
			}
			mu.Unlock()
			c.Check(distinct == n, "clone nc set should have %d distinct values, got %d (%v)", n, distinct, ncSeen)
			c.Check(!dup, "nc values reused between clones: %v", ncSeen)
			c.Check(len(ncSeen) == n, "expected %d authenticated sends, got %d", n, len(ncSeen))
			fx.c.Async = false
		}

		// --- sessions are per collector, not global ---
		{
			const realm, user, pass = "reports", "collector", "s3cret"
			cnonces := map[string]string{}
			var mu sync.Mutex
			makeCollector := func(label string) (*Collector, *digestTestTransport) {
				tr := &digestTestTransport{handle: func(tr *digestTestTransport, r *http.Request) (*http.Response, error) {
					if r.Header.Get("Authorization") == "" {
						return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge(realm, "same-nonce", "SHA-256", "auth")}}, []byte("c"), nil), nil
					}
					params, _ := digestTestParams(t, r.Header.Get("Authorization"))
					mu.Lock()
					cnonces[label] = params["cnonce"]
					mu.Unlock()
					return digestResponse(r, 200, nil, []byte("ok"), nil), nil
				}}
				col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
				col.WithTransport(tr)
				c.Check(col.SetDigestAuth("http://digest.test", user, pass) == nil, "SetDigestAuth failed")
				return col, tr
			}
			c1, _ := makeCollector("one")
			c2, _ := makeCollector("two")
			c.Check(c1.Visit("http://digest.test/report") == nil, "collector one visit failed")
			c.Check(c2.Visit("http://digest.test/report") == nil, "collector two visit failed")
			c.Check(cnonces["one"] != cnonces["two"], "separate collectors must not share a cnonce: %q vs %q", cnonces["one"], cnonces["two"])
		}

		// --- cookies with the jar disabled still authenticate ---
		{
			fx := newDigestFixture(t, func(tr *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			})
			fx.c.DisableCookies()
			if err := fx.c.SetDigestAuth("http://digest.test", "collector", "s3cret"); err != nil {
				c.Check(false, "SetDigestAuth: %v", err)
				return
			}
			if err := fx.c.Visit("http://digest.test/report"); err != nil {
				c.Check(false, "auth failed without jar: %v", err)
			}
		}

		// --- SHA-256-sess ---
		{
			fx := newDigestFixture(t, func(tr *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "sess-n", "SHA-256-sess", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			})
			if err := fx.c.SetDigestAuth("http://digest.test", "collector", "s3cret"); err != nil {
				c.Check(false, "SetDigestAuth: %v", err)
				return
			}
			if err := fx.c.Visit("http://digest.test/report?x=1"); err != nil {
				c.Check(false, "sess auth failed: %v", err)
			}
			sends := fx.transport.snapshot()
			if len(sends) == 2 {
				_, ok := digestServerVerify(t, sends[1].header.Get("Authorization"), "collector", "s3cret", "GET", "/report?x=1", nil, true)
				c.Check(ok, "SHA-256-sess digest verification failed")
			} else {
				c.Check(false, "expected 2 sends, got %d", len(sends))
			}
		}

		// --- cross-origin redirect: the auth header must not leak ---
		{
			var mu sync.Mutex
			sawAuthOnOther := false
			tr := &digestTestTransport{}
			tr.handle = func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				switch {
				case r.URL.Host == "digest.test" && r.Header.Get("Authorization") == "":
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n1", "SHA-256", "auth")}, "Location": {"http://other.test/landing"}}, []byte("c"), nil), nil
				case r.URL.Host == "digest.test":
					return digestResponse(r, 302, http.Header{"Location": {"http://other.test/landing"}}, []byte("redirect"), nil), nil
				default:
					mu.Lock()
					if r.Header.Get("Authorization") != "" {
						sawAuthOnOther = true
					}
					mu.Unlock()
					return digestResponse(r, 200, nil, []byte("other"), nil), nil
				}
			}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			if err := col.SetDigestAuth("http://digest.test", "collector", "s3cret"); err != nil {
				c.Check(false, "SetDigestAuth: %v", err)
				return
			}
			var responses atomic.Int32
			col.OnResponse(func(r *Response) { responses.Add(1) })
			err := col.Visit("http://digest.test/report")
			c.Check(err == nil, "cross-origin redirect failed: %v", err)
			c.Check(!sawAuthOnOther, "library Authorization leaked across origins")
			c.Check(responses.Load() == 1, "final response not delivered once: %d", responses.Load())
			sends := tr.snapshot()
			c.Check(len(sends) >= 2, "expected redirect chain, got %d sends", len(sends))
		}

		// --- same-origin redirect is replayed unauthenticated, then challenged ---
		{
			var redirectSends, secondHops atomic.Int32
			tr := &digestTestTransport{}
			tr.handle = func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.URL.Path == "/move" {
					redirectSends.Add(1)
					if r.Header.Get("Authorization") == "" {
						return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n-move", "SHA-256", "auth")}}, []byte("c"), nil), nil
					}
					return digestResponse(r, 302, http.Header{"Location": {"/final"}}, []byte("go"), nil), nil
				}
				secondHops.Add(1)
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n-final", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("final"), nil), nil
			}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			var responses atomic.Int32
			col.OnResponse(func(r *Response) { responses.Add(1) })
			err := col.Visit("http://digest.test/move")
			c.Check(err == nil, "same-origin redirect auth failed: %v", err)
			c.Check(redirectSends.Load() >= 2, "redirect hop should be challenged and retried: %d", redirectSends.Load())
			c.Check(secondHops.Load() >= 2, "final hop should be challenged separately: %d", secondHops.Load())
			c.Check(responses.Load() == 1, "final response count wrong: %d", responses.Load())
		}

		// --- redirect requests never carry the library Authorization ---
		{
			var leaked atomic.Bool
			seenSecond := map[string]int{}
			var seenMu sync.Mutex
			tr := &digestTestTransport{}
			tr.handle = func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				seenMu.Lock()
				seenSecond[r.URL.Path]++
				first := seenSecond[r.URL.Path] == 1
				seenMu.Unlock()
				if r.URL.Path == "/second" && first && r.Header.Get("Authorization") != "" {
					// The first /second request arrives via redirect; it must
					// not carry the Authorization computed for /first.
					leaked.Store(true)
				}
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n-"+r.URL.Path, "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				if r.URL.Path == "/first" {
					return digestResponse(r, 302, http.Header{"Location": {"http://digest.test/second"}}, []byte("go"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("final"), nil), nil
			}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			var responses atomic.Int32
			col.OnResponse(func(r *Response) { responses.Add(1) })
			err := col.Visit("http://digest.test/first")
			c.Check(err == nil, "same-origin redirect auth failed: %v", err)
			c.Check(!leaked.Load(), "redirect request carried library Authorization")
			c.Check(responses.Load() == 1, "final response count wrong: %d", responses.Load())
		}

		{
			var otherChallenged atomic.Int32
			tr := &digestTestTransport{}
			tr.handle = func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.URL.Host == "a.test" {
					if r.Header.Get("Authorization") == "" {
						return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("ra", "na", "SHA-256", "auth")}}, []byte("ca"), nil), nil
					}
					return digestResponse(r, 302, http.Header{"Location": {"http://b.test/next"}}, []byte("go"), nil), nil
				}
				if r.Header.Get("Authorization") == "" {
					otherChallenged.Add(1)
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("rb", "nb", "SHA-256", "auth")}}, []byte("cb"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("done"), nil), nil
			}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://a.test", "collector", "s3cret") == nil, "SetDigestAuth a failed")
			c.Check(col.SetDigestAuth("http://b.test", "collector", "other-secret") == nil, "SetDigestAuth b failed")
			var responses, errorsSeen atomic.Int32
			col.OnResponse(func(r *Response) { responses.Add(1) })
			col.OnError(func(r *Response, err error) { errorsSeen.Add(1) })
			err := col.Visit("http://a.test/report")
			c.Check(err == nil, "per-hop challenge failed: %v", err)
			c.Check(otherChallenged.Load() == 1, "redirect target not challenged separately: %d", otherChallenged.Load())
			c.Check(responses.Load() == 1 && errorsSeen.Load() == 0, "final callbacks wrong: %d/%d", responses.Load(), errorsSeen.Load())
		}

		// --- transport failure stops the flow and leaves no retry behind ---
		{
			var sends atomic.Int32
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				sends.Add(1)
				return nil, errors.New("boom")
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			var errSeen error
			col.OnError(func(r *Response, err error) { errSeen = err })
			err := col.Visit("http://digest.test/report")
			c.Check(err != nil && strings.Contains(err.Error(), "boom"), "transport error should surface: %v", err)
			c.Check(sends.Load() == 1, "no retry after transport failure, got %d", sends.Load())
			c.Check(errSeen != nil, "OnError should see the transport error")
		}

		// --- cancelled context does not authenticate ---
		{
			var sends atomic.Int32
			ctx, cancel := context.WithCancel(context.Background())
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				sends.Add(1)
				if r.Header.Get("Authorization") != "" {
					c.Check(false, "authenticated after cancellation")
				}
				cancel()
				return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit(), StdlibContext(ctx))
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			err := col.Visit("http://digest.test/report")
			c.Check(err != nil, "cancelled request should return an error, got %v", err)
			c.Check(errors.Is(err, context.Canceled), "cancellation error identity lost: %v", err)
			c.Check(sends.Load() == 1, "no retry after cancellation, got %d sends", sends.Load())
		}

		// --- unreplayable body returns the sentinel without a second send ---
		{
			var unauthorizedCloses atomic.Int32
			var sends atomic.Int32
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				sends.Add(1)
				return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth-int")}}, []byte("c"), &unauthorizedCloses), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			var errSeen error
			col.OnError(func(r *Response, err error) { errSeen = err })
			err := col.Request("POST", "http://digest.test/report", io.NopCloser(strings.NewReader("body")), NewContext(), nil)
			c.Check(errors.Is(err, ErrDigestBodyUnreplayable), "expected ErrDigestBodyUnreplayable, got %v", err)
			c.Check(errors.Is(errSeen, ErrDigestBodyUnreplayable), "the sentinel should reach OnError, got %v", errSeen)
			c.Check(sends.Load() == 1, "unreplayable body must not be sent again, got %d", sends.Load())
			c.Check(unauthorizedCloses.Load() == 1, "intermediate 401 body not closed once: %d", unauthorizedCloses.Load())
		}

		// --- GetBody failure keeps the error identity via errors.Is ---
		{
			sentinel := errors.New("cannot rebuild")
			var opens atomic.Int32
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth-int")}}, []byte("c"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			req, _ := http.NewRequest("POST", "http://digest.test/report", strings.NewReader("entity"))
			// First call fulfils the entity hash, the second (for the actual
			// send) fails. Both preserve identity through wrapping.
			req.GetBody = func() (io.ReadCloser, error) {
				if opens.Add(1) == 1 {
					return io.NopCloser(strings.NewReader("entity")), nil
				}
				return nil, sentinel
			}
			var checkHeadersCalls atomic.Int32
			_, err := col.doRequest(req, 0, func(r *http.Request) bool {
				checkHeadersCalls.Add(1)
				return true
			}, func(r *http.Request, status int, h http.Header) bool { return true })
			c.Check(errors.Is(err, sentinel), "GetBody error identity lost: %v", err)
		}

		// --- entity hash reader is independent and closed exactly once ---
		{
			var hashCloses atomic.Int32
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth-int")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			entity := []byte("streamed-entity")
			req, _ := http.NewRequest("POST", "http://digest.test/report", bytes.NewReader(entity))
			req.GetBody = func() (io.ReadCloser, error) {
				return &digestTestBody{r: bytes.NewReader(entity), closes: &hashCloses}, nil
			}
			_, err := col.doRequest(req, 0, func(r *http.Request) bool { return true }, func(r *http.Request, status int, h http.Header) bool { return true })
			c.Check(err == nil, "auth-int with independent GetBody failed: %v", err)
			c.Check(hashCloses.Load() == 2, "hash reader and replayed body should each close once: %d", hashCloses.Load())
		}

		// --- caller Authorization bypasses the whole digest flow ---
		{
			var sends atomic.Int32
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				sends.Add(1)
				return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			hdr := http.Header{"Authorization": {"Digest caller-supplied"}}
			var errSeen error
			col.OnError(func(r *Response, err error) { errSeen = err })
			err := col.Request("GET", "http://digest.test/report", nil, NewContext(), hdr)
			c.Check(err != nil, "caller Authorization should not be retried")
			c.Check(sends.Load() == 1, "caller Authorization must send once, got %d", sends.Load())
			c.Check(hdr.Get("Authorization") == "Digest caller-supplied", "caller header mutated: %q", hdr.Get("Authorization"))
			c.Check(errSeen != nil, "the 401 should still reach OnError for a caller-supplied header")
		}

		// --- callback ordering for the final result only ---
		{
			var order []string
			var mu sync.Mutex
			fx := newDigestFixture(t, func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, http.Header{"Content-Type": {"text/html"}}, []byte("<html><body><p id=\"x\">hi</p></body></html>"), nil), nil
			})
			c.Check(fx.c.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			fx.c.OnRequest(func(r *Request) { mu.Lock(); order = append(order, "request"); mu.Unlock() })
			fx.c.OnRequestHeaders(func(r *Request) { mu.Lock(); order = append(order, "requestHeaders"); mu.Unlock() })
			fx.c.OnResponseHeaders(func(r *Response) { mu.Lock(); order = append(order, "responseHeaders"); mu.Unlock() })
			fx.c.OnResponse(func(r *Response) { mu.Lock(); order = append(order, "response"); mu.Unlock() })
			fx.c.OnHTML("#x", func(e *HTMLElement) { mu.Lock(); order = append(order, "html"); mu.Unlock() })
			fx.c.OnScraped(func(r *Response) { mu.Lock(); order = append(order, "scraped"); mu.Unlock() })
			err := fx.c.Visit("http://digest.test/report")
			c.Check(err == nil, "visit failed: %v", err)
			mu.Lock()
			got := strings.Join(order, ",")
			mu.Unlock()
			c.Check(got == "request,requestHeaders,responseHeaders,response,html,scraped", "callback order wrong: %s", got)
		}

		// --- escaped path and raw query in the digest uri ---
		{
			var mu sync.Mutex
			var uriSeen string
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				params, _ := digestTestParams(t, r.Header.Get("Authorization"))
				mu.Lock()
				uriSeen = params["uri"]
				mu.Unlock()
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			err := col.Visit("http://digest.test/a%2Fb/c?q=a%20b&z=%2F")
			c.Check(err == nil, "escaped-path visit failed: %v", err)
			mu.Lock()
			got := uriSeen
			mu.Unlock()
			c.Check(got == "/a%2Fb/c?q=a%20b&z=%2F", "digest uri must keep escaped path and raw query, got %q", got)
		}

		// --- origin matching uses the request URL, not the Host header ---
		{
			var sends atomic.Int32
			var mu sync.Mutex
			var hostHeader string
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				sends.Add(1)
				mu.Lock()
				hostHeader = r.Host
				mu.Unlock()
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			_ = col.SetDigestAuth("http://digest.test", "collector", "s3cret")
			hdr := http.Header{"Host": {"digest.test"}}
			err := col.Request("GET", "http://unconfigured.test/report", nil, NewContext(), hdr)
			c.Check(err != nil, "Host header must not substitute for the request URL origin")
			c.Check(sends.Load() == 1, "unconfigured URL must not authenticate, got %d sends", sends.Load())
			mu.Lock()
			got := hostHeader
			mu.Unlock()
			c.Check(got == "digest.test", "Host header should still be sent, got %q", got)
		}

		// --- ports and schemes are distinct origins ---
		{
			var sends atomic.Int32
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				sends.Add(1)
				return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test:80", "collector", "s3cret") == nil, "SetDigestAuth :80 failed")
			var errSeen atomic.Int32
			col.OnError(func(r *Response, err error) { errSeen.Add(1) })
			c.Check(col.Visit("http://digest.test:8080/report") != nil, "non-matching origin should surface the 401")
			c.Check(sends.Load() == 1, "non-matching port must not authenticate: %d", sends.Load())
			c.Check(errSeen.Load() == 1, "non-matching origin should surface the 401")
			c.Check(col.Visit("https://digest.test/report") != nil, "https origin should surface the 401")
			c.Check(sends.Load() == 2, "https origin must not reuse http credentials: %d", sends.Load())
		}

		// --- configured origins bypass the cache; others still use it ---
		{
			dir := t.TempDir()
			var sends atomic.Int32
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				sends.Add(1)
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("payload"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit(), CacheDir(dir))
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			_ = col.Visit("http://digest.test/report")
			before := sends.Load()
			_ = col.Visit("http://digest.test/report")
			c.Check(sends.Load() == before+2, "digest origin must bypass the cache; sends grew by %d", sends.Load()-before)
			c.Check(digestTestCountFiles(t, dir) == 0, "digest origin must not write cache files: %d", digestTestCountFiles(t, dir))

			// A different, unconfigured origin on the same collector still
			// uses the cache: only origins with credentials bypass it.
			plainSends := atomic.Int32{}
			plain := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				plainSends.Add(1)
				return digestResponse(r, 200, nil, []byte("plain"), nil), nil
			}}
			plainCol := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit(), CacheDir(dir))
			plainCol.WithTransport(plain)
			c.Check(plainCol.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			c.Check(plainCol.Visit("http://other.test/page") == nil, "plain visit failed")
			c.Check(plainCol.Visit("http://other.test/page") == nil, "plain revisit failed")
			c.Check(plainSends.Load() == 1, "unconfigured origin should be served from cache, sends=%d", plainSends.Load())
			c.Check(digestTestCountFiles(t, dir) > 0, "unconfigured origin should write cache files")
		}

		// --- TraceHTTP still works with digest ---
		{
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit(), TraceHTTP())
			col.WithTransport(tr)
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			var resp *Response
			col.OnResponse(func(r *Response) { resp = r })
			if err := col.Visit("http://digest.test/report"); err != nil {
				c.Check(false, "TraceHTTP digest visit failed: %v", err)
			}
			c.Check(resp != nil && resp.Trace != nil, "TraceHTTP should still populate Response.Trace")
		}

		// --- SetClient keeps digest working ---
		{
			tr := &digestTestTransport{handle: func(_ *digestTestTransport, r *http.Request) (*http.Response, error) {
				if r.Header.Get("Authorization") == "" {
					return digestResponse(r, 401, http.Header{"Www-Authenticate": {digestTestChallenge("r", "n", "SHA-256", "auth")}}, []byte("c"), nil), nil
				}
				return digestResponse(r, 200, nil, []byte("ok"), nil), nil
			}}
			col := NewCollector(IgnoreRobotsTxt(), AllowURLRevisit())
			col.SetClient(&http.Client{Transport: tr})
			c.Check(col.SetDigestAuth("http://digest.test", "collector", "s3cret") == nil, "SetDigestAuth failed")
			c.Check(col.Visit("http://digest.test/report") == nil, "digest with SetClient failed")
		}
	})
}
