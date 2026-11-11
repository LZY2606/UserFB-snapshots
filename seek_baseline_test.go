package mp3_test

import (
	"bytes"
	"io"
	"os"
	"testing"

	mp3 "github.com/hajimehoshi/go-mp3"
)

func firstMismatch(a, b []byte) int {
	n := len(a)
	if len(b) < n {
		n = len(b)
	}
	for i := 0; i < n; i++ {
		if a[i] != b[i] {
			return i
		}
	}
	if len(a) != len(b) {
		return n
	}
	return -1
}

func checkPCM(t *testing.T, label string, pos int64, got, want []byte) {
	t.Helper()
	if i := firstMismatch(got, want); i >= 0 {
		t.Errorf("%s: target byte position %d: first differing byte index %d (%#v != %#v)",
			label, pos, i, got[i], want[i])
		return
	}
	t.Logf("%s: target byte position %d: first differing byte: none (%d bytes equal)", label, pos, len(got))
}

func readAt(t *testing.T, d *mp3.Decoder, pos int64, n int64, pcm []byte) {
	t.Helper()
	if _, err := d.Seek(pos, io.SeekStart); err != nil {
		t.Fatalf("Seek(%d) failed: %v", pos, err)
	}
	if n > int64(len(pcm))-pos {
		n = int64(len(pcm)) - pos
	}
	got := make([]byte, n)
	if _, err := io.ReadFull(d, got); err != nil {
		t.Fatalf("reading %d bytes at %d failed: %v", n, pos, err)
	}
	checkPCM(t, "seek", pos, got, pcm[pos:pos+n])
}

func TestSeekBaseline(t *testing.T) {
	data, err := os.ReadFile("example/mpeg2.mp3")
	if err != nil {
		t.Fatal(err)
	}

	// Reference: a separate decoder read sequentially from the beginning.
	ref, err := mp3.NewDecoder(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	pcm, err := io.ReadAll(ref)
	if err != nil || len(pcm) == 0 {
		t.Fatalf("decode %d %v", len(pcm), err)
	}

	const chunk = 512
	positions := []int64{9216, 18432, 36864, 46080}

	// A fresh decoder for every target.
	for _, pos := range positions {
		d, err := mp3.NewDecoder(bytes.NewReader(data))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := d.Seek(pos, io.SeekStart); err != nil {
			t.Fatalf("seek %d error=%v", pos, err)
		}
		got := make([]byte, chunk)
		if _, err := io.ReadFull(d, got); err != nil {
			t.Fatal(err)
		}
		checkPCM(t, "fresh decoder", pos, got, pcm[pos:pos+chunk])
	}

	// One decoder reused across forward, backward and repeated seeks.
	d, err := mp3.NewDecoder(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	for _, pos := range positions {
		readAt(t, d, pos, chunk, pcm)
	}
	for i := len(positions) - 1; i >= 0; i-- {
		readAt(t, d, positions[i], chunk, pcm)
	}
	for _, pos := range positions {
		readAt(t, d, pos, chunk, pcm)
		readAt(t, d, pos, chunk, pcm)
	}

	// Seeking to Length is allowed, Read returns EOF, and the decoder can
	// then return to the start.
	length := d.Length()
	n, err := d.Seek(length, io.SeekStart)
	if err != nil || n != length {
		t.Fatalf("Seek(Length=%d) = %d, %v", length, n, err)
	}
	if n, err := d.Read(make([]byte, chunk)); n != 0 || err != io.EOF {
		t.Fatalf("Read after Seek(Length) = %d, %v; want 0, EOF", n, err)
	}
	readAt(t, d, 0, chunk, pcm)

	// After a short unaligned read, seek forwards and backwards again.
	if _, err := d.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	if _, err := io.ReadFull(d, make([]byte, 17)); err != nil {
		t.Fatal(err)
	}
	for _, pos := range positions {
		readAt(t, d, pos, chunk, pcm)
	}
}
