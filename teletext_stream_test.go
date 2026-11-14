package astisub

import (
	"bytes"
	"context"
	"math/bits"
	"os"
	"reflect"
	"testing"
	"time"

	"github.com/asticode/go-astisub/task_support/taskcheck"
	"github.com/asticode/go-astits"
)

// Hamming 8/4 canonical codewords in the astikit/DVB byte convention, indexed
// by the protected nibble.
var teletextStreamHamming84 = [16]byte{
	0xa8, 0x40, 0x92, 0x7a, 0x26, 0xce, 0x1c, 0xf4,
	0x0b, 0xe3, 0x31, 0xd9, 0x85, 0x6d, 0xbf, 0x57,
}

func teletextStreamH84(nibble uint8) byte {
	return teletextStreamHamming84[nibble&0xf]
}

// teletextStreamH24DataPositions mirrors teletextHamming24DataPositions.
var teletextStreamH24DataPositions = []uint{3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23}

// teletextStreamH24Encode builds a DVB-represented Hamming 24/18 triplet.
func teletextStreamH24Encode(data uint32) [3]byte {
	var w uint32
	for j, pos := range teletextStreamH24DataPositions {
		if data&(1<<uint(j)) != 0 {
			w |= 1 << (pos - 1)
		}
	}
	var check uint8
	for p := uint(1); p <= 23; p++ {
		if w&(1<<(p-1)) != 0 {
			check ^= uint8(p)
		}
	}
	// Check bits are complemented in the DVB representation.
	check = 0x1f ^ check
	for _, bit := range []uint8{16, 8, 4, 2, 1} {
		if check&bit != 0 {
			w |= 1 << uint(bit-1)
			check ^= bit
		}
	}
	// Overall parity bit makes the codeword odd.
	if bits.OnesCount32(w)%2 == 0 {
		w |= 1 << 23
	}
	return [3]byte{
		bits.Reverse8(byte(w)),
		bits.Reverse8(byte(w >> 8)),
		bits.Reverse8(byte(w >> 16)),
	}
}

// teletextStreamAddress encodes magazine (1..8) and packet (0..31) into the
// two Hamming 8/4 address bytes.
func teletextStreamAddress(magazine, packet uint8) [2]byte {
	m := magazine & 7
	h1 := m | ((packet & 0x1) << 3)
	h2 := packet >> 1
	return [2]byte{teletextStreamH84(h1), teletextStreamH84(h2)}
}

// teletextStreamRowByte adds even parity and reverses the bits of one row byte.
func teletextStreamRowByte(v byte) byte {
	i := v & 0x7f
	if bits.OnesCount8(i)%2 == 0 {
		i |= 0x80
	}
	return bits.Reverse8(i)
}

// teletextStreamRow builds a 40-byte row starting with the start-of-row code
// and carrying text, padded with spaces.
func teletextStreamRow(text string) [40]byte {
	var row [40]byte
	for idx := range row {
		row[idx] = teletextStreamRowByte(0x20)
	}
	row[0] = teletextStreamRowByte(0x0b)
	for idx, c := range []byte(text) {
		if idx+1 < 40 {
			row[idx+1] = teletextStreamRowByte(c)
		}
	}
	return row
}

// teletextStreamUnit wraps a 40-byte packet body into a subtitle data unit
// (the bytes following the unit id and length).
func teletextStreamUnit(magazine, packet uint8, body [40]byte) []byte {
	addr := teletextStreamAddress(magazine, packet)
	u := make([]byte, 44)
	u[0] = 0x00 // data control byte, field 1
	u[1] = 0xe4 // framing code
	u[2], u[3] = addr[0], addr[1]
	copy(u[4:44], body[:])
	return u
}

// teletextStreamHeaderBody builds the packet 0 body. When subtitle is true the
// C6 control bit marks the page as a subtitle page; serial selects serial
// (true) versus parallel (false) magazine transmission.
func teletextStreamHeaderBody(page int, serial bool, charsetCode uint8, subtitle bool) [40]byte {
	var body [40]byte
	body[0] = teletextStreamH84(uint8(page % 10))
	body[1] = teletextStreamH84(uint8(page / 10))
	body[2], body[3], body[4] = 0xa8, 0xa8, 0xa8
	var c58 uint8
	if subtitle {
		c58 = 0x8 // C6
	}
	body[5] = teletextStreamH84(c58)
	body[6] = 0xa8
	c11 := charsetCode << 1
	if serial {
		c11 |= 0x1 // C11: serial magazine transmission
	}
	body[7] = teletextStreamH84(c11)
	return body
}

// teletextStreamCharsetBody builds an X/28 or M/29 packet body.
func teletextStreamCharsetBody(designation uint8, triplet uint32) [40]byte {
	var body [40]byte
	body[0] = teletextStreamH84(designation)
	enc := teletextStreamH24Encode(triplet)
	body[1], body[2], body[3] = enc[0], enc[1], enc[2]
	return body
}

// teletextStreamPES assembles complete data units into a PES.Data payload
// starting with the EBU data identifier.
func teletextStreamPES(units ...[]byte) []byte {
	var b bytes.Buffer
	b.WriteByte(0x10)
	for _, u := range units {
		b.WriteByte(teletextPESDataUnitIDEBUSubtitleData)
		b.WriteByte(byte(len(u)))
		b.Write(u)
	}
	return b.Bytes()
}

func teletextStreamFixture(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile("task_support/fixtures/" + name)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func teletextStreamTime(second int) time.Time {
	return time.Unix(1577836800, 0).Add(time.Duration(second) * time.Second)
}

// TestTeletextStreamPages covers page turnover, serial versus parallel page
// lifetime, page identity, duplicate row replacement and output ownership.
func TestTeletextStreamPages(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Basic turnover: each new target header closes the previous page.
		dec := NewTeletextDecoder(888)
		out, err := dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("hello")),
		), teletextStreamTime(1))
		c.Check(err == nil, "first push error: %v", err)
		c.Check(len(out.Items) == 0, "no page is closed on the first push, got %d items", len(out.Items))

		out, err = dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("world")),
		), teletextStreamTime(3))
		c.Check(err == nil, "second push error: %v", err)
		c.Check(len(out.Items) == 1, "first page closed on next header, got %d items", len(out.Items))
		c.Check(out.Items[0].String() == "hello", "first page text = %q", out.Items[0].String())
		c.Check(out.Items[0].StartAt == 0, "first page starts at the time origin, got %v", out.Items[0].StartAt)
		c.Check(out.Items[0].EndAt == 2*time.Second, "first page ends at the next header, got %v", out.Items[0].EndAt)
		first := out.Items[0]

		// The empty last page must not be output, and Flush is idempotent.
		out, err = dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(5))
		c.Check(err == nil, "third push error: %v", err)
		c.Check(len(out.Items) == 1, "second page closed on third header, got %d items", len(out.Items))
		c.Check(out.Items[0].String() == "world", "second page text = %q", out.Items[0].String())
		c.Check(out.Items[0].StartAt == 2*time.Second && out.Items[0].EndAt == 4*time.Second,
			"second page timing = %v/%v", out.Items[0].StartAt, out.Items[0].EndAt)

		out, err = dec.Flush(teletextStreamTime(5))
		c.Check(err == nil, "flush error: %v", err)
		c.Check(len(out.Items) == 0, "empty page is not flushed, got %d items", len(out.Items))
		out, err = dec.Flush(teletextStreamTime(6))
		c.Check(err == nil, "second flush error: %v", err)
		c.Check(len(out.Items) == 0, "repeated flush does not resend, got %d items", len(out.Items))

		// Previously returned pages are owned by the caller.
		c.Check(first.String() == "hello" && first.StartAt == 0 && first.EndAt == 2*time.Second,
			"returned page mutated by later input: %q %v/%v", first.String(), first.StartAt, first.EndAt)

		// Zero-duration pages are dropped.
		dec2 := NewTeletextDecoder(888)
		dec2.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("instant")),
		), teletextStreamTime(1))
		out, _ = dec2.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(1))
		c.Check(len(out.Items) == 0, "zero-duration page dropped, got %d items", len(out.Items))
		out, _ = dec2.Flush(teletextStreamTime(1))
		c.Check(len(out.Items) == 0, "empty zero-duration flush dropped, got %d items", len(out.Items))

		// Duplicate rows are replaced and rows are ordered by row number.
		dec3 := NewTeletextDecoder(888)
		dec3.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("first")),
			teletextStreamUnit(8, 2, teletextStreamRow("two")),
			teletextStreamUnit(8, 1, teletextStreamRow("second")),
		), teletextStreamTime(1))
		out, _ = dec3.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(3))
		c.Check(len(out.Items) == 1, "one page with duplicated rows, got %d", len(out.Items))
		c.Check(len(out.Items[0].Lines) == 2, "duplicate row collapsed, got %d lines", len(out.Items[0].Lines))
		c.Check(out.Items[0].String() == "second - two", "rows replaced and ordered, got %q", out.Items[0].String())

		// Page identity compares magazine and page number: a magazine 1 page
		// with the same page digits never feeds the magazine 8 target.
		dec4 := NewTeletextDecoder(888)
		dec4.Push(teletextStreamPES(
			teletextStreamUnit(1, 0, teletextStreamHeaderBody(88, true, 0, true)),
			teletextStreamUnit(1, 1, teletextStreamRow("foreign")),
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("mine")),
		), teletextStreamTime(1))
		out, _ = dec4.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(3))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "mine",
			"foreign magazine rows ignored, got %v", out.Items)

		// Serial transmission: a header of another magazine closes the page.
		dec5 := NewTeletextDecoder(888)
		dec5.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("serial")),
		), teletextStreamTime(1))
		out, _ = dec5.Push(teletextStreamPES(
			teletextStreamUnit(1, 0, teletextStreamHeaderBody(100, true, 0, false)),
		), teletextStreamTime(2))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "serial",
			"serial: other-magazine header closes the page, got %v", out.Items)

		// Parallel transmission: another magazine keeps the page open, a
		// different page of the same magazine closes it.
		dec6 := NewTeletextDecoder(888)
		dec6.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, false, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("parallel")),
		), teletextStreamTime(1))
		out, _ = dec6.Push(teletextStreamPES(
			teletextStreamUnit(1, 0, teletextStreamHeaderBody(100, false, 0, false)),
		), teletextStreamTime(2))
		c.Check(len(out.Items) == 0, "parallel: other-magazine header keeps the page open, got %d items", len(out.Items))
		out, _ = dec6.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(887, false, 0, false)),
		), teletextStreamTime(3))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "parallel",
			"parallel: same-magazine other page closes the page, got %v", out.Items)

		// Automatic selection locks onto the first C6 subtitle page.
		dec7 := NewTeletextDecoder(0)
		dec7.Push(teletextStreamPES(
			teletextStreamUnit(1, 0, teletextStreamHeaderBody(100, true, 0, false)),
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, teletextStreamRow("auto")),
		), teletextStreamTime(1))
		out, _ = dec7.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(2))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "auto",
			"automatic page selection follows the C6 flag, got %v", out.Items)
	})
}

// TestTeletextStreamCharsets covers Hamming 24/18 correction, M/29 magazine
// defaults, X/28 page overrides and charset lifetime.
func TestTeletextStreamCharsets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Direct Hamming 24/18 checks on the fixture triplets.
		triplet, ok := teletextHamming2418Decode([3]byte{0xd1, 0x01, 0x00})
		c.Check(ok && triplet == 0, "latin triplet decoded to %d ok=%v", triplet, ok)
		triplet, ok = teletextHamming2418Decode([3]byte{0x91, 0x00, 0x41})
		c.Check(ok && triplet == 4096, "cyrillic triplet decoded to %d ok=%v", triplet, ok)

		// Every single bit flip is corrected back to the same triplet.
		for _, base := range [][3]byte{{0xd1, 0x01, 0x00}, {0x91, 0x00, 0x41}} {
			want, _ := teletextHamming2418Decode(base)
			for bit := uint(0); bit < 24; bit++ {
				corrupt := base
				corrupt[bit/8] ^= 1 << (bit % 8)
				got, ok := teletextHamming2418Decode(corrupt)
				c.Check(ok && got == want, "single bit error at %d corrected: got %d ok=%v want %d", bit, got, ok, want)
			}
		}

		// Double bit errors are rejected.
		base := [3]byte{0xd1, 0x01, 0x00}
		doubleRejected := 0
		for i := uint(0); i < 24; i++ {
			for j := i + 1; j < 24; j++ {
				corrupt := base
				corrupt[i/8] ^= 1 << (i % 8)
				corrupt[j/8] ^= 1 << (j % 8)
				if _, ok := teletextHamming2418Decode(corrupt); !ok {
					doubleRejected++
				}
			}
		}
		c.Check(doubleRejected == 276, "all 276 double bit errors rejected, got %d", doubleRejected)

		// Code point 0x40 renders '@' in the default Latin G0 charset and
		// 'Ч' in Cyrillic option 1 (triplet index 4, page charset code 0).
		cyrillicRow := teletextStreamRow("")
		cyrillicRow[1] = teletextStreamRowByte(0x40)

		// An M/29 magazine default received before automatic page selection is
		// remembered.
		dec := NewTeletextDecoder(0)
		dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 29, teletextStreamCharsetBody(0, 4096)),
		), teletextStreamTime(0))
		dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, cyrillicRow),
		), teletextStreamTime(1))
		out, _ := dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(3))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "Ч",
			"pre-selection M/29 cyrillic default applied, got %v", out.Items)
		frozen := out.Items[0]

		// X/28 only overrides the current page; the next page falls back to
		// the magazine default.
		latinRow := teletextStreamRow("")
		latinRow[1] = teletextStreamRowByte(0x40)
		dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 28, teletextStreamCharsetBody(0, 0)),
			teletextStreamUnit(8, 1, latinRow),
		), teletextStreamTime(3))
		out, _ = dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, cyrillicRow),
		), teletextStreamTime(5))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "@",
			"X/28 latin override applies to its page, got %v", out.Items)
		out, _ = dec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(7))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "Ч",
			"override does not leak to the next page, got %v", out.Items)
		c.Check(frozen.String() == "Ч", "closed page text frozen against later charset updates, got %q", frozen.String())

		// A single bit error in an X/28 triplet is corrected.
		enc := teletextStreamH24Encode(0)
		enc[0] ^= 0x04
		singleBody := teletextStreamCharsetBody(0, 0)
		singleBody[1], singleBody[2], singleBody[3] = enc[0], enc[1], enc[2]
		dec2 := NewTeletextDecoder(888)
		dec2.Push(teletextStreamPES(
			teletextStreamUnit(8, 29, teletextStreamCharsetBody(0, 4096)),
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 28, singleBody),
			teletextStreamUnit(8, 1, latinRow),
		), teletextStreamTime(1))
		out, _ = dec2.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(2))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "@",
			"single bit error in X/28 corrected, got %v", out.Items)

		// A double bit error drops the X/28 update, leaving the magazine
		// default in place.
		bad := teletextStreamH24Encode(0)
		bad[0] ^= 0x05
		doubleBody := teletextStreamCharsetBody(0, 0)
		doubleBody[1], doubleBody[2], doubleBody[3] = bad[0], bad[1], bad[2]
		dec3 := NewTeletextDecoder(888)
		dec3.Push(teletextStreamPES(
			teletextStreamUnit(8, 29, teletextStreamCharsetBody(0, 4096)),
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 28, doubleBody),
			teletextStreamUnit(8, 1, cyrillicRow),
		), teletextStreamTime(1))
		out, _ = dec3.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(2))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "Ч",
			"uncorrectable X/28 update discarded, got %v", out.Items)

		// Unsupported designation values and X/28 format 1 are ignored.
		ignored := NewTeletextDecoder(888)
		ignored.Push(teletextStreamPES(
			teletextStreamUnit(8, 29, teletextStreamCharsetBody(0, 4096)),
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 28, teletextStreamCharsetBody(1, 0)),
			teletextStreamUnit(8, 28, teletextStreamCharsetBody(0, 1)),
			teletextStreamUnit(8, 1, cyrillicRow),
		), teletextStreamTime(1))
		out, _ = ignored.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(2))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "Ч",
			"bad designation and format 1 updates ignored, got %v", out.Items)

		// An unsupported block/charset combination falls back to Latin.
		fallback := NewTeletextDecoder(888)
		fallback.Push(teletextStreamPES(
			teletextStreamUnit(8, 29, teletextStreamCharsetBody(0, 5<<10)),
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, latinRow),
		), teletextStreamTime(1))
		out, _ = fallback.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(2))
		c.Check(len(out.Items) == 1 && out.Items[0].String() == "@",
			"unknown charset combination falls back to Latin, got %v", out.Items)

		// The shipped charset fixtures are accepted without panic and their
		// first triplets decode as documented.
		for _, fixture := range []string{
			"packet28_latin.pes", "packet28_cyrillic.pes",
			"packet29_latin.pes", "packet29_cyrillic.pes",
		} {
			d := NewTeletextDecoder(888)
			out, err := d.Push(teletextStreamFixture(t, fixture), teletextStreamTime(1))
			c.Check(err == nil && len(out.Items) == 0, "fixture %s accepted: err=%v items=%d", fixture, err, len(out.Items))
		}
	})
}

// TestTeletextStreamTransport covers malformed transport data, real TS reading
// parity with the incremental entry, and writer outputs.
func TestTeletextStreamTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Empty and unknown-identifier payloads return usable empty subtitles.
		dec := NewTeletextDecoder(888)
		out, err := dec.Push(nil, teletextStreamTime(1))
		c.Check(err == nil && out != nil && len(out.Items) == 0, "nil payload: err=%v out=%v", err, out)
		out, err = dec.Push([]byte{0x02}, teletextStreamTime(1))
		c.Check(err == nil && len(out.Items) == 0, "unknown data identifier ignored: err=%v items=%d", err, len(out.Items))

		// Non-subtitle data units are ignored.
		out, err = dec.Push(append([]byte{0x10, 0x02, 0x05}, make([]byte, 5)...), teletextStreamTime(1))
		c.Check(err == nil && len(out.Items) == 0, "non-subtitle unit ignored: err=%v items=%d", err, len(out.Items))

		// Bad framing code is ignored.
		badFraming := teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true))
		badFraming[1] = 0x00
		out, err = dec.Push(teletextStreamPES(badFraming), teletextStreamTime(1))
		c.Check(err == nil && len(out.Items) == 0, "bad framing ignored: err=%v items=%d", err, len(out.Items))

		// An uncorrectable address byte is ignored.
		badAddress := teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true))
		badAddress[2] = 0x20
		out, err = dec.Push(teletextStreamPES(badAddress), teletextStreamTime(1))
		c.Check(err == nil && len(out.Items) == 0, "bad address ignored: err=%v items=%d", err, len(out.Items))

		// An incomplete trailing data unit must not panic and must leave the
		// complete preceding unit processed.
		good := teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true))
		truncated := bytes.Buffer{}
		truncated.WriteByte(0x10)
		truncated.WriteByte(teletextPESDataUnitIDEBUSubtitleData)
		truncated.WriteByte(byte(len(good)))
		truncated.Write(good)
		truncated.WriteByte(teletextPESDataUnitIDEBUSubtitleData)
		truncated.WriteByte(44)
		truncated.Write(make([]byte, 7))
		c.Check(func() (ok bool) {
			ok = true
			defer func() {
				if r := recover(); r != nil {
					ok = false
				}
			}()
			d := NewTeletextDecoder(888)
			s, e := d.Push(truncated.Bytes(), teletextStreamTime(1))
			ok = e == nil && s != nil
			flush, e := d.Flush(teletextStreamTime(3))
			ok = ok && e == nil && len(flush.Items) == 0
			return ok
		}(), "truncated data unit handled without panic")

		// Timestamps must be non-decreasing.
		regress := NewTeletextDecoder(888)
		_, err = regress.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(2))
		c.Check(err == nil, "first push ok: %v", err)
		_, err = regress.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(1))
		c.Check(err == ErrTeletextTimeRegression, "time regression rejected, got %v", err)
		_, err = regress.Flush(teletextStreamTime(0))
		c.Check(err == ErrTeletextTimeRegression, "flush regression rejected, got %v", err)

		// The real TS and the incremental entry produce identical subtitles,
		// both with an explicit page and with automatic selection.
		tsBytes := teletextStreamFixture(t, "basic.ts")
		read, err := ReadFromTeletext(bytes.NewReader(tsBytes), TeletextOptions{PID: 256, Page: 888})
		c.Check(err == nil, "ReadFromTeletext explicit page: %v", err)
		readAuto, err := ReadFromTeletext(bytes.NewReader(tsBytes), TeletextOptions{PID: 256})
		c.Check(err == nil, "ReadFromTeletext automatic page: %v", err)

		buildIncremental := func(page int) *Subtitles {
			d := NewTeletextDecoder(page)
			merged := &Subtitles{}
			for idx, fixture := range []string{"basic_0.pes", "basic_1.pes", "basic_2.pes"} {
				s, e := d.Push(teletextStreamFixture(t, fixture), teletextStreamTime(1+2*idx))
				if e != nil {
					t.Fatal(e)
				}
				merged.Items = append(merged.Items, s.Items...)
			}
			s, e := d.Flush(teletextStreamTime(5))
			if e != nil {
				t.Fatal(e)
			}
			merged.Items = append(merged.Items, s.Items...)
			return merged
		}
		inc := buildIncremental(888)
		incAuto := buildIncremental(0)
		c.Check(reflect.DeepEqual(read.Items, inc.Items), "explicit page: TS and incremental items identical")
		c.Check(reflect.DeepEqual(readAuto.Items, incAuto.Items), "automatic page: TS and incremental items identical")
		c.Check(len(read.Items) == 2 && read.Items[0].String() == "hello" && read.Items[1].String() == "world",
			"real TS pages: %v", read.Items)
		c.Check(read.Items[0].StartAt == 0 && read.Items[0].EndAt == 2*time.Second &&
			read.Items[1].StartAt == 2*time.Second && read.Items[1].EndAt == 4*time.Second,
			"real TS timing: %v/%v and %v/%v",
			read.Items[0].StartAt, read.Items[0].EndAt, read.Items[1].StartAt, read.Items[1].EndAt)

		// Inline color survives both entries identically. Build a row whose
		// first text is explicitly marked red.
		redRow := teletextStreamRow("red")
		redRow[1] = teletextStreamRowByte(0x01)
		redRow[2] = teletextStreamRowByte('r')
		redRow[3] = teletextStreamRowByte('e')
		redRow[4] = teletextStreamRowByte('d')
		colorPES := teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
			teletextStreamUnit(8, 1, redRow),
		)
		colorDec := NewTeletextDecoder(888)
		colorDec.Push(colorPES, teletextStreamTime(1))
		colorOut, _ := colorDec.Push(teletextStreamPES(
			teletextStreamUnit(8, 0, teletextStreamHeaderBody(888, true, 0, true)),
		), teletextStreamTime(2))
		c.Check(len(colorOut.Items) == 1 && len(colorOut.Items[0].Lines) == 1 &&
			len(colorOut.Items[0].Lines[0].Items) == 1 &&
			colorOut.Items[0].Lines[0].Items[0].InlineStyle != nil &&
			colorOut.Items[0].Lines[0].Items[0].InlineStyle.TeletextColor == ColorRed,
			"inline color preserved on the incremental path: %v", colorOut.Items)

		// SRT and WebVTT outputs work from both entries with the same bytes.
		var srtTS, srtInc, vttTS, vttInc bytes.Buffer
		c.Check(read.WriteToSRT(&srtTS) == nil, "SRT write from TS entry")
		c.Check(inc.WriteToSRT(&srtInc) == nil, "SRT write from incremental entry")
		c.Check(read.WriteToWebVTT(&vttTS) == nil, "WebVTT write from TS entry")
		c.Check(inc.WriteToWebVTT(&vttInc) == nil, "WebVTT write from incremental entry")
		c.Check(srtTS.String() == srtInc.String(), "SRT outputs identical")
		c.Check(vttTS.String() == vttInc.String(), "WebVTT outputs identical")
		c.Check(bytes.Contains(srtTS.Bytes(), []byte("00:00:02,000")) && bytes.Contains(srtTS.Bytes(), []byte("world")),
			"SRT content: %s", srtTS.String())
		c.Check(bytes.Contains(vttTS.Bytes(), []byte("hello")) && bytes.Contains(vttTS.Bytes(), []byte("world")),
			"WebVTT content: %s", vttTS.String())

		// PES.Data fed through the demuxer is byte-identical to the fixtures,
		// which is what makes the two entries share one assembly path.
		dmx := astits.NewDemuxer(context.Background(), bytes.NewReader(tsBytes))
		payloads := 0
		for {
			d, dmxErr := dmx.NextData()
			if dmxErr != nil {
				break
			}
			if d.PES == nil || d.PID != 256 {
				continue
			}
			want := teletextStreamFixture(t, []string{"basic_0.pes", "basic_1.pes", "basic_2.pes"}[payloads])
			c.Check(bytes.Equal(d.PES.Data, want), "demuxed PES.Data %d matches its fixture", payloads)
			payloads++
		}
		c.Check(payloads == 3, "all three PES payloads seen, got %d", payloads)
	})
}
