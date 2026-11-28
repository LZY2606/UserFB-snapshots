// Command exportopusogg exports a single Opus audio track from a
// non-fragmented, self-contained, unencrypted MP4 file into an Ogg Opus
// stream. The compressed Opus packets are copied byte-for-byte; only the
// container changes.
//
// Usage:
//
//	exportopusogg -input input.mp4 -output output.ogg -track 1 -serial 12345
package main

import (
	"flag"
	"fmt"
	"os"

	mp4 "github.com/abema/go-mp4"
)

func main() {
	input := flag.String("input", "", "input MP4 file")
	output := flag.String("output", "", "output Ogg Opus file")
	trackID := flag.Uint("track", 0, "track ID (tkhd.track_ID) of the Opus track")
	serial := flag.Uint("serial", 0, "Ogg stream serial number")
	flag.Parse()

	if *input == "" || *output == "" || *trackID == 0 {
		fmt.Fprintln(os.Stderr, "input, output and track are required")
		flag.Usage()
		os.Exit(2)
	}

	in, err := os.Open(*input)
	if err != nil {
		fatal(err)
	}
	defer in.Close()

	out, err := os.Create(*output)
	if err != nil {
		fatal(err)
	}
	ok := false
	defer func() {
		if !ok {
			out.Close()
			os.Remove(*output) // validation failures must not leave a partial Ogg
		}
	}()

	// ExportOpusOgg parses the whole file from offset zero and restores the
	// reader's position before returning, on both success and failure. All
	// table/window validation completes before the first page is written.
	if err := mp4.ExportOpusOgg(in, out, uint32(*trackID), uint32(*serial)); err != nil {
		fatal(err)
	}
	if err := out.Close(); err != nil {
		fatal(err)
	}
	ok = true
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "exportopusogg:", err)
	os.Exit(1)
}
