package main

// Indexed FLV recording example.
//
// Run standalone:
//   go run example/example_mux_indexed_flv.go out.flv
//
// The output starts with a fixed-size onMetaData script tag whose keyframes
// index (times + filepositions) is backfilled once when Finalize is called,
// so players can seek by keyframe without moving any already written media.

import (
	"fmt"
	"os"

	"github.com/yapingcat/gomedia/go-flv"
)

func main() {
	path := "indexed.flv"
	if len(os.Args) > 1 {
		path = os.Args[1]
	}
	fd, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0666)
	if err != nil {
		fmt.Println("open output failed:", err)
		return
	}
	defer fd.Close()

	// Keep at most 4096 keyframes in the onMetaData keyframes index.
	writer, err := flv.CreateIndexedFlvWriter(fd, 4096)
	if err != nil {
		fmt.Println("create indexed writer failed:", err)
		return
	}
	if err := writer.WriteFlvHeader(); err != nil {
		fmt.Println("write flv header failed:", err)
		return
	}

	// Feed Annex-B H.264 frames and ADTS AAC frames while recording.
	// writer.WriteH264(h264Frame, pts, dts)
	// writer.WriteAAC(aacFrame, pts, dts)

	// Backfill onMetaData (duration, filesize, keyframes, ...) in place.
	if err := writer.Finalize(); err != nil {
		fmt.Println("finalize failed:", err)
		return
	}
	fmt.Println("indexed flv written to", path)
}
