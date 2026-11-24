package marionette_client

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/njasm/marionette_client/task_support/taskcheck"
)

// recordedCommand captures one request frame observed by the mock Marionette
// peer, together with the connection generation it arrived on.
type recordedCommand struct {
	gen     int
	id      int
	command string
}

// peerTurn hands a decoded command's connection to the test and pauses the
// server read loop until the test has injected the reply, so the server never
// consumes bytes that are meant for the client.
type peerTurn struct {
	conn    net.Conn
	release chan struct{}
}

func (turn peerTurn) reply(frame string) {
	writeTestFrame(turn.conn, frame)
	close(turn.release)
}

// mockPeer is a local TCP Marionette peer used to drive the real transport
// and client without Firefox. Handshake and command ordering are coordinated
// through channels so the tests never rely on sleeps.
type mockPeer struct {
	listener net.Listener

	mu  sync.Mutex
	gen int
	log []recordedCommand

	// accepted receives one paused turn per decoded command frame.
	accepted chan peerTurn
	// recorded receives one entry per decoded command frame.
	recorded chan recordedCommand
	stopped  chan struct{}
}

func newMockPeer(t *testing.T, greeting string) *mockPeer {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("mock peer could not listen: %v", err)
	}

	m := &mockPeer{
		listener: listener,
		accepted: make(chan peerTurn, 256),
		recorded: make(chan recordedCommand, 256),
		stopped:  make(chan struct{}),
	}

	go func() {
		defer close(m.stopped)
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			m.mu.Lock()
			m.gen++
			gen := m.gen
			m.mu.Unlock()

			go m.serve(conn, gen, greeting)
		}
	}()

	t.Cleanup(m.Stop)
	return m
}

func (m *mockPeer) serve(conn net.Conn, gen int, greeting string) {
	defer func() { _ = conn.Close() }()
	if greeting != "" {
		writeTestFrame(conn, greeting)
	}
	for {
		body, err := readTestFrame(conn)
		if err != nil {
			return
		}
		var message []json.RawMessage
		if err = json.Unmarshal(body, &message); err != nil || len(message) < 3 {
			return
		}
		var id int
		if err = json.Unmarshal(message[1], &id); err != nil {
			return
		}
		var command string
		if err = json.Unmarshal(message[2], &command); err != nil {
			return
		}

		entry := recordedCommand{gen: gen, id: id, command: command}
		m.mu.Lock()
		m.log = append(m.log, entry)
		m.mu.Unlock()

		turn := peerTurn{conn: conn, release: make(chan struct{})}
		m.recorded <- entry
		m.accepted <- turn
		// Pause until the test has injected the reply for this command.
		<-turn.release
	}
}

func (m *mockPeer) Stop() {
	_ = m.listener.Close()
	<-m.stopped
}

func (m *mockPeer) port() int {
	return m.listener.Addr().(*net.TCPAddr).Port
}

// nextCommand blocks until the peer decodes another command frame.
func (m *mockPeer) nextCommand(t *testing.T) recordedCommand {
	t.Helper()
	select {
	case cmd := <-m.recorded:
		return cmd
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for a recorded command")
		return recordedCommand{}
	}
}

// accept returns the paused turn carrying the next decoded command.
func (m *mockPeer) accept(t *testing.T) peerTurn {
	t.Helper()
	select {
	case turn := <-m.accepted:
		return turn
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for the peer to record a command")
		return peerTurn{}
	}
}

// turn returns the next command's record and its paused turn together.
func (m *mockPeer) turn(t *testing.T) (recordedCommand, peerTurn) {
	t.Helper()
	return m.nextCommand(t), m.accept(t)
}

func (m *mockPeer) snapshot() []recordedCommand {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]recordedCommand, len(m.log))
	copy(out, m.log)
	return out
}

func writeTestFrame(conn net.Conn, payload string) {
	_, _ = fmt.Fprintf(conn, "%d:%s", len(payload), payload)
}

func readTestFrame(conn net.Conn) ([]byte, error) {
	var size []byte
	one := make([]byte, 1)
	for {
		if _, err := io.ReadFull(conn, one); err != nil {
			return nil, err
		}
		if one[0] == ':' {
			break
		}
		size = append(size, one[0])
	}
	n, err := strconv.Atoi(string(size))
	if err != nil {
		return nil, err
	}
	body := make([]byte, n)
	if _, err = io.ReadFull(conn, body); err != nil {
		return nil, err
	}
	return body, nil
}

func validGreeting() string {
	return `{"applicationType":"gecko","marionetteProtocol":3}`
}

func connectedClient(t *testing.T) (*Client, *MarionetteTransport, *mockPeer) {
	t.Helper()
	peer := newMockPeer(t, validGreeting())
	transport := &MarionetteTransport{}
	if err := transport.Connect("127.0.0.1", peer.port()); err != nil {
		t.Fatalf("connect failed: %v", err)
	}
	client := NewClient()
	client.Transport(transport)
	return client, transport, peer
}

// replyOnce answers the next outstanding command frame with a single frame.
func replyOnce(t *testing.T, peer *mockPeer, frame string) {
	t.Helper()
	peer.accept(t).reply(frame)
}

// TestTransportTransactions covers serialized command/response transactions on
// a shared transport, including concurrent callers and a blocked reply.
func TestTransportTransactions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("serial transactions keep replies matched", func(t *testing.T) {
			client, transport, peer := connectedClient(t)
			defer func() { _ = transport.Close() }()

			titleDone := make(chan string, 1)
			go func() {
				title, err := client.Title()
				if err != nil {
					titleDone <- ""
					return
				}
				titleDone <- title
			}()
			cmd, turn := peer.turn(t)
			c.Check(cmd.id == 1 && cmd.command == "WebDriver:GetTitle", "first command is GetTitle with ID 1")
			turn.reply(`[1,1,null,{"value":"first-title"}]`)
			c.Check(<-titleDone == "first-title", "first Title returned its own value")

			urlDone := make(chan string, 1)
			go func() {
				url, err := client.Url()
				if err != nil {
					urlDone <- ""
					return
				}
				urlDone <- url
			}()
			cmd, turn = peer.turn(t)
			c.Check(cmd.id == 2 && cmd.command == "WebDriver:GetCurrentURL", "second command is GetCurrentURL with ID 2")
			turn.reply(`[1,2,null,{"value":"http://example.test/second"}]`)
			c.Check(<-urlDone == "http://example.test/second", "second Url returned its own value")

			go func() { _, _ = client.Title() }()
			cmd, turn = peer.turn(t)
			c.Check(cmd.id == 3, "third command uses the next message ID, got %d", cmd.id)
			c.Check(cmd.command == "WebDriver:GetTitle", "third command is GetTitle, got %q", cmd.command)
			turn.reply(`[1,3,null,{"value":"third-title"}]`)

			c.Check(transport.MessageID() == 3, "MessageID tracks the last used ID, got %d", transport.MessageID())
			recorded := peer.snapshot()
			c.Check(len(recorded) == 3 && recorded[0].id == 1 && recorded[1].id == 2 && recorded[2].id == 3,
				"peer observed commands 1,2,3 in order, got %#v", recorded)
		})

		t.Run("concurrent commands do not swap replies", func(t *testing.T) {
			client, transport, peer := connectedClient(t)
			defer func() { _ = transport.Close() }()

			const n = 4
			start := make(chan struct{})
			var wg sync.WaitGroup
			results := make([]string, n)
			errs := make([]error, n)
			wg.Add(n)
			for i := 0; i < n; i++ {
				i := i
				go func() {
					defer wg.Done()
					<-start
					if i%2 == 0 {
						results[i], errs[i] = client.Title()
					} else {
						results[i], errs[i] = client.Url()
					}
				}()
			}
			close(start)

			// The peer labels every reply with the command it actually
			// observed and echoes the same ID. A swapped frame either fails the
			// transport's ID correlation or hands a caller the wrong command
			// family's value.
			seenIDs := map[int]bool{}
			for k := 0; k < n; k++ {
				cmd, turn := peer.turn(t)
				c.Check(cmd.id >= 1 && !seenIDs[cmd.id], "each concurrent command gets a unique ID, repeated %d", cmd.id)
				seenIDs[cmd.id] = true
				var value string
				if cmd.command == "WebDriver:GetTitle" {
					value = fmt.Sprintf("title-%d", cmd.id)
				} else {
					value = fmt.Sprintf("http://example.test/%d", cmd.id)
				}
				turn.reply(fmt.Sprintf(`[1,%d,null,{"value":%q}]`, cmd.id, value))
			}
			wg.Wait()

			for i := 0; i < n; i++ {
				c.Check(errs[i] == nil, "concurrent command %d completed without error: %v", i+1, errs[i])
				if i%2 == 0 {
					c.Check(strings.HasPrefix(results[i], "title-"),
						"Title caller %d received a title reply, got %q", i, results[i])
				} else {
					c.Check(strings.HasPrefix(results[i], "http"),
						"Url caller %d received a url reply, got %q", i, results[i])
				}
			}
			distinct := map[string]bool{}
			for _, value := range results {
				distinct[value] = true
			}
			c.Check(len(distinct) == n, "every caller got a distinct matching reply, got %#v", results)
			c.Check(len(peer.snapshot()) == n, "the peer recorded exactly %d commands", n)
			c.Check(transport.MessageID() == n, "the last used message ID is %d, got %d", n, transport.MessageID())
		})

		t.Run("a command is written at most once while waiting", func(t *testing.T) {
			client, transport, peer := connectedClient(t)
			defer func() { _ = transport.Close() }()

			done := make(chan error, 1)
			go func() {
				_, err := client.Title()
				done <- err
			}()

			cmd, blocked := peer.turn(t)
			c.Check(cmd.id == 1 && cmd.command == "WebDriver:GetTitle", "the command was written once with ID 1")

			// The serialized transaction blocks on its reply and must never
			// re-send; a second frame would show up on the recorded channel.
			select {
			case extra := <-peer.recorded:
				t.Fatalf("the blocked command was re-sent: %#v", extra)
			default:
			}
			c.Check(len(peer.snapshot()) == 1, "the blocked command was written exactly once")

			blocked.reply(`[1,1,null,{"value":"late-title"}]`)
			c.Check(<-done == nil, "the blocked command completes once its reply arrives")
			c.Check(len(peer.snapshot()) == 1, "no duplicate frame was ever written")
		})

		t.Run("Close interrupts a Send blocked on its reply", func(t *testing.T) {
			client, transport, peer := connectedClient(t)

			done := make(chan error, 1)
			go func() {
				_, err := client.Title()
				done <- err
			}()
			_, blockedTurn := peer.turn(t)

			closedAt := time.Now()
			c.Check(transport.Close() == nil, "Close while a Send is pending succeeds")
			close(blockedTurn.release) // let the old server loop observe the close
			select {
			case err := <-done:
				c.Check(err != nil, "the blocked Send fails promptly instead of waiting for the read timeout, got %v", err)
			case <-time.After(2 * time.Second):
				t.Fatal("blocked Send was not interrupted by Close")
			}
			c.Check(time.Since(closedAt) < 2*time.Second, "interruption happened immediately, not after the timeout")

			c.Check(transport.Close() == nil, "Close is safe to call repeatedly")
			_, err := client.Title()
			c.Check(err != nil, "Send without a connection returns an error")
		})
	})
}

// TestTransportProtocol covers v3 envelope validation, driver errors and
// recovery after protocol corruption.
func TestTransportProtocol(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("valid driver error keeps the connection usable", func(t *testing.T) {
			client, transport, peer := connectedClient(t)
			defer func() { _ = transport.Close() }()

			done := make(chan error, 1)
			go func() { _, err := client.Title(); done <- err }()
			peer.nextCommand(t)
			replyOnce(t, peer, `[1,1,{"error":"no such element","message":"missing","stacktrace":"trace"},null]`)

			var driverErr *DriverError
			select {
			case err := <-done:
				c.Check(errors.As(err, &driverErr), "a valid envelope surfaces as DriverError, got %T", err)
			case <-time.After(2 * time.Second):
				t.Fatal("driver error reply was not delivered")
			}
			if driverErr != nil {
				c.Check(driverErr.ErrorType == "no such element", "driver error type is preserved")
				c.Check(driverErr.Message == "missing", "driver error message is preserved")
				c.Check(driverErr.Stacktrace != nil && *driverErr.Stacktrace == "trace", "driver error stacktrace is preserved")
			}

			// The next command must still run on the same connection and keep
			// the message ID sequence going.
			titleDone := make(chan string, 1)
			go func() {
				title, err := client.Title()
				if err != nil {
					titleDone <- ""
					return
				}
				titleDone <- title
			}()
			cmd := peer.nextCommand(t)
			c.Check(cmd.id == 2, "the command after a driver error uses ID 2, got %d", cmd.id)
			replyOnce(t, peer, `[1,2,null,{"value":"recovered"}]`)
			c.Check(<-titleDone == "recovered", "the connection still serves commands after a driver error")
			c.Check(transport.MessageID() == 2, "message IDs continue across the driver error")
		})

		t.Run("malformed envelopes are errors and discard the connection", func(t *testing.T) {
			badFrames := []string{
				`[1,1,null]`,
				`[0,1,null,{"value":"x"}]`,
				`[1,1.5,null,{"value":"x"}]`,
				`[1,-3,null,{"value":"x"}]`,
				`[1,"1",null,{"value":"x"}]`,
				`[1,1,{"message":"no error field"},null]`,
				`[1,1,{"error":"e","message":"m","stacktrace":7},null]`,
				`not-json`,
			}
			for _, frame := range badFrames {
				client, transport, peer := connectedClient(t)

				done := make(chan error, 1)
				go func() { _, err := client.Title(); done <- err }()
				peer.nextCommand(t)
				replyOnce(t, peer, frame)

				select {
				case err := <-done:
					c.Check(err != nil, "corrupt frame %q returns a plain error, not success", frame)
					var driverErr *DriverError
					c.Check(!errors.As(err, &driverErr), "corrupt frame %q is not delivered as a DriverError", frame)
				case <-time.After(2 * time.Second):
					t.Fatalf("corrupt frame %q did not unblock Send", frame)
				}

				_, err := client.Title()
				c.Check(err != nil, "after corrupt frame %q the connection is discarded", frame)
				c.Check(len(peer.snapshot()) == 1, "no further frame is written after the corrupt packet %q", frame)
				c.Check(transport.MessageID() == 1, "MessageID stays at the last used ID after corruption, got %d", transport.MessageID())
				c.Check(transport.Close() == nil, "closing an already discarded transport is safe")
			}
		})

		t.Run("a mismatched reply ID discards the connection", func(t *testing.T) {
			client, transport, peer := connectedClient(t)

			done := make(chan error, 1)
			go func() { _, err := client.Title(); done <- err }()
			peer.nextCommand(t)
			replyOnce(t, peer, `[1,99,null,{"value":"wrong"}]`)

			select {
			case err := <-done:
				c.Check(err != nil, "a wrong message ID is rejected")
			case <-time.After(2 * time.Second):
				t.Fatal("wrong-ID reply did not unblock Send")
			}
			c.Check(len(peer.snapshot()) == 1, "the rejected command is never re-sent")
			_, err := client.Title()
			c.Check(err != nil, "the transport refuses further commands after a wrong-ID reply")
			_ = transport.Close()
		})

		t.Run("an incomplete frame discards the connection", func(t *testing.T) {
			client, transport, peer := connectedClient(t)

			done := make(chan error, 1)
			go func() { _, err := client.Title(); done <- err }()
			_, turn := peer.turn(t)
			// Declare more bytes than are actually sent, then half-close.
			_, _ = fmt.Fprintf(turn.conn, "128:{")
			_ = turn.conn.(*net.TCPConn).CloseWrite()
			close(turn.release)

			select {
			case err := <-done:
				c.Check(err != nil, "a truncated frame returns an error")
			case <-time.After(2 * time.Second):
				t.Fatal("truncated frame did not unblock Send")
			}
			_, err := client.Title()
			c.Check(err != nil, "the connection is discarded after an incomplete frame")
			_ = transport.Close()
		})

		t.Run("driver error without stacktrace is accepted", func(t *testing.T) {
			client, transport, peer := connectedClient(t)
			defer func() { _ = transport.Close() }()

			done := make(chan error, 1)
			go func() { _, err := client.Url(); done <- err }()
			peer.nextCommand(t)
			replyOnce(t, peer, `[1,1,{"error":"unknown error","message":"failed","stacktrace":null},null]`)
			var driverErr *DriverError
			select {
			case err := <-done:
				c.Check(errors.As(err, &driverErr), "null stacktrace is a valid driver error, got %v", err)
			case <-time.After(2 * time.Second):
				t.Fatal("null-stacktrace reply did not unblock Send")
			}
			if driverErr != nil {
				c.Check(driverErr.Stacktrace == nil, "null stacktrace decodes to a nil pointer")
			}
		})
	})
}

// TestTransportReconnect covers handshake validation, retries and generation
// handling across reconnects.
func TestTransportReconnect(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("handshake must be a complete gecko v3 greeting", func(t *testing.T) {
			badGreetings := []string{
				`{"applicationType":"gecko"}`,
				`{"marionetteProtocol":3}`,
				`{"applicationType":"fennec","marionetteProtocol":3}`,
				`{"applicationType":"gecko","marionetteProtocol":2}`,
				`not-json`,
			}
			for _, greeting := range badGreetings {
				peer := newMockPeer(t, greeting)
				transport := &MarionetteTransport{}
				err := transport.Connect("127.0.0.1", peer.port())
				c.Check(err != nil, "handshake %q is rejected", greeting)
				c.Check(transport.Close() == nil, "a failed handshake leaves nothing to close (idempotent Close)")
				peer.Stop()
			}
		})

		t.Run("a failed handshake does not occupy the connection slot", func(t *testing.T) {
			peer := newMockPeer(t, `{"applicationType":"gecko","marionetteProtocol":2}`)
			transport := &MarionetteTransport{}
			c.Check(transport.Connect("127.0.0.1", peer.port()) != nil, "unsupported protocol version fails Connect")
			peer.Stop()

			good := newMockPeer(t, validGreeting())
			defer good.Stop()
			c.Check(transport.Connect("127.0.0.1", good.port()) == nil, "retry after a failed handshake succeeds")
			c.Check(transport.Connect("127.0.0.1", good.port()) != nil, "duplicate Connect while connected errors")

			client := NewClient()
			client.Transport(transport)
			go func() { _, _ = client.Title() }()
			cmd, turn := good.turn(t)
			c.Check(cmd.id == 1, "a fresh connection numbers commands from 1, got %d", cmd.id)
			turn.reply(`[1,1,null,{"value":"fresh"}]`)
			c.Check(transport.Close() == nil, "Close after reconnect succeeds")
		})

		t.Run("reconnect after Close restarts IDs from 1", func(t *testing.T) {
			peer := newMockPeer(t, validGreeting())
			transport := &MarionetteTransport{}
			c.Check(transport.Connect("127.0.0.1", peer.port()) == nil, "first Connect succeeds")
			client := NewClient()
			client.Transport(transport)

			go func() { _, _ = client.Title() }()
			replyOnce(t, peer, `[1,1,null,{"value":"one"}]`)
			c.Check(transport.MessageID() == 1, "first connection used ID 1")
			c.Check(transport.Close() == nil, "closing the first connection succeeds")

			c.Check(transport.Connect("127.0.0.1", peer.port()) == nil, "Connect after Close succeeds")
			go func() { _, _ = client.Title() }()
			cmd, turn := peer.turn(t)
			c.Check(cmd.gen == 2, "the command runs on a new peer connection, got generation %d", cmd.gen)
			c.Check(cmd.id == 1, "the new connection restarts message IDs at 1, got %d", cmd.id)
			turn.reply(`[1,1,null,{"value":"two"}]`)
			c.Check(transport.MessageID() == 1, "MessageID reflects the new connection, got %d", transport.MessageID())
			_ = transport.Close()
		})

		t.Run("old connection calls fail and are never replayed on the new one", func(t *testing.T) {
			peer := newMockPeer(t, validGreeting())
			transport := &MarionetteTransport{}
			c.Check(transport.Connect("127.0.0.1", peer.port()) == nil, "initial Connect succeeds")
			client := NewClient()
			client.Transport(transport)

			oldDone := make(chan error, 1)
			go func() { _, err := client.Title(); oldDone <- err }()
			_, staleTurn := peer.turn(t) // command 1 is in flight on the old connection

			c.Check(transport.Close() == nil, "Close discards the old connection while a command waits")
			close(staleTurn.release) // retire the dead gen-1 turn
			c.Check(transport.Connect("127.0.0.1", peer.port()) == nil, "a new connection is established")

			select {
			case err := <-oldDone:
				c.Check(err != nil, "the queued old-connection command fails instead of replaying")
			case <-time.After(2 * time.Second):
				t.Fatal("old command did not exit after reconnect")
			}

			newDone := make(chan string, 1)
			go func() {
				title, err := client.Title()
				if err != nil {
					newDone <- ""
					return
				}
				newDone <- title
			}()
			cmd, turn := peer.turn(t)
			c.Check(cmd.gen == 2, "the new command uses the new connection generation")
			c.Check(cmd.id == 1, "the new connection starts numbering from 1, got %d", cmd.id)
			turn.reply(`[1,1,null,{"value":"new-connection"}]`)
			c.Check(<-newDone == "new-connection", "the new command succeeds on the new connection")

			recorded := peer.snapshot()
			genOne, genTwo := 0, 0
			for _, recorded := range recorded {
				if recorded.gen == 1 {
					genOne++
				}
				if recorded.gen == 2 {
					genTwo++
				}
			}
			c.Check(genOne == 1, "the old command was sent once and never replayed, gen1 count=%d", genOne)
			c.Check(genTwo == 1, "only the new command ran on the new connection, gen2 count=%d", genTwo)
			_ = transport.Close()
		})

		t.Run("Close interrupts a handshake in progress", func(t *testing.T) {
			// A peer that accepts but never sends the greeting.
			listener, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatalf("listen failed: %v", err)
			}
			accepted := make(chan net.Conn, 1)
			go func() {
				conn, acceptErr := listener.Accept()
				if acceptErr == nil {
					accepted <- conn
				}
			}()

			transport := &MarionetteTransport{}
			handshakeDone := make(chan error, 1)
			go func() { handshakeDone <- transport.Connect("127.0.0.1", listener.Addr().(*net.TCPAddr).Port) }()

			select {
			case <-accepted:
			case <-time.After(2 * time.Second):
				t.Fatal("peer never accepted the dialing connection")
			}

			closedAt := time.Now()
			c.Check(transport.Close() == nil, "Close during a pending handshake succeeds")
			select {
			case err := <-handshakeDone:
				c.Check(err != nil, "the interrupted handshake fails")
			case <-time.After(2 * time.Second):
				t.Fatal("handshake was not interrupted by Close")
			}
			c.Check(time.Since(closedAt) < 2*time.Second, "the handshake was interrupted immediately")

			good := newMockPeer(t, validGreeting())
			defer good.Stop()
			c.Check(transport.Connect("127.0.0.1", good.port()) == nil, "Connect works again after interrupting a handshake")
			_ = transport.Close()
			_ = listener.Close()
		})
	})
}
