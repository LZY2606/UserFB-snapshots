package oggvorbis_test

import (
	"bytes"
	"fmt"
	"io"
	"os"

	"github.com/jfreymuth/oggvorbis"
)

// ExampleReader_streaming shows sequential playback of an ogg/vorbis file
// that may contain several chained logical streams. The same Reader API works
// for a single stream and for a chain; no special "chained" interface exists.
func ExampleReader_streaming() {
	// Any io.Reader works; a non-seekable input simply cannot use SetPosition.
	var in io.Reader = bytes.NewReader(exampleOggData)

	r, err := oggvorbis.NewReader(in)
	if err != nil {
		fmt.Println("open:", err)
		return
	}
	// SampleRate/Channels/Bitrate/CommentHeader describe the first segment
	// and stay constant for the whole chain. Only chains whose segments all
	// share the first segment's sample rate and channel count are supported.
	fmt.Println("format:", r.SampleRate(), "Hz", r.Channels(), "channels")

	buf := make([]float32, 4096)
	frames := int64(0)
	for {
		n, err := r.Read(buf)
		frames += int64(n / r.Channels())
		if err == io.EOF {
			break
		}
		if err != nil {
			// A malformed page, bad CRC or a broken chain boundary is reported
			// here; nothing is skipped silently.
			fmt.Println("read:", err)
			return
		}
	}
	// Position uses global frame coordinates across all segments.
	fmt.Println("frames:", frames, "position:", r.Position())
	// Output: format: 44100 Hz 1 channels
	// frames: 44100 position: 44100
}

// ExampleReader_seeking shows the index-backed seek path, available when the
// input implements io.ReadSeeker.
func ExampleReader_seeking() {
	r, err := oggvorbis.NewReader(bytes.NewReader(exampleOggData))
	if err != nil {
		fmt.Println("open:", err)
		return
	}
	// NewReader scanned the whole container once and built a page/segment
	// index, so the total length is known immediately. The index keeps only
	// metadata (page offsets, segment headers); it never caches compressed
	// page bodies or decoded PCM.
	fmt.Println("length:", r.Length())

	// Positions are global frames; SetPosition(0), boundary positions and
	// positions at or beyond the length are all valid. A negative position is
	// an error and leaves the playback position unchanged.
	if err := r.SetPosition(1000); err != nil {
		fmt.Println("seek:", err)
		return
	}
	fmt.Println("position:", r.Position())
	buf := make([]float32, 128)
	if _, err := r.Read(buf); err != nil && err != io.EOF {
		fmt.Println("read:", err)
		return
	}
	fmt.Println("read frames:", len(buf)/r.Channels())
	// Output: length: 44100
	// position: 1000
	// read frames: 128
}

// ExampleReadAll_chained shows ReadAll and GetLength using the same global
// sample coordinates as Reader.
func ExampleReadAll_chained() {
	pcm, format, err := oggvorbis.ReadAll(bytes.NewReader(exampleOggData))
	if err != nil {
		fmt.Println("decode:", err)
		return
	}
	fmt.Println("frames:", len(pcm)/format.Channels)

	length, format, err := oggvorbis.GetLength(bytes.NewReader(exampleOggData))
	if err != nil {
		fmt.Println("length:", err)
		return
	}
	fmt.Println("length:", length, "channels:", format.Channels)
	// Output: frames: 44100
	// length: 44100 channels: 1
}

// exampleOggData stands in for the repository's testdata/test.ogg fixture;
// examples need a concrete byte stream, so load it when the examples run.
var exampleOggData = func() []byte {
	b, err := os.ReadFile("testdata/test.ogg")
	if err != nil {
		// Examples run from the package directory where testdata is available.
		return nil
	}
	return b
}()
