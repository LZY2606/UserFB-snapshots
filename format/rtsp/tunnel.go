package rtsp

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// maxHTTPPostPayload is the amount of base64 encoded RTSP request data the
// single POST connection may carry. Gateways that accept requests on the POST
// connection advertise this limit in their GET reply; for now the tunnel
// supports sessions whose cumulative upstream traffic stays within it.
const maxHTTPPostPayload = 32767

// httpTunnelDial is the dial seam used for both HTTP connections. Tests
// replace it to observe or constrain the underlying connections.
var httpTunnelDial = func(network, addr string, timeout time.Duration) (net.Conn, error) {
	dialer := net.Dialer{Timeout: timeout}
	return dialer.Dial(network, addr)
}

// DialHTTPTimeout connects to a camera gateway that only exposes an HTTP
// endpoint and opens an RTSP-over-HTTP tunnel through it.
//
// uri is the rtsp:// media address used for the RTSP request line,
// authentication, Session, CSeq and relative track control urls. endpoint is
// the http:// access address (optionally carrying a path and query) whose
// path, query and Host header drive the HTTP requests. timeout bounds both
// TCP connections and the GET handshake; once the tunnel is up the Client's
// own RtspTimeout applies again.
func DialHTTPTimeout(uri, endpoint string, timeout time.Duration) (self *Client, err error) {
	mediaURL, err := url.Parse(uri)
	if err != nil {
		return nil, err
	}
	if mediaURL.Scheme == "" || mediaURL.Host == "" {
		return nil, fmt.Errorf("rtsp: invalid media address %q", uri)
	}
	if _, _, err = net.SplitHostPort(mediaURL.Host); err != nil {
		mediaURL.Host = mediaURL.Host + ":554"
	}

	httpURL, err := url.Parse(endpoint)
	if err != nil {
		return nil, err
	}
	if httpURL.Scheme != "http" {
		return nil, fmt.Errorf("rtsp: unsupported http endpoint scheme %q", httpURL.Scheme)
	}
	if httpURL.Host == "" {
		return nil, fmt.Errorf("rtsp: invalid http endpoint %q", endpoint)
	}
	// The Host header keeps the endpoint's authority as written; the dial
	// address only gains the default HTTP port when none was supplied.
	dialAddr := httpURL.Host
	if _, _, err = net.SplitHostPort(dialAddr); err != nil {
		dialAddr = net.JoinHostPort(httpURL.Hostname(), "80")
	}

	cookie, err := newTunnelCookie()
	if err != nil {
		return nil, err
	}

	t := &httpTunnel{
		endpoint: httpURL,
		dialAddr: dialAddr,
		cookie:   cookie,
		timeout:  timeout,
	}

	getConn, err := httpTunnelDial("tcp", dialAddr, timeout)
	if err != nil {
		return nil, err
	}
	t.getConn = &connWithTimeout{Conn: getConn}
	if err = t.handshakeGET(timeout); err != nil {
		t.getConn.Conn.Close()
		return nil, err
	}

	postConn, err := httpTunnelDial("tcp", dialAddr, timeout)
	if err != nil {
		// Release the downstream half we already hold, keep the dial error.
		t.getConn.Conn.Close()
		return nil, err
	}
	t.postConn = &connWithTimeout{Conn: postConn}
	if err = t.handshakePOST(timeout); err != nil {
		t.postConn.Conn.Close()
		t.getConn.Conn.Close()
		return nil, err
	}
	// From here on deadlines are set per operation: reads from RtspTimeout via
	// connWithTimeout, writes explicitly by the POST path.
	t.postConn.Timeout = 0
	t.getConn.Timeout = 0

	go t.watchPOST()

	self = &Client{
		conn:            t.getConn,
		brconn:          t.brconn,
		url:             mediaURL,
		requestUri:      mediaRequestUri(mediaURL),
		DebugRtp:        DebugRtp,
		DebugRtsp:       DebugRtsp,
		SkipErrRtpBlock: SkipErrRtpBlock,
		httpTunnel:      t,
	}
	return self, nil
}

// mediaRequestUri keeps the media address for RTSP request lines, dropping
// only the credentials that are carried in authentication headers instead.
func mediaRequestUri(u *url.URL) string {
	u2 := *u
	u2.User = nil
	return u2.String()
}

func newTunnelCookie() (string, error) {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(b[:]), nil
}

// httpTunnel holds the two HTTP connections of an RTSP-over-HTTP session.
//
// getConn/brconn are the downstream channel: raw RTSP replies and '$'
// interleaved media, delimited by the connection closing. postConn is the
// upstream channel: every RTSP request is base64 encoded on its own, with
// padding, and streamed as the body of a single POST.
type httpTunnel struct {
	endpoint *url.URL
	dialAddr string
	cookie   string

	getConn  *connWithTimeout
	brconn   *bufio.Reader
	postConn *connWithTimeout

	writeMu sync.Mutex // serializes POST body writes

	mu      sync.Mutex
	written int // encoded bytes already delivered to the POST body
	timeout time.Duration
	closed  bool
	err     error
}

// handshakeGET opens the downstream channel. It reads only the HTTP response
// head; any bytes already buffered past it (a first RTSP reply or media
// block) stay in the reader handed to the Client.
func (t *httpTunnel) handshakeGET(timeout time.Duration) error {
	t.getConn.Timeout = timeout
	req := fmt.Sprintf("GET %s HTTP/1.0\r\n"+
		"Host: %s\r\n"+
		"x-sessioncookie: %s\r\n"+
		"Accept: application/x-rtsp-tunnelled\r\n"+
		"\r\n",
		requestTarget(t.endpoint), t.endpoint.Host, t.cookie)
	if err := writeHTTPBody(t.getConn, []byte(req)); err != nil {
		return err
	}

	br := bufio.NewReader(t.getConn)
	status, headers, err := readHTTPResponseHead(br)
	if err != nil {
		return err
	}
	if status != 200 {
		return fmt.Errorf("rtsp: http tunnel GET rejected with status %d", status)
	}
	if _, ok := headers["transfer-encoding"]; ok {
		return fmt.Errorf("rtsp: unsupported http tunnel transfer-encoding %q", headers["transfer-encoding"])
	}
	if _, ok := headers["content-length"]; ok {
		return fmt.Errorf("rtsp: unsupported length-delimited http tunnel body")
	}
	if ct := headers["content-type"]; !strings.Contains(strings.ToLower(ct), "application/x-rtsp-tunnelled") {
		return fmt.Errorf("rtsp: unexpected http tunnel content type %q", ct)
	}

	t.brconn = br
	return nil
}

// handshakePOST opens the upstream channel. It deliberately does not wait for
// an HTTP response: gateways may answer only when the tunnel ends.
func (t *httpTunnel) handshakePOST(timeout time.Duration) error {
	t.postConn.Timeout = timeout
	req := fmt.Sprintf("POST %s HTTP/1.0\r\n"+
		"Host: %s\r\n"+
		"x-sessioncookie: %s\r\n"+
		"Content-Type: application/x-rtsp-tunnelled\r\n"+
		"Content-Length: %d\r\n"+
		"\r\n",
		requestTarget(t.endpoint), t.endpoint.Host, t.cookie, maxHTTPPostPayload)
	return writeHTTPBody(t.postConn, []byte(req))
}

// requestTarget returns the path and query that belong to the HTTP endpoint.
func requestTarget(u *url.URL) string {
	target := u.EscapedPath()
	if target == "" {
		target = "/"
	}
	if u.RawQuery != "" {
		target += "?" + u.RawQuery
	}
	return target
}

// writeHTTPBody writes p completely, tolerating short writes from the
// connection and reporting an error instead of silently dropping bytes.
func writeHTTPBody(w io.Writer, p []byte) error {
	for len(p) > 0 {
		n, err := w.Write(p)
		if n > 0 {
			p = p[n:]
		}
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
	}
	return nil
}

// readHTTPResponseHead parses a plain HTTP/1.x status line and headers. Only
// status and headers are consumed; extra bytes remain buffered in br.
func readHTTPResponseHead(br *bufio.Reader) (status int, headers map[string]string, err error) {
	line, err := readHTTPLine(br)
	if err != nil {
		return 0, nil, err
	}
	fields := strings.Fields(line)
	if len(fields) < 2 || !strings.HasPrefix(fields[0], "HTTP/1.") {
		return 0, nil, fmt.Errorf("rtsp: invalid http response %q", line)
	}
	status, err = strconv.Atoi(fields[1])
	if err != nil {
		return 0, nil, fmt.Errorf("rtsp: invalid http status %q", line)
	}
	headers = make(map[string]string)
	for {
		line, err = readHTTPLine(br)
		if err != nil {
			return 0, nil, err
		}
		if line == "" {
			return status, headers, nil
		}
		key, val, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		headers[strings.ToLower(strings.TrimSpace(key))] = strings.TrimSpace(val)
	}
}

func readHTTPLine(br *bufio.Reader) (string, error) {
	line, err := br.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimRight(line, "\r\n"), nil
}

// writeRequest encodes one complete RTSP request as its own padded base64
// blob and streams it to the POST body. Encoding and writing are separate
// steps: a short write retries the remaining encoded bytes, never re-encodes
// them. A retried request (after a 401) gets its own independent blob.
//
// The POST response is not awaited: some gateways answer only when the
// tunnel ends, so RTSP must flow as soon as the bytes are written.
func (t *httpTunnel) writeRequest(raw []byte, timeout time.Duration) error {
	enc := make([]byte, base64.StdEncoding.EncodedLen(len(raw)))
	base64.StdEncoding.Encode(enc, raw)

	t.mu.Lock()
	if t.closed {
		err := t.err
		t.mu.Unlock()
		if err == nil {
			err = net.ErrClosed
		}
		return err
	}
	if t.written+len(enc) > maxHTTPPostPayload {
		t.mu.Unlock()
		return fmt.Errorf("rtsp: http tunnel upload exceeds %d bytes", maxHTTPPostPayload)
	}
	t.written += len(enc)
	t.timeout = timeout
	t.mu.Unlock()

	if err := t.writeEncoded(enc, timeout); err != nil {
		wrapped := fmt.Errorf("rtsp: http tunnel POST write: %w", err)
		t.fail(wrapped)
		return wrapped
	}
	return nil
}

// writeEncoded writes already encoded request bytes under the write deadline
// that belongs to the POST connection.
func (t *httpTunnel) writeEncoded(enc []byte, timeout time.Duration) error {
	t.writeMu.Lock()
	defer t.writeMu.Unlock()

	if timeout > 0 {
		t.postConn.Conn.SetWriteDeadline(time.Now().Add(timeout))
	} else {
		t.postConn.Conn.SetWriteDeadline(time.Time{})
	}
	return writeHTTPBody(t.postConn, enc)
}

// watchPOST consumes an optional HTTP response on the POST connection. The
// RTSP session does not depend on it, but an explicit rejection is surfaced
// instead of being mistaken for an RTSP reply.
func (t *httpTunnel) watchPOST() {
	br := bufio.NewReader(t.postConn)
	status, _, err := readHTTPResponseHead(br)
	if err != nil {
		// No HTTP response, connection closed, or non-HTTP traffic.
		return
	}
	if status < 200 || status >= 300 {
		t.fail(fmt.Errorf("rtsp: http tunnel POST rejected with status %d", status))
		return
	}
	_, _ = io.Copy(io.Discard, br)
}

// fail records a terminal error and releases both halves of the pair.
func (t *httpTunnel) fail(err error) {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return
	}
	t.closed = true
	if t.err == nil {
		t.err = err
	}
	getConn, postConn := t.getConn, t.postConn
	t.mu.Unlock()

	if postConn != nil {
		postConn.Conn.Close()
	}
	if getConn != nil {
		getConn.Conn.Close()
	}
}

// close tears down both halves of the pair, waking blocked readers and
// writers. It is safe to call repeatedly.
func (t *httpTunnel) close() error {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return nil
	}
	t.closed = true
	getConn, postConn := t.getConn, t.postConn
	t.mu.Unlock()

	if postConn != nil {
		postConn.Conn.Close()
	}
	if getConn != nil {
		getConn.Conn.Close()
	}
	return nil
}

// terminalError reports the first transport error recorded by the tunnel.
func (t *httpTunnel) terminalError() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.err
}

// noteReadError releases the upstream half when the downstream channel fails
// for a reason other than a read timeout. Timeouts only end the pending
// operation, leaving the session available for the next read.
func (t *httpTunnel) noteReadError(err error) {
	if ne, ok := err.(net.Error); ok && ne.Timeout() {
		return
	}
	t.fail(err)
}
