package graphql

import (
	stderrors "errors"
	"io"
	"testing"
)

// oneByteReader hands out one byte per Read, like a socket delivering tiny
// fragments, so boundary lines that span reads are exercised through bufio.
type oneByteReader struct {
	s string
	i int
}

func (r *oneByteReader) Read(p []byte) (int, error) {
	if r.i >= len(r.s) {
		return 0, io.EOF
	}
	p[0] = r.s[r.i]
	r.i++
	return 1, nil
}

func collectParts(t *testing.T, input string) ([][]byte, error) {
	t.Helper()
	r := newMixedStreamReader(&oneByteReader{s: input}, "parts")
	var got [][]byte
	for {
		body, err := r.NextPart()
		if stderrors.Is(err, io.EOF) {
			return got, nil
		}
		if err != nil {
			return got, err
		}
		got = append(got, body)
	}
}

func TestMixedStreamHappy(t *testing.T) {
	in := "--parts\r\nH: v\r\n\r\n{\"a\":1}\r\n--parts\r\nH: v\r\n\r\n{\"b\":2}\r\n--parts--\r\n"
	got, err := collectParts(t, in)
	if err != nil {
		t.Fatalf("err=%v", err)
	}
	if len(got) != 2 || string(got[0]) != `{"a":1}`+"\r\n" || string(got[1]) != `{"b":2}`+"\r\n" {
		t.Fatalf("parts=%q", got)
	}
}

func TestMixedStreamTruncation(t *testing.T) {
	cases := map[string]string{
		"no boundary at all":      "--parts\r\nH: v\r\n\r\n{\"a\":1}\r\n",
		"final doc no close":      "--parts\r\nH: v\r\n\r\n{\"a\":1}\r\n--parts\r\nH: v\r\n\r\n{\"b\":2}\r\n",
		"cut inside boundary":     "--parts\r\nH: v\r\n\r\n{\"a\":1}\r\n--par",
		"cut in headers":          "--parts\r\nH: v\r",
		"cut before first marker": "preamble garbage",
	}
	for name, in := range cases {
		_, err := collectParts(t, in)
		// Every cut must surface as a failure, never as a clean io.EOF.
		if err == nil || stderrors.Is(err, io.EOF) {
			t.Errorf("%s: err=%v, want a non-EOF error", name, err)
		}
	}
}

func TestMixedStreamCleanEOF(t *testing.T) {
	in := "--parts\r\nH: v\r\n\r\n{\"hasNext\":false}\r\n--parts--\r\n"
	got, err := collectParts(t, in)
	if err != nil || len(got) != 1 {
		t.Fatalf("got=%d err=%v", len(got), err)
	}
}
