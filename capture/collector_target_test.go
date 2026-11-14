package capture_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/capture"
	"github.com/mafredri/cdp/protocol/fetch"
	cdpio "github.com/mafredri/cdp/protocol/io"
	"github.com/mafredri/cdp/protocol/network"
	"github.com/mafredri/cdp/rpcc"
	"github.com/mafredri/cdp/task_support/taskcheck"
)

// rpcCall is a decoded JSON-RPC request from the mock server.
type rpcCall struct {
	ID     int             `json:"id"`
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

type rpcErrBody struct {
	Code    int64  `json:"code"`
	Message string `json:"message"`
}

// mockServer is a scriptable JSON-RPC peer over a net.Pipe. Every
// request is dispatched in its own goroutine so that a blocked reply
// cannot stall other requests, exactly like a real DevTools target.
type mockServer struct {
	conn net.Conn

	writeMu sync.Mutex
	enc     *json.Encoder

	mu    sync.Mutex
	calls []rpcCall

	closeOnce sync.Once
	closed    chan struct{}

	onCall func(s *mockServer, call rpcCall)
}

func newMockServer(conn net.Conn, onCall func(*mockServer, rpcCall)) *mockServer {
	return &mockServer{
		conn:   conn,
		enc:    json.NewEncoder(conn),
		closed: make(chan struct{}),
		onCall: onCall,
	}
}

func (s *mockServer) serve() error {
	dec := json.NewDecoder(s.conn)
	for {
		var call rpcCall
		if err := dec.Decode(&call); err != nil {
			return err
		}
		s.mu.Lock()
		s.calls = append(s.calls, call)
		s.mu.Unlock()
		go s.onCall(s, call)
	}
}

func (s *mockServer) sendResult(id int, result any) {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	if result == nil {
		result = map[string]any{}
	}
	_ = s.enc.Encode(map[string]any{"id": id, "result": result})
}

func (s *mockServer) sendRPCError(id int, code int64, message string) {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	_ = s.enc.Encode(map[string]any{"id": id, "error": rpcErrBody{Code: code, Message: message}})
}

func (s *mockServer) event(method string, params any) {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	_ = s.enc.Encode(map[string]any{"method": method, "params": params})
}

func (s *mockServer) recorded() []rpcCall {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]rpcCall, len(s.calls))
	copy(out, s.calls)
	return out
}

func (s *mockServer) count(method string) int {
	n := 0
	for _, c := range s.recorded() {
		if c.Method == method {
			n++
		}
	}
	return n
}

func (s *mockServer) indexOf(method string) int {
	for i, c := range s.recorded() {
		if c.Method == method {
			return i
		}
	}
	return -1
}

func (s *mockServer) paramsOf(method string) []json.RawMessage {
	var out []json.RawMessage
	for _, c := range s.recorded() {
		if c.Method == method {
			out = append(out, c.Params)
		}
	}
	return out
}

func (s *mockServer) Close() {
	s.closeOnce.Do(func() {
		close(s.closed)
		_ = s.conn.Close()
	})
}

// dialPipe wires a rpcc connection to a mock server over net.Pipe.
func dialPipe(t *testing.T, onCall func(*mockServer, rpcCall)) (*cdp.Client, *mockServer, func()) {
	t.Helper()
	clientConn, serverConn := net.Pipe()
	server := newMockServer(serverConn, onCall)
	serverDone := make(chan error, 1)
	go func() { serverDone <- server.serve() }()

	dialCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	conn, err := rpcc.DialContext(dialCtx, "pipe", rpcc.WithDialer(
		func(context.Context, string) (io.ReadWriteCloser, error) { return clientConn, nil }))
	if err != nil {
		cancel()
		server.Close()
		t.Fatalf("dial: %v", err)
	}

	cleanup := func() {
		cancel()
		_ = conn.Close()
		server.Close()
		<-serverDone
	}
	return cdp.NewClient(conn), server, cleanup
}

func waitSignal(t *testing.T, ch <-chan struct{}, what string) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(5 * time.Second):
		t.Fatalf("timed out waiting for %s", what)
	}
}

// waitFor polls for an observable condition with a hard deadline. It is
// reserved for effects that carry no protocol handshake — specifically
// the atomic sample-directory rename, which is itself the documented
// publication point. All wire-level sequencing uses channel handshakes.
func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.NewTimer(5 * time.Second)
	defer deadline.Stop()
	tick := time.NewTicker(2 * time.Millisecond)
	defer tick.Stop()
	if cond() {
		return
	}
	for {
		select {
		case <-tick.C:
			if cond() {
				return
			}
		case <-deadline.C:
			t.Fatalf("timed out waiting for %s", what)
		}
	}
}

func pausedEvent(id, method, url string, status int, headers []fetch.HeaderEntry) fetch.RequestPausedReply {
	return fetch.RequestPausedReply{
		RequestID:          fetch.RequestID(id),
		Request:            network.Request{Method: method, URL: url},
		ResourceType:       network.ResourceTypeXHR,
		ResponseStatusCode: &status,
		ResponseHeaders:    headers,
	}
}

func sampleDirFor(id string) string {
	sum := sha256.Sum256([]byte(id))
	return hex.EncodeToString(sum[:])
}

// fakeIOClient is an in-process IO domain client used to exercise the
// stream readers without a network peer.
type fakeIOClient struct {
	mu         sync.Mutex
	replies    []cdpio.ReadReply
	readArgs   []*cdpio.ReadArgs
	closeCount int
	closeErr   error
}

func (f *fakeIOClient) Read(_ context.Context, args *cdpio.ReadArgs) (*cdpio.ReadReply, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	cp := *args
	f.readArgs = append(f.readArgs, &cp)
	if len(f.replies) == 0 {
		return nil, io.EOF
	}
	reply := f.replies[0]
	f.replies = f.replies[1:]
	return &reply, nil
}

func (f *fakeIOClient) Close(context.Context, *cdpio.CloseArgs) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closeCount++
	return f.closeErr
}

func b64Reply(data string, eof bool) cdpio.ReadReply {
	encoded := true
	return cdpio.ReadReply{Data: data, Base64Encoded: &encoded, EOF: eof}
}

func textReply(data string, eof bool) cdpio.ReadReply {
	return cdpio.ReadReply{Data: data, EOF: eof}
}

func TestCaptureReader(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("old reader sends offset and sequential peer rejects it", func(t *testing.T) {
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "IO.read":
					var args cdpio.ReadArgs
					if err := json.Unmarshal(call.Params, &args); err != nil {
						t.Error(err)
					}
					if args.Offset != nil {
						s.sendRPCError(call.ID, -32602, "sequential stream forbids offset")
						return
					}
					s.sendResult(call.ID, map[string]any{"data": "ab", "eof": false})
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			reader := client.NewIOStreamReader(context.Background(), "body")
			_, err := reader.Read(make([]byte, 8))
			var protocolErr *rpcc.ResponseError
			c.Check(errors.As(err, &protocolErr), "offset read must return a protocol response error, got %v", err)
			c.Check(protocolErr != nil && protocolErr.Code == -32602, "expected code -32602, got %v", protocolErr)
			c.Check(server.count("IO.read") == 1, "expected one IO.read, got %d", server.count("IO.read"))
			var args cdpio.ReadArgs
			params := server.paramsOf("IO.read")
			c.Check(len(params) == 1, "expected one recorded IO.read")
			c.Check(json.Unmarshal(params[0], &args) == nil && args.Offset != nil, "old reader must send offset")
			c.Check(reader.Close() == nil, "close")
		})

		t.Run("nil body omits field while empty slice sends empty body", func(t *testing.T) {
			nilWire, err := json.Marshal(fetch.NewFulfillRequestArgs("r", 200))
			c.Check(err == nil, "marshal nil body")
			var nilFields map[string]json.RawMessage
			c.Check(json.Unmarshal(nilWire, &nilFields) == nil, "decode nil body wire")
			_, present := nilFields["body"]
			c.Check(!present, "nil Body must omit body field")

			emptyWire, err := json.Marshal(fetch.NewFulfillRequestArgs("r", 200).SetBody([]byte{}))
			c.Check(err == nil, "marshal empty body")
			var emptyFields map[string]json.RawMessage
			c.Check(json.Unmarshal(emptyWire, &emptyFields) == nil, "decode empty body wire")
			raw, present := emptyFields["body"]
			c.Check(present, "non-nil empty Body must keep body field")
			c.Check(string(raw) == `""`, `empty body must encode as "", got %s`, raw)

			binWire, err := json.Marshal(fetch.NewFulfillRequestArgs("r", 200).SetBody([]byte{1, 2, 3}))
			c.Check(err == nil, "marshal binary body")
			var binFields map[string]json.RawMessage
			c.Check(json.Unmarshal(binWire, &binFields) == nil, "decode binary body wire")
			c.Check(string(binFields["body"]) == `"`+base64.StdEncoding.EncodeToString([]byte{1, 2, 3})+`"`,
				"body must be base64 encoded exactly once")
		})

		t.Run("sequential reader omits offset and decodes mixed chunks", func(t *testing.T) {
			fake := &fakeIOClient{replies: []cdpio.ReadReply{
				textReply("ab", false),
				b64Reply("AP8=", true),
			}}
			reader := cdpio.NewSequentialStreamReader(context.Background(), fake, "h")

			got, err := io.ReadAll(reader)
			c.Check(err == nil, "read all: %v", err)
			c.Check(bytes.Equal(got, []byte{'a', 'b', 0, 255}), "mixed text/base64 decode, got %x", got)
			for _, a := range fake.readArgs {
				c.Check(a.Offset == nil, "sequential IO.read must omit offset")
			}
			c.Check(len(fake.readArgs) == 2, "expected two IO.read calls, got %d", len(fake.readArgs))
			c.Check(reader.Close() == nil, "close")
		})

		t.Run("chunk larger than buffer and EOF carrying data", func(t *testing.T) {
			fake := &fakeIOClient{replies: []cdpio.ReadReply{
				textReply("abcdef", false),
				textReply("xy", true),
			}}
			reader := cdpio.NewSequentialStreamReader(context.Background(), fake, "h")

			var got []byte
			small := make([]byte, 2)
			for {
				n, err := reader.Read(small)
				got = append(got, small[:n]...)
				if err == io.EOF {
					break
				}
				c.Check(err == nil, "unexpected read error: %v", err)
			}
			c.Check(string(got) == "abcdefxy", "small buffer assembly, got %q", got)
			c.Check(len(fake.readArgs) == 2, "buffered chunk must not trigger extra IO.read, got %d", len(fake.readArgs))
		})

		t.Run("zero length read returns nothing without a remote call", func(t *testing.T) {
			fake := &fakeIOClient{replies: []cdpio.ReadReply{textReply("abc", true)}}
			reader := cdpio.NewSequentialStreamReader(context.Background(), fake, "h")

			n, err := reader.Read(nil)
			c.Check(n == 0 && err == nil, "zero-length Read must return (0, nil), got (%d, %v)", n, err)
			c.Check(len(fake.readArgs) == 0, "zero-length Read must not issue IO.read")

			got, err := io.ReadAll(reader)
			c.Check(err == nil && string(got) == "abc", "subsequent read: %q %v", got, err)
		})

		t.Run("small buffers over base64 chunks", func(t *testing.T) {
			want := []byte{0, 1, 2, 3, 4, 5}
			fake := &fakeIOClient{replies: []cdpio.ReadReply{
				b64Reply(base64.StdEncoding.EncodeToString(want), true),
			}}
			reader := cdpio.NewSequentialStreamReader(context.Background(), fake, "h")

			var got []byte
			one := make([]byte, 1)
			for {
				n, err := reader.Read(one)
				got = append(got, one[:n]...)
				if err == io.EOF {
					break
				}
				c.Check(err == nil, "unexpected read error: %v", err)
			}
			c.Check(bytes.Equal(got, want), "byte-by-byte base64 decode, got %x", got)
		})

		t.Run("corrupt base64 is reported as error", func(t *testing.T) {
			fake := &fakeIOClient{replies: []cdpio.ReadReply{
				b64Reply("!!!not-base64", true),
			}}
			reader := cdpio.NewSequentialStreamReader(context.Background(), fake, "h")
			_, err := io.ReadAll(reader)
			c.Check(err != nil, "corrupt base64 must error")
		})

		t.Run("close is idempotent", func(t *testing.T) {
			fake := &fakeIOClient{}
			reader := cdpio.NewSequentialStreamReader(context.Background(), fake, "h")
			c.Check(reader.Close() == nil, "first close")
			c.Check(reader.Close() == nil, "second close")
			c.Check(fake.closeCount == 1, "IO.close must run once, got %d", fake.closeCount)
		})

		t.Run("parallel streams progress while one read is suspended", func(t *testing.T) {
			releaseSlow := make(chan struct{})
			slowReadStarted := make(chan struct{})
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "IO.read":
					var args cdpio.ReadArgs
					if err := json.Unmarshal(call.Params, &args); err != nil {
						t.Error(err)
					}
					c.Check(args.Offset == nil, "parallel sequential reads must omit offset")
					switch args.Handle {
					case "slow":
						select {
						case slowReadStarted <- struct{}{}:
						default:
						}
						select {
						case <-releaseSlow:
							s.sendResult(call.ID, map[string]any{"data": "s", "eof": true})
						case <-s.closed:
						}
					case "fast":
						s.sendResult(call.ID, map[string]any{"data": "f", "eof": true})
					default:
						s.sendRPCError(call.ID, -32602, "unknown handle")
					}
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx := context.Background()
			slow := client.NewSequentialIOStreamReader(ctx, "slow")
			fast := client.NewSequentialIOStreamReader(ctx, "fast")

			slowDone := make(chan struct{})
			var slowBody []byte
			var slowErr error
			go func() {
				defer close(slowDone)
				slowBody, slowErr = io.ReadAll(slow)
			}()
			waitSignal(t, slowReadStarted, "suspended IO.read")

			fastDone := make(chan struct{})
			go func() {
				defer close(fastDone)
				body, err := io.ReadAll(fast)
				c.Check(err == nil && string(body) == "f", "fast stream completes while slow is suspended, got %q %v", body, err)
				c.Check(fast.Close() == nil, "fast stream closes while slow is suspended")
			}()
			waitSignal(t, fastDone, "fast stream completion")
			c.Check(server.count("IO.close") == 1, "only the fast handle closed so far, got %d", server.count("IO.close"))

			close(releaseSlow)
			waitSignal(t, slowDone, "slow stream after release")
			c.Check(slowErr == nil && string(slowBody) == "s", "slow stream after release, got %q %v", slowBody, slowErr)
			c.Check(slow.Close() == nil, "close slow reader")
			c.Check(server.count("IO.close") == 2, "both handles closed once, got %d", server.count("IO.close"))
		})
	})
}

func TestCapturePublish(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("invalid options return before any protocol command", func(t *testing.T) {
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				s.sendResult(call.ID, nil)
			})
			defer cleanup()

			empty := t.TempDir()
			cases := []capture.Options{
				{Dir: empty, Workers: 0, MaxBodyBytes: 1},
				{Dir: empty, Workers: 1, MaxBodyBytes: 0},
				{Dir: empty, Workers: -1, MaxBodyBytes: -1},
				{Dir: filepath.Join(t.TempDir(), "missing"), Workers: 1, MaxBodyBytes: 1},
			}
			for _, opts := range cases {
				n, err := capture.Run(context.Background(), client, opts)
				c.Check(err != nil && n == 0, "invalid options must error without publishing, opts=%+v", opts)
			}
			n, err := capture.Run(context.Background(), nil, capture.Options{Dir: empty, Workers: 1, MaxBodyBytes: 1})
			c.Check(err != nil && n == 0, "nil client must error")

			nonEmpty := t.TempDir()
			c.Check(os.WriteFile(filepath.Join(nonEmpty, "x"), []byte("y"), 0o600) == nil, "seed non-empty dir")
			n, err = capture.Run(context.Background(), client, capture.Options{Dir: nonEmpty, Workers: 1, MaxBodyBytes: 1})
			c.Check(err != nil && n == 0, "non-empty dir must error")
			c.Check(server.count("Fetch.enable") == 0, "no protocol command allowed on invalid options")
		})

		t.Run("sample is invisible until fulfill and complete afterwards", func(t *testing.T) {
			dir := t.TempDir()
			headers := []fetch.HeaderEntry{
				{Name: "X-Dup", Value: "a"},
				{Name: "X-Dup", Value: "b"},
				{Name: "Content-Length", Value: "999"},
				{Name: "Transfer-Encoding", Value: "chunked"},
				{Name: "Content-Type", Value: "text/plain"},
			}
			event := pausedEvent("req-1", "GET", "https://example.com/a", 200, headers)
			fulfillStarted := make(chan struct{})
			releaseFulfill := make(chan struct{})

			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					var args fetch.EnableArgs
					c.Check(json.Unmarshal(call.Params, &args) == nil, "decode enable args")
					c.Check(len(args.Patterns) == 1, "one pattern")
					c.Check(args.Patterns[0].URLPattern != nil && *args.Patterns[0].URLPattern == "*", "urlPattern *")
					c.Check(args.Patterns[0].RequestStage == fetch.RequestStageResponse, "response stage")
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h1"})
				case "IO.read":
					var args cdpio.ReadArgs
					c.Check(json.Unmarshal(call.Params, &args) == nil, "decode read args")
					c.Check(args.Offset == nil, "capture must read sequentially")
					if args.Handle != "h1" {
						s.sendRPCError(call.ID, -32602, "unknown handle")
						return
					}
					if s.count("IO.read") == 1 {
						s.sendResult(call.ID, map[string]any{"data": "hello "})
					} else {
						s.sendResult(call.ID, map[string]any{"data": base64.StdEncoding.EncodeToString([]byte("world!")), "base64Encoded": true, "eof": true})
					}
				case "IO.close":
					s.sendResult(call.ID, nil)
				case "Fetch.fulfillRequest":
					close(fulfillStarted)
					<-releaseFulfill
					s.sendResult(call.ID, nil)
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			var n int
			var runErr error
			go func() {
				n, runErr = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 2, MaxBodyBytes: 1 << 20})
				close(done)
			}()

			waitSignal(t, fulfillStarted, "blocked fulfillRequest")
			final := filepath.Join(dir, sampleDirFor("req-1"))
			entries, err := os.ReadDir(dir)
			c.Check(err == nil, "read output dir")
			_, statErr := os.Stat(final)
			c.Check(os.IsNotExist(statErr), "final sample must not exist before fulfill succeeds")
			for _, e := range entries {
				c.Check(strings.HasPrefix(e.Name(), ".tmp-"), "only temp dirs visible before publish, found %q", e.Name())
			}

			close(releaseFulfill)
			// Wait for the atomic publish (rename), the effect with no
			// wire handshake, before canceling — otherwise cancellation
			// could race the fulfill response and lose the sample.
			waitFor(t, "sample publication", func() bool {
				_, err := os.Stat(final)
				return err == nil
			})
			cancel()
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("Run did not return after cancel")
			}
			c.Check(n == 1, "one published sample, got %d", n)
			c.Check(errors.Is(runErr, context.Canceled), "cancel-driven shutdown error: %v", runErr)

			body, err := os.ReadFile(filepath.Join(final, "body.bin"))
			c.Check(err == nil, "body.bin present")
			c.Check(string(body) == "hello world!", "archived body, got %q", body)

			rawMeta, err := os.ReadFile(filepath.Join(final, "meta.json"))
			c.Check(err == nil, "meta.json present")
			var m map[string]any
			c.Check(json.Unmarshal(rawMeta, &m) == nil, "meta decodes")
			c.Check(m["request_id"] == "req-1", "meta request_id")
			c.Check(m["url"] == "https://example.com/a", "meta url")
			c.Check(m["status"] == float64(200), "meta status")
			c.Check(m["size"] == float64(len("hello world!")), "meta size")
			wantSum := sha256.Sum256([]byte("hello world!"))
			c.Check(m["sha256"] == hex.EncodeToString(wantSum[:]), "meta sha256")

			// Headers keep the original order and duplicates.
			metaHeaders, ok := m["headers"].([]any)
			c.Check(ok && len(metaHeaders) == 5, "original header array archived, got %v", m["headers"])
			firstDup, _ := json.Marshal(metaHeaders[0])
			secondDup, _ := json.Marshal(metaHeaders[1])
			c.Check(string(firstDup) == `{"name":"X-Dup","value":"a"}` && string(secondDup) == `{"name":"X-Dup","value":"b"}`,
				"duplicate headers retained in order: %s %s", firstDup, secondDup)

			// The fulfillment reworks headers and re-encodes the body once.
			fulfillParams := server.paramsOf("Fetch.fulfillRequest")
			c.Check(len(fulfillParams) == 1, "one fulfillRequest")
			var fulfill fetch.FulfillRequestArgs
			c.Check(json.Unmarshal(fulfillParams[0], &fulfill) == nil, "decode fulfill args")
			c.Check(fulfill.ResponseCode == 200, "fulfill status")
			c.Check(bytes.Equal(fulfill.Body, []byte("hello world!")), "fulfill body bytes, got %x", fulfill.Body)
			var clValues []string
			var hasTE, hasDup bool
			for _, h := range fulfill.ResponseHeaders {
				switch h.Name {
				case "Content-Length":
					clValues = append(clValues, h.Value)
				case "Transfer-Encoding":
					hasTE = true
				case "X-Dup":
					hasDup = true
				}
			}
			c.Check(!hasTE, "Transfer-Encoding removed")
			c.Check(hasDup, "duplicate custom headers retained")
			c.Check(len(clValues) == 1 && clValues[0] == "12", "single recomputed Content-Length, got %v", clValues)
		})

		t.Run("non-capturable responses are continued without taking bodies", func(t *testing.T) {
			dir := t.TempDir()
			events := []fetch.RequestPausedReply{
				pausedEvent("post", "POST", "https://example.com/post", 200, nil),
				pausedEvent("redirect", "GET", "https://example.com/r", 301, nil),
				pausedEvent("gzip", "GET", "https://example.com/g", 200, []fetch.HeaderEntry{{Name: "Content-Encoding", Value: "gzip"}}),
				pausedEvent("identity", "GET", "https://example.com/i", 200, []fetch.HeaderEntry{{Name: "Content-Encoding", Value: "identity"}}),
			}
			fulfillSeen := make(chan struct{})
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					for _, ev := range events {
						s.event("Fetch.requestPaused", ev)
					}
				case "Fetch.fulfillRequest":
					s.sendResult(call.ID, nil)
					close(fulfillSeen)
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h"})
				case "IO.read":
					s.sendResult(call.ID, map[string]any{"data": "", "eof": true})
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			var n int
			go func() {
				n, _ = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 4, MaxBodyBytes: 10})
				close(done)
			}()

			waitSignal(t, fulfillSeen, "fulfill of captured identity response")
			waitFor(t, "identity sample publication", func() bool {
				_, err := os.Stat(filepath.Join(dir, sampleDirFor("identity")))
				return err == nil
			})
			cancel()
			<-done

			continued := server.paramsOf("Fetch.continueRequest")
			c.Check(len(continued) == 3, "three non-capturable events continued, got %d", len(continued))
			ids := map[string]bool{}
			for _, raw := range continued {
				var args fetch.ContinueRequestArgs
				c.Check(json.Unmarshal(raw, &args) == nil, "decode continue args")
				ids[string(args.RequestID)] = true
			}
			c.Check(ids["post"] && ids["redirect"] && ids["gzip"], "correct requests continued: %v", ids)
			c.Check(n == 1, "one sample published, got %d", n)
			entries, err := os.ReadDir(dir)
			c.Check(err == nil && len(entries) == 1 && entries[0].Name() == sampleDirFor("identity"), "only identity sample archived")
		})

		t.Run("empty body publishes empty file and empty fulfill body", func(t *testing.T) {
			dir := t.TempDir()
			event := pausedEvent("empty", "GET", "https://example.com/empty", 200, nil)
			fulfillSeen := make(chan struct{})
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.fulfillRequest":
					s.sendResult(call.ID, nil)
					close(fulfillSeen)
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h"})
				case "IO.read":
					s.sendResult(call.ID, map[string]any{"data": "", "eof": true})
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			var n int
			go func() {
				n, _ = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 1, MaxBodyBytes: 10})
				close(done)
			}()
			waitSignal(t, fulfillSeen, "empty body fulfill")
			waitFor(t, "empty sample publication", func() bool {
				_, err := os.Stat(filepath.Join(dir, sampleDirFor("empty")))
				return err == nil
			})
			cancel()
			<-done
			c.Check(n == 1, "empty body sample published")

			final := filepath.Join(dir, sampleDirFor("empty"))
			info, err := os.Stat(filepath.Join(final, "body.bin"))
			c.Check(err == nil, "body.bin exists")
			c.Check(info.Size() == 0, "body.bin is empty")

			params := server.paramsOf("Fetch.fulfillRequest")
			c.Check(len(params) == 1, "one fulfill")
			var fields map[string]json.RawMessage
			c.Check(json.Unmarshal(params[0], &fields) == nil, "fulfill wire decodes")
			raw, ok := fields["body"]
			c.Check(ok && string(raw) == `""`, "empty body must be sent as body=\"\", got %v", raw)
			var args fetch.FulfillRequestArgs
			c.Check(json.Unmarshal(params[0], &args) == nil, "fulfill args decode")
			c.Check(args.Body != nil && len(args.Body) == 0, "decoded body is non-nil empty slice")
		})

		t.Run("oversized body is rejected with ErrBodyTooLarge and not published", func(t *testing.T) {
			dir := t.TempDir()
			event := pausedEvent("big", "GET", "https://example.com/big", 200, nil)
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h"})
				case "IO.read":
					if s.count("IO.read") == 1 {
						s.sendResult(call.ID, map[string]any{"data": "ab"})
					} else {
						s.sendResult(call.ID, map[string]any{"data": "AP8=", "base64Encoded": true, "eof": true})
					}
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan struct{})
			var n int
			var err error
			go func() {
				n, err = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 1, MaxBodyBytes: 3})
				close(done)
			}()
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("Run did not stop on oversized body")
			}

			c.Check(errors.Is(err, capture.ErrBodyTooLarge), "run error must wrap ErrBodyTooLarge: %v", err)
			c.Check(n == 0, "oversized sample not published")
			entries, readErr := os.ReadDir(dir)
			c.Check(readErr == nil && len(entries) == 0, "temp dir removed, found %v", entries)
			c.Check(server.count("IO.close") == 1, "handle closed once, got %d", server.count("IO.close"))
			failParams := server.paramsOf("Fetch.failRequest")
			c.Check(len(failParams) == 1, "oversized request failed once, got %d", len(failParams))
			var failArgs fetch.FailRequestArgs
			c.Check(json.Unmarshal(failParams[0], &failArgs) == nil, "decode fail args")
			c.Check(string(failArgs.RequestID) == "big" && failArgs.ErrorReason == network.ErrorReasonAborted, "fail with Aborted")
			c.Check(server.count("Fetch.fulfillRequest") == 0, "oversized request never fulfilled")
		})

		t.Run("a suspended read does not block another worker from publishing", func(t *testing.T) {
			dir := t.TempDir()
			events := []fetch.RequestPausedReply{
				pausedEvent("slow", "GET", "https://example.com/slow", 200, nil),
				pausedEvent("fast", "GET", "https://example.com/fast", 200, nil),
			}
			slowReadStarted := make(chan struct{})
			releaseSlow := make(chan struct{})
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					for _, ev := range events {
						s.event("Fetch.requestPaused", ev)
					}
				case "Fetch.takeResponseBodyAsStream":
					var args fetch.TakeResponseBodyAsStreamArgs
					if err := json.Unmarshal(call.Params, &args); err != nil {
						t.Error(err)
					}
					s.sendResult(call.ID, map[string]any{"stream": "stream-" + string(args.RequestID)})
				case "IO.read":
					var args cdpio.ReadArgs
					if err := json.Unmarshal(call.Params, &args); err != nil {
						t.Error(err)
					}
					switch args.Handle {
					case "stream-slow":
						select {
						case slowReadStarted <- struct{}{}:
						default:
						}
						select {
						case <-releaseSlow:
							s.sendResult(call.ID, map[string]any{"data": "slow", "eof": true})
						case <-s.closed:
						}
					case "stream-fast":
						s.sendResult(call.ID, map[string]any{"data": "fast", "eof": true})
					default:
						s.sendRPCError(call.ID, -32602, "unknown handle")
					}
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			var n int
			go func() {
				n, _ = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 2, MaxBodyBytes: 1 << 20})
				close(done)
			}()

			waitSignal(t, slowReadStarted, "suspended slow IO.read")
			// The fast worker must be able to finish end to end
			// (read, close, fulfill, publish) while the slow read hangs.
			waitFor(t, "fast worker publication", func() bool {
				_, err := os.Stat(filepath.Join(dir, sampleDirFor("fast")))
				return err == nil
			})
			c.Check(server.count("Fetch.fulfillRequest") == 1, "fast response fulfilled while slow read hangs, got %d", server.count("Fetch.fulfillRequest"))

			// Slow is still suspended: it cannot have published yet.
			_, slowStat := os.Stat(filepath.Join(dir, sampleDirFor("slow")))
			c.Check(os.IsNotExist(slowStat), "slow sample must not exist while its read is suspended")

			close(releaseSlow)
			waitFor(t, "slow worker publication", func() bool {
				_, err := os.Stat(filepath.Join(dir, sampleDirFor("slow")))
				return err == nil
			})
			cancel()
			<-done
			c.Check(n == 2, "both samples published in completion order, got %d", n)
			fastBody, err := os.ReadFile(filepath.Join(dir, sampleDirFor("fast"), "body.bin"))
			c.Check(err == nil && string(fastBody) == "fast", "fast body archived")
			slowBody, err := os.ReadFile(filepath.Join(dir, sampleDirFor("slow"), "body.bin"))
			c.Check(err == nil && string(slowBody) == "slow", "slow body archived")
			c.Check(server.count("Fetch.failRequest") == 0, "no request failed")
		})
	})
}

func TestCaptureShutdown(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("cancel during suspended IO.read closes handle and fails request", func(t *testing.T) {
			dir := t.TempDir()
			event := pausedEvent("slow", "GET", "https://example.com/slow", 200, nil)
			readStarted := make(chan struct{})
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h"})
				case "IO.read":
					select {
					case readStarted <- struct{}{}:
					default:
					}
					// Never reply: cancellation must release the call.
					<-s.closed
				case "IO.close":
					s.sendResult(call.ID, nil)
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			var n int
			var err error
			go func() {
				n, err = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 1, MaxBodyBytes: 10})
				close(done)
			}()
			waitSignal(t, readStarted, "suspended IO.read")
			cancel()
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("Run did not shut down after cancel")
			}

			c.Check(errors.Is(err, context.Canceled), "canceled error chain: %v", err)
			c.Check(n == 0, "nothing published after cancel")
			c.Check(server.count("IO.close") == 1, "handle closed exactly once, got %d", server.count("IO.close"))
			c.Check(server.count("Fetch.failRequest") == 1, "paused request failed, got %d", server.count("Fetch.failRequest"))
			c.Check(server.count("Fetch.fulfillRequest") == 0, "no fulfill after cancel")
			c.Check(server.count("Fetch.disable") == 1, "Fetch disabled once, got %d", server.count("Fetch.disable"))
			c.Check(server.indexOf("IO.close") < server.indexOf("Fetch.disable"), "disable waits for worker cleanup")
			c.Check(server.indexOf("Fetch.failRequest") < server.indexOf("Fetch.disable"), "fail happens before disable")
			entries, readErr := os.ReadDir(dir)
			c.Check(readErr == nil && len(entries) == 0, "temp dirs cleaned, found %v", entries)
		})

		t.Run("take stream sent without handle only fails by request id", func(t *testing.T) {
			dir := t.TempDir()
			event := pausedEvent("nostream", "GET", "https://example.com/nostream", 200, nil)
			takeStarted := make(chan struct{})
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.takeResponseBodyAsStream":
					close(takeStarted)
					<-s.closed // Handle never arrives.
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			var err error
			go func() {
				_, err = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 1, MaxBodyBytes: 10})
				close(done)
			}()
			waitSignal(t, takeStarted, "suspended takeResponseBodyAsStream")
			cancel()
			<-done

			c.Check(errors.Is(err, context.Canceled), "canceled error chain: %v", err)
			failParams := server.paramsOf("Fetch.failRequest")
			c.Check(len(failParams) == 1, "request failed once, got %d", len(failParams))
			var args fetch.FailRequestArgs
			c.Check(json.Unmarshal(failParams[0], &args) == nil, "decode fail args")
			c.Check(string(args.RequestID) == "nostream" && args.ErrorReason == network.ErrorReasonAborted, "fail known request id with Aborted")
			c.Check(server.count("IO.close") == 0, "no handle means no IO.close, got %d", server.count("IO.close"))
			c.Check(server.count("IO.read") == 0, "no handle means no IO.read, got %d", server.count("IO.read"))
			c.Check(server.count("Fetch.disable") == 1, "disable still attempted")
		})

		t.Run("continued and fulfilled requests are not failed", func(t *testing.T) {
			dir := t.TempDir()
			events := []fetch.RequestPausedReply{
				pausedEvent("done", "GET", "https://example.com/done", 200, nil),
				pausedEvent("skipped", "POST", "https://example.com/skip", 200, nil),
			}
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					for _, ev := range events {
						s.event("Fetch.requestPaused", ev)
					}
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h"})
				case "IO.read":
					s.sendResult(call.ID, map[string]any{"data": "", "eof": true})
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			var n int
			go func() {
				n, _ = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 2, MaxBodyBytes: 10})
				close(done)
			}()
			// Wait for the stable end state of both kinds of work:
			// the skipped request continued and the capture published.
			waitFor(t, "both requests settled", func() bool {
				if server.count("Fetch.continueRequest") != 1 {
					return false
				}
				_, err := os.Stat(filepath.Join(dir, sampleDirFor("done")))
				return err == nil
			})
			cancel()
			<-done

			c.Check(n == 1, "captured sample published")
			c.Check(server.count("Fetch.failRequest") == 0, "settled requests are not failed")
			c.Check(server.count("Fetch.disable") == 1, "disable attempted")
		})

		t.Run("fulfilled request is not failed when publish rename fails", func(t *testing.T) {
			dir := t.TempDir()
			event := pausedEvent("rename", "GET", "https://example.com/rename", 200, nil)
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h"})
				case "IO.read":
					s.sendResult(call.ID, map[string]any{"data": "data", "eof": true})
				case "Fetch.fulfillRequest":
					// Occupy the final sample path so the publish rename fails.
					blocker := filepath.Join(dir, sampleDirFor("rename"))
					if err := os.WriteFile(blocker, []byte("x"), 0o600); err != nil {
						t.Error(err)
					}
					s.sendResult(call.ID, nil)
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan struct{})
			var n int
			var err error
			go func() {
				n, err = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 1, MaxBodyBytes: 10})
				close(done)
			}()
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("Run did not return after rename failure")
			}

			c.Check(err != nil, "rename failure surfaces")
			c.Check(n == 0, "failed rename is not counted")
			c.Check(server.count("Fetch.fulfillRequest") == 1, "fulfill happened")
			c.Check(server.count("Fetch.failRequest") == 0, "fulfilled request must not be failed")
			tmpRemaining := false
			entries, _ := os.ReadDir(dir)
			for _, e := range entries {
				if strings.HasPrefix(e.Name(), ".tmp-") {
					tmpRemaining = true
				}
			}
			c.Check(!tmpRemaining, "unpublished temp dir removed, entries=%v", entries)
		})

		t.Run("cleanup errors stay discoverable in the joined error", func(t *testing.T) {
			dir := t.TempDir()
			event := pausedEvent("big", "GET", "https://example.com/big", 200, nil)
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.takeResponseBodyAsStream":
					s.sendResult(call.ID, map[string]any{"stream": "h"})
				case "IO.read":
					s.sendResult(call.ID, map[string]any{"data": "toolarge", "eof": true})
				case "IO.close":
					s.sendRPCError(call.ID, -32000, "close failed")
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan struct{})
			var err error
			go func() {
				_, err = capture.Run(ctx, client, capture.Options{Dir: dir, Workers: 1, MaxBodyBytes: 3})
				close(done)
			}()
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("Run did not stop")
			}

			c.Check(errors.Is(err, capture.ErrBodyTooLarge), "body limit error preserved: %v", err)
			var protocolErr *rpcc.ResponseError
			c.Check(errors.As(err, &protocolErr), "protocol cleanup error preserved via errors.As: %v", err)
			c.Check(protocolErr != nil && protocolErr.Code == -32000, "close error code, got %v", protocolErr)
			c.Check(server.count("Fetch.failRequest") == 1, "request still failed after close error")
		})

		t.Run("connection close ends the run without panic and still disables", func(t *testing.T) {
			dir := t.TempDir()
			event := pausedEvent("drop", "GET", "https://example.com/drop", 200, nil)
			takeSeen := make(chan struct{})
			client, server, cleanup := dialPipe(t, func(s *mockServer, call rpcCall) {
				switch call.Method {
				case "Fetch.enable":
					s.sendResult(call.ID, nil)
					s.event("Fetch.requestPaused", event)
				case "Fetch.takeResponseBodyAsStream":
					close(takeSeen)
					s.Close()
				default:
					s.sendResult(call.ID, nil)
				}
			})
			defer cleanup()

			done := make(chan struct{})
			var n int
			var err error
			go func() {
				n, err = capture.Run(context.Background(), client, capture.Options{Dir: dir, Workers: 1, MaxBodyBytes: 10})
				close(done)
			}()
			waitSignal(t, takeSeen, "take stream before connection drop")
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("Run did not return after connection closed")
			}

			c.Check(err != nil, "connection closure reported as error")
			c.Check(n == 0, "nothing published through a dropped connection")
			// Fetch.disable is still attempted during shutdown, but the
			// closed connection makes rpcc fail locally so no wire
			// request is recorded. The closure error must remain
			// reachable through the returned error chain.
			var closed interface{ Closed() bool }
			c.Check(errors.As(err, &closed) && closed.Closed(), "error chain reports a closed connection: %v", err)
			c.Check(server.count("Fetch.disable") == 0, "closed connection cannot transmit disable, got %d", server.count("Fetch.disable"))
		})
	})
}
