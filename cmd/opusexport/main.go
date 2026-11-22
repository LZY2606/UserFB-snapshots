// Command opusexport repackages an Opus audio track of a non-fragmented
// MP4 into an Ogg Opus stream without decoding or re-encoding the audio.
//
// Usage:
//
//	opusexport -i input.mp4 -o output.ogg -track 1 -serial 123456
//
// This is a small usage example for mp4.ExportOpusOgg; the library function
// itself works on any io.ReadSeeker/io.Writer pair.
package main

import (
	"flag"
	"fmt"
	"os"

	mp4 "github.com/abema/go-mp4"
)

func main() {
	input := flag.String("i", "", "input MP4 file")
	output := flag.String("o", "", "output Ogg Opus file")
	trackID := flag.Uint("track", 1, "track ID of the Opus track")
	serial := flag.Uint("serial", 1, "Ogg stream serial number")
	flag.Parse()

	if *input == "" || *output == "" {
		fmt.Fprintln(os.Stderr, "both -i and -o are required")
		flag.Usage()
		os.Exit(2)
	}
	if err := run(*input, *output, uint32(*trackID), uint32(*serial)); err != nil {
		fmt.Fprintf(os.Stderr, "opusexport: %v\n", err)
		os.Exit(1)
	}
}

func run(inputPath, outputPath string, trackID, serial uint32) error {
	in, err := os.Open(inputPath)
	if err != nil {
		return err
	}
	defer in.Close()

	out, err := os.Create(outputPath)
	if err != nil {
		return err
	}
	defer out.Close()

	// ExportOpusOgg restores in's read position in every case and performs
	// all validation before writing the first Ogg page to out.
	return mp4.ExportOpusOgg(in, out, trackID, serial)
}
