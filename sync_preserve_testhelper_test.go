package cdp_test

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"testing"
	"time"

	"github.com/mafredri/cdp/rpcc"
)

// pipeCodec is a controllable JSON codec used by the SyncPreserve tests. It
// communicates over an in-memory net.Pipe and lets the test write
// notifications directly, bypassing any client-side subscription.
type pipeCodec struct {
	enc *json.Encoder
	dec *json.Decoder
}

func (c *pipeCodec) WriteRequest(r *rpcc.Request) error  { return c.enc.Encode(r) }
func (c *pipeCodec) ReadResponse(r *rpcc.Response) error { return c.dec.Decode(r) }

// testPeer is the server side of a cdp connection over net.Pipe. It reads
// requests and can interleave notifications with command replies.
type testPeer struct {
	t    testing.TB
	conn net.Conn
	mu   chan struct{} // Serializes writes from the test with replies.
}

// newTestConn dials an rpcc connection over net.Pipe. The peer responds to
// every request with an empty result and calls beforeReply before writing
// the reply, allowing the test to insert notifications at a precise point
// relative to a command (a receive barrier).
func newTestConn(t testing.TB, beforeReply func(peer *testPeer, req *rpcc.Request)) (*rpcc.Conn, *testPeer, context.CancelFunc) {
	t.Helper()

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)

	local, remote := net.Pipe()
	peer := &testPeer{t: t, conn: remote}
	peer.mu = make(chan struct{}, 1)
	peer.mu <- struct{}{}

	go func() {
		dec := json.NewDecoder(remote)
		for {
			var req rpcc.Request
			if err := dec.Decode(&req); err != nil {
				return
			}
			if beforeReply != nil {
				beforeReply(peer, &req)
			}
			<-peer.mu
			resp := map[string]interface{}{"id": req.ID, "result": map[string]interface{}{}}
			err := json.NewEncoder(remote).Encode(resp)
			peer.mu <- struct{}{}
			if err != nil {
				return
			}
		}
	}()

	conn, err := rpcc.DialContext(ctx, "pipe",
		rpcc.WithDialer(func(context.Context, string) (io.ReadWriteCloser, error) { return local, nil }),
		rpcc.WithCodec(func(r io.ReadWriter) rpcc.Codec {
			return &pipeCodec{enc: json.NewEncoder(r), dec: json.NewDecoder(r)}
		}),
	)
	if err != nil {
		cancel()
		t.Fatal(err)
	}

	return conn, peer, func() {
		conn.Close()
		cancel()
	}
}

// notify writes a notification over the pipe. It blocks until the bytes
// have been read by the connection, making it a reliable synchronization
// point.
func (p *testPeer) notify(method string, params interface{}) {
	p.t.Helper()
	p.write(map[string]interface{}{"method": method, "params": params})
}

func (p *testPeer) write(v interface{}) {
	p.t.Helper()
	<-p.mu
	err := json.NewEncoder(p.conn).Encode(v)
	p.mu <- struct{}{}
	if err != nil && err != io.ErrClosedPipe {
		p.t.Errorf("peer write: %v", err)
	}
}

// barrier sends an ordinary command and waits for its reply. Because the
// connection processes responses in order, all notifications written before
// the request have been dispatched to their streams when the barrier returns.
func barrier(t testing.TB, ctx context.Context, conn *rpcc.Conn) {
	t.Helper()
	if err := rpcc.Invoke(ctx, "Test.barrier", nil, nil, conn); err != nil {
		t.Fatalf("barrier: %v", err)
	}
}
