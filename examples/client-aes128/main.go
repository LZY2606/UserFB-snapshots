// Package main contains an example.
package main

import (
	"log"

	"github.com/bluenviron/gohlslib/v2"
	"github.com/bluenviron/gohlslib/v2/pkg/codecs"
)

// This example shows how to read an AES-128 encrypted HLS stream.
// The client decrypts segments transparently: no additional configuration
// is required.
func main() {
	c := &gohlslib.Client{
		URI: "https://myserver/mystream/index.m3u8",
	}

	c.OnTracks = func(tracks []*gohlslib.Track) error {
		for i, track := range tracks {
			ci := i
			ctrack := track

			log.Printf("detected track with codec %T\n", track.Codec)

			switch track.Codec.(type) {
			case *codecs.H264, *codecs.H265:
				c.OnDataH26x(track, func(pts int64, _ int64, _ [][]byte) {
					log.Printf("received decrypted data from track %d, codec = %T, pts = %v\n",
						ci, ctrack.Codec, pts)
				})

			case *codecs.MPEG4Audio:
				c.OnDataMPEG4Audio(track, func(pts int64, _ [][]byte) {
					log.Printf("received decrypted data from track %d, codec = %T, pts = %v\n",
						ci, ctrack.Codec, pts)
				})
			}
		}
		return nil
	}

	err := c.Start()
	if err != nil {
		panic(err)
	}
	defer c.Close()

	panic(c.Wait2())
}
