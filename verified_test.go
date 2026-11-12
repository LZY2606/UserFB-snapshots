// Copyright (C) 2018 Minio Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package sio

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"testing"
)

var verifiedTestKey = bytes.Repeat([]byte{0x42}, 32)

func verifiedConfig(cipher byte) Config {
	return Config{
		Key:          verifiedTestKey,
		MinVersion:   Version20,
		MaxVersion:   Version20,
		CipherSuites: []byte{cipher},
		Rand:         bytes.NewReader(bytes.Repeat([]byte{0x99}, 64)),
	}
}

func verifiedPlain(size int) []byte {
	plain := make([]byte, size)
	for i := range plain {
		plain[i] = byte(i*7 + 13)
	}
	return plain
}

func encryptVerifiedObject(t *testing.T, cipher byte, plain []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	if _, err := Encrypt(&buf, bytes.NewReader(plain), verifiedConfig(cipher)); err != nil {
		t.Fatalf("failed to encrypt test object: %v", err)
	}
	return buf.Bytes()
}

func encryptVerifiedV10Object(t *testing.T, plain []byte) []byte {
	t.Helper()
	config := Config{Key: verifiedTestKey, MinVersion: Version10, MaxVersion: Version10}
	var buf bytes.Buffer
	if _, err := Encrypt(&buf, bytes.NewReader(plain), config); err != nil {
		t.Fatalf("failed to encrypt DARE 1.0 test object: %v", err)
	}
	return buf.Bytes()
}

// chunkReader limits every Read to n bytes, which lets tests feed the input
// byte-by-byte or in chunks that span package boundaries.
type chunkReader struct {
	r io.Reader
	n int
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if r.n > 0 && len(p) > r.n {
		p = p[:r.n]
	}
	return r.r.Read(p)
}

// dataErrReader returns data from data, but the Read that completes errAt
// bytes returns that data together with err in the same call.
type dataErrReader struct {
	data  []byte
	errAt int
	err   error
	pos   int
	fired bool
}

func (r *dataErrReader) Read(p []byte) (int, error) {
	if r.fired {
		return 0, r.err
	}
	if r.pos >= len(r.data) {
		return 0, io.EOF
	}
	if r.errAt > len(r.data) {
		r.errAt = len(r.data)
	}
	end := len(r.data)
	if end > r.errAt {
		end = r.errAt
	}
	if r.pos >= r.errAt {
		r.fired = true
		return 0, r.err
	}
	n := copy(p, r.data[r.pos:end])
	r.pos += n
	if r.pos >= r.errAt {
		r.fired = true
		return n, r.err
	}
	return n, nil
}

// recordingWriter counts Write calls and records accepted bytes.
type recordingWriter struct {
	buf    bytes.Buffer
	writes int
	closed bool
}

func (w *recordingWriter) Write(p []byte) (int, error) {
	w.writes++
	return w.buf.Write(p)
}

func (w *recordingWriter) Close() error {
	w.closed = true
	return nil
}

// failWriter accepts at most limit bytes, then returns err.
type failWriter struct {
	limit  int
	err    error
	buf    bytes.Buffer
	writes int
}

func (w *failWriter) Write(p []byte) (int, error) {
	w.writes++
	if w.buf.Len() >= w.limit {
		return 0, w.err
	}
	remaining := w.limit - w.buf.Len()
	if len(p) <= remaining {
		return w.buf.Write(p)
	}
	w.buf.Write(p[:remaining])
	return remaining, w.err
}

// shortWriter always reports one byte less than it accepted, without an error.
type shortWriter struct {
	writes int
	buf    bytes.Buffer
}

func (w *shortWriter) Write(p []byte) (int, error) {
	w.writes++
	if len(p) == 0 {
		return 0, nil
	}
	n, _ := w.buf.Write(p[:len(p)-1])
	return n, nil
}

var (
	errTestScratch = errors.New("scratch failure")
	errTestSeek    = errors.New("scratch seek failure")
	errTestSrc     = errors.New("src read failure")
	errTestDst     = errors.New("dst write failure")
)

// faultScratch is an in-memory io.ReadWriteSeeker with fault injection.
type faultScratch struct {
	data []byte
	off  int64

	readErr  error
	writeErr error
	seekErr  error
	failSeek int // 1-based Seek call to fail; 0 disables

	seeks  int
	writes int
	closed bool
}

func newFaultScratch(initial []byte) *faultScratch {
	return &faultScratch{data: append([]byte(nil), initial...)}
}

func (s *faultScratch) Read(p []byte) (int, error) {
	if s.readErr != nil {
		return 0, s.readErr
	}
	if s.off >= int64(len(s.data)) {
		return 0, io.EOF
	}
	n := copy(p, s.data[s.off:])
	s.off += int64(n)
	return n, nil
}

func (s *faultScratch) Write(p []byte) (int, error) {
	s.writes++
	if s.writeErr != nil {
		return 0, s.writeErr
	}
	end := int(s.off) + len(p)
	if end > len(s.data) {
		s.data = append(s.data, make([]byte, end-len(s.data))...)
	}
	copy(s.data[s.off:], p)
	s.off += int64(len(p))
	return len(p), nil
}

func (s *faultScratch) Seek(offset int64, whence int) (int64, error) {
	s.seeks++
	if s.seekErr != nil && s.failSeek == 0 {
		return 0, s.seekErr
	}
	if s.failSeek > 0 && s.seeks == s.failSeek {
		return 0, errTestSeek
	}
	var base int64
	switch whence {
	case io.SeekStart:
		base = 0
	case io.SeekCurrent:
		base = s.off
	case io.SeekEnd:
		base = int64(len(s.data))
	default:
		return 0, errors.New("invalid whence")
	}
	next := base + offset
	if next < 0 {
		return 0, errors.New("negative seek")
	}
	s.off = next
	return next, nil
}

func (s *faultScratch) Close() error {
	s.closed = true
	return nil
}

func TestVerifiedRoundTrip(t *testing.T) {
	ciphers := []struct {
		name string
		id   byte
	}{
		{"AES_GCM", AES_GCM},
		{"CHACHA20_POLY1305", CHACHA20_POLY1305},
	}
	sizes := []int{1, maxPayloadSize, maxPayloadSize + 1}
	chunks := []int{1, maxPackageSize + 1}

	for _, cipher := range ciphers {
		for _, size := range sizes {
			plain := verifiedPlain(size)
			ciphertext := encryptVerifiedObject(t, cipher.id, plain)

			for _, chunk := range chunks {
				t.Run(fmt.Sprintf("%s/reader/%d/chunk-%d", cipher.name, size, chunk), func(t *testing.T) {
					scratch := newFaultScratch(nil)
					dst := &recordingWriter{}
					n, err := DecryptVerified(dst, &chunkReader{r: bytes.NewReader(ciphertext), n: chunk}, scratch, verifiedConfig(cipher.id))
					if err != nil {
						t.Fatalf("DecryptVerified failed: %v", err)
					}
					if n != int64(size) {
						t.Fatalf("DecryptVerified returned n=%d, want %d", n, size)
					}
					if dst.writes == 0 || dst.buf.Len() != size {
						t.Fatalf("DecryptVerified published %d bytes in %d writes, want %d", dst.buf.Len(), dst.writes, size)
					}
					if !bytes.Equal(dst.buf.Bytes(), plain) {
						t.Fatalf("DecryptVerified plaintext mismatch")
					}
					if dst.closed || scratch.closed {
						t.Fatalf("DecryptVerified closed a borrowed object")
					}
				})
			}

			for _, chunk := range chunks {
				t.Run(fmt.Sprintf("%s/writer/%d/chunk-%d", cipher.name, size, chunk), func(t *testing.T) {
					scratch := newFaultScratch(nil)
					dst := &recordingWriter{}
					w, err := DecryptVerifiedWriter(dst, scratch, verifiedConfig(cipher.id))
					if err != nil {
						t.Fatalf("DecryptVerifiedWriter failed: %v", err)
					}
					for off := 0; off < len(ciphertext); {
						end := min(off+chunk, len(ciphertext))
						n, err := w.Write(ciphertext[off:end])
						if err != nil {
							t.Fatalf("Write failed: %v", err)
						}
						if n != end-off {
							t.Fatalf("Write returned n=%d, want %d", n, end-off)
						}
						if dst.writes != 0 {
							t.Fatalf("Write published data before Close")
						}
						off = end
					}
					if err := w.Close(); err != nil {
						t.Fatalf("Close failed: %v", err)
					}
					if dst.buf.Len() != size || !bytes.Equal(dst.buf.Bytes(), plain) {
						t.Fatalf("Close published %d bytes, want %d", dst.buf.Len(), size)
					}
					if dst.closed || scratch.closed {
						t.Fatalf("DecryptVerifiedWriter closed a borrowed object")
					}
				})
			}
		}
	}
}

func TestVerifiedRejectsInvalidInput(t *testing.T) {
	plain := verifiedPlain(maxPayloadSize + 1) // two packages
	valid := encryptVerifiedObject(t, AES_GCM, plain)

	badTag := append([]byte(nil), valid...)
	badTag[len(badTag)-1] ^= 0x01

	missingFinal := append([]byte(nil), valid[:maxPackageSize]...)

	badFinalBit := append([]byte(nil), valid...)
	badFinalBit[len(badFinalBit)-(headerSize+1+tagSize)+4] ^= 0x80

	// Setting the final bit on a full, non-final package changes the nonce
	// used for authentication but not the tag, so authentication fails.
	longPlain := verifiedPlain(2*maxPayloadSize + 1)
	longObject := encryptVerifiedObject(t, AES_GCM, longPlain)
	badFinalSet := append([]byte(nil), longObject...)
	badFinalSet[4] |= 0x80

	trailingObject := append(append([]byte(nil), valid...), valid...)

	cases := []struct {
		name string
		data []byte
		err  error
	}{
		{"empty", nil, errUnexpectedEOF},
		{"dare-v1", encryptVerifiedV10Object(t, verifiedPlain(1024)), errUnsupportedVersion},
		{"bad-tail-tag", badTag, errTagMismatch},
		{"missing-final-package", missingFinal, errUnexpectedEOF},
		{"final-bit-cleared", badFinalBit, errInvalidPayloadSize},
		{"final-bit-set-on-non-final", badFinalSet, errTagMismatch},
		{"trailing-object", trailingObject, errUnexpectedData},
		{"truncated-header", valid[:10], errInvalidPayloadSize},
	}

	for _, test := range cases {
		t.Run("reader/"+test.name, func(t *testing.T) {
			scratch := newFaultScratch(bytes.Repeat([]byte{0xEE}, 4096))
			dst := &recordingWriter{}
			n, err := DecryptVerified(dst, bytes.NewReader(test.data), scratch, verifiedConfig(AES_GCM))
			if err == nil {
				t.Fatalf("DecryptVerified accepted malformed input %q (n=%d)", test.name, n)
			}
			if test.err != nil && err != test.err {
				t.Fatalf("DecryptVerified returned %v, want %v", err, test.err)
			}
			if dst.writes != 0 || dst.buf.Len() != 0 {
				t.Fatalf("DecryptVerified published %d bytes in %d writes for malformed input %q", dst.buf.Len(), dst.writes, test.name)
			}
		})

		t.Run("writer/"+test.name, func(t *testing.T) {
			scratch := newFaultScratch(bytes.Repeat([]byte{0xEE}, 4096))
			dst := &recordingWriter{}
			w, err := DecryptVerifiedWriter(dst, scratch, verifiedConfig(AES_GCM))
			if err != nil {
				t.Fatalf("DecryptVerifiedWriter failed: %v", err)
			}
			for off := 0; off < len(test.data); off += 1000 {
				end := min(off+1000, len(test.data))
				if _, err := w.Write(test.data[off:end]); err != nil {
					t.Fatalf("Write failed while staging: %v", err)
				}
			}
			closeErr := w.Close()
			if closeErr == nil {
				t.Fatalf("Close accepted malformed input %q", test.name)
			}
			if test.err != nil && closeErr != test.err {
				t.Fatalf("Close returned %v, want %v", closeErr, test.err)
			}
			if dst.writes != 0 || dst.buf.Len() != 0 {
				t.Fatalf("Close published %d bytes in %d writes for malformed input %q", dst.buf.Len(), dst.writes, test.name)
			}
			if again := w.Close(); again != closeErr {
				t.Fatalf("second Close returned %v, want %v", again, closeErr)
			}
			if _, err := w.Write(test.data); err != closeErr {
				t.Fatalf("Write after failed Close returned %v, want %v", err, closeErr)
			}
			if dst.writes != 0 {
				t.Fatalf("terminal writer touched dst")
			}
		})
	}

	t.Run("config-excludes-v2", func(t *testing.T) {
		config := verifiedConfig(AES_GCM)
		config.MinVersion = Version10
		config.MaxVersion = Version10
		scratch := newFaultScratch(nil)
		dst := &recordingWriter{}
		if _, err := DecryptVerified(dst, bytes.NewReader(valid), scratch, config); err != errUnsupportedVersion {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errUnsupportedVersion)
		}
		if _, err := DecryptVerifiedWriter(dst, scratch, config); err != errUnsupportedVersion {
			t.Fatalf("DecryptVerifiedWriter returned %v, want %v", err, errUnsupportedVersion)
		}
		if dst.writes != 0 {
			t.Fatalf("config rejection touched dst")
		}
	})

	t.Run("non-zero-sequence-number", func(t *testing.T) {
		config := verifiedConfig(AES_GCM)
		config.SequenceNumber = 7
		scratch := newFaultScratch(nil)
		dst := &recordingWriter{}
		if _, err := DecryptVerified(dst, bytes.NewReader(valid), scratch, config); err != errVerifiedSequence {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errVerifiedSequence)
		}
		if _, err := DecryptVerifiedWriter(dst, scratch, config); err != errVerifiedSequence {
			t.Fatalf("DecryptVerifiedWriter returned %v, want %v", err, errVerifiedSequence)
		}
		if dst.writes != 0 {
			t.Fatalf("sequence rejection touched dst")
		}
	})
}

func TestVerifiedReadReturnsDataWithError(t *testing.T) {
	plain := verifiedPlain(2*maxPayloadSize + 1) // three packages
	ciphertext := encryptVerifiedObject(t, AES_GCM, plain)

	t.Run("error-with-full-header", func(t *testing.T) {
		src := &dataErrReader{data: ciphertext, errAt: headerSize, err: errTestSrc}
		scratch := newFaultScratch(nil)
		dst := &recordingWriter{}
		n, err := DecryptVerified(dst, src, scratch, verifiedConfig(AES_GCM))
		if !errors.Is(err, errTestSrc) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestSrc)
		}
		if n != 0 || dst.writes != 0 {
			t.Fatalf("DecryptVerified published n=%d in %d writes, want none", n, dst.writes)
		}
	})

	t.Run("error-after-authenticated-prefix", func(t *testing.T) {
		src := &dataErrReader{data: ciphertext, errAt: 2 * maxPackageSize, err: errTestSrc}
		scratch := newFaultScratch(nil)
		dst := &recordingWriter{}
		n, err := DecryptVerified(dst, src, scratch, verifiedConfig(AES_GCM))
		if !errors.Is(err, errTestSrc) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestSrc)
		}
		if n != 0 || dst.writes != 0 || dst.buf.Len() != 0 {
			t.Fatalf("DecryptVerified published an authenticated prefix: n=%d writes=%d len=%d", n, dst.writes, dst.buf.Len())
		}
	})
}

func TestVerifiedScratchOldTail(t *testing.T) {
	oldTail := bytes.Repeat([]byte{0xEE}, 128*1024)
	plain := verifiedPlain(1)
	ciphertext := encryptVerifiedObject(t, CHACHA20_POLY1305, plain)

	t.Run("reader", func(t *testing.T) {
		scratch := newFaultScratch(oldTail)
		dst := &recordingWriter{}
		n, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, verifiedConfig(CHACHA20_POLY1305))
		if err != nil {
			t.Fatalf("DecryptVerified failed: %v", err)
		}
		if n != 1 || dst.buf.Len() != 1 || !bytes.Equal(dst.buf.Bytes(), plain) {
			t.Fatalf("DecryptVerified published n=%d, %d bytes; want exactly the plaintext", n, dst.buf.Len())
		}
		if bytes.Contains(dst.buf.Bytes(), []byte{0xEE}) {
			t.Fatalf("old scratch data escaped")
		}
	})

	t.Run("writer", func(t *testing.T) {
		scratch := newFaultScratch(oldTail)
		dst := &recordingWriter{}
		w, err := DecryptVerifiedWriter(dst, scratch, verifiedConfig(CHACHA20_POLY1305))
		if err != nil {
			t.Fatalf("DecryptVerifiedWriter failed: %v", err)
		}
		if _, err := w.Write(ciphertext); err != nil {
			t.Fatalf("Write failed: %v", err)
		}
		if err := w.Close(); err != nil {
			t.Fatalf("Close failed: %v", err)
		}
		if dst.buf.Len() != 1 || !bytes.Equal(dst.buf.Bytes(), plain) {
			t.Fatalf("Close published %d bytes; want exactly the plaintext", dst.buf.Len())
		}
	})
}

func TestVerifiedPublishFailures(t *testing.T) {
	plain := verifiedPlain(1000)
	ciphertext := encryptVerifiedObject(t, AES_GCM, plain)

	t.Run("dst-fails-immediately", func(t *testing.T) {
		dst := &failWriter{limit: 0, err: errTestDst}
		scratch := newFaultScratch(nil)
		n, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, verifiedConfig(AES_GCM))
		if !errors.Is(err, errTestDst) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestDst)
		}
		if n != 0 || dst.buf.Len() != 0 {
			t.Fatalf("DecryptVerified reported n=%d, published %d bytes", n, dst.buf.Len())
		}
	})

	t.Run("dst-fails-partway", func(t *testing.T) {
		dst := &failWriter{limit: 100, err: errTestDst}
		scratch := newFaultScratch(nil)
		n, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, verifiedConfig(AES_GCM))
		if !errors.Is(err, errTestDst) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestDst)
		}
		if n != 100 || dst.buf.Len() != 100 {
			t.Fatalf("DecryptVerified reported n=%d, published %d bytes; want 100", n, dst.buf.Len())
		}
	})

	t.Run("dst-short-write", func(t *testing.T) {
		dst := &shortWriter{}
		scratch := newFaultScratch(nil)
		n, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, verifiedConfig(AES_GCM))
		if !errors.Is(err, io.ErrShortWrite) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, io.ErrShortWrite)
		}
		if n != 999 || dst.buf.Len() != 999 {
			t.Fatalf("DecryptVerified reported n=%d, published %d bytes; want 999", n, dst.buf.Len())
		}
	})

	t.Run("seek-before-publish-fails", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		scratch.failSeek = 2 // first Seek is at entry, second is before publishing
		dst := &recordingWriter{}
		_, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, verifiedConfig(AES_GCM))
		if !errors.Is(err, errTestSeek) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestSeek)
		}
		if dst.writes != 0 {
			t.Fatalf("DecryptVerified touched dst after a pre-publish Seek failure")
		}
	})

	t.Run("seek-at-entry-fails", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		scratch.failSeek = 1
		dst := &recordingWriter{}
		if _, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, verifiedConfig(AES_GCM)); !errors.Is(err, errTestSeek) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestSeek)
		}
		if dst.writes != 0 {
			t.Fatalf("DecryptVerified touched dst after an entry Seek failure")
		}
	})
}

func TestVerifiedStagingFailures(t *testing.T) {
	plain := verifiedPlain(2*maxPayloadSize + 1)
	ciphertext := encryptVerifiedObject(t, AES_GCM, plain)
	config := verifiedConfig(AES_GCM)

	t.Run("reader-scratch-write-fails", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		scratch.writeErr = errTestScratch
		dst := &recordingWriter{}
		_, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, config)
		if !errors.Is(err, errTestScratch) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestScratch)
		}
		if dst.writes != 0 {
			t.Fatalf("DecryptVerified touched dst after a scratch write failure")
		}
	})

	t.Run("reader-scratch-read-fails", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		scratch.readErr = errTestScratch
		dst := &recordingWriter{}
		_, err := DecryptVerified(dst, bytes.NewReader(ciphertext), scratch, config)
		if !errors.Is(err, errTestScratch) {
			t.Fatalf("DecryptVerified returned %v, want %v", err, errTestScratch)
		}
		if dst.writes != 0 {
			t.Fatalf("DecryptVerified touched dst after a scratch read failure")
		}
	})

	t.Run("writer-staging-write-fails", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		scratch.writeErr = errTestScratch
		dst := &recordingWriter{}
		w, err := DecryptVerifiedWriter(dst, scratch, config)
		if err != nil {
			t.Fatalf("DecryptVerifiedWriter failed: %v", err)
		}
		if _, err := w.Write(ciphertext); !errors.Is(err, errTestScratch) {
			t.Fatalf("Write returned %v, want %v", err, errTestScratch)
		}
		scratchWrites := scratch.writes
		if _, err := w.Write(ciphertext); !errors.Is(err, errTestScratch) {
			t.Fatalf("terminal Write returned %v, want %v", err, errTestScratch)
		}
		if scratch.writes != scratchWrites {
			t.Fatalf("terminal Write touched scratch")
		}
		if err := w.Close(); !errors.Is(err, errTestScratch) {
			t.Fatalf("Close returned %v, want %v", err, errTestScratch)
		}
		if dst.writes != 0 {
			t.Fatalf("terminal writer touched dst")
		}
		if again := w.Close(); !errors.Is(again, errTestScratch) {
			t.Fatalf("second Close returned %v, want %v", again, errTestScratch)
		}
	})

	t.Run("writer-verification-write-fails", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		dst := &recordingWriter{}
		w, err := DecryptVerifiedWriter(dst, scratch, config)
		if err != nil {
			t.Fatalf("DecryptVerifiedWriter failed: %v", err)
		}
		if _, err := w.Write(ciphertext); err != nil {
			t.Fatalf("Write failed: %v", err)
		}
		scratch.writeErr = errTestScratch // fail while writing decrypted data into scratch
		if err := w.Close(); !errors.Is(err, errTestScratch) {
			t.Fatalf("Close returned %v, want %v", err, errTestScratch)
		}
		if dst.writes != 0 {
			t.Fatalf("Close touched dst after a scratch write failure")
		}
		if again := w.Close(); !errors.Is(again, errTestScratch) {
			t.Fatalf("second Close returned %v, want %v", again, errTestScratch)
		}
	})

	t.Run("writer-close-seek-fails", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		dst := &recordingWriter{}
		w, err := DecryptVerifiedWriter(dst, scratch, config)
		if err != nil {
			t.Fatalf("DecryptVerifiedWriter failed: %v", err)
		}
		if _, err := w.Write(ciphertext); err != nil {
			t.Fatalf("Write failed: %v", err)
		}
		scratch.seekErr = errTestSeek
		if err := w.Close(); !errors.Is(err, errTestSeek) {
			t.Fatalf("Close returned %v, want %v", err, errTestSeek)
		}
		if dst.writes != 0 {
			t.Fatalf("Close touched dst after a Seek failure")
		}
	})
}

func TestVerifiedWriterCloseSemantics(t *testing.T) {
	plain := verifiedPlain(maxPayloadSize + 1)
	ciphertext := encryptVerifiedObject(t, AES_GCM, plain)
	config := verifiedConfig(AES_GCM)

	t.Run("repeated-close-success", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		dst := &recordingWriter{}
		w, err := DecryptVerifiedWriter(dst, scratch, config)
		if err != nil {
			t.Fatalf("DecryptVerifiedWriter failed: %v", err)
		}
		if _, err := w.Write(ciphertext); err != nil {
			t.Fatalf("Write failed: %v", err)
		}
		if err := w.Close(); err != nil {
			t.Fatalf("first Close failed: %v", err)
		}
		writesAfterFirst := dst.writes
		if err := w.Close(); err != nil {
			t.Fatalf("second Close failed: %v", err)
		}
		if dst.writes != writesAfterFirst {
			t.Fatalf("second Close published again")
		}
		if dst.buf.Len() != len(plain) || !bytes.Equal(dst.buf.Bytes(), plain) {
			t.Fatalf("Close published %d bytes, want %d", dst.buf.Len(), len(plain))
		}
		if _, err := w.Write(ciphertext); err != errWriteAfterClose {
			t.Fatalf("Write after successful Close returned %v, want %v", err, errWriteAfterClose)
		}
		if dst.writes != writesAfterFirst {
			t.Fatalf("Write after successful Close touched dst")
		}
	})

	t.Run("publish-failure-is-terminal", func(t *testing.T) {
		scratch := newFaultScratch(nil)
		dst := &failWriter{limit: 10, err: errTestDst}
		w, err := DecryptVerifiedWriter(dst, scratch, config)
		if err != nil {
			t.Fatalf("DecryptVerifiedWriter failed: %v", err)
		}
		if _, err := w.Write(ciphertext); err != nil {
			t.Fatalf("Write failed: %v", err)
		}
		if err := w.Close(); !errors.Is(err, errTestDst) {
			t.Fatalf("Close returned %v, want %v", err, errTestDst)
		}
		if dst.buf.Len() != 10 {
			t.Fatalf("Close published %d bytes, want 10", dst.buf.Len())
		}
		writesAfterFirst := dst.writes
		if err := w.Close(); !errors.Is(err, errTestDst) {
			t.Fatalf("second Close returned %v, want %v", err, errTestDst)
		}
		if dst.writes != writesAfterFirst || dst.buf.Len() != 10 {
			t.Fatalf("second Close published again")
		}
		if _, err := w.Write(ciphertext); !errors.Is(err, errTestDst) {
			t.Fatalf("Write after failed Close returned %v, want %v", err, errTestDst)
		}
		if dst.writes != writesAfterFirst {
			t.Fatalf("Write after failed Close touched dst")
		}
	})
}
