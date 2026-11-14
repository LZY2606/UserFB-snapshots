// Copyright 2018 Adam Tauber
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package colly

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
)

// ErrDigestBodyUnreplayable is returned when a Digest-authenticated request
// receives a 401 challenge but its request body cannot be rebuilt through
// http.Request.GetBody. The challenge is not retried in that case.
var ErrDigestBodyUnreplayable = errors.New("Digest authentication body cannot be replayed")

// digestScope identifies a Digest authentication session. Sessions are shared
// between collectors cloned from the same collector.
type digestScope struct {
	origin    string
	username  string
	realm     string
	algorithm string
	nonce     string
}

type digestSession struct {
	cnonce string
	nc     uint64
}

type digestCredentials struct {
	origin   string
	username string
	password string
}

// digestState holds configured credentials and the session counters for a
// collector and every collector cloned from it. The mutex only guards the
// maps and counters; it is never held while a request is in flight.
type digestState struct {
	mu       sync.Mutex
	creds    map[string]digestCredentials
	sessions map[digestScope]*digestSession
}

func newDigestState() *digestState {
	return &digestState{
		creds:    make(map[string]digestCredentials),
		sessions: make(map[digestScope]*digestSession),
	}
}

func (s *digestState) setCredentials(creds digestCredentials) {
	s.mu.Lock()
	s.creds[creds.origin] = creds
	s.mu.Unlock()
}

func (s *digestState) credentialsFor(origin string) (digestCredentials, bool) {
	s.mu.Lock()
	creds, ok := s.creds[origin]
	s.mu.Unlock()
	return creds, ok
}

// acquire returns the cnonce and the next nc for a session scope. The counter
// is advanced before returning so a failed send does not recycle the number.
func (s *digestState) acquire(scope digestScope) *digestSession {
	s.mu.Lock()
	defer s.mu.Unlock()
	sess, ok := s.sessions[scope]
	if !ok {
		cnonce, err := newCnonce()
		if err != nil {
			sum := sha256.Sum256([]byte(scope.origin + "\x00" + scope.nonce))
			cnonce = hex.EncodeToString(sum[:8])
		}
		sess = &digestSession{cnonce: cnonce}
		s.sessions[scope] = sess
	}
	sess.nc++
	return &digestSession{cnonce: sess.cnonce, nc: sess.nc}
}

// SetDigestAuth registers ASCII HTTP Digest credentials for an origin. The
// origin must be an absolute http(s) URL with an empty or "/" path and no
// userinfo, query or fragment. Default and explicit default ports are
// equivalent; every other port is a distinct origin. Multiple calls configure
// multiple origins. All configuration is expected to happen before scraping.
func (c *Collector) SetDigestAuth(origin, username, password string) error {
	normalized, err := normalizeDigestOrigin(origin)
	if err != nil {
		return err
	}
	if err := checkASCII("username", username); err != nil {
		return err
	}
	if err := checkASCII("password", password); err != nil {
		return err
	}
	c.digestInit()
	c.digest.setCredentials(digestCredentials{
		origin:   normalized,
		username: username,
		password: password,
	})
	return nil
}

func (c *Collector) digestInit() {
	c.lock.Lock()
	if c.digest == nil {
		c.digest = newDigestState()
	}
	c.lock.Unlock()
}

func checkASCII(field, value string) error {
	for i := 0; i < len(value); i++ {
		if value[i] > 0x7f {
			return fmt.Errorf("digest auth %s must be ASCII", field)
		}
	}
	return nil
}

// normalizeDigestOrigin validates an origin and reduces it to a canonical
// "scheme://hostname:port" form.
func normalizeDigestOrigin(raw string) (string, error) {
	if raw == "" {
		return "", errors.New("digest auth origin must not be empty")
	}
	u, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("digest auth origin %q: %w", raw, err)
	}
	if !u.IsAbs() || u.Host == "" {
		return "", fmt.Errorf("digest auth origin %q is not an absolute URL", raw)
	}
	scheme := strings.ToLower(u.Scheme)
	if scheme != "http" && scheme != "https" {
		return "", fmt.Errorf("digest auth origin %q must use http or https", raw)
	}
	if u.User != nil {
		return "", fmt.Errorf("digest auth origin %q must not contain userinfo", raw)
	}
	if u.Opaque != "" {
		return "", fmt.Errorf("digest auth origin %q is not a hierarchical URL", raw)
	}
	if u.RawQuery != "" || u.ForceQuery {
		return "", fmt.Errorf("digest auth origin %q must not contain a query", raw)
	}
	if u.Fragment != "" || u.RawFragment != "" || strings.Contains(raw, "#") {
		return "", fmt.Errorf("digest auth origin %q must not contain a fragment", raw)
	}
	if u.Path != "" && u.Path != "/" {
		return "", fmt.Errorf("digest auth origin %q must have an empty path or %q", raw, "/")
	}
	if u.RawPath != "" && u.RawPath != "/" {
		return "", fmt.Errorf("digest auth origin %q must have an empty path or %q", raw, "/")
	}
	host := strings.ToLower(u.Hostname())
	if host == "" {
		return "", fmt.Errorf("digest auth origin %q has no host", raw)
	}
	port, err := canonicalPort(scheme, u.Port())
	if err != nil {
		return "", fmt.Errorf("digest auth origin %q: %w", raw, err)
	}
	return canonicalOriginString(scheme, host, port), nil
}

// canonicalOriginString formats an origin. IPv6 literals are bracketed so a
// host and port can never be confused with another host's literal.
func canonicalOriginString(scheme, host, port string) string {
	if strings.Contains(host, ":") {
		return scheme + "://[" + host + "]:" + port
	}
	return scheme + "://" + host + ":" + port
}

func canonicalPort(scheme, port string) (string, error) {
	if port == "" {
		if scheme == "https" {
			return "443", nil
		}
		return "80", nil
	}
	for i := 0; i < len(port); i++ {
		if port[i] < '0' || port[i] > '9' {
			return "", errors.New("invalid port")
		}
	}
	n, err := strconv.Atoi(port)
	if err != nil || n < 0 || n > 65535 {
		return "", errors.New("invalid port")
	}
	return strconv.Itoa(n), nil
}

// originOf reduces a request URL to the same canonical origin form used for
// configuration.
func originOf(u *url.URL) string {
	scheme := strings.ToLower(u.Scheme)
	port, err := canonicalPort(scheme, u.Port())
	if err != nil {
		port = u.Port()
	}
	return canonicalOriginString(scheme, strings.ToLower(u.Hostname()), port)
}

// ---------------------------------------------------------------------------
// Challenge parsing
// ---------------------------------------------------------------------------

type authParam struct {
	name  string
	value string
}

type rawAuthChallenge struct {
	scheme  string
	params  []authParam
	invalid bool
}

type digestChallenge struct {
	realm     string
	nonce     string
	opaque    string
	hasOpaque bool
	algorithm string
	qop       string
	charset   string
	stale     bool
}

// selectDigestChallenge returns the first supported, complete Digest
// challenge in header order. Fields are scanned in order and a single field
// may mix several challenges or schemes.
func selectDigestChallenge(headers http.Header) *digestChallenge {
	for _, value := range wwwAuthenticateValues(headers) {
		for _, raw := range parseAuthField(value) {
			if raw.invalid || !strings.EqualFold(raw.scheme, "Digest") {
				continue
			}
			if ch := supportedDigestChallenge(raw.params); ch != nil {
				return ch
			}
		}
	}
	return nil
}

// wwwAuthenticateValues collects WWW-Authenticate field values without
// relying on map key canonicalization, so a hand-built http.Header still
// works. The canonical key is preferred; any other spelling follows.
func wwwAuthenticateValues(headers http.Header) []string {
	values := headers.Values("WWW-Authenticate")
	for key, vals := range headers {
		if strings.EqualFold(key, "WWW-Authenticate") && key != http.CanonicalHeaderKey("WWW-Authenticate") {
			values = append(values, vals...)
		}
	}
	return values
}

func isTokenByte(b byte) bool {
	switch {
	case b >= 'a' && b <= 'z', b >= 'A' && b <= 'Z', b >= '0' && b <= '9':
		return true
	}
	switch b {
	case '!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~':
		return true
	}
	return false
}

func isAuthOWS(b byte) bool {
	return b == ' ' || b == '\t'
}

// parseAuthField tokenizes one WWW-Authenticate field value into challenges.
// Commas inside quoted strings are preserved, and escaped quotes and
// backslashes are unescaped.
func parseAuthField(s string) []rawAuthChallenge {
	var out []rawAuthChallenge
	var current *rawAuthChallenge
	finish := func() {
		if current != nil {
			out = append(out, *current)
			current = nil
		}
	}
	pos := 0
	for pos < len(s) {
		for pos < len(s) && (isAuthOWS(s[pos]) || s[pos] == ',') {
			pos++
		}
		if pos >= len(s) {
			break
		}
		start := pos
		for pos < len(s) && isTokenByte(s[pos]) {
			pos++
		}
		if start == pos {
			// Not a token; skip a byte so scanning always progresses.
			pos++
			continue
		}
		token := s[start:pos]
		look := pos
		for look < len(s) && isAuthOWS(s[look]) {
			look++
		}
		if look < len(s) && s[look] == '=' {
			// auth-param: name = ( token / quoted-string )
			pos = look + 1
			for pos < len(s) && isAuthOWS(s[pos]) {
				pos++
			}
			var value string
			if pos < len(s) && s[pos] == '"' {
				var ok bool
				value, pos, ok = parseQuotedString(s, pos)
				if !ok {
					if current != nil {
						current.invalid = true
					}
					break
				}
			} else {
				valueStart := pos
				for pos < len(s) && s[pos] != ',' && !isAuthOWS(s[pos]) {
					pos++
				}
				value = s[valueStart:pos]
			}
			if current != nil {
				current.params = append(current.params, authParam{name: token, value: value})
			}
			continue
		}
		// A bare token starts a new challenge scheme.
		finish()
		current = &rawAuthChallenge{scheme: token}
	}
	finish()
	return out
}

func parseQuotedString(s string, pos int) (string, int, bool) {
	if pos >= len(s) || s[pos] != '"' {
		return "", pos, false
	}
	pos++
	var b strings.Builder
	for pos < len(s) {
		switch s[pos] {
		case '\\':
			if pos+1 >= len(s) {
				return "", pos, false
			}
			b.WriteByte(s[pos+1])
			pos += 2
		case '"':
			return b.String(), pos + 1, true
		default:
			b.WriteByte(s[pos])
			pos++
		}
	}
	return "", pos, false
}

var digestRecognizedParams = map[string]bool{
	"realm":     true,
	"domain":    true,
	"nonce":     true,
	"opaque":    true,
	"stale":     true,
	"algorithm": true,
	"qop":       true,
	"charset":   true,
	"userhash":  true,
}

// supportedDigestChallenge validates parameters against the supported profile.
// Duplicate recognized parameters invalidate the challenge; unrecognized
// parameters are ignored.
func supportedDigestChallenge(params []authParam) *digestChallenge {
	ch := &digestChallenge{}
	seen := make(map[string]bool, len(params))
	for _, p := range params {
		name := strings.ToLower(p.name)
		if !digestRecognizedParams[name] {
			continue
		}
		if seen[name] {
			return nil
		}
		seen[name] = true
		switch name {
		case "realm":
			ch.realm = p.value
		case "nonce":
			ch.nonce = p.value
		case "opaque":
			ch.opaque = p.value
			ch.hasOpaque = true
		case "algorithm":
			switch {
			case strings.EqualFold(p.value, "SHA-256"):
				ch.algorithm = "SHA-256"
			case strings.EqualFold(p.value, "SHA-256-sess"):
				ch.algorithm = "SHA-256-sess"
			default:
				return nil
			}
		case "qop":
			qop, ok := selectQop(p.value)
			if !ok {
				return nil
			}
			ch.qop = qop
		case "charset":
			if p.value != "" && !strings.EqualFold(strings.TrimSpace(p.value), "UTF-8") {
				return nil
			}
			ch.charset = p.value
		case "stale":
			ch.stale = strings.EqualFold(strings.TrimSpace(p.value), "true")
		case "userhash":
			if strings.EqualFold(strings.TrimSpace(p.value), "true") {
				return nil
			}
		}
	}
	if !seen["realm"] || !seen["nonce"] || !seen["algorithm"] || !seen["qop"] {
		return nil
	}
	if ch.nonce == "" {
		return nil
	}
	return ch
}

func selectQop(raw string) (string, bool) {
	var hasAuth, hasAuthInt bool
	for _, item := range strings.Split(raw, ",") {
		switch strings.ToLower(strings.TrimSpace(item)) {
		case "auth":
			hasAuth = true
		case "auth-int":
			hasAuthInt = true
		}
	}
	if hasAuthInt {
		return "auth-int", true
	}
	if hasAuth {
		return "auth", true
	}
	return "", false
}

// ---------------------------------------------------------------------------
// Authorization header construction
// ---------------------------------------------------------------------------

func quoteDigest(s string) string {
	var b strings.Builder
	b.Grow(len(s) + 2)
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		if s[i] == '"' || s[i] == '\\' {
			b.WriteByte('\\')
		}
		b.WriteByte(s[i])
	}
	b.WriteByte('"')
	return b.String()
}

func sha256Hex(s string) string {
	sum := sha256.Sum256([]byte(s))
	return hex.EncodeToString(sum[:])
}

func newCnonce() (string, error) {
	var buf [16]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf[:]), nil
}

func buildDigestAuthorization(ch *digestChallenge, creds digestCredentials, method string, u *url.URL, sess *digestSession, entityHash string) string {
	uri := u.RequestURI()
	nc := fmt.Sprintf("%08x", sess.nc)
	var ha1 string
	if strings.EqualFold(ch.algorithm, "SHA-256-sess") {
		ha1 = sha256Hex(sha256Hex(creds.username+":"+ch.realm+":"+creds.password) + ":" + ch.nonce + ":" + sess.cnonce)
	} else {
		ha1 = sha256Hex(creds.username + ":" + ch.realm + ":" + creds.password)
	}
	var ha2 string
	if ch.qop == "auth-int" {
		ha2 = sha256Hex(method + ":" + uri + ":" + entityHash)
	} else {
		ha2 = sha256Hex(method + ":" + uri)
	}
	response := sha256Hex(ha1 + ":" + ch.nonce + ":" + nc + ":" + sess.cnonce + ":" + ch.qop + ":" + ha2)

	var b strings.Builder
	b.WriteString("Digest username=")
	b.WriteString(quoteDigest(creds.username))
	b.WriteString(", realm=")
	b.WriteString(quoteDigest(ch.realm))
	b.WriteString(", nonce=")
	b.WriteString(quoteDigest(ch.nonce))
	b.WriteString(", uri=")
	b.WriteString(quoteDigest(uri))
	b.WriteString(", response=")
	b.WriteString(quoteDigest(response))
	b.WriteString(", algorithm=")
	b.WriteString(ch.algorithm)
	b.WriteString(", qop=")
	b.WriteString(ch.qop)
	b.WriteString(", nc=")
	b.WriteString(nc)
	b.WriteString(", cnonce=")
	b.WriteString(quoteDigest(sess.cnonce))
	if ch.hasOpaque {
		b.WriteString(", opaque=")
		b.WriteString(quoteDigest(ch.opaque))
	}
	return b.String()
}

// hashEntity streams the entity through SHA-256 using a reader from getBody.
// The reader is independent of the request's own body and is closed exactly
// once. A nil factory means an empty entity.
func hashEntity(getBody func() (io.ReadCloser, error)) (string, error) {
	if getBody == nil {
		return sha256Hex(""), nil
	}
	r, err := getBody()
	if err != nil {
		return "", err
	}
	defer r.Close()
	h := sha256.New()
	if _, err := io.Copy(h, r); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// replayableBody reports whether the request carries an entity that can be
// rebuilt for a retry.
func replayableBody(req *http.Request) bool {
	if req.Body == nil || req.Body == http.NoBody {
		return true
	}
	return req.GetBody != nil
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

type digestHopState struct {
	challenge *digestChallenge
	sends     int
}

// doRequest routes a logical request through the Digest machinery when its
// origin has configured credentials and the caller did not supply an
// Authorization header. Everything else takes the original backend path,
// including the response cache.
func (c *Collector) doRequest(req *http.Request, bodySize int, checkHeaders func(*http.Request) bool, checkResponse func(*http.Request, int, http.Header) bool) (*Response, error) {
	if req.Header.Get("Authorization") != "" || c.digestCredentialsFor(req.URL) == nil {
		return c.backend.Cache(req, bodySize, checkHeaders, checkResponse, c.CacheDir, c.CacheExpiration)
	}
	return c.doDigest(req, bodySize, checkHeaders, checkResponse)
}

func (c *Collector) digestCredentialsFor(u *url.URL) *digestCredentials {
	c.lock.RLock()
	state := c.digest
	c.lock.RUnlock()
	if state == nil {
		return nil
	}
	creds, ok := state.credentialsFor(originOf(u))
	if !ok {
		return nil
	}
	return &creds
}

// doDigest performs the first unauthenticated request and, when the response
// is a 401 carrying a supported Digest challenge for the request URL's
// origin, up to two authenticated sends for that hop. Intermediate 401
// responses never reach the collector callbacks. Caching is bypassed.
func (c *Collector) doDigest(req *http.Request, bodySize int, checkHeaders func(*http.Request) bool, checkResponse func(*http.Request, int, http.Header) bool) (*Response, error) {
	c.lock.RLock()
	state := c.digest
	c.lock.RUnlock()
	if state == nil {
		return c.backend.Cache(req, bodySize, checkHeaders, checkResponse, c.CacheDir, c.CacheExpiration)
	}

	// The caller's context is used for every retry. Per-send contexts must
	// not be reused: net/http cancels them once the response body is closed.
	baseCtx := req.Context()
	hops := make(map[string]*digestHopState)
	// generatedAuth records header values this collector produced, so a
	// redirect that carries one forward is refreshed rather than mistaken
	// for a caller-supplied Authorization header.
	generatedAuth := make(map[string]bool)
	firstHeaders := true
	current := req
	for {
		var finalReq *http.Request
		var suppressed bool
		var retryChallenge *digestChallenge
		var retryCreds digestCredentials
		var retryHop *digestHopState

		var client *http.Client
		if current.Header.Get("Authorization") != "" {
			// Library-generated credentials were just attached; use the
			// stripping redirect handler so each real hop starts unauthenticated.
			client = c.digestRedirectClient()
		} else {
			client = c.backend.Client
		}
		resp, err := c.backend.doWithClient(client, current, bodySize,
			func(r *http.Request) bool {
				if firstHeaders {
					return checkHeaders(r)
				}
				return true
			},
			func(r *http.Request, status int, headers http.Header) bool {
				finalReq = r
				retryChallenge, retryCreds, retryHop = nil, digestCredentials{}, nil
				if status != http.StatusUnauthorized {
					return checkResponse(r, status, headers)
				}
				creds := c.digestCredentialsFor(r.URL)
				if creds == nil {
					return checkResponse(r, status, headers)
				}
				key := originOf(r.URL) + r.URL.RequestURI()
				hop := hops[key]
				if hop == nil {
					hop = &digestHopState{}
					hops[key] = hop
				}
				if hop.sends >= 2 {
					return checkResponse(r, status, headers)
				}
				if auth := r.Header.Get("Authorization"); auth != "" && !generatedAuth[auth] {
					// A caller-supplied header must not be replaced by
					// library-generated credentials.
					return checkResponse(r, status, headers)
				}
				challenge := selectDigestChallenge(headers)
				if challenge == nil {
					return checkResponse(r, status, headers)
				}
				if hop.challenge != nil && (!challenge.stale || challenge.nonce == hop.challenge.nonce) {
					return checkResponse(r, status, headers)
				}
				// This 401 will either be answered or returned as an error;
				// either way its headers and body do not reach the callbacks.
				suppressed = true
				retryChallenge, retryCreds, retryHop = challenge, *creds, hop
				return true
			})
		firstHeaders = false
		if err != nil {
			return nil, err
		}
		if finalReq == nil {
			finalReq = current
		}
		if !suppressed {
			return resp, nil
		}
		if err := current.Context().Err(); err != nil {
			return nil, err
		}
		if !replayableBody(finalReq) {
			// The intermediate 401 body has already been read and closed by
			// the backend. Return the sentinel without a second send.
			return nil, ErrDigestBodyUnreplayable
		}

		entityHash := ""
		if retryChallenge.qop == "auth-int" {
			entityHash, err = hashEntity(finalReq.GetBody)
			if err != nil {
				return nil, fmt.Errorf("digest auth: hash request body: %w", err)
			}
		}

		scope := digestScope{
			origin:    retryCreds.origin,
			username:  retryCreds.username,
			realm:     retryChallenge.realm,
			algorithm: retryChallenge.algorithm,
			nonce:     retryChallenge.nonce,
		}
		sess := state.acquire(scope)
		authHeader := buildDigestAuthorization(retryChallenge, retryCreds, finalReq.Method, finalReq.URL, sess, entityHash)

		next := finalReq.Clone(baseCtx)
		next.Header = finalReq.Header.Clone()
		// Cookies are owned by Client.Jar in this mode; drop whatever the
		// previous hop carried and let the jar supply them for this URL.
		next.Header.Del("Cookie")
		next.Header.Set("Authorization", authHeader)
		switch {
		case finalReq.Body == nil || finalReq.Body == http.NoBody:
			// Empty or absent entity: keep it exactly as the original
			// request expressed it so no framing changes on the wire.
			next.Body = finalReq.Body
			next.GetBody = finalReq.GetBody
		case finalReq.GetBody != nil:
			body, err := finalReq.GetBody()
			if err != nil {
				return nil, fmt.Errorf("digest auth: rebuild request body: %w", err)
			}
			next.Body = body
			next.GetBody = finalReq.GetBody
		default:
			return nil, ErrDigestBodyUnreplayable
		}

		retryHop.sends++
		retryHop.challenge = retryChallenge
		generatedAuth[authHeader] = true
		current = next
	}
}

// digestRedirectClient returns a shallow copy of the backend client whose
// redirect handling removes the library-generated Authorization header before
// every redirected request is sent: each real hop must start unauthenticated
// and face its own challenge. The collector's own redirect handler (or Go's
// default redirect limit) still runs.
func (c *Collector) digestRedirectClient() *http.Client {
	base := c.backend.Client
	if base == nil {
		return nil
	}
	clone := *base
	prev := base.CheckRedirect
	clone.CheckRedirect = func(req *http.Request, via []*http.Request) error {
		if prev != nil {
			if err := prev(req, via); err != nil {
				return err
			}
		} else if len(via) >= 10 {
			return http.ErrUseLastResponse
		}
		req.Header.Del("Authorization")
		return nil
	}
	return &clone
}
