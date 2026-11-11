package imapclient_test

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"io"
	"math/big"
	"net"
	"os"
	"sync"
	"testing"
	"time"

	"github.com/emersion/go-imap/v2"
	"github.com/emersion/go-imap/v2/imapclient"
	"github.com/emersion/go-imap/v2/imapserver"
	"github.com/emersion/go-imap/v2/imapserver/imapmemserver"
)

const (
	testUsername = "test-user"
	testPassword = "test-password"
)

const simpleRawMessage = `MIME-Version: 1.0
Message-Id: <191101702316132@example.com>
Content-Transfer-Encoding: 8bit
Content-Type: text/plain; charset=utf-8

This is my letter!`

// Generate a disposable certificate for in-memory server tests.
var rsaCertPEM, rsaKeyPEM = ephemeralTLSMaterial()

func ephemeralTLSMaterial() (string, string) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		panic(err)
	}
	tmpl := &x509.Certificate{SerialNumber: big.NewInt(1), DNSNames: []string{"localhost"}, NotBefore: time.Unix(0, 0), NotAfter: time.Date(2099, 1, 1, 0, 0, 0, 0, time.UTC), KeyUsage: x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		panic(err)
	}
	return string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})), string(pem.EncodeToMemory(&pem.Block{Type: "RSA " + "PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)}))
}

func newMemClientServerPair(t *testing.T) (net.Conn, io.Closer) {
	memServer := imapmemserver.New()

	user := imapmemserver.NewUser(testUsername, testPassword)
	user.Create("INBOX", nil)

	memServer.AddUser(user)

	cert, err := tls.X509KeyPair([]byte(rsaCertPEM), []byte(rsaKeyPEM))
	if err != nil {
		t.Fatalf("tls.X509KeyPair() = %v", err)
	}

	server := imapserver.New(&imapserver.Options{
		NewSession: func(conn *imapserver.Conn) (imapserver.Session, *imapserver.GreetingData, error) {
			return memServer.NewSession(), nil, nil
		},
		TLSConfig: &tls.Config{
			Certificates: []tls.Certificate{cert},
		},
		InsecureAuth: true,
		Caps: imap.CapSet{
			imap.CapIMAP4rev1: {},
			imap.CapIMAP4rev2: {},
		},
	})

	ln, err := net.Listen("tcp", "localhost:0")
	if err != nil {
		t.Fatalf("net.Listen() = %v", err)
	}

	go func() {
		if err := server.Serve(ln); err != nil {
			t.Errorf("Serve() = %v", err)
		}
	}()

	conn, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatalf("net.Dial() = %v", err)
	}

	return conn, server
}

func newClientServerPair(t *testing.T, initialState imap.ConnState) (*imapclient.Client, io.Closer) {
	return newClientServerPairWithOptions(t, initialState, nil)
}

func newClientServerPairWithOptions(t *testing.T, initialState imap.ConnState, options *imapclient.Options) (*imapclient.Client, io.Closer) {
	var useDovecot bool
	switch os.Getenv("GOIMAP_TEST_DOVECOT") {
	case "0", "":
		// ok
	case "1":
		useDovecot = true
	default:
		t.Fatalf("invalid GOIMAP_TEST_DOVECOT env var")
	}

	var (
		conn   net.Conn
		server io.Closer
	)
	if useDovecot {
		if initialState < imap.ConnStateAuthenticated {
			t.Skip("Dovecot connections are pre-authenticated")
		}
		conn, server = newDovecotClientServerPair(t)
	} else {
		conn, server = newMemClientServerPair(t)
	}

	var debugWriter swapWriter
	debugWriter.Swap(io.Discard)

	if options == nil {
		options = &imapclient.Options{}
	}
	if testing.Verbose() {
		options.DebugWriter = &debugWriter
	}
	client := imapclient.New(conn, options)

	if initialState >= imap.ConnStateAuthenticated {
		// Dovecot connections are pre-authenticated
		if !useDovecot {
			if err := client.Login(testUsername, testPassword).Wait(); err != nil {
				t.Fatalf("Login().Wait() = %v", err)
			}
		}

		appendCmd := client.Append("INBOX", int64(len(simpleRawMessage)), nil)
		appendCmd.Write([]byte(simpleRawMessage))
		appendCmd.Close()
		if _, err := appendCmd.Wait(); err != nil {
			t.Fatalf("AppendCommand.Wait() = %v", err)
		}
	}
	if initialState >= imap.ConnStateSelected {
		if _, err := client.Select("INBOX", nil).Wait(); err != nil {
			t.Fatalf("Select().Wait() = %v", err)
		}
	}

	// Turn on debug logs after we're done initializing the test
	debugWriter.Swap(os.Stderr)

	return client, server
}

// swapWriter is an io.Writer which can be swapped at runtime.
type swapWriter struct {
	w     io.Writer
	mutex sync.Mutex
}

func (sw *swapWriter) Write(b []byte) (int, error) {
	sw.mutex.Lock()
	w := sw.w
	sw.mutex.Unlock()

	return w.Write(b)
}

func (sw *swapWriter) Swap(w io.Writer) {
	sw.mutex.Lock()
	sw.w = w
	sw.mutex.Unlock()
}

func TestLogin(t *testing.T) {
	client, server := newClientServerPair(t, imap.ConnStateNotAuthenticated)
	defer client.Close()
	defer server.Close()

	if err := client.Login(testUsername, testPassword).Wait(); err != nil {
		t.Errorf("Login().Wait() = %v", err)
	}
}

func TestLogout(t *testing.T) {
	client, server := newClientServerPair(t, imap.ConnStateAuthenticated)
	defer server.Close()

	if _, ok := server.(*dovecotServer); ok {
		t.Skip("Dovecot connections don't reply to LOGOUT")
	}

	if err := client.Logout().Wait(); err != nil {
		t.Errorf("Logout().Wait() = %v", err)
	}
	if err := client.Close(); err != nil {
		t.Errorf("Close() = %v", err)
	}
}

// https://github.com/emersion/go-imap/issues/562
func TestFetch_invalid(t *testing.T) {
	client, server := newClientServerPair(t, imap.ConnStateSelected)
	defer client.Close()
	defer server.Close()

	_, err := client.Fetch(imap.UIDSet(nil), nil).Collect()
	if err == nil {
		t.Fatalf("UIDFetch().Collect() = %v", err)
	}
}

func TestFetch_closeUnreadBody(t *testing.T) {
	client, server := newClientServerPair(t, imap.ConnStateSelected)
	defer client.Close()
	defer server.Close()

	fetchCmd := client.Fetch(imap.SeqSetNum(1), &imap.FetchOptions{
		BodySection: []*imap.FetchItemBodySection{
			{
				Specifier: imap.PartSpecifierNone,
				Peek:      true,
			},
		},
	})
	if err := fetchCmd.Close(); err != nil {
		t.Fatalf("UIDFetch().Close() = %v", err)
	}
}

func TestWaitGreeting_eof(t *testing.T) {
	// bad server: connected but without greeting
	clientConn, serverConn := net.Pipe()

	client := imapclient.New(clientConn, nil)
	defer client.Close()

	if err := serverConn.Close(); err != nil {
		t.Fatalf("serverConn.Close() = %v", err)
	}

	if err := client.WaitGreeting(); err == nil {
		t.Fatalf("WaitGreeting() should fail")
	}
}
