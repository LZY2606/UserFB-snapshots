package gos7

// Copyright 2018 Trung Hieu Le. All rights reserved.
// This software may be modified and distributed under the terms
// of the BSD license. See the LICENSE file for details.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"testing"
	"time"
)

// ---- frame builders: every response is constructed independently ----------

// okItem builds a successful result item (return code 0xFF) followed by its
// body. When pad is true an explicit padding byte is appended.
func okItem(transportSize byte, declaredLength int, body []byte, pad bool) []byte {
	b := []byte{0xFF, transportSize, byte(declaredLength >> 8), byte(declaredLength)}
	b = append(b, body...)
	if pad {
		b = append(b, 0)
	}
	return b
}

// failItem builds a per-item CPU error. Such an item only carries a 4 byte
// header and has no body.
func failItem(status byte) []byte {
	return []byte{status, 0, 0, 0}
}

// wrapS7 wraps a raw item section into a complete TPKT/COTP/S7 read response.
// TPKT length and the S7 declared data length are derived from the actual
// payload, so they always agree with the bytes handed to the transporter.
func wrapS7(itemCount byte, payload []byte) []byte {
	pdu := []byte{
		3, 0, 0, 0, // TPKT
		2, 0xf0, 0x80, // COTP
		0x32, 3, // S7 id, ROSCTR=ack data
		0, 0, 0, 1, // redundancy, PDU reference
		0, 2, // parameter length
		0, 0, // data length (patched below)
		0, 0, // global error code
		4, itemCount, // function read var, item count
	}
	pdu = append(pdu, payload...)
	binary.BigEndian.PutUint16(pdu[15:], uint16(len(payload)))
	binary.BigEndian.PutUint16(pdu[2:], uint16(len(pdu)))
	return pdu
}

// pipeHandler wires a real TCPClientHandler to one end of net.Pipe. The peer
// reads exactly one request (respecting the TPKT boundary) and writes reply,
// split into the given chunks when chunking needs to be controlled.
func pipeHandler(t *testing.T, reply []byte, chunks ...[]byte) (*TCPClientHandler, chan error) {
	t.Helper()
	a, b := net.Pipe()
	h := NewTCPClientHandler("127.0.0.1:102", 0, 1)
	h.conn = a
	h.PDULength = 480
	h.IdleTimeout = 0
	h.Timeout = 5 * time.Second
	done := make(chan error, 1)
	go func() {
		defer b.Close()
		head := make([]byte, 4)
		if _, e := io.ReadFull(b, head); e != nil {
			done <- e
			return
		}
		rest := int(binary.BigEndian.Uint16(head[2:])) - 4
		if _, e := io.ReadFull(b, make([]byte, rest)); e != nil {
			done <- e
			return
		}
		if len(chunks) == 0 {
			_, e := b.Write(reply)
			done <- e
			return
		}
		for _, c := range chunks {
			if _, e := b.Write(c); e != nil {
				done <- e
				return
			}
		}
		done <- nil
	}()
	return h, done
}

func dbItem(wordLen, start, amount int, data []byte) S7DataItem {
	return S7DataItem{Area: s7areadb, WordLen: wordLen, DBNumber: 1, Start: start, Amount: amount, Data: data}
}

// TestMultiReadResultIntegrity covers the multi variable read result handling
// end to end through the real TCP transporter.
func TestMultiReadResultIntegrity(t *testing.T) {
	t.Run("truncated second item is an error, not a zero", func(t *testing.T) {
		items := []S7DataItem{
			dbItem(s7wlbyte, 0, 1, []byte{0xEE}),
			dbItem(s7wlword, 2, 1, []byte{0xEE, 0xEE}),
		}
		payload := append(okItem(tsResByte, 8, []byte{0x11}, true),
			okItem(tsResByte, 16, []byte{0x22, 0x33}, false)...)
		full := wrapS7(2, payload)

		// Complete frame: values must land verbatim.
		h, done := pipeHandler(t, full)
		if err := NewClient(h).AGReadMulti(items, 2); err != nil {
			t.Fatalf("full read: %v", err)
		}
		h.Close()
		if err := <-done; err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(items[0].Data, []byte{0x11}) || !bytes.Equal(items[1].Data, []byte{0x22, 0x33}) {
			t.Fatalf("full read values = %x / %x", items[0].Data, items[1].Data)
		}

		// Same frame with the last byte removed (TPKT length adjusted by the
		// wrapper): the missing byte must surface as an error and never be
		// interpreted as a real zero.
		short := wrapS7(2, payload[:len(payload)-1])
		items[0].Data, items[1].Data = []byte{0xEE}, []byte{0xEE, 0xEE}
		h, done = pipeHandler(t, short)
		err := NewClient(h).AGReadMulti(items, 2)
		h.Close()
		if err == nil {
			t.Fatal("truncated frame accepted without error")
		}
		if serr := <-done; serr != nil && !errors.Is(serr, io.ErrClosedPipe) {
			t.Fatal(serr)
		}
		if !bytes.Equal(items[0].Data, []byte{0xEE}) || !bytes.Equal(items[1].Data, []byte{0xEE, 0xEE}) {
			t.Fatalf("caller data changed on truncated frame: %x / %x", items[0].Data, items[1].Data)
		}
		if items[0].Error != "" || items[1].Error != "" {
			t.Fatalf("stale/error strings written on truncated frame: %q / %q", items[0].Error, items[1].Error)
		}

		// The same complete frame delivered in controlled small chunks must be
		// reassembled by the transporter and parsed identically.
		items[0].Data, items[1].Data = []byte{0xEE}, []byte{0xEE, 0xEE}
		chunks := [][]byte{full[:3], full[3:11], full[11:17], full[17:]}
		h, done = pipeHandler(t, full, chunks...)
		if err = NewClient(h).AGReadMulti(items, 2); err != nil {
			t.Fatalf("chunked read: %v", err)
		}
		h.Close()
		if err = <-done; err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(items[0].Data, []byte{0x11}) || !bytes.Equal(items[1].Data, []byte{0x22, 0x33}) {
			t.Fatalf("chunked read values = %x / %x", items[0].Data, items[1].Data)
		}

		// One byte at a time is the most aggressive fragmentation pattern.
		items[0].Data, items[1].Data = []byte{0xEE}, []byte{0xEE, 0xEE}
		oneByte := make([][]byte, len(full))
		for i := range full {
			oneByte[i] = full[i : i+1]
		}
		h, done = pipeHandler(t, full, oneByte...)
		if err = NewClient(h).AGReadMulti(items, 2); err != nil {
			t.Fatalf("one-byte chunked read: %v", err)
		}
		h.Close()
		if err = <-done; err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(items[0].Data, []byte{0x11}) || !bytes.Equal(items[1].Data, []byte{0x22, 0x33}) {
			t.Fatalf("one-byte chunked values = %x / %x", items[0].Data, items[1].Data)
		}
	})

	t.Run("success failure success keeps other readings", func(t *testing.T) {
		items := []S7DataItem{
			dbItem(s7wlword, 0, 1, []byte{0xEE, 0xEE}),
			dbItem(s7wlword, 2, 1, []byte{0xEE, 0xEE}),
			dbItem(s7wlword, 4, 1, []byte{0xEE, 0xEE}),
		}
		payload := bytes.Join([][]byte{
			okItem(tsResByte, 16, []byte{0xA1, 0xA2}, false),
			failItem(code7AddressOutOfRange), // 0x05: address out of range
			okItem(tsResByte, 16, []byte{0xC1, 0xC2}, false),
		}, nil)
		h, done := pipeHandler(t, wrapS7(3, payload))
		err := NewClient(h).AGReadMulti(items, 3)
		h.Close()
		if err != nil {
			t.Fatalf("per-item device failure must not fail AGReadMulti: %v", err)
		}
		if serr := <-done; serr != nil && !errors.Is(serr, io.ErrClosedPipe) {
			t.Fatal(serr)
		}
		if !bytes.Equal(items[0].Data, []byte{0xA1, 0xA2}) || items[0].Error != "" {
			t.Fatalf("first successful item wrong: %x %q", items[0].Data, items[0].Error)
		}
		if !bytes.Equal(items[1].Data, []byte{0xEE, 0xEE}) {
			t.Fatalf("failed item data was modified: %x", items[1].Data)
		}
		if want := ErrorText(CPUError(code7AddressOutOfRange)); items[1].Error != want {
			t.Fatalf("failed item error = %q, want %q", items[1].Error, want)
		}
		if !bytes.Equal(items[2].Data, []byte{0xC1, 0xC2}) || items[2].Error != "" {
			t.Fatalf("last successful item wrong: %x %q", items[2].Data, items[2].Error)
		}
	})

	t.Run("one bit and one timer", func(t *testing.T) {
		for _, bitValue := range []byte{0x01, 0x00} {
			items := []S7DataItem{
				{Area: s7areadb, WordLen: s7wlbit, DBNumber: 1, Start: 0, Bit: 3, Amount: 1, Data: []byte{0xEE}},
				{Area: s7areatm, WordLen: s7wltimer, Start: 7, Amount: 1, Data: []byte{0xEE, 0xEE}},
			}
			// Bit: transport size 3, length in bits (1), one output byte, then
			// the mandatory inter-item padding. Timer: octet transport, length
			// in bytes (2), even body so no padding.
			payload := append(okItem(tsResBit, 1, []byte{bitValue}, true),
				okItem(tsResOctet, 2, []byte{0x31, 0x32}, false)...)
			h, done := pipeHandler(t, wrapS7(2, payload))
			if err := NewClient(h).AGReadMulti(items, 2); err != nil {
				t.Fatalf("bit=%d read: %v", bitValue, err)
			}
			h.Close()
			if err := <-done; err != nil && !errors.Is(err, io.ErrClosedPipe) {
				t.Fatal(err)
			}
			if items[0].Data[0] != bitValue {
				t.Fatalf("bit value = %x, want %x (a real zero must stay zero)", items[0].Data[0], bitValue)
			}
			if !bytes.Equal(items[1].Data, []byte{0x31, 0x32}) {
				t.Fatalf("timer value = %x", items[1].Data)
			}
		}
	})

	t.Run("last item may omit tail padding", func(t *testing.T) {
		items := []S7DataItem{
			dbItem(s7wlbyte, 0, 1, []byte{0xEE}),
			dbItem(s7wlbyte, 1, 1, []byte{0xEE}),
		}
		// First (odd) item keeps its mandatory padding; the final odd item has
		// no tail padding byte.
		noTrailingPad := bytes.Join([][]byte{
			okItem(tsResByte, 8, []byte{0x11}, true),
			okItem(tsResByte, 8, []byte{0x22}, false),
		}, nil)
		h, done := pipeHandler(t, wrapS7(2, noTrailingPad))
		if err := NewClient(h).AGReadMulti(items, 2); err != nil {
			t.Fatalf("omitted tail padding rejected: %v", err)
		}
		h.Close()
		if err := <-done; err != nil && !errors.Is(err, io.ErrClosedPipe) {
			t.Fatal(err)
		}
		if !bytes.Equal(items[0].Data, []byte{0x11}) || !bytes.Equal(items[1].Data, []byte{0x22}) {
			t.Fatalf("values = %x / %x", items[0].Data, items[1].Data)
		}

		// Including the optional tail padding byte must also be accepted.
		withTrailingPad := bytes.Join([][]byte{
			okItem(tsResByte, 8, []byte{0x11}, true),
			okItem(tsResByte, 8, []byte{0x22}, true),
		}, nil)
		items[0].Data, items[1].Data = []byte{0xEE}, []byte{0xEE}
		h, done = pipeHandler(t, wrapS7(2, withTrailingPad))
		if err := NewClient(h).AGReadMulti(items, 2); err != nil {
			t.Fatalf("present tail padding rejected: %v", err)
		}
		h.Close()
		if err := <-done; err != nil && !errors.Is(err, io.ErrClosedPipe) {
			t.Fatal(err)
		}
		if !bytes.Equal(items[0].Data, []byte{0x11}) || !bytes.Equal(items[1].Data, []byte{0x22}) {
			t.Fatalf("values = %x / %x", items[0].Data, items[1].Data)
		}
	})

	t.Run("all supported word lengths", func(t *testing.T) {
		cases := []struct {
			name          string
			wordLen       int
			amount        int
			transportSize byte
			declaredLen   int
			body          []byte
		}{
			{"byte x2", s7wlbyte, 2, tsResByte, 16, []byte{0x01, 0x02}},
			{"word x1", s7wlword, 1, tsResByte, 16, []byte{0x12, 0x34}},
			{"dword x1", s7wldword, 1, tsResByte, 32, []byte{0x01, 0x02, 0x03, 0x04}},
			{"real x1", s7wlreal, 1, tsResReal, 4, []byte{0x41, 0x48, 0x00, 0x00}},
			{"timer x1", s7wltimer, 1, tsResOctet, 2, []byte{0x21, 0x43}},
			{"counter x1", s7wlcounter, 1, tsResOctet, 2, []byte{0x00, 0x07}},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				area := s7areadb
				if tc.wordLen == s7wltimer {
					area = s7areatm
				}
				if tc.wordLen == s7wlcounter {
					area = s7areact
				}
				items := []S7DataItem{{Area: area, WordLen: tc.wordLen, DBNumber: 1, Amount: tc.amount, Data: make([]byte, len(tc.body))}}
				payload := okItem(tc.transportSize, tc.declaredLen, tc.body, false)
				h, done := pipeHandler(t, wrapS7(1, payload))
				if err := NewClient(h).AGReadMulti(items, 1); err != nil {
					t.Fatal(err)
				}
				h.Close()
				if err := <-done; err != nil && !errors.Is(err, io.ErrClosedPipe) {
					t.Fatal(err)
				}
				if !bytes.Equal(items[0].Data, tc.body) {
					t.Fatalf("%s: got %x, want %x", tc.name, items[0].Data, tc.body)
				}
			})
		}
	})

	t.Run("second read clears stale item error and only updates prefix", func(t *testing.T) {
		items := []S7DataItem{
			dbItem(s7wlbyte, 0, 1, []byte{0xEE, 0xCC, 0xDD}), // longer caller buffer with tail sentinels
			dbItem(s7wlbyte, 1, 1, []byte{0xEE}),
		}
		first := bytes.Join([][]byte{
			okItem(tsResByte, 8, []byte{0x11}, true),
			failItem(code7AddressOutOfRange),
		}, nil)
		h, done := pipeHandler(t, wrapS7(2, first))
		if err := NewClient(h).AGReadMulti(items, 2); err != nil {
			t.Fatal(err)
		}
		h.Close()
		if err := <-done; err != nil && !errors.Is(err, io.ErrClosedPipe) {
			t.Fatal(err)
		}
		if items[0].Error != "" || items[1].Error == "" {
			t.Fatalf("first round errors = %q / %q", items[0].Error, items[1].Error)
		}
		if !bytes.Equal(items[0].Data, []byte{0x11, 0xCC, 0xDD}) {
			t.Fatalf("tail sentinel modified: %x", items[0].Data)
		}

		// Reuse the same items: a fully successful second round must clear the
		// stale error of the previously failed item.
		second := bytes.Join([][]byte{
			okItem(tsResByte, 8, []byte{0x33}, true),
			okItem(tsResByte, 8, []byte{0x44}, false),
		}, nil)
		h, done = pipeHandler(t, wrapS7(2, second))
		if err := NewClient(h).AGReadMulti(items, 2); err != nil {
			t.Fatal(err)
		}
		h.Close()
		if err := <-done; err != nil && !errors.Is(err, io.ErrClosedPipe) {
			t.Fatal(err)
		}
		if items[0].Error != "" || items[1].Error != "" {
			t.Fatalf("stale errors not cleared: %q / %q", items[0].Error, items[1].Error)
		}
		if !bytes.Equal(items[0].Data, []byte{0x33, 0xCC, 0xDD}) || !bytes.Equal(items[1].Data, []byte{0x44}) {
			t.Fatalf("second round values = %x / %x", items[0].Data, items[1].Data)
		}
	})

	t.Run("malformed frames change nothing", func(t *testing.T) {
		goodPayload := bytes.Join([][]byte{
			okItem(tsResByte, 8, []byte{0x11}, true),
			okItem(tsResByte, 16, []byte{0x22, 0x33}, false),
		}, nil)

		malformed := map[string][]byte{}
		// Item header of the second item cut after two bytes.
		malformed["item header truncated"] = wrapS7(2, append(okItem(tsResByte, 8, []byte{0x11}, true), 0xFF, tsResByte))
		// Successful item declares two bytes but only one is present.
		malformed["item body truncated"] = wrapS7(2, append(okItem(tsResByte, 8, []byte{0x11}, true), okItem(tsResByte, 16, []byte{0x22}, false)...))
		// Declared item count does not match the items in the frame.
		malformed["item count mismatch"] = wrapS7(3, goodPayload)
		// Declared S7 data length does not match the actual payload.
		badLen := wrapS7(2, goodPayload)
		binary.BigEndian.PutUint16(badLen[15:], binary.BigEndian.Uint16(badLen[15:])+1)
		malformed["declared data length mismatch"] = badLen
		// Unknown transport size on a successful item.
		malformed["unknown transport size"] = wrapS7(2, bytes.Join([][]byte{
			okItem(tsResByte, 8, []byte{0x11}, true),
			okItem(0x06, 16, []byte{0x22, 0x33}, false),
		}, nil))
		// Extra byte that cannot be attributed to any item or padding rule.
		extra := append(append([]byte{}, goodPayload...), 0x55)
		malformed["unassignable trailing byte"] = wrapS7(2, extra)
		// Odd first item missing its mandatory inter-item padding.
		malformed["missing inter-item padding"] = wrapS7(2, bytes.Join([][]byte{
			okItem(tsResByte, 8, []byte{0x11}, false),
			okItem(tsResByte, 16, []byte{0x22, 0x33}, false),
		}, nil))
		// Bit/byte length declared in bits that is not a whole number of bytes.
		malformed["bit length not byte aligned"] = wrapS7(1, okItem(tsResByte, 3, []byte{0x11}, false))

		for name, frame := range malformed {
			t.Run(name, func(t *testing.T) {
				items := []S7DataItem{
					dbItem(s7wlbyte, 0, 1, []byte{0xAA}),
					dbItem(s7wlword, 2, 1, []byte{0xAA, 0xAA}),
				}
				items[0].Error, items[1].Error = "stale", "stale"
				h, done := pipeHandler(t, frame)
				err := NewClient(h).AGReadMulti(items, 2)
				h.Close()
				if err == nil {
					t.Fatalf("%s: expected error, got nil", name)
				}
				if serr := <-done; serr != nil && !errors.Is(serr, io.ErrClosedPipe) && !errors.Is(serr, io.ErrUnexpectedEOF) && !errors.Is(serr, io.EOF) {
					t.Fatal(serr)
				}
				if !bytes.Equal(items[0].Data, []byte{0xAA}) || !bytes.Equal(items[1].Data, []byte{0xAA, 0xAA}) {
					t.Fatalf("%s: caller data modified: %x / %x", name, items[0].Data, items[1].Data)
				}
				if items[0].Error != "stale" || items[1].Error != "stale" {
					t.Fatalf("%s: caller error strings modified: %q / %q", name, items[0].Error, items[1].Error)
				}
			})
		}

		// A global CPU error is also a frame-level failure: nothing is committed.
		t.Run("global cpu error", func(t *testing.T) {
			frame := wrapS7(2, goodPayload)
			frame[17], frame[18] = 0xD2, 0x01
			items := []S7DataItem{
				dbItem(s7wlbyte, 0, 1, []byte{0xAA}),
				dbItem(s7wlword, 2, 1, []byte{0xAA, 0xAA}),
			}
			h, done := pipeHandler(t, frame)
			err := NewClient(h).AGReadMulti(items, 2)
			h.Close()
			if err == nil || err.Error() != ErrorText(CPUError(0xD201)) {
				t.Fatalf("global cpu error: got %v", err)
			}
			if serr := <-done; serr != nil && !errors.Is(serr, io.ErrClosedPipe) {
				t.Fatal(serr)
			}
			if !bytes.Equal(items[0].Data, []byte{0xAA}) || !bytes.Equal(items[1].Data, []byte{0xAA, 0xAA}) {
				t.Fatalf("caller data modified after global error: %x / %x", items[0].Data, items[1].Data)
			}
		})
	})

	t.Run("invalid parameters send nothing", func(t *testing.T) {
		guard := &noSendTransport{}
		goodData := make([]byte, 4)
		cases := map[string]struct {
			items []S7DataItem
			count int
		}{
			"zero items":            {[]S7DataItem{dbItem(s7wlbyte, 0, 1, goodData)}, 0},
			"too many items":        {make([]S7DataItem, 21), 21},
			"count exceeds slice":   {[]S7DataItem{dbItem(s7wlbyte, 0, 1, goodData)}, 2},
			"amount not positive":   {[]S7DataItem{dbItem(s7wlbyte, 0, 0, goodData)}, 1},
			"invalid word length":   {[]S7DataItem{dbItem(0x09, 0, 1, goodData)}, 1},
			"bit amount larger 1":   {[]S7DataItem{dbItem(s7wlbit, 0, 2, goodData)}, 1},
			"data buffer too small": {[]S7DataItem{dbItem(s7wlword, 0, 1, []byte{0})}, 1},
		}
		for name, tc := range cases {
			t.Run(name, func(t *testing.T) {
				guard.calls = 0
				err := NewClient2(&tcpPackager{}, guard).AGReadMulti(tc.items, tc.count)
				if err == nil {
					t.Fatalf("%s: expected error", name)
				}
				if guard.calls != 0 {
					t.Fatalf("%s: request was sent %d times", name, guard.calls)
				}
			})
		}
	})
}

type noSendTransport struct {
	calls int
}

func (n *noSendTransport) Send([]byte) ([]byte, error) {
	n.calls++
	return nil, errors.New("invalid call must never reach the transporter")
}
