// Package oggopus writes Ogg Opus streams as defined by RFC 7845.
//
// The writer uses the deterministic layout required by the MP4 exporter:
// the OpusHead packet occupies the BOS page, a single OpusTags page follows,
// and every audio packet occupies exactly one page without continuation.
// Only the final audio page carries the EOS flag.
package oggopus

import (
	"encoding/binary"
	"errors"
	"hash"
	"io"
)

// MaxPacketSize is the largest Opus packet carried on a single Ogg page in
// this exporter: 254 lacing values of 255 plus one final value of 254.
// (A 65025-byte packet would consume all 255 segment slots without a
// terminator, which this deterministic layout does not emit.)
const MaxPacketSize = 255*255 - 1

const (
	pageHeaderSize = 27
	opusHeadSize   = 19
)

// Head describes the OpusHead packet (RFC 7845 section 5.1).
// Only version 1 / mapping family 0 is produced.
type Head struct {
	Channels        uint8
	PreSkip         uint16
	InputSampleRate uint32
	OutputGain      int16
}

var (
	// ErrPacketSize means a packet was empty or larger than MaxPacketSize.
	ErrPacketSize = errors.New("oggopus: packet size out of range")
	// ErrHead means the OpusHead parameters were invalid.
	ErrHead = errors.New("oggopus: invalid OpusHead")
)

// CRC-32 according to the Ogg framing specification: polynomial 0x04c11db7,
// non-reflected, initial value zero, no final XOR.
var crcTable [256]uint32

func init() {
	for n := 0; n < 256; n++ {
		c := uint32(n) << 24
		for k := 0; k < 8; k++ {
			if c&0x80000000 != 0 {
				c = c<<1 ^ 0x04c11db7
			} else {
				c <<= 1
			}
		}
		crcTable[n] = c
	}
}

type crc32Ogg struct {
	sum uint32
}

func (c *crc32Ogg) Write(p []byte) (int, error) {
	for _, b := range p {
		c.sum = (c.sum << 8) ^ crcTable[byte(c.sum>>24)^b]
	}
	return len(p), nil
}

func (c *crc32Ogg) Sum(b []byte) []byte {
	v := c.Sum32()
	return append(b, byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
}

func (c *crc32Ogg) Reset()         { c.sum = 0 }
func (c *crc32Ogg) Size() int      { return 4 }
func (c *crc32Ogg) BlockSize() int { return 1 }
func (c *crc32Ogg) Sum32() uint32  { return c.sum }

var _ hash.Hash32 = (*crc32Ogg)(nil)

// Writer emits an Ogg Opus stream.
type Writer struct {
	w       io.Writer
	serial  uint32
	pageSeq uint32
	crc     hash.Hash32
}

// NewWriter writes the BOS page containing the OpusHead packet.
// The serial number is stored verbatim in every page header.
func NewWriter(w io.Writer, serial uint32, head *Head) (*Writer, error) {
	if head == nil || (head.Channels != 1 && head.Channels != 2) {
		return nil, ErrHead
	}
	packet := make([]byte, opusHeadSize)
	copy(packet[0:8], "OpusHead")
	packet[8] = 1 // OpusHead version
	packet[9] = head.Channels
	binary.LittleEndian.PutUint16(packet[10:12], head.PreSkip)
	binary.LittleEndian.PutUint32(packet[12:16], head.InputSampleRate)
	binary.LittleEndian.PutUint16(packet[16:18], uint16(head.OutputGain))
	packet[18] = 0 // channel mapping family 0

	ow := &Writer{
		w:      w,
		serial: serial,
		crc:    &crc32Ogg{},
	}
	if err := ow.writePage(0, 0x02, packet); err != nil {
		return nil, err
	}
	return ow, nil
}

// WriteTags writes one OpusTags page. The comment count is always zero.
func (ow *Writer) WriteTags(vendor string) error {
	if len(vendor) > 0xffffffff {
		return errors.New("oggopus: vendor string too long")
	}
	packet := make([]byte, 8+4+len(vendor)+4)
	copy(packet[0:8], "OpusTags")
	binary.LittleEndian.PutUint32(packet[8:12], uint32(len(vendor)))
	copy(packet[12:], vendor)
	binary.LittleEndian.PutUint32(packet[12+len(vendor):], 0) // zero comments
	return ow.writePage(0, 0x00, packet)
}

// WriteAudioPacket writes one audio packet on its own page.
// granule is the 48 kHz granule position; eos marks the final audio page.
func (ow *Writer) WriteAudioPacket(packet []byte, granule int64, eos bool) error {
	headerType := byte(0x00)
	if eos {
		headerType = 0x04
	}
	return ow.writePage(granule, headerType, packet)
}

// PageSequence returns the number of pages written so far.
func (ow *Writer) PageSequence() uint32 {
	return ow.pageSeq
}

func lacingValues(packetLen int) []byte {
	full, rem := packetLen/255, packetLen%255
	segments := make([]byte, 0, full+1)
	for i := 0; i < full; i++ {
		segments = append(segments, 255)
	}
	if rem != 0 {
		segments = append(segments, byte(rem))
	} else {
		// A length divisible by 255 still needs a zero terminator.
		segments = append(segments, 0)
	}
	return segments
}

func (ow *Writer) writePage(granule int64, headerType byte, packet []byte) error {
	if len(packet) < 1 || len(packet) > MaxPacketSize {
		return ErrPacketSize
	}
	segments := lacingValues(len(packet))
	if len(segments) > 255 {
		return ErrPacketSize
	}

	var header [pageHeaderSize]byte
	copy(header[0:4], "OggS")
	header[4] = 0 // stream structure version
	header[5] = headerType
	binary.LittleEndian.PutUint64(header[6:14], uint64(granule))
	binary.LittleEndian.PutUint32(header[14:18], ow.serial)
	binary.LittleEndian.PutUint32(header[18:22], ow.pageSeq)
	// header[22:26] is the CRC field and stays zero while calculating.
	header[26] = byte(len(segments))

	ow.crc.Reset()
	if _, err := ow.crc.Write(header[:]); err != nil {
		return err
	}
	if _, err := ow.crc.Write(segments); err != nil {
		return err
	}
	if _, err := ow.crc.Write(packet); err != nil {
		return err
	}
	binary.LittleEndian.PutUint32(header[22:26], ow.crc.Sum32())

	if _, err := ow.w.Write(header[:]); err != nil {
		return err
	}
	if _, err := ow.w.Write(segments); err != nil {
		return err
	}
	if _, err := ow.w.Write(packet); err != nil {
		return err
	}
	ow.pageSeq++
	return nil
}
