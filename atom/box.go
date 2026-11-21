package atom

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
)

const (
	// BoxHeaderSize Size of box header.
	BoxHeaderSize = 8
	// LargeBoxHeaderSize Size of a box header using the 64-bit largesize field.
	LargeBoxHeaderSize = 16
	UuidSize           = 16
)

// BoxError reports a malformed box or a failure encountered while reading one.
// Type holds the four character box type (empty when the header itself could
// not be read) and Offset is the absolute offset of the box header in the
// file. Unwrap exposes the underlying error, so errors.Is works for failures
// coming from the ReaderAt.
type BoxError struct {
	Type   string
	Offset int64
	Err    error
}

func (e *BoxError) Error() string {
	return fmt.Sprintf("box %q at offset %d: %v", e.Type, e.Offset, e.Err)
}

// Unwrap returns the underlying error.
func (e *BoxError) Unwrap() error { return e.Err }

func boxError(name string, offset int64, format string, args ...interface{}) error {
	return &BoxError{Type: name, Offset: offset, Err: fmt.Errorf(format, args...)}
}

// Mp4Reader defines an mp4 reader structure.
type Mp4Reader struct {
	Reader io.ReaderAt
	Ftyp   *FtypBox
	Moov   *MoovBox
	Mdat   *MdatBox
	Uuids  []*UuidBox
	Size   int64

	IsFragmented bool
}

// Parse reads an MP4 reader for atom boxes.
func (m *Mp4Reader) Parse() error {
	if m.Size == 0 {
		if ofile, ok := m.Reader.(*os.File); ok {
			info, err := ofile.Stat()
			if err != nil {
				return err
			}
			m.Size = info.Size()
		}
	}

	boxes, err := readBoxes(m, int64(0), m.Size)
	if err != nil {
		return err
	}
	for _, box := range boxes {
		switch box.Name {
		case "ftyp":
			m.Ftyp = &FtypBox{Box: box}
			if err := m.Ftyp.parse(); err != nil {
				return err
			}
		case "mdat":
			m.Mdat = &MdatBox{Box: box}
		case "moov":
			m.Moov = &MoovBox{Box: box}
			if err := m.Moov.parse(); err != nil {
				return err
			}
			m.IsFragmented = m.Moov.IsFragmented
		case "uuid":
			uuidBox := &UuidBox{Box: box}
			if err := uuidBox.parse(); err != nil {
				return err
			}
			m.Uuids = append(m.Uuids, uuidBox)
		}
	}
	return nil
}

// ReadBoxAt reads a box from an offset.
//
// It is kept for backwards compatibility and reports the 32-bit size field
// only. Callers that need ISO BMFF extended (largesize) headers should rely on
// the parser, which reads the full header internally.
func (m *Mp4Reader) ReadBoxAt(offset int64) (boxSize uint32, boxType string) {
	buf := m.ReadBytesAt(BoxHeaderSize, offset)
	if len(buf) < BoxHeaderSize {
		return 0, ""
	}
	boxSize = binary.BigEndian.Uint32(buf[0:4])
	// check malformed data
	if boxSize > 1 && offset+int64(boxSize) > m.Size {
		return 0, ""
	}
	boxType = string(buf[4:8])
	return
}

// ReadBytesAt reads a box at n and offset.
func (m *Mp4Reader) ReadBytesAt(n int64, offset int64) (word []byte) {
	buf := make([]byte, n)
	if _, err := m.Reader.ReadAt(buf, offset); err != nil {
		return
	}
	return buf
}

// readAt reads exactly n bytes at offset. Unlike ReadBytesAt it reports errors
// from the underlying ReaderAt instead of silently returning a short buffer.
func (m *Mp4Reader) readAt(n int64, offset int64) ([]byte, error) {
	if n < 0 {
		return nil, fmt.Errorf("negative read length %d", n)
	}
	if n == 0 {
		return []byte{}, nil
	}
	buf := make([]byte, n)
	read, err := m.Reader.ReadAt(buf, offset)
	if err != nil && !(errors.Is(err, io.EOF) && int64(read) == n) {
		return nil, err
	}
	if int64(read) != n {
		return nil, io.ErrUnexpectedEOF
	}
	return buf, nil
}

// Box defines an Atom Box structure.
type Box struct {
	Name        string
	Size, Start int64
	// HeaderSize is the number of bytes occupied by the box header (including
	// the largesize field for extended boxes). It is 8 for a regular box and
	// 16 for a box using the 64-bit extended size.
	HeaderSize int64
	Reader     *Mp4Reader
}

// ReadBoxData reads the box data from an atom box.
func (b *Box) ReadBoxData() []byte {
	data, _ := b.readPayload()
	return data
}

// readPayload returns the bytes following the box header.
func (b *Box) readPayload() ([]byte, error) {
	headerSize := b.HeaderSize
	if headerSize <= 0 {
		headerSize = BoxHeaderSize
	}
	n := b.Size - headerSize
	if n < 0 {
		return nil, boxError(b.Name, b.Start, "box size %d is smaller than header size %d", b.Size, headerSize)
	}
	if n == 0 {
		return nil, nil
	}
	data, err := b.Reader.readAt(n, b.Start+headerSize)
	if err != nil {
		return nil, &BoxError{Type: b.Name, Offset: b.Start, Err: err}
	}
	return data, nil
}

// requirePayload checks that at least n payload bytes are available.
func (b *Box) requirePayload(data []byte, n int) error {
	if len(data) < n {
		return boxError(b.Name, b.Start, "truncated payload: need at least %d bytes, have %d", n, len(data))
	}
	return nil
}

func readBoxes(m *Mp4Reader, start int64, end int64) (l []*Box, err error) {
	if end < start {
		return nil, boxError("", start, "invalid container bounds [%d, %d)", start, end)
	}
	for offset := start; offset < end; {
		remaining := end - offset
		if remaining < BoxHeaderSize {
			return nil, boxError("", offset, "truncated box header: %d bytes remaining", remaining)
		}

		head, err := m.readAt(BoxHeaderSize, offset)
		if err != nil {
			return nil, &BoxError{Offset: offset, Err: err}
		}
		size32 := binary.BigEndian.Uint32(head[0:4])
		name := string(head[4:8])
		headerSize := int64(BoxHeaderSize)

		var total int64
		switch size32 {
		case 0:
			// A zero size means the box extends to the end of the file. Inside
			// a container this is only valid when the container itself reaches
			// the end of the file; the box then fills the remainder.
			if end != m.Size {
				return nil, boxError(name, offset, "zero-sized box is only allowed when its container ends at the end of the file")
			}
			total = remaining

		case 1:
			if remaining < LargeBoxHeaderSize {
				return nil, boxError(name, offset, "truncated extended box header: need %d bytes, have %d", LargeBoxHeaderSize, remaining)
			}
			ext, err := m.readAt(8, offset+BoxHeaderSize)
			if err != nil {
				return nil, &BoxError{Type: name, Offset: offset, Err: err}
			}
			large := binary.BigEndian.Uint64(ext)
			if large > uint64(math.MaxInt64) {
				return nil, boxError(name, offset, "box size %d is not representable as a signed 64-bit length", large)
			}
			total = int64(large)
			headerSize = LargeBoxHeaderSize
			if total < headerSize {
				return nil, boxError(name, offset, "extended box size %d is smaller than the %d byte header", total, headerSize)
			}

		default:
			total = int64(size32)
			if total < BoxHeaderSize {
				return nil, boxError(name, offset, "box size %d is smaller than the %d byte header", total, BoxHeaderSize)
			}
		}

		if total > remaining {
			return nil, boxError(name, offset, "box size %d exceeds container ending at offset %d", total, end)
		}

		b := &Box{
			Name:       name,
			Size:       total,
			HeaderSize: headerSize,
			Reader:     m,
			Start:      offset,
		}
		l = append(l, b)
		offset += total
	}
	return l, nil
}

// childBoxes reads the boxes in [start, b.Start+b.Size). A malformed header
// that has no type of its own is reported against the container, so the error
// still identifies a box and an absolute offset.
func (b *Box) childBoxes(start int64) ([]*Box, error) {
	boxes, err := readBoxes(b.Reader, start, b.Start+b.Size)
	if err != nil {
		var be *BoxError
		if errors.As(err, &be) && be.Type == "" {
			return nil, &BoxError{Type: b.Name, Offset: be.Offset, Err: be.Err}
		}
		return nil, err
	}
	return boxes, nil
}

// parseChildren parses the boxes directly contained in b.
func (b *Box) parseChildren() ([]*Box, error) {
	start := b.Start + b.HeaderSize
	if start > b.Start+b.Size {
		return nil, boxError(b.Name, b.Start, "container payload starts beyond its end")
	}
	return b.childBoxes(start)
}
