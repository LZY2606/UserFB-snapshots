// Rendezvous example: two SRT v5 live peers connect directly to each other.
// Neither side is a listener; each binds its own local IPv4 UDP address and
// dials the other's. Run the demo with:
//
//	go run ./contrib/rendezvous
//
// To start a single side against an external peer, use:
//
//	go run ./contrib/rendezvous -local 127.0.0.1:31001 -remote 127.0.0.1:31002
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"time"

	srt "github.com/datarhei/gosrt"
)

// peer runs one rendezvous endpoint. It binds localAddress, connects to
// remoteAddress, and exchanges one media payload in each direction.
func peer(name, localAddress, remoteAddress string, out, in []byte) error {
	config := srt.DefaultConfig()
	config.ConnectionTimeout = 3 * time.Second
	config.ReceiverLatency = 120 * time.Millisecond
	config.PeerLatency = 120 * time.Millisecond

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	conn, err := srt.RendezvousWithContext(ctx, localAddress, remoteAddress, config)
	if err != nil {
		return fmt.Errorf("%s: rendezvous: %w", name, err)
	}
	defer conn.Close()

	fmt.Printf("%s: established rendezvous socket=%#08x peer=%#08x\n",
		name, conn.SocketId(), conn.PeerSocketId())

	// Send our media payload.
	if n, err := conn.Write(out); err != nil || n != len(out) {
		return fmt.Errorf("%s: write: n=%d err=%w", name, n, err)
	}

	// Receive the peer's media payload.
	if err := conn.SetReadDeadline(time.Now().Add(3 * time.Second)); err != nil {
		return fmt.Errorf("%s: set read deadline: %w", name, err)
	}

	got := make([]byte, len(in))
	if _, err := io.ReadFull(conn, got); err != nil {
		return fmt.Errorf("%s: read: %w", name, err)
	}

	fmt.Printf("%s: received %x\n", name, got)
	return nil
}

func main() {
	local := flag.String("local", "", "local IPv4 host:port to bind (non-zero port)")
	remote := flag.String("remote", "", "remote IPv4 host:port to connect to")
	flag.Parse()

	// Single-endpoint mode: pair with an externally started peer.
	if *local != "" && *remote != "" {
		if err := peer("peer", *local, *remote,
			[]byte{0xD5, 0x55, 0x7F, 0xFF}, []byte{0x11, 0x22, 0x33, 0x44}); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}

	// Demo mode: start both ends in this process and cross-connect them.
	addressA := "127.0.0.1:31001"
	addressB := "127.0.0.1:31002"

	payloadA := []byte{0xD5, 0x55, 0x7F, 0xFF}
	payloadB := []byte{0x11, 0x22, 0x33, 0x44}

	errChan := make(chan error, 2)

	go func() {
		errChan <- peer("A", addressA, addressB, payloadA, payloadB)
	}()
	go func() {
		errChan <- peer("B", addressB, addressA, payloadB, payloadA)
	}()

	for range 2 {
		if err := <-errChan; err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}

	fmt.Println("rendezvous demo: both peers exchanged media bidirectionally")
}
