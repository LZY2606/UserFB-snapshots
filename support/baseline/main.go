package main

import (
	"bytes"
	stdbzip "compress/bzip2"
	"crypto/sha256"
	"fmt"
	"io"
	"os"

	"github.com/dsnet/compress/bzip2"
)

func must(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

type shortReader struct{ r io.Reader }

func (s shortReader) Read(p []byte) (int, error) {
	if len(p) > 3 {
		p = p[:3]
	}
	return s.r.Read(p)
}

func main() {
	noise := make([]byte, 240017)
	x := uint32(0x425a6831)
	for i := range noise {
		x ^= x << 13
		x ^= x >> 17
		x ^= x << 5
		noise[i] = byte(x)
	}
	parts := [][]byte{noise, nil, bytes.Repeat([]byte{0x5a}, 1200000), bytes.Repeat([]byte("record\x00key=value\n"), 311)}
	levels := []int{1, 9, 2, 9}
	var archive, plain bytes.Buffer
	for i, p := range parts {
		var member bytes.Buffer
		w, err := bzip2.NewWriter(&member, &bzip2.WriterConfig{Level: levels[i]})
		must(err)
		n, err := w.Write(p)
		must(err)
		if n != len(p) {
			must(io.ErrShortWrite)
		}
		must(w.Close())
		ref, err := io.ReadAll(stdbzip.NewReader(bytes.NewReader(member.Bytes())))
		must(err)
		if !bytes.Equal(ref, p) {
			must(fmt.Errorf("standard-library decode differs: member %d", i))
		}
		fmt.Printf("MEMBER %d level=%d raw=%d compressed=%d\n", i, levels[i], len(p), member.Len())
		archive.Write(member.Bytes())
		plain.Write(p)
	}
	for _, short := range []bool{false, true} {
		var input io.Reader = bytes.NewReader(archive.Bytes())
		if short {
			input = shortReader{input}
		}
		r, err := bzip2.NewReader(input, nil)
		must(err)
		var got bytes.Buffer
		_, err = io.CopyBuffer(&got, r, make([]byte, 7))
		must(err)
		must(r.Close())
		if !bytes.Equal(got.Bytes(), plain.Bytes()) {
			must(fmt.Errorf("concatenated decode differs: short=%v", short))
		}
	}
	bad := bytes.Clone(archive.Bytes())
	bad[len(bad)-2] ^= 1
	r, err := bzip2.NewReader(bytes.NewReader(bad), nil)
	must(err)
	_, err = io.Copy(io.Discard, r)
	if err == nil {
		must(fmt.Errorf("corrupt stream checksum accepted"))
	}
	fmt.Println("SEQUENTIAL_BAD_FOOTER_REJECTED")
	fmt.Printf("BZ2_SEQUENTIAL_OK members=4 raw=%d compressed=%d sha256=%x\n", plain.Len(), archive.Len(), sha256.Sum256(plain.Bytes()))
}
