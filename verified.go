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
	"errors"
	"io"
)

var errVerifiedSequence = Error{"sio: verified decryption requires sequence number 0"}

// DecryptVerified decrypts a complete DARE 2.0 object read from src and
// publishes the plaintext to dst only after the entire object has been
// authenticated.
//
// Unlike Decrypt, DecryptVerified never writes to dst if any part of the
// object is malformed or not authentic. It only accepts a non-empty DARE 2.0
// object that starts at sequence number zero; a Config that excludes version
// 2.0, a non-zero SequenceNumber, DARE 1.0 input and empty input are rejected.
// Any data following the final package is an error, even if it forms another
// valid object.
//
// scratch must be a caller-owned temporary store. DecryptVerified uses it
// starting at offset zero and never closes it. The store may contain old
// data; on success only the plaintext of this object is published, so old
// trailing data is never written to dst. After the call scratch may still
// contain plaintext and is owned by the caller.
//
// During authentication DecryptVerified writes plaintext to scratch. If
// authentication, reading src, writing scratch or seeking scratch before
// publishing fails, dst receives no Write call. Once authentication is
// complete, failures writing to dst follow ordinary stream semantics: the
// returned count is the number of plaintext bytes actually published, a
// short write without an error becomes io.ErrShortWrite, already published
// bytes are not rolled back, and returned I/O errors can be matched with
// errors.Is. DecryptVerified does not close dst, src or scratch.
func DecryptVerified(dst io.Writer, src io.Reader, scratch io.ReadWriteSeeker, config Config) (int64, error) {
	if err := setConfigDefaults(&config); err != nil {
		return 0, err
	}
	if config.MaxVersion != Version20 {
		return 0, errUnsupportedVersion
	}
	if config.SequenceNumber != 0 {
		return 0, errVerifiedSequence
	}
	if _, err := scratch.Seek(0, io.SeekStart); err != nil {
		return 0, err
	}

	size, err := decryptVerifiedInto(scratch, src, &config)
	if err != nil {
		return 0, err
	}
	if _, err := scratch.Seek(0, io.SeekStart); err != nil {
		return 0, err
	}
	return publishVerified(dst, scratch, size)
}

// DecryptVerifiedWriter wraps dst and returns an io.WriteCloser which accepts
// encrypted DARE 2.0 data and publishes its plaintext to dst on Close, after
// the whole object has been authenticated.
//
// Write only accepts and stages ciphertext in scratch; it never writes to dst. Close
// verifies the complete object and then publishes the plaintext. It uses the
// same verification as DecryptVerified and therefore accepts only a non-empty
// DARE 2.0 object starting at sequence number zero. Once authentication or
// staging fails, the writer is terminal: subsequent Write calls neither touch
// dst nor scratch, and Close keeps returning the same error without
// publishing. After a successful Close, Write returns errWriteAfterClose.
// Calling Close more than once returns the same result and never publishes
// twice.
//
// scratch must be a caller-owned temporary store used from offset zero. The
// returned io.WriteCloser does not close dst or scratch. After Close scratch
// may still contain plaintext (and staged ciphertext) and is owned by the
// caller.
func DecryptVerifiedWriter(dst io.Writer, scratch io.ReadWriteSeeker, config Config) (io.WriteCloser, error) {
	if err := setConfigDefaults(&config); err != nil {
		return nil, err
	}
	if config.MaxVersion != Version20 {
		return nil, errUnsupportedVersion
	}
	if config.SequenceNumber != 0 {
		return nil, errVerifiedSequence
	}
	if _, err := scratch.Seek(0, io.SeekStart); err != nil {
		return nil, err
	}
	return &verifiedWriter{
		dst:     dst,
		scratch: scratch,
		config:  config,
	}, nil
}

type verifiedWriter struct {
	dst     io.Writer
	scratch io.ReadWriteSeeker
	config  Config

	staged   int64 // number of ciphertext bytes staged in scratch
	closeErr error
	closed   bool
}

func (w *verifiedWriter) Write(p []byte) (int, error) {
	if w.closeErr != nil {
		return 0, w.closeErr
	}
	if w.closed {
		return 0, errWriteAfterClose
	}
	if len(p) == 0 {
		return 0, nil
	}
	if _, err := w.scratch.Seek(w.staged, io.SeekStart); err != nil {
		w.closeErr = err
		return 0, err
	}
	n, err := writeAll(w.scratch, p)
	w.staged += int64(n)
	if err != nil {
		w.closeErr = err
		return n, err
	}
	return n, nil
}

func (w *verifiedWriter) Close() error {
	if w.closed {
		return w.closeErr
	}
	w.closed = true
	if w.closeErr != nil {
		return w.closeErr
	}
	if w.staged == 0 {
		w.closeErr = errUnexpectedEOF
		return w.closeErr
	}

	if _, err := w.scratch.Seek(0, io.SeekStart); err != nil {
		w.closeErr = err
		return err
	}

	src := &seekReader{rs: w.scratch, limit: w.staged}
	dst := &seekWriter{rs: w.scratch}
	size, err := decryptVerifiedInto(dst, src, &w.config)
	if err != nil {
		w.closeErr = err
		return err
	}
	if _, err := w.scratch.Seek(0, io.SeekStart); err != nil {
		w.closeErr = err
		return err
	}
	_, err = publishVerified(w.dst, w.scratch, size)
	w.closeErr = err
	return err
}

// decryptVerifiedInto verifies every package of a complete DARE 2.0 object,
// using the same authDecV20 implementation as the streaming reader and writer.
// It writes authenticated plaintext to dst and returns its total length. The
// caller must only publish dst's content after this function returned nil.
func decryptVerifiedInto(dst io.Writer, src io.Reader, config *Config) (int64, error) {
	ad, err := newAuthDecV20(config)
	if err != nil {
		return 0, err
	}
	buffer, recycle := getBuffer()
	defer recycle()
	plaintext := make([]byte, maxPayloadSize)

	var size int64
	for {
		var header [headerSize]byte
		n, err := readFull(src, header[:])
		if err != nil {
			if errors.Is(err, io.EOF) && n == 0 {
				// Empty input as well as input that ends before the
				// authenticated final package.
				return 0, errUnexpectedEOF
			}
			if errors.Is(err, io.ErrUnexpectedEOF) {
				return 0, errInvalidPayloadSize
			}
			return 0, err
		}
		if header[0] != Version20 {
			return 0, errUnsupportedVersion
		}

		payloadLen := headerV20(header[:]).Length()
		packageLen := headerSize + payloadLen + tagSize
		copy(buffer[:headerSize], header[:])
		if _, err := readFull(src, buffer[headerSize:packageLen]); err != nil {
			if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
				return 0, errInvalidPayloadSize
			}
			return 0, err
		}

		// Open performs the package authentication as well as the
		// sequence-number, nonce and final-package checks.
		if err := ad.Open(plaintext[:payloadLen], buffer[:packageLen]); err != nil {
			return 0, err
		}
		if _, err := writeAll(dst, plaintext[:payloadLen]); err != nil {
			return 0, err
		}
		size += int64(payloadLen)

		if ad.finalized {
			var extra [1]byte
			n, err := readFull(src, extra[:])
			if n > 0 {
				return 0, errUnexpectedData // trailing bytes, even another object
			}
			if err == nil {
				// A reader returning (0, nil) is invalid but not fatal
				// for the single extra-byte probe; report no extra data.
				return size, nil
			}
			if errors.Is(err, io.EOF) {
				return size, nil
			}
			return 0, err
		}
	}
}

// readFull behaves like io.ReadFull but reports a non-EOF error that a
// reader returned together with the final bytes that filled buf. Truncation
// is reported as io.ErrUnexpectedEOF.
func readFull(r io.Reader, buf []byte) (n int, err error) {
	for n < len(buf) && err == nil {
		var nn int
		nn, err = r.Read(buf[n:])
		n += nn
	}
	if n >= len(buf) {
		if errors.Is(err, io.EOF) {
			return n, nil
		}
		return n, err
	}
	if n > 0 && errors.Is(err, io.EOF) {
		return n, io.ErrUnexpectedEOF
	}
	return n, err
}

func writeAll(w io.Writer, p []byte) (int, error) {
	var n int
	for n < len(p) {
		nn, err := w.Write(p[n:])
		n += nn
		if err != nil {
			return n, err
		}
		if nn == 0 {
			return n, io.ErrShortWrite
		}
	}
	return n, nil
}

func publishVerified(dst io.Writer, src io.Reader, size int64) (int64, error) {
	var published int64
	buffer := make([]byte, 32*1024)
	for published < size {
		remaining := size - published
		if remaining < int64(len(buffer)) {
			buffer = buffer[:remaining]
		}
		n, err := src.Read(buffer)
		if n > 0 {
			wn, werr := dst.Write(buffer[:n])
			published += int64(wn)
			if werr != nil {
				return published, werr
			}
			if wn != n {
				return published, io.ErrShortWrite
			}
		}
		if err != nil {
			return published, err
		}
		if n == 0 {
			return published, io.ErrNoProgress
		}
	}
	return published, nil
}

type seekReader struct {
	rs    io.ReadWriteSeeker
	off   int64
	limit int64
}

func (r *seekReader) Read(p []byte) (int, error) {
	if r.limit >= 0 {
		remaining := r.limit - r.off
		if remaining <= 0 {
			return 0, io.EOF
		}
		if int64(len(p)) > remaining {
			p = p[:remaining]
		}
	}
	if _, err := r.rs.Seek(r.off, io.SeekStart); err != nil {
		return 0, err
	}
	n, err := r.rs.Read(p)
	r.off += int64(n)
	return n, err
}

type seekWriter struct {
	rs  io.ReadWriteSeeker
	off int64
}

func (w *seekWriter) Write(p []byte) (int, error) {
	if _, err := w.rs.Seek(w.off, io.SeekStart); err != nil {
		return 0, err
	}
	n, err := w.rs.Write(p)
	w.off += int64(n)
	return n, err
}
