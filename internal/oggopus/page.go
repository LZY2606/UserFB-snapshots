// Package oggopus writes Ogg Opus streams following RFC 7845 and the
// Ogg framing specification (https://www.xiph.org/ogg/doc/framing.html).
//
// The writer emits a deterministic layout: the OpusHead packet occupies the
// BOS page alone, the OpusTags packet occupies the following page, and each
// audio packet occupies one page without continuation pages. All checksums
// are computed by the writer; callers never supply a CRC.
package oggopus

import (
	"encoding/binary"
	"errors"
	"io"
)

// MaxPacketSize is the largest packet length the Ogg lacing scheme used here
// can encode on a single page (255*255 - 1).
const MaxPacketSize = 65024

const (
	pageHeaderSize = 27
	pageMagic      = "OggS"
	pageVersion    = 0
	flagBOS        = 0x02
	flagEOS        = 0x04
)

// ErrPacketTooLarge is returned when a packet exceeds MaxPacketSize.
var ErrPacketTooLarge = errors.New("oggopus: packet size exceeds 65024 bytes")

// crcTable is the Ogg page check lookup table for polynomial 0x04c11db7
// (non-reflected, initial value zero, no final XOR).
var crcTable [256]uint32

func init() {
	for i := 0; i < 256; i++ {
		r := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if r&0x80000000 != 0 {
				r = (r << 1) ^ 0x04c11db7
			} else {
				r <<= 1
			}
		}
		crcTable[i] = r
	}
}

func crc32Ogg(crc uint32, data []byte) uint32 {
	for _, b := range data {
		crc = (crc << 8) ^ crcTable[byte(crc>>24)^b]
	}
	return crc
}

// Head is the content of the OpusHead packet (RFC 7845 section 5).
// Integer fields are stored in native byte order; the writer serializes them
// little-endian. Family 0 produces the 19-byte version-1 header.
type Head struct {
	Channels        uint8
	PreSkip         uint16
	InputSampleRate uint32
	OutputGain      int16
}

// Writer writes a single Ogg Opus stream.
type Writer struct {
	w        io.Writer
	serial   uint32
	seq      uint32
	started  bool
	tagged   bool
	finished bool
}

// NewWriter creates a Writer bound to serial. The stream is not emitted
// until WriteHead is called.
func NewWriter(w io.Writer, serial uint32) *Writer {
	return &Writer{w: w, serial: serial}
}

func (ow *Writer) writePage(flags byte, granule uint64, packet []byte) error {
	if len(packet) > MaxPacketSize {
		return ErrPacketTooLarge
	}

	header := make([]byte, pageHeaderSize)
	copy(header[0:4], pageMagic)
	header[4] = pageVersion
	header[5] = flags
	binary.LittleEndian.PutUint64(header[6:14], granule)
	binary.LittleEndian.PutUint32(header[14:18], ow.serial)
	binary.LittleEndian.PutUint32(header[18:22], ow.seq)
	// header[22:26] is the checksum field and stays zero while computing it.
	header[26] = lacingCount(len(packet))

	lacing := make([]byte, header[26])
	n := len(packet)
	for i := range lacing {
		if n >= 255 {
			lacing[i] = 255
			n -= 255
		} else {
			lacing[i] = byte(n)
			n = 0
		}
	}

	crc := crc32Ogg(0, header)
	crc = crc32Ogg(crc, lacing)
	crc = crc32Ogg(crc, packet)
	binary.LittleEndian.PutUint32(header[22:26], crc)

	if _, err := ow.w.Write(header); err != nil {
		return err
	}
	if _, err := ow.w.Write(lacing); err != nil {
		return err
	}
	if _, err := ow.w.Write(packet); err != nil {
		return err
	}
	ow.seq++
	return nil
}

// lacingCount returns the number of lacing values needed for size bytes.
// A size divisible by 255 still requires a terminating zero value.
func lacingCount(size int) byte {
	return byte(size/255 + 1)
}

// WriteHead writes the BOS page containing exactly one OpusHead packet.
// It must be called first and only once.
func (ow *Writer) WriteHead(head Head) error {
	if ow.started {
		return errors.New("oggopus: OpusHead already written")
	}
	packet := make([]byte, 19)
	copy(packet[0:8], "OpusHead")
	packet[8] = 1 // version
	packet[9] = head.Channels
	binary.LittleEndian.PutUint16(packet[10:12], head.PreSkip)
	binary.LittleEndian.PutUint32(packet[12:16], head.InputSampleRate)
	binary.LittleEndian.PutUint16(packet[16:18], uint16(head.OutputGain))
	packet[18] = 0 // channel mapping family 0
	if err := ow.writePage(flagBOS, 0, packet); err != nil {
		return err
	}
	ow.started = true
	return nil
}

// WriteTags writes the OpusTags page. vendor is the vendor string and the
// comment count is zero. It must be called once, directly after WriteHead.
func (ow *Writer) WriteTags(vendor string) error {
	if !ow.started {
		return errors.New("oggopus: WriteHead must be called first")
	}
	if ow.tagged {
		return errors.New("oggopus: OpusTags already written")
	}
	packet := make([]byte, 8+4+len(vendor)+4)
	copy(packet[0:8], "OpusTags")
	binary.LittleEndian.PutUint32(packet[8:12], uint32(len(vendor)))
	copy(packet[12:12+len(vendor)], vendor)
	binary.LittleEndian.PutUint32(packet[12+len(vendor):], 0) // user comment count
	if err := ow.writePage(0, 0, packet); err != nil {
		return err
	}
	ow.tagged = true
	return nil
}

// WriteAudio writes one audio packet on its own page. granule is the page's
// 48 kHz granule position. The final audio page must be flushed with
// Close instead so the EOS flag is attached.
func (ow *Writer) WriteAudio(granule uint64, packet []byte) error {
	if !ow.tagged {
		return errors.New("oggopus: WriteTags must be called before audio packets")
	}
	if ow.finished {
		return errors.New("oggopus: stream already finished")
	}
	if len(packet) == 0 {
		return errors.New("oggopus: audio packet must not be empty")
	}
	return ow.writePage(0, granule, packet)
}

// Close writes the final audio packet on an EOS page at granule and marks
// the stream finished. At least one audio packet is required by RFC 7845.
func (ow *Writer) Close(granule uint64, packet []byte) error {
	if !ow.tagged {
		return errors.New("oggopus: WriteTags must be called before audio packets")
	}
	if ow.finished {
		return errors.New("oggopus: stream already finished")
	}
	if len(packet) == 0 {
		return errors.New("oggopus: audio packet must not be empty")
	}
	if err := ow.writePage(flagEOS, granule, packet); err != nil {
		return err
	}
	ow.finished = true
	return nil
}
