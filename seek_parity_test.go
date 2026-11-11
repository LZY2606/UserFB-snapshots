package mp3

import (
	"bytes"
	"io"
	"math/rand"
	"os"
	"testing"

	"github.com/hajimehoshi/go-mp3/internal/frameheader"
	"github.com/hajimehoshi/go-mp3/internal/sideinfo"
)

var parityFiles = []string{"example/mpeg2.mp3", "example/classic.mp3"}

func readFile(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

// sequentialPCM returns the reference PCM by decoding a freshly created
// decoder from the very beginning, independently from the seek path.
func sequentialPCM(t *testing.T, data []byte, path string) []byte {
	t.Helper()
	ref, err := NewDecoder(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("%s: NewDecoder: %v", path, err)
	}
	pcm, err := io.ReadAll(ref)
	if err != nil {
		t.Fatalf("%s: sequential decode: %v", path, err)
	}
	if len(pcm) == 0 {
		t.Fatalf("%s: sequential decode produced no PCM", path)
	}
	return pcm
}

func newSeekableDecoder(t *testing.T, data []byte, path string) *Decoder {
	t.Helper()
	d, err := NewDecoder(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("%s: NewDecoder: %v", path, err)
	}
	return d
}

// mainDataBegins returns sideinfo.MainDataBegin for every compressed frame.
func mainDataBegins(t *testing.T, data []byte, frameStarts []int64) []int {
	t.Helper()
	s := &source{reader: bytes.NewReader(data)}
	begins := make([]int, len(frameStarts))
	for i, start := range frameStarts {
		if _, err := s.Seek(start, io.SeekStart); err != nil {
			t.Fatalf("frame %d: seek: %v", i, err)
		}
		h, _, err := frameheader.Read(s, start)
		if err != nil {
			t.Fatalf("frame %d: header: %v", i, err)
		}
		si, err := sideinfo.Read(s, h)
		if err != nil {
			t.Fatalf("frame %d: sideinfo: %v", i, err)
		}
		begins[i] = si.MainDataBegin
	}
	return begins
}

// compressedFrameSize returns the number of compressed bytes of the
// frame starting at start.
func compressedFrameSize(t *testing.T, data []byte, start int64) int64 {
	t.Helper()
	s := &source{reader: bytes.NewReader(data)}
	if _, err := s.Seek(start, io.SeekStart); err != nil {
		t.Fatalf("seek to frame start %d: %v", start, err)
	}
	h, _, err := frameheader.Read(s, start)
	if err != nil {
		t.Fatalf("frame header at %d: %v", start, err)
	}
	size, err := h.FrameSize()
	if err != nil {
		t.Fatalf("frame size at %d: %v", start, err)
	}
	return int64(size)
}

func checkPCMEqual(t *testing.T, label string, pos int64, got, want []byte) {
	t.Helper()
	if len(got) != len(want) {
		t.Errorf("%s: target byte position %d: length %d != %d", label, pos, len(got), len(want))
		return
	}
	for i := range got {
		if got[i] != want[i] {
			t.Errorf("%s: target byte position %d: first differing byte index %d (%#v != %#v)",
				label, pos, i, got[i], want[i])
			return
		}
	}
	t.Logf("%s: target byte position %d: first differing byte: none (%d bytes equal)", label, pos, len(got))
}

func seekAndRead(t *testing.T, d *Decoder, pos int64, pcm []byte, label string) {
	t.Helper()
	if _, err := d.Seek(pos, io.SeekStart); err != nil {
		t.Fatalf("%s: Seek(%d): %v", label, pos, err)
	}
	n := int64(512)
	if rem := int64(len(pcm)) - pos; rem < n {
		n = rem
	}
	if n <= 0 {
		t.Fatalf("%s: no bytes to read at %d", label, pos)
	}
	got := make([]byte, n)
	if _, err := io.ReadFull(d, got); err != nil {
		t.Fatalf("%s: read at %d: %v", label, pos, err)
	}
	checkPCMEqual(t, label, pos, got, pcm[pos:pos+n])
}

// parityTargets builds targets that cover frame boundaries, positions in
// the middle of a decoded frame, unaligned sample positions, and frames
// whose bit reservoir references previous frames.
func parityTargets(t *testing.T, d *Decoder, data []byte, pcmLen int64) []int64 {
	t.Helper()
	frames := len(d.frameStarts)
	bf := d.bytesPerFrame
	begins := mainDataBegins(t, data, d.frameStarts)

	maxReservoir := 0
	for i, b := range begins {
		if b > begins[maxReservoir] {
			maxReservoir = i
		}
	}

	seen := map[int64]bool{}
	var targets []int64
	add := func(frame int, inside int64) {
		if frame < 0 || frame >= frames {
			return
		}
		pos := int64(frame)*bf + inside
		if pos < 0 || pos >= pcmLen || seen[pos] {
			return
		}
		seen[pos] = true
		targets = append(targets, pos)
	}

	// Frame boundaries and mid-frame offsets. The offsets include
	// non-multiple-of-4 positions so sample-frame alignment is covered.
	frameIdx := []int{0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 15, 16, 17, 20,
		31, 32, 33, 47, 48, 63, 64, 65, 100, 127, 128, 129, 500, 1024,
		frames/2 - 1, frames / 2, frames/2 + 1, frames - 2, frames - 1}
	inside := []int64{0, 1, 2, 3, 4, 5, 7, bf/2 - 1, bf / 2, bf/2 + 1, bf - 4, bf - 1}
	for _, i := range frameIdx {
		for _, x := range inside {
			add(i, x)
		}
	}
	// Explicit bit-reservoir frames, including the one with the largest
	// main_data_begin.
	for _, i := range []int{maxReservoir, maxReservoir - 1, maxReservoir + 1, 10, 11} {
		add(i, 0)
		add(i, bf/2+1)
	}
	return targets
}

func TestSeekPCMParity(t *testing.T) {
	for _, path := range parityFiles {
		data := readFile(t, path)
		pcm := sequentialPCM(t, data, path)
		d := newSeekableDecoder(t, data, path)
		if d.Length() != int64(len(pcm)) {
			t.Fatalf("%s: Length() = %d, sequential PCM = %d", path, d.Length(), len(pcm))
		}

		targets := parityTargets(t, d, data, int64(len(pcm)))
		rand.New(rand.NewSource(int64(len(data)))).Shuffle(len(targets), func(i, j int) {
			targets[i], targets[j] = targets[j], targets[i]
		})
		t.Logf("%s: comparing %d shuffled seek targets against %d sequential PCM bytes",
			path, len(targets), len(pcm))
		for _, pos := range targets {
			seekAndRead(t, d, pos, pcm, path)
		}
	}
}

func TestSeekPCMParitySeekWithinAndEnd(t *testing.T) {
	for _, path := range parityFiles {
		data := readFile(t, path)
		pcm := sequentialPCM(t, data, path)
		d := newSeekableDecoder(t, data, path)
		length := d.Length()

		positions := []int64{0, 1, 17, 3 * d.bytesPerFrame, 5*d.bytesPerFrame + d.bytesPerFrame/2,
			int64(1000) * d.bytesPerFrame, length/2 + 3, length - 512, length - 1}
		for _, pos := range positions {
			if pos < 0 || pos >= length {
				continue
			}
			// SeekStart baseline.
			if _, err := d.Seek(pos, io.SeekStart); err != nil {
				t.Fatalf("%s: SeekStart(%d): %v", path, pos, err)
			}
			// Same target through SeekCurrent, starting from 0.
			if _, err := d.Seek(0, io.SeekStart); err != nil {
				t.Fatalf("%s: rewind: %v", path, err)
			}
			if n, err := d.Seek(pos, io.SeekCurrent); err != nil || n != pos {
				t.Fatalf("%s: SeekCurrent to %d = %d, %v", path, pos, n, err)
			}
			// Same target through SeekEnd.
			if n, err := d.Seek(pos-length, io.SeekEnd); err != nil || n != pos {
				t.Fatalf("%s: SeekEnd to %d = %d, %v", path, pos, n, err)
			}
			seekAndRead(t, d, pos, pcm, path+" seek within")
		}

		// Seek to Length is valid, then Read returns EOF and the decoder
		// can go back to the beginning.
		if n, err := d.Seek(length, io.SeekStart); err != nil || n != length {
			t.Fatalf("%s: Seek(Length=%d) = %d, %v", path, length, n, err)
		}
		if n, err := d.Read(make([]byte, 512)); n != 0 || err != io.EOF {
			t.Fatalf("%s: Read at Length = %d, %v; want 0, EOF", path, n, err)
		}
		seekAndRead(t, d, 0, pcm, path+" after Length")

		// Out-of-range and invalid whence must return errors, not panic,
		// and the decoder must remain usable.
		if _, err := d.Seek(0, io.SeekStart); err != nil {
			t.Fatalf("%s: rewind: %v", path, err)
		}
		for _, tc := range []struct {
			offset int64
			whence int
		}{
			{-1, io.SeekStart},
			{length + 1, io.SeekStart},
			{-1, io.SeekCurrent},
			{1, io.SeekEnd},
			{0, 42},
		} {
			if _, err := d.Seek(tc.offset, tc.whence); err == nil {
				t.Errorf("%s: Seek(%d, whence=%d) returned no error", path, tc.offset, tc.whence)
			}
		}
		seekAndRead(t, d, 0, pcm, path+" after errors")
	}
}

func TestSeekPCMParityReuse(t *testing.T) {
	for _, path := range parityFiles {
		data := readFile(t, path)
		pcm := sequentialPCM(t, data, path)
		d := newSeekableDecoder(t, data, path)

		// Leave 17 decoded bytes in the output buffer before seeking.
		if _, err := io.ReadFull(d, make([]byte, 17)); err != nil {
			t.Fatal(err)
		}

		bf := d.bytesPerFrame
		frames := int64(len(d.frameStarts))
		var targets []int64
		for i := int64(0); i < frames; i += 37 {
			targets = append(targets, i*bf+bf/2+1)
		}
		for i := len(targets) - 1; i >= 0; i-- {
			targets = append(targets, targets[i])
		}
		rand.New(rand.NewSource(7)).Shuffle(len(targets), func(i, j int) {
			targets[i], targets[j] = targets[j], targets[i]
		})
		for _, pos := range targets {
			if pos >= int64(len(pcm)) {
				continue
			}
			seekAndRead(t, d, pos, pcm, path+" reused")
			// Seeking to the same target twice must give the same bytes.
			seekAndRead(t, d, pos, pcm, path+" repeated")
		}
	}
}

func TestSeekPCMParityCheckpointBoundary(t *testing.T) {
	data := readFile(t, "example/mpeg2.mp3")
	probe := newSeekableDecoder(t, data, "example/mpeg2.mp3")
	if len(probe.frameStarts) < seekCheckpointInterval*2+1 {
		t.Skip("sample is too short for checkpoint-boundary truncation")
	}
	// Truncate right after a multiple of the checkpoint interval: these
	// streams end exactly on a checkpoint, including the case where seeking
	// to Length would address a non-existent frame.
	for _, n := range []int{seekCheckpointInterval, seekCheckpointInterval + 1,
		seekCheckpointInterval * 2, seekCheckpointInterval*2 + 1} {
		trunc := data[:probe.frameStarts[n]]
		pcm := sequentialPCM(t, trunc, "mpeg2 truncated")
		d := newSeekableDecoder(t, trunc, "mpeg2 truncated")
		if d.Length() != int64(len(pcm)) {
			t.Fatalf("truncated after %d frames: Length() = %d, sequential PCM = %d",
				n, d.Length(), len(pcm))
		}
		length := d.Length()
		for _, pos := range []int64{0, 1, length - 1, length} {
			if pos == length {
				if got, err := d.Seek(length, io.SeekStart); err != nil || got != length {
					t.Fatalf("truncated after %d frames: Seek(Length) = %d, %v", n, got, err)
				}
				if got, err := d.Read(make([]byte, 64)); got != 0 || err != io.EOF {
					t.Fatalf("truncated after %d frames: Read at Length = %d, %v", n, got, err)
				}
				continue
			}
			seekAndRead(t, d, pos, pcm, "mpeg2 truncated")
		}
		seekAndRead(t, d, 0, pcm, "mpeg2 truncated after Length")
	}
}

func TestSeekPCMParityNonSeekableStream(t *testing.T) {
	data := readFile(t, "example/mpeg2.mp3")
	pcm := sequentialPCM(t, data, "example/mpeg2.mp3")

	var buf bytes.Buffer
	buf.Write(data)
	// *bytes.Buffer is an io.Reader but not an io.Seeker.
	d, err := NewDecoder(&buf)
	if err != nil {
		t.Fatalf("NewDecoder(non-seeker): %v", err)
	}
	if d.Length() != invalidLength {
		t.Errorf("Length() for non-seeker = %d, want %d", d.Length(), invalidLength)
	}

	first := make([]byte, 1024)
	if _, err := io.ReadFull(d, first); err != nil {
		t.Fatalf("streaming read: %v", err)
	}
	checkPCMEqual(t, "non-seeker streaming", 0, first, pcm[:len(first)])

	for _, whence := range []int{io.SeekStart, io.SeekCurrent, io.SeekEnd} {
		if _, err := d.Seek(0, whence); err == nil {
			t.Errorf("Seek on a non-seeker (whence=%d) returned no error", whence)
		}
	}

	// A failed Seek must not disturb the streaming position.
	next := make([]byte, 512)
	if _, err := io.ReadFull(d, next); err != nil {
		t.Fatalf("streaming read after failed Seek: %v", err)
	}
	checkPCMEqual(t, "non-seeker streaming after failed Seek", 1024, next, pcm[1024:1024+512])
}

type countingReader struct {
	r     *bytes.Reader
	bytes int64
	calls int
}

func (c *countingReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.bytes += int64(n)
	c.calls++
	return n, err
}

func (c *countingReader) Seek(offset int64, whence int) (int64, error) {
	return c.r.Seek(offset, whence)
}

func TestSeekPCMParityWork(t *testing.T) {
	for _, path := range parityFiles {
		data := readFile(t, path)
		cr := &countingReader{r: bytes.NewReader(data)}
		d, err := NewDecoder(cr)
		if err != nil {
			t.Fatalf("%s: NewDecoder: %v", path, err)
		}
		length := d.Length()

		randSrc := rand.New(rand.NewSource(int64(len(data)) + 1))
		targets := []int64{0, 1, length - 1, length}
		for i := 0; i < 60; i++ {
			targets = append(targets, randSrc.Int63n(length+1))
		}
		for _, pos := range targets {
			before := cr.bytes
			if _, err := d.Seek(pos, io.SeekStart); err != nil {
				t.Fatalf("%s: Seek(%d): %v", path, pos, err)
			}
			read := cr.bytes - before

			// Bytes that should be read: exactly the compressed frames
			// from the checkpoint restart frame through the target frame
			// (inclusive). Seeking to Length reads nothing.
			decodedFrames := int64(0)
			want := int64(0)
			if pos < length {
				targetFrame := pos / d.bytesPerFrame
				restartFrame := targetFrame - targetFrame%seekCheckpointInterval
				for i := restartFrame; i <= targetFrame; i++ {
					want += compressedFrameSize(t, data, d.frameStarts[i])
				}
				decodedFrames = targetFrame - restartFrame + 1
			}
			if decodedFrames > seekCheckpointInterval {
				t.Errorf("%s: target %d re-decoded %d frames, limit is %d",
					path, pos, decodedFrames, seekCheckpointInterval)
			}
			if read != want {
				t.Errorf("%s: target %d read %d compressed bytes, want %d (%d frames)",
					path, pos, read, want, decodedFrames)
			}
			t.Logf("%s: target byte position %d: re-read %d compressed bytes across %d frames (limit %d)",
				path, pos, read, decodedFrames, seekCheckpointInterval)
		}
	}
}
