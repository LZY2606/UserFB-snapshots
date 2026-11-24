package marionette_client

import (
	"encoding/json"
	"errors"
	"io"
	"net"
	"strconv"
	"sync"
	"time"
)

type Transporter interface {
	MessageID() int
	Connect(host string, port int) error
	Close() error
	Send(command string, values any) (*Response, error)
	Receive() ([]byte, error)
}

type MarionetteTransport struct {
	ApplicationType    string
	MarionetteProtocol int32

	mu        sync.Mutex
	cond      *sync.Cond
	conn      net.Conn
	de        DecoderEncoder
	gen       uint64
	busy      bool
	messageID int
}

type Response struct {
	MessageID   int32
	Size        int32
	Value       string
	DriverError *DriverError
}

var (
	errNoActiveConnection = errors.New("marionette: no active connection")
	errStaleConnection    = errors.New("marionette: the connection was closed before the command could be delivered")
	errHandshakeAborted   = errors.New("marionette: handshake aborted because the transport was closed")
)

func connDefaultTimeout() time.Time {
	return time.Now().Add(time.Minute * 5)
}

func (t *MarionetteTransport) initLocked() {
	if t.cond == nil {
		t.cond = sync.NewCond(&t.mu)
	}
}

func (t *MarionetteTransport) MessageID() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.messageID
}

func (t *MarionetteTransport) Connect(host string, port int) error {
	if host == "" {
		host = "127.0.0.1"
	}

	if port == 0 {
		port = 2828
	}

	t.mu.Lock()
	t.initLocked()
	if t.conn != nil {
		t.mu.Unlock()
		return errors.New("a connection is already established. please disconnect before connecting")
	}
	gen := t.gen
	t.mu.Unlock()

	hostname := host + ":" + strconv.Itoa(port)
	c, err := net.Dial("tcp", hostname)
	if err != nil {
		return err
	}

	// Register the dialing connection before the handshake read so that a
	// concurrent Close can interrupt a handshake that is still in progress.
	t.mu.Lock()
	if t.gen != gen || t.conn != nil {
		t.mu.Unlock()
		_ = c.Close()
		if t.conn != nil {
			return errors.New("a connection is already established. please disconnect before connecting")
		}
		return errHandshakeAborted
	}
	t.conn = c
	t.mu.Unlock()

	r, err := readFrame(c)

	t.mu.Lock()
	if t.gen != gen || t.conn != c {
		t.mu.Unlock()
		_ = c.Close()
		return errHandshakeAborted
	}

	failHandshake := func(cause error) error {
		t.conn = nil
		t.de = nil
		t.busy = false
		t.gen++
		t.cond.Broadcast()
		t.mu.Unlock()
		_ = c.Close()
		return cause
	}

	if err != nil {
		return failHandshake(err)
	}

	var greeting struct {
		ApplicationType    string `json:"applicationType"`
		MarionetteProtocol int32  `json:"marionetteProtocol"`
	}
	if err = json.Unmarshal(r, &greeting); err != nil {
		return failHandshake(err)
	}

	if greeting.ApplicationType != "gecko" || greeting.MarionetteProtocol != MarionetteProtocolV3 {
		return failHandshake(errors.New("marionette: unsupported handshake response: applicationType must be \"gecko\" and marionetteProtocol must be 3"))
	}

	d, err := NewDecoderEncoder(greeting.MarionetteProtocol)
	if err != nil {
		return failHandshake(err)
	}

	t.ApplicationType = greeting.ApplicationType
	t.MarionetteProtocol = greeting.MarionetteProtocol
	t.de = d
	t.messageID = 0
	t.busy = false
	t.cond.Broadcast()
	t.mu.Unlock()

	return nil
}

// Close discards the current connection. It is safe to call repeatedly and
// interrupts any Send or handshake currently blocked on the connection.
func (t *MarionetteTransport) Close() error {
	t.mu.Lock()
	t.initLocked()
	conn := t.conn
	t.conn = nil
	t.de = nil
	t.busy = false
	t.gen++
	t.cond.Broadcast()
	t.mu.Unlock()

	if conn == nil {
		return nil
	}

	return conn.Close()
}

// Send runs a complete command/response transaction. Transactions on the same
// connection are serialized and each command is written at most once. A
// protocol violation or transport error permanently discards the connection;
// legitimate driver errors leave it usable.
func (t *MarionetteTransport) Send(command string, values any) (*Response, error) {
	t.mu.Lock()
	t.initLocked()
	if t.conn == nil || t.de == nil {
		t.mu.Unlock()
		return nil, errNoActiveConnection
	}
	gen := t.gen
	for t.busy {
		t.cond.Wait()
		if t.gen != gen || t.conn == nil {
			t.mu.Unlock()
			return nil, errStaleConnection
		}
	}
	if t.gen != gen || t.conn == nil {
		t.mu.Unlock()
		return nil, errStaleConnection
	}

	t.busy = true
	t.messageID++
	id := int32(t.messageID)
	conn := t.conn
	de := t.de
	t.mu.Unlock()

	finish := func(discard bool) {
		t.mu.Lock()
		if t.gen == gen {
			t.busy = false
			if discard && t.conn != nil {
				closed := t.conn
				t.conn = nil
				t.de = nil
				t.gen++
				_ = closed.Close()
			}
			t.cond.Broadcast()
		}
		t.mu.Unlock()
	}

	buf, err := de.Encode(t, command, values)
	if err != nil {
		finish(false)
		return nil, err
	}

	if _, err = write(conn, buf); err != nil {
		finish(true)
		return nil, err
	}

	rBuf, err := readFrame(conn)
	if err != nil {
		finish(true)
		return nil, err
	}

	data := &Response{}
	err = de.Decode(rBuf, data)
	if err != nil {
		var driverError *DriverError
		if errors.As(err, &driverError) {
			// A well-formed driver error is a legitimate reply: the
			// connection stays usable for the next command.
			finish(false)
			return data, err
		}
		finish(true)
		return nil, err
	}

	if data.MessageID != id {
		finish(true)
		return nil, errors.New("marionette: response message ID does not match the outstanding command")
	}

	finish(false)
	return data, nil
}

func write(c net.Conn, b []byte) (int, error) {
	err := c.SetDeadline(connDefaultTimeout())
	if err != nil {
		return 0, err
	}

	return c.Write(b)
}

// Receive reads a single length-prefixed frame. It is intended for the
// handshake and standalone diagnostics, not for concurrent use with Send.
func (t *MarionetteTransport) Receive() ([]byte, error) {
	t.mu.Lock()
	t.initLocked()
	conn := t.conn
	gen := t.gen
	t.mu.Unlock()

	if conn == nil {
		return nil, errNoActiveConnection
	}

	buf, err := readFrame(conn)
	if err != nil {
		t.discard(gen)
		return nil, err
	}

	return buf, nil
}

// discard closes and forgets the connection only if it still belongs to the
// given connection generation. A late failure from an old generation never
// touches a newer connection.
func (t *MarionetteTransport) discard(gen uint64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.gen != gen || t.conn == nil {
		return
	}
	conn := t.conn
	t.conn = nil
	t.de = nil
	t.busy = false
	t.gen++
	t.cond.Broadcast()
	_ = conn.Close()
}

func readFrame(c net.Conn) ([]byte, error) {
	if err := c.SetDeadline(connDefaultTimeout()); err != nil {
		return nil, err
	}
	return read(c)
}

// ReadFull reads exactly len(buf) bytes from r into buf.
// It returns the number of bytes copied and an error if fewer bytes were read.
// The error is EOF only if no bytes were read.
// If an EOF happens after reading some but not all the bytes,
// ReadFull returns ErrUnexpectedEOF.
// On return, n == len(buf) if and only if err == nil.
func read(c net.Conn) ([]byte, error) {
	var msgSize, err = messageLength(c)
	if err != nil {
		return nil, err
	}

	msgBuf := make([]byte, msgSize)
	_, err = io.ReadFull(c, msgBuf)
	if err != nil {
		return nil, err
	}

	return msgBuf, nil
}

// Reads from the connection byte by byte until the message length is found, according to
// marionette's protocol.
// the protocol say's that message length is the first part for the message until ":" is found.
// this signals the next bytes as the message
func messageLength(c net.Conn) (int, error) {
	var byteSize = make([]byte, 0)
	tmp := make([]byte, 1)
	for {
		_, err := c.Read(tmp)
		if err != nil {
			return 0, err
		}

		if string(tmp) != ":" {
			byteSize = append(byteSize, tmp...)
			continue
		}

		// the message length
		intSize, err := strconv.Atoi(string(byteSize))
		if err != nil {
			return 0, err
		}

		return intSize, err
	}
}
