// Command http_tunnel shows how to read an RTSP camera through a gateway
// that only exposes an HTTP endpoint. The media address stays an rtsp:// URL
// while all HTTP traffic (path, query, Host) follows the separate endpoint.
package main

import (
	"fmt"
	"log"
	"os"
	"time"

	"github.com/deepch/vdk/format/rtsp"
)

func main() {
	media := "rtsp://admin:secret@192.168.1.50:554/stream1"
	endpoint := "http://gateway.example.com:8080/rtsp/tunnel?camera=front-door"

	client, err := rtsp.DialHTTPTimeout(media, endpoint, 5*time.Second)
	if err != nil {
		log.Fatalf("dial through http gateway: %v", err)
	}
	defer client.Close()

	// The rest of the API is unchanged: Describe, SetupAll, Play, ReadPacket
	// and the keepalive timer all run over the two tunnel connections.
	client.RtspTimeout = 10 * time.Second
	client.RtpKeepAliveTimeout = 30 * time.Second

	streams, err := client.Describe()
	if err != nil {
		log.Fatalf("describe: %v", err)
	}
	fmt.Printf("streams: %d, first control track: %q\n", len(streams), streams[0].Control)

	if err = client.SetupAll(); err != nil {
		log.Fatalf("setup: %v", err)
	}
	if err = client.Play(); err != nil {
		log.Fatalf("play: %v", err)
	}

	codecs, err := client.Streams()
	if err != nil {
		log.Fatalf("codec data: %v", err)
	}
	for i, cd := range codecs {
		fmt.Printf("stream %d: %s\n", i, cd.Type())
	}

	for {
		pkt, err := client.ReadPacket()
		if err != nil {
			fmt.Fprintf(os.Stderr, "read packet: %v\n", err)
			return
		}
		fmt.Printf("track %d t=%v key=%v bytes=%d\n", pkt.Idx, pkt.Time, pkt.IsKeyFrame, len(pkt.Data))
	}
}
