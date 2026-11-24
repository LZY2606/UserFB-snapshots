package rtsp

// Tests for the RTSP-over-HTTP tunnel. A local gateway accepts the two HTTP
// connections, decodes the base64 RTSP requests arriving on the POST channel,
// and scripts RTSP replies plus '$' interleaved media on the GET channel.

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"fmt"
	"io"
	"net"
	"net/textproto"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/deepch/vdk/task_support/taskcheck"
)

const testTunnelSDP = "v=0\r\n" +
	"m=audio 0 RTP/AVP 8\r\n" +
	"a=rtpmap:8 PCMA/8000/1\r\n" +
	"a=control:track0\r\n"

// ---------------------------------------------------------------------------
// Local gateway.
// ---------------------------------------------------------------------------

type testServerOptions struct {
	getStatus           int
	getContentType      string
	omitContentType     bool
	getTransferEncoding string
	getContentLength    string
	getDelay            time.Duration
	getInitial          []byte // sent right after the GET response head
	postResponse        []byte // optional HTTP response on the POST channel
	fragment            int    // split every GET write into chunks of this size
}

type testHTTPHead struct {
	Method string
	Target string
	Proto  string
	Header textproto.MIMEHeader
}

func (h testHTTPHead) cookie() string {
	return h.Header.Get("x-sessioncookie")
}

type testTunnelSession struct {
	server *testTunnelServer
	cookie string
	get    net.Conn

	mu     sync.Mutex
	cond   *sync.Cond
	buf    []byte // undecoded base64 quad remainder
	raw    []byte // raw base64 body bytes received on POST
	req    []byte // decoded RTSP request bytes, in arrival order
	blobs  int    // count of base64 quads terminated by padding
	postOK bool
	err    error
}

type testTunnelServer struct {
	t        *testing.T
	ln       net.Listener
	addr     string
	endpoint string
	opts     testServerOptions

	getHeads  chan testHTTPHead
	postHeads chan testHTTPHead
	pairErr   chan error

	mu       sync.Mutex
	cond     *sync.Cond
	sessions map[string]*testTunnelSession
	active   int32
	conns    []net.Conn
}

func newTestTunnelServer(t *testing.T, opts testServerOptions) *testTunnelServer {
	t.Helper()
	if opts.getStatus == 0 {
		opts.getStatus = 200
	}
	if opts.getContentType == "" && !opts.omitContentType {
		opts.getContentType = "application/x-rtsp-tunnelled"
	}
	ln, addr := listenTestGateway(t)
	s := &testTunnelServer{
		t:         t,
		ln:        ln,
		addr:      addr,
		endpoint:  "http://" + addr + "/gateway/cam?channel=1",
		opts:      opts,
		getHeads:  make(chan testHTTPHead, 8),
		postHeads: make(chan testHTTPHead, 8),
		pairErr:   make(chan error, 8),
		sessions:  make(map[string]*testTunnelSession),
	}
	s.cond = sync.NewCond(&s.mu)
	go s.acceptLoop()
	t.Cleanup(s.close)
	return s
}

// listenTestGateway prefers a real loopback TCP listener. When the host
// forbids binding local ports, an in-memory listener built from net.Pipe
// pairs stands in so the same gateway logic can still run; the ordinary TCP
// path is what is exercised whenever it is available.
func listenTestGateway(t *testing.T) (net.Listener, string) {
	t.Helper()
	if ln, err := net.Listen("tcp", "127.0.0.1:0"); err == nil {
		return ln, ln.Addr().String()
	}
	ml := newMemListener()
	addr := fmt.Sprintf("mem-%d", atomic.AddInt64(&memSeq, 1))
	memRegistry.Store(addr+":80", ml)
	installMemDial()
	return ml, addr
}

func (s *testTunnelServer) close() {
	s.ln.Close()
	s.mu.Lock()
	for _, c := range s.conns {
		c.Close()
	}
	s.mu.Unlock()
}

func (s *testTunnelServer) acceptLoop() {
	for {
		conn, err := s.ln.Accept()
		if err != nil {
			return
		}
		atomic.AddInt32(&s.active, 1)
		s.mu.Lock()
		s.conns = append(s.conns, conn)
		s.mu.Unlock()
		go s.serveConn(conn)
	}
}

func (s *testTunnelServer) serveConn(conn net.Conn) {
	defer func() {
		conn.Close()
		atomic.AddInt32(&s.active, -1)
	}()

	br := bufio.NewReader(conn)
	head, err := readTestHTTPHead(br)
	if err != nil {
		return
	}
	switch head.Method {
	case "GET":
		s.getHeads <- head
		s.serveGET(conn, head)
	case "POST":
		s.postHeads <- head
		s.servePOST(conn, br, head)
	}
}

func readTestHTTPHead(br *bufio.Reader) (testHTTPHead, error) {
	line, err := br.ReadString('\n')
	if err != nil {
		return testHTTPHead{}, err
	}
	line = strings.TrimRight(line, "\r\n")
	fields := strings.Fields(line)
	if len(fields) < 3 {
		return testHTTPHead{}, fmt.Errorf("bad request line %q", line)
	}
	header, err := textproto.NewReader(br).ReadMIMEHeader()
	if err != nil {
		return testHTTPHead{}, err
	}
	return testHTTPHead{Method: fields[0], Target: fields[1], Proto: fields[2], Header: header}, nil
}

func (s *testTunnelServer) serveGET(conn net.Conn, head testHTTPHead) {
	if s.opts.getDelay > 0 {
		time.Sleep(s.opts.getDelay)
	}

	var resp strings.Builder
	fmt.Fprintf(&resp, "HTTP/1.0 %d %s\r\n", s.opts.getStatus, testStatusText(s.opts.getStatus))
	if !s.opts.omitContentType {
		resp.WriteString("Content-Type: " + s.opts.getContentType + "\r\n")
	}
	if s.opts.getTransferEncoding != "" {
		resp.WriteString("Transfer-Encoding: " + s.opts.getTransferEncoding + "\r\n")
	}
	if s.opts.getContentLength != "" {
		resp.WriteString("Content-Length: " + s.opts.getContentLength + "\r\n")
	}
	resp.WriteString("\r\n")
	if head.cookie() == "" {
		s.pairErr <- fmt.Errorf("GET without session cookie")
		return
	}

	sess := &testTunnelSession{server: s, cookie: head.cookie(), get: conn}
	sess.cond = sync.NewCond(&sess.mu)

	// The response head is written before the session becomes visible, so
	// downstream RTSP replies never interleave with it. The POST half may
	// arrive at any time afterwards and waits for this session to appear.
	payload := append([]byte(resp.String()), s.opts.getInitial...)
	if err := s.writeFragmented(conn, payload); err != nil {
		s.pairErr <- err
		return
	}
	if s.opts.getStatus != 200 {
		return
	}

	s.mu.Lock()
	s.sessions[head.cookie()] = sess
	s.cond.Broadcast()
	s.mu.Unlock()

	// Keep the downstream half open until the client closes it.
	_, _ = io.Copy(io.Discard, conn)

	s.mu.Lock()
	delete(s.sessions, head.cookie())
	s.mu.Unlock()
}

func (s *testTunnelServer) servePOST(conn net.Conn, br *bufio.Reader, head testHTTPHead) {
	cookie := head.cookie()
	if cookie == "" {
		s.pairErr <- fmt.Errorf("POST without session cookie")
		return
	}

	s.mu.Lock()
	deadline := time.Now().Add(5 * time.Second)
	timer := time.AfterFunc(5*time.Second, func() {
		s.mu.Lock()
		s.cond.Broadcast()
		s.mu.Unlock()
	})
	for s.sessions[cookie] == nil && time.Now().Before(deadline) {
		s.cond.Wait()
	}
	timer.Stop()
	sess := s.sessions[cookie]
	s.mu.Unlock()
	if sess == nil {
		s.pairErr <- fmt.Errorf("POST cookie %q does not match any GET", cookie)
		return
	}
	sess.mu.Lock()
	sess.postOK = true
	sess.cond.Broadcast()
	sess.mu.Unlock()

	if s.opts.postResponse != nil {
		if err := testWriteAll(conn, s.opts.postResponse); err != nil {
			s.pairErr <- err
			return
		}
	}
	s.decodePOST(sess, br)
}

// decodePOST decodes the POST body incrementally, four base64 characters at a
// time. Independent padded encodings concatenate cleanly this way, so a
// re-encoded leftover or a double-encoded body shows up as garbage in req.
func (s *testTunnelServer) decodePOST(sess *testTunnelSession, br *bufio.Reader) {
	chunk := make([]byte, 4096)
	for {
		n, err := br.Read(chunk)
		if n > 0 {
			sess.mu.Lock()
			sess.raw = append(sess.raw, chunk[:n]...)
			sess.buf = append(sess.buf, chunk[:n]...)
			sess.decodeLocked()
			sess.cond.Broadcast()
			sess.mu.Unlock()
		}
		if err != nil {
			return
		}
	}
}

func (sess *testTunnelSession) decodeLocked() {
	var dst [3]byte
	for len(sess.buf) >= 4 {
		quad := sess.buf[:4]
		n, err := base64.StdEncoding.Decode(dst[:], quad)
		if err != nil {
			sess.err = err
			sess.server.pairErr <- err
			return
		}
		sess.req = append(sess.req, dst[:n]...)
		if quad[2] == '=' || quad[3] == '=' {
			sess.blobs++
		}
		sess.buf = sess.buf[4:]
	}
}

func testStatusText(status int) string {
	switch status {
	case 200:
		return "OK"
	case 401:
		return "Unauthorized"
	case 403:
		return "Forbidden"
	case 404:
		return "Not Found"
	case 500:
		return "Internal Server Error"
	}
	return "Status"
}

func testWriteAll(w io.Writer, p []byte) error {
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

func (s *testTunnelServer) writeFragmented(conn net.Conn, p []byte) error {
	if s.opts.fragment > 0 && len(p) > s.opts.fragment {
		for len(p) > 0 {
			chunk := p
			if len(chunk) > s.opts.fragment {
				chunk = chunk[:s.opts.fragment]
			}
			if err := testWriteAll(conn, chunk); err != nil {
				return err
			}
			p = p[len(chunk):]
		}
		return nil
	}
	return testWriteAll(conn, p)
}

func splitRequests(req []byte) []string {
	s := string(req)
	var out []string
	for {
		i := strings.Index(s, "\r\n\r\n")
		if i < 0 {
			break
		}
		out = append(out, s[:i+4])
		s = s[i+4:]
	}
	return out
}

// waitRequests blocks until n complete requests have been decoded.
func (sess *testTunnelSession) waitRequests(n int) ([]string, error) {
	sess.mu.Lock()
	defer sess.mu.Unlock()
	deadline := time.Now().Add(5 * time.Second)
	for len(splitRequests(sess.req)) < n && sess.err == nil {
		if time.Now().After(deadline) {
			return nil, fmt.Errorf("timed out waiting for %d requests, have %d", n, len(splitRequests(sess.req)))
		}
		sess.cond.Wait()
	}
	return splitRequests(sess.req), sess.err
}

// snapshot returns the decoded requests and a copy of the raw POST body so a
// test can compare the body with independently computed encodings.
func (sess *testTunnelSession) snapshot() ([]string, []byte, int) {
	sess.mu.Lock()
	defer sess.mu.Unlock()
	return splitRequests(sess.req), append([]byte(nil), sess.raw...), sess.blobs
}

// waitSession blocks until the GET half carrying cookie has been accepted and
// returns its session.
func (srv *testTunnelServer) waitSession(t *testing.T, cookie string) *testTunnelSession {
	t.Helper()
	srv.mu.Lock()
	defer srv.mu.Unlock()
	deadline := time.Now().Add(5 * time.Second)
	timer := time.AfterFunc(5*time.Second, func() {
		srv.mu.Lock()
		srv.cond.Broadcast()
		srv.mu.Unlock()
	})
	defer timer.Stop()
	for srv.sessions[cookie] == nil {
		if !time.Now().Before(deadline) {
			t.Fatalf("no tunnel session for cookie %q", cookie)
		}
		srv.cond.Wait()
	}
	return srv.sessions[cookie]
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

func testInterleavedBlock(channel byte, payload []byte) []byte {
	b := make([]byte, 4+len(payload))
	b[0] = '$'
	b[1] = channel
	b[2] = byte(len(payload) >> 8)
	b[3] = byte(len(payload))
	copy(b[4:], payload)
	return b
}

func testRTPPayload(seq uint16, payload []byte) []byte {
	b := make([]byte, 12+len(payload))
	b[0] = 0x80
	b[1] = 8
	b[2] = byte(seq >> 8)
	b[3] = byte(seq)
	copy(b[12:], payload)
	return b
}

func testRTSPResponse(cseq int, status int, headers map[string]string, body string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "RTSP/1.0 %d %s\r\n", status, testStatusText(status))
	fmt.Fprintf(&b, "CSeq: %d\r\n", cseq)
	for k, v := range headers {
		fmt.Fprintf(&b, "%s: %s\r\n", k, v)
	}
	if body != "" {
		fmt.Fprintf(&b, "Content-Length: %d\r\n", len(body))
	}
	b.WriteString("\r\n")
	b.WriteString(body)
	return b.String()
}

func errText(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

func waitActive(t *testing.T, srv *testTunnelServer, want int32) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for atomic.LoadInt32(&srv.active) > want {
		if time.Now().After(deadline) {
			t.Fatalf("connections still active: %d (want <= %d)", atomic.LoadInt32(&srv.active), want)
		}
		time.Sleep(2 * time.Millisecond)
	}
}

func expectNoPairErr(t *testing.T, srv *testTunnelServer) {
	t.Helper()
	select {
	case err := <-srv.pairErr:
		t.Errorf("gateway pairing error: %v", err)
	case <-time.After(100 * time.Millisecond):
	}
}

// expectedBody joins the independently padded encoding of every decoded
// request. If the tunnel re-encoded leftovers or merged requests into an
// unterminated stream, the raw body would differ.
func expectedBody(reqs []string) []byte {
	var out []byte
	for _, req := range reqs {
		out = append(out, []byte(base64.StdEncoding.EncodeToString([]byte(req)))...)
	}
	return out
}

// shortWriteConn accepts only a few bytes per Write call, producing legal
// short writes that the tunnel must loop over rather than drop.
type shortWriteConn struct {
	net.Conn
	limit int
}

func (c *shortWriteConn) Write(p []byte) (int, error) {
	if len(p) > c.limit {
		p = p[:c.limit]
	}
	return c.Conn.Write(p)
}

// ---------------------------------------------------------------------------
// In-memory transport used only when the host forbids binding TCP ports.
// ---------------------------------------------------------------------------

var (
	memSeq      int64
	memRegistry sync.Map // dial address -> *memListener
	memDialOnce sync.Once
)

type memListener struct {
	conns  chan net.Conn
	closed chan struct{}
	once   sync.Once
}

func newMemListener() *memListener {
	return &memListener{conns: make(chan net.Conn, 16), closed: make(chan struct{})}
}

func (l *memListener) Accept() (net.Conn, error) {
	select {
	case c := <-l.conns:
		return c, nil
	case <-l.closed:
		return nil, net.ErrClosed
	}
}

func (l *memListener) Close() error {
	l.once.Do(func() { close(l.closed) })
	return nil
}

func (l *memListener) Addr() net.Addr { return memAddr("mem") }

type memAddr string

func (a memAddr) Network() string { return "mem" }
func (a memAddr) String() string  { return string(a) }

func installMemDial() {
	memDialOnce.Do(func() {
		old := httpTunnelDial
		httpTunnelDial = func(network, addr string, timeout time.Duration) (net.Conn, error) {
			if v, ok := memRegistry.Load(addr); ok {
				client, server := memPipe()
				select {
				case v.(*memListener).conns <- server:
					return client, nil
				default:
					client.Close()
					server.Close()
					return nil, fmt.Errorf("mem gateway backlog full")
				}
			}
			return old(network, addr, timeout)
		}
	})
}

// installDialSeam replaces httpTunnelDial for the duration of a test.
func installDialSeam(t *testing.T, seam func(network, addr string, timeout time.Duration) (net.Conn, error)) {
	t.Helper()
	old := httpTunnelDial
	httpTunnelDial = seam
	t.Cleanup(func() { httpTunnelDial = old })
}

// ---------------------------------------------------------------------------
// TestHTTPTunnelSession: two connections, cookie pairing, 401 retry, media.
// ---------------------------------------------------------------------------

func TestHTTPTunnelSession(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		srv := newTestTunnelServer(t, testServerOptions{getStatus: 200})
		uri := "rtsp://user:pass@192.0.2.10:8554/live/ch0"

		cli, err := DialHTTPTimeout(uri, srv.endpoint, 2*time.Second)
		c.Check(err == nil, "DialHTTPTimeout: %v", err)
		if err != nil {
			return
		}
		defer cli.Close()
		cli.RtspTimeout = 2 * time.Second

		head := <-srv.getHeads
		c.Check(head.Target == "/gateway/cam?channel=1", "GET target %q", head.Target)
		c.Check(head.Header.Get("Accept") == "application/x-rtsp-tunnelled", "GET accept %q", head.Header.Get("Accept"))
		c.Check(head.cookie() != "", "GET cookie is empty")
		c.Check(head.Header.Get("Host") == srv.addr, "GET host %q", head.Header.Get("Host"))

		sess := srv.waitSession(t, head.cookie())

		// The gateway answers DESCRIBE 401 first, then expects the retry.
		go func() {
			sess.waitRequests(1)
			srv.writeFragmented(sess.get, []byte(testRTSPResponse(1, 401, map[string]string{
				"WWW-Authenticate": `Digest realm="cam", nonce="abc"`,
			}, "")))
			sess.waitRequests(2)
			srv.writeFragmented(sess.get, []byte(testRTSPResponse(2, 200, nil, testTunnelSDP)))
			sess.waitRequests(3)
			srv.writeFragmented(sess.get, []byte(testRTSPResponse(3, 200, map[string]string{"Session": "sess42"}, "")))
			sess.waitRequests(4)
			srv.writeFragmented(sess.get, []byte(testRTSPResponse(4, 200, nil, "")))
			payload := []byte{1, 2, 3, 4, 5, 6}
			srv.writeFragmented(sess.get, testInterleavedBlock(0, testRTPPayload(1, payload)))
		}()

		streams, err := cli.Describe()
		c.Check(err == nil, "Describe: %v", err)
		c.Check(len(streams) == 1, "Describe streams %d", len(streams))

		// POST channel: same endpoint target and Host, same non-empty
		// cookie, tunnel content type and fixed upstream length.
		postHead := <-srv.postHeads
		c.Check(postHead.Header.Get("x-sessioncookie") == head.cookie(), "cookie mismatch GET %q POST %q", head.cookie(), postHead.Header.Get("x-sessioncookie"))
		c.Check(postHead.Header.Get("Content-Type") == "application/x-rtsp-tunnelled", "POST content type %q", postHead.Header.Get("Content-Type"))
		c.Check(postHead.Header.Get("Content-Length") == "32767", "POST length %q", postHead.Header.Get("Content-Length"))
		c.Check(postHead.Target == "/gateway/cam?channel=1", "POST target %q", postHead.Target)
		c.Check(postHead.Header.Get("Host") == srv.addr, "POST host %q", postHead.Header.Get("Host"))

		reqs, err := sess.waitRequests(2)
		c.Check(err == nil, "request decode: %v", err)
		if len(reqs) >= 2 {
			c.Check(strings.HasPrefix(reqs[0], "DESCRIBE rtsp://192.0.2.10:8554/live/ch0 RTSP/1.0\r\n"), "first request %q", reqs[0])
			c.Check(strings.Contains(reqs[0], "CSeq: 1\r\n"), "first CSeq missing in %q", reqs[0])
			c.Check(!strings.Contains(reqs[0], "Authorization:"), "first request should not authenticate: %q", reqs[0])
			c.Check(strings.HasPrefix(reqs[1], "DESCRIBE rtsp://192.0.2.10:8554/live/ch0 RTSP/1.0\r\n"), "retry request %q", reqs[1])
			c.Check(strings.Contains(reqs[1], "CSeq: 2\r\n"), "retry CSeq missing in %q", reqs[1])
			c.Check(strings.Contains(reqs[1], "Authorization: Digest"), "retry not authenticated: %q", reqs[1])
		}

		if err := cli.SetupAll(); err != nil {
			c.Check(false, "SetupAll: %v", err)
			return
		}
		if err := cli.Play(); err != nil {
			c.Check(false, "Play: %v", err)
			return
		}

		pkt, err := cli.ReadPacket()
		c.Check(err == nil, "ReadPacket: %v", err)
		c.Check(pkt.Idx == 0, "packet idx %d", pkt.Idx)
		c.Check(bytes.Equal(pkt.Data, []byte{1, 2, 3, 4, 5, 6}), "packet data %x", pkt.Data)

		reqs, err = sess.waitRequests(4)
		c.Check(err == nil, "setup/play decode: %v", err)
		if len(reqs) >= 4 {
			c.Check(strings.HasPrefix(reqs[2], "SETUP rtsp://192.0.2.10:8554/live/ch0/track0 RTSP/1.0\r\n"), "setup %q", reqs[2])
			c.Check(!strings.Contains(reqs[2], "Session:"), "SETUP before session should not carry it: %q", reqs[2])
			c.Check(strings.HasPrefix(reqs[3], "PLAY rtsp://192.0.2.10:8554/live/ch0 RTSP/1.0\r\n"), "play %q", reqs[3])
			c.Check(strings.Contains(reqs[3], "Session: sess42\r\n"), "play session missing in %q", reqs[3])
		}

		// Each complete request travelled as its own independently padded
		// base64 blob; nothing was re-encoded.
		reqs, raw, _ := sess.snapshot()
		c.Check(bytes.Equal(raw, expectedBody(reqs)), "POST body does not match per-request encodings")
		if len(reqs) > 0 {
			last := reqs[len(reqs)-1]
			enc := []byte(base64.StdEncoding.EncodeToString([]byte(last)))
			c.Check(bytes.HasSuffix(raw, enc), "last request not independently encoded at body end")
		}

		// A second client gets a different cookie and its own pairing.
		cli2, err := DialHTTPTimeout(uri, srv.endpoint, 2*time.Second)
		c.Check(err == nil, "second DialHTTPTimeout: %v", err)
		if err == nil {
			head2 := <-srv.getHeads
			postHead2 := <-srv.postHeads
			c.Check(head2.cookie() != head.cookie(), "cookies reused across clients: %q", head2.cookie())
			c.Check(postHead2.Header.Get("x-sessioncookie") == head2.cookie(), "second client POST cookie %q, GET %q", postHead2.Header.Get("x-sessioncookie"), head2.cookie())
			cli2.RtspTimeout = 2 * time.Second
			cli2.Close()
		}
		expectNoPairErr(t, srv)

		c.Check(len(streams) == 1 && streams[0].Control == "track0", "stream control %+v", streams)
	})
}

// ---------------------------------------------------------------------------
// TestHTTPTunnelFraming: downstream buffering and arbitrary fragmentation.
// ---------------------------------------------------------------------------

func TestHTTPTunnelFraming(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The GET response head is followed immediately by the first RTSP
		// reply and an interleaved block, everything in tiny fragments.
		media := testInterleavedBlock(0, testRTPPayload(7, []byte{0xaa, 0xbb, 0xcc, 0xdd}))
		initial := append([]byte(testRTSPResponse(1, 200, nil, testTunnelSDP)), media...)
		srv := newTestTunnelServer(t, testServerOptions{
			getStatus:  200,
			getInitial: initial,
			fragment:   2,
		})

		cli, err := DialHTTPTimeout("rtsp://127.0.0.1/live", srv.endpoint, 2*time.Second)
		c.Check(err == nil, "DialHTTPTimeout: %v", err)
		if err != nil {
			return
		}
		defer cli.Close()
		cli.RtspTimeout = 2 * time.Second

		head := <-srv.getHeads
		sess := srv.waitSession(t, head.cookie())
		c.Check(head.cookie() == sess.cookie, "GET cookie %q session %q", head.cookie(), sess.cookie)

		// Bytes read past the HTTP head must stay with the RTSP receiver.
		streams, err := cli.Describe()
		c.Check(err == nil, "Describe from buffered bytes: %v", err)
		c.Check(len(streams) == 1, "streams %d", len(streams))

		res, err := cli.poll()
		c.Check(err == nil, "poll buffered media: %v", err)
		c.Check(bytes.Equal(res.Block, media), "buffered block %x want %x", res.Block, media)
		<-srv.postHeads

		// Upstream framing: short writes must not lose bytes, and each RTSP
		// request must still arrive as its own padded blob. Plain TCP is the
		// control run.
		runFramingRound(t, c, "plain")
		installDialSeam(t, func(network, addr string, timeout time.Duration) (net.Conn, error) {
			base, derr := memOrDial(network, addr, timeout)
			if derr != nil {
				return nil, derr
			}
			return &shortWriteConn{Conn: base, limit: 5}, nil
		})
		runFramingRound(t, c, "short")
	})
}

// memOrDial resolves in-memory gateway addresses, falling back to real TCP.
func memOrDial(network, addr string, timeout time.Duration) (net.Conn, error) {
	if v, ok := memRegistry.Load(addr); ok {
		client, server := memPipe()
		select {
		case v.(*memListener).conns <- server:
			return client, nil
		default:
			client.Close()
			server.Close()
			return nil, fmt.Errorf("mem gateway backlog full")
		}
	}
	return (&net.Dialer{Timeout: timeout}).Dial(network, addr)
}

// memPipe returns two connected in-memory net.Conn values. Unlike net.Pipe,
// each direction buffers writes, so a gateway can script replies and media
// ahead of the reader, matching a real socket's kernel buffer.
func memPipe() (*memConn, *memConn) {
	ab := newPipeBuffer()
	ba := newPipeBuffer()
	a := &memConn{r: ba, w: ab}
	b := &memConn{r: ab, w: ba}
	return a, b
}

type memConn struct {
	r  *pipeBuffer
	w  *pipeBuffer
	id int64
}

func (c *memConn) Read(p []byte) (int, error)  { return c.r.read(p) }
func (c *memConn) Write(p []byte) (int, error) { return c.w.write(p) }
func (c *memConn) Close() error {
	c.r.closeRead()
	c.w.closeWrite()
	return nil
}
func (c *memConn) LocalAddr() net.Addr  { return memAddr("mem") }
func (c *memConn) RemoteAddr() net.Addr { return memAddr("mem") }
func (c *memConn) SetDeadline(t time.Time) error {
	c.r.setReadDeadline(t)
	c.w.setWriteDeadline(t)
	return nil
}
func (c *memConn) SetReadDeadline(t time.Time) error  { c.r.setReadDeadline(t); return nil }
func (c *memConn) SetWriteDeadline(t time.Time) error { c.w.setWriteDeadline(t); return nil }

type pipeBuffer struct {
	mu        sync.Mutex
	cond      *sync.Cond
	buf       []byte
	eof       bool
	err       error
	rdeadline time.Time
	wdeadline time.Time
}

func newPipeBuffer() *pipeBuffer {
	b := &pipeBuffer{}
	b.cond = sync.NewCond(&b.mu)
	return b
}

func (b *pipeBuffer) read(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	for {
		if len(b.buf) > 0 {
			n := copy(p, b.buf)
			b.buf = b.buf[n:]
			b.cond.Broadcast()
			return n, nil
		}
		if b.eof {
			if b.err != nil {
				return 0, b.err
			}
			return 0, io.EOF
		}
		if !b.rdeadline.IsZero() && !time.Now().Before(b.rdeadline) {
			return 0, os.ErrDeadlineExceeded
		}
		stop := b.wakeAtLocked(b.rdeadline)
		b.cond.Wait()
		stop()
	}
}

func (b *pipeBuffer) write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.eof {
		if b.err != nil {
			return 0, b.err
		}
		return 0, net.ErrClosed
	}
	if !b.wdeadline.IsZero() && !time.Now().Before(b.wdeadline) {
		return 0, os.ErrDeadlineExceeded
	}
	b.buf = append(b.buf, p...)
	b.cond.Broadcast()
	return len(p), nil
}

// wakeAtLocked arranges for cond to be broadcast when the deadline passes.
func (b *pipeBuffer) wakeAtLocked(deadline time.Time) func() {
	if deadline.IsZero() {
		return func() {}
	}
	d := time.Until(deadline)
	if d <= 0 {
		return func() {}
	}
	timer := time.AfterFunc(d, func() {
		b.mu.Lock()
		b.cond.Broadcast()
		b.mu.Unlock()
	})
	return func() { timer.Stop() }
}

func (b *pipeBuffer) closeRead() {
	b.mu.Lock()
	b.eof = true
	b.err = net.ErrClosed
	b.cond.Broadcast()
	b.mu.Unlock()
}

func (b *pipeBuffer) closeWrite() {
	b.mu.Lock()
	b.eof = true
	b.cond.Broadcast()
	b.mu.Unlock()
}

func (b *pipeBuffer) setReadDeadline(t time.Time) {
	b.mu.Lock()
	b.rdeadline = t
	b.cond.Broadcast()
	b.mu.Unlock()
}

func (b *pipeBuffer) setWriteDeadline(t time.Time) {
	b.mu.Lock()
	b.wdeadline = t
	b.cond.Broadcast()
	b.mu.Unlock()
}

func runFramingRound(t *testing.T, c *taskcheck.C, label string) {
	srv := newTestTunnelServer(t, testServerOptions{getStatus: 200})
	cli, err := DialHTTPTimeout("rtsp://127.0.0.1/live", srv.endpoint, 2*time.Second)
	c.Check(err == nil, "%s DialHTTPTimeout: %v", label, err)
	if err != nil {
		return
	}
	defer cli.Close()
	cli.RtspTimeout = 2 * time.Second
	head := <-srv.getHeads
	sess := srv.waitSession(t, head.cookie())

	go func() {
		if _, err := sess.waitRequests(1); err != nil {
			return
		}
		srv.writeFragmented(sess.get, []byte(testRTSPResponse(1, 200, nil, testTunnelSDP)))
	}()

	if err := cli.WriteRequest(Request{Method: "DESCRIBE", Uri: cli.requestUri}); err != nil {
		c.Check(false, "%s first WriteRequest: %v", label, err)
		return
	}
	if _, err := cli.ReadResponse(); err != nil {
		c.Check(false, "%s first ReadResponse: %v", label, err)
	}
	if err := cli.WriteRequest(Request{Method: "DESCRIBE", Uri: cli.requestUri}); err != nil {
		c.Check(false, "%s second WriteRequest: %v", label, err)
		return
	}

	reqs, err := sess.waitRequests(2)
	c.Check(err == nil, "%s decode: %v", label, err)
	if len(reqs) >= 2 {
		c.Check(strings.HasPrefix(reqs[0], "DESCRIBE "), "%s first request %q", label, reqs[0])
		c.Check(strings.HasPrefix(reqs[1], "DESCRIBE "), "%s second request %q", label, reqs[1])
		c.Check(reqs[0] != reqs[1], "%s identical requests suggest duplication", label)
	}
	reqs, raw, _ := sess.snapshot()
	c.Check(bytes.Equal(raw, expectedBody(reqs)), "%s POST body does not match per-request encodings", label)
	for i, req := range reqs {
		enc := base64.StdEncoding.EncodeToString([]byte(req))
		c.Check(strings.Contains(string(raw), enc), "%s request %d not encoded independently", label, i)
	}
}

// ---------------------------------------------------------------------------
// TestHTTPTunnelLifetime: close, timeouts and failure cleanup.
// ---------------------------------------------------------------------------

func TestHTTPTunnelLifetime(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Close tears down both halves, wakes a blocked read, and is safe to
		// repeat.
		srv := newTestTunnelServer(t, testServerOptions{getStatus: 200})
		cli, err := DialHTTPTimeout("rtsp://127.0.0.1/live", srv.endpoint, 2*time.Second)
		c.Check(err == nil, "DialHTTPTimeout: %v", err)
		if err == nil {
			<-srv.getHeads
			<-srv.postHeads

			blocked := make(chan error, 1)
			go func() {
				_, rerr := cli.ReadResponse()
				blocked <- rerr
			}()
			time.Sleep(50 * time.Millisecond)
			c.Check(cli.Close() == nil, "first close")
			c.Check(cli.Close() == nil, "repeated close")
			select {
			case rerr := <-blocked:
				c.Check(rerr != nil, "blocked read returned nil error")
			case <-time.After(3 * time.Second):
				c.Check(false, "Close did not wake the blocked read")
			}
			waitActive(t, srv, 0)
		}

		// A read timeout only ends that pending read; the session survives.
		srv2 := newTestTunnelServer(t, testServerOptions{getStatus: 200})
		cli2, err := DialHTTPTimeout("rtsp://127.0.0.1/live", srv2.endpoint, 2*time.Second)
		c.Check(err == nil, "timeout DialHTTPTimeout: %v", err)
		if err == nil {
			head := <-srv2.getHeads
			sess := srv2.waitSession(t, head.cookie())
			cli2.RtspTimeout = 150 * time.Millisecond

			go func() {
				sess.waitRequests(1)
				srv2.writeFragmented(sess.get, []byte(testRTSPResponse(1, 200, nil, testTunnelSDP)))
				sess.waitRequests(2)
				srv2.writeFragmented(sess.get, []byte(testRTSPResponse(2, 200, map[string]string{"Session": "s1"}, "")))
				sess.waitRequests(3)
				srv2.writeFragmented(sess.get, []byte(testRTSPResponse(3, 200, nil, "")))
			}()

			_, err = cli2.Describe()
			c.Check(err == nil, "timeout Describe: %v", err)
			c.Check(cli2.SetupAll() == nil, "timeout SetupAll")
			c.Check(cli2.Play() == nil, "timeout Play")

			// No data: the pending read times out and nothing else breaks.
			_, err = cli2.ReadPacket()
			ne, ok := err.(net.Error)
			c.Check(ok && ne.Timeout(), "expected timeout error, got %v", err)

			// The same session then delivers media.
			payload := []byte{9, 8, 7}
			c.Check(srv2.writeFragmented(sess.get, testInterleavedBlock(0, testRTPPayload(2, payload))) == nil, "write after timeout")
			cli2.RtspTimeout = 2 * time.Second
			pkt, err := cli2.ReadPacket()
			c.Check(err == nil, "read after timeout: %v", err)
			c.Check(bytes.Equal(pkt.Data, payload), "packet after timeout %x", pkt.Data)
			cli2.Close()
			waitActive(t, srv2, 0)
		}

		// GET rejected with an HTTP status: explicit error, no RTSP reply is
		// fabricated, and the half already held is released.
		rejected := newTestTunnelServer(t, testServerOptions{getStatus: 403})
		_, err = DialHTTPTimeout("rtsp://127.0.0.1/live", rejected.endpoint, 2*time.Second)
		c.Check(err != nil, "GET 403 must fail")
		c.Check(strings.Contains(errText(err), "403"), "GET 403 error %q", errText(err))
		waitActive(t, rejected, 0)

		// Unsupported downstream framing is refused explicitly.
		framingCases := []testServerOptions{
			{getStatus: 200, omitContentType: true},
			{getStatus: 200, getContentType: "application/octet-stream"},
			{getStatus: 200, getContentLength: "10"},
			{getStatus: 200, getTransferEncoding: "chunked"},
		}
		for i, opts := range framingCases {
			bad := newTestTunnelServer(t, opts)
			_, derr := DialHTTPTimeout("rtsp://127.0.0.1/live", bad.endpoint, 2*time.Second)
			c.Check(derr != nil, "unsupported framing case %d must fail", i)
			waitActive(t, bad, 0)
		}

		// GET handshake timeout: error and release.
		slow := newTestTunnelServer(t, testServerOptions{getStatus: 200, getDelay: 500 * time.Millisecond})
		_, err = DialHTTPTimeout("rtsp://127.0.0.1/live", slow.endpoint, 100*time.Millisecond)
		c.Check(err != nil, "GET handshake timeout must fail")
		if ne, ok := err.(net.Error); ok {
			c.Check(ne.Timeout(), "handshake error should be a timeout: %v", err)
		}
		time.Sleep(700 * time.Millisecond)
		waitActive(t, slow, 0)

		// POST establishment failure releases the GET half already held.
		postFail := newTestTunnelServer(t, testServerOptions{getStatus: 200})
		var dials int32
		installDialSeam(t, func(network, addr string, timeout time.Duration) (net.Conn, error) {
			if atomic.AddInt32(&dials, 1) == 2 {
				return nil, fmt.Errorf("synthetic post dial failure")
			}
			return memOrDial(network, addr, timeout)
		})
		_, err = DialHTTPTimeout("rtsp://127.0.0.1/live", postFail.endpoint, 2*time.Second)
		c.Check(err != nil && strings.Contains(errText(err), "synthetic post dial failure"), "POST dial failure %q", errText(err))
		waitActive(t, postFail, 0)

		// HTTP rejection on the POST channel is surfaced as an error and
		// releases the GET half, never treated as an RTSP status.
		postReject := newTestTunnelServer(t, testServerOptions{
			getStatus:    200,
			postResponse: []byte("HTTP/1.0 403 Forbidden\r\n\r\n"),
		})
		cli3, err := DialHTTPTimeout("rtsp://127.0.0.1/live", postReject.endpoint, 2*time.Second)
		c.Check(err == nil, "POST reject DialHTTPTimeout: %v", err)
		if err == nil {
			cli3.RtspTimeout = time.Second
			_, werr := cli3.Describe()
			c.Check(werr != nil && strings.Contains(errText(werr), "403"), "POST 403 surfaced as %q", errText(werr))
			cli3.Close()
			waitActive(t, postReject, 0)
		}
	})
}
