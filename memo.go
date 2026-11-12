package shp

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// dbfVersionVisualFoxPro is the DBF version byte used by Visual FoxPro
// tables. Such tables may store long texts in a sidecar FPT memo file and
// carry an extended header (e.g. a backlink area) between the field
// descriptor terminator and the first record.
const dbfVersionVisualFoxPro = 0x30

// Memo kinds stored in the FPT block header.
const (
	// MemoKindBinary marks a binary (e.g. picture or object) memo block.
	MemoKindBinary uint32 = 0
	// MemoKindText marks a plain text memo block.
	MemoKindText uint32 = 1
)

// Errors that can be returned when reading memo values. They are wrapped
// with additional context, so use errors.Is to test for them.
var (
	// ErrMemoField indicates that the requested field is not a memo field
	// (wrong index, wrong type or wrong length).
	ErrMemoField = errors.New("shp: not a memo field")
	// ErrMemoPointer indicates that the 4 byte memo block pointer inside
	// the DBF record could not be read completely.
	ErrMemoPointer = errors.New("shp: truncated memo pointer")
	// ErrMemoBlockHeader indicates that the 8 byte memo block header in the
	// FPT file is missing or truncated.
	ErrMemoBlockHeader = errors.New("shp: truncated memo block header")
	// ErrMemoBody indicates that the memo body could not be read
	// completely.
	ErrMemoBody = errors.New("shp: truncated memo body")
	// ErrMemoLength indicates that the memo body length declared in the
	// block header exceeds the data available in the FPT file.
	ErrMemoLength = errors.New("shp: memo length out of range")
	// ErrMemoType indicates a memo block type other than binary (0) or
	// text (1).
	ErrMemoType = errors.New("shp: unsupported memo type")
	// ErrMemoUnavailable indicates that no FPT memo file is available for
	// the table.
	ErrMemoUnavailable = errors.New("shp: memo file unavailable")
)

// MemoValue is the content of a single DBF memo (M) field. Present reports
// whether the field references a memo block at all; a zero block number
// yields Present == false while a memo block with a zero length body yields
// Present == true with an empty Data. Kind is the memo type stored in the
// FPT block header (MemoKindBinary or MemoKindText) and Data holds the raw
// memo bytes exactly as stored, without any trimming or decoding. Data is
// never shared between calls and may be modified by the caller.
type MemoValue struct {
	Present bool
	Kind    uint32
	Data    []byte
}

// readFieldDescriptors reads 32 byte field descriptors from r, starting at
// the current position, until the standalone 0x0D terminator byte is
// encountered. Bytes between the terminator and the first record (such as
// the Visual FoxPro backlink area) are not consumed.
func readFieldDescriptors(r io.Reader) ([]Field, error) {
	var fields []Field
	var raw [32]byte
	for {
		if _, err := io.ReadFull(r, raw[:1]); err != nil {
			return nil, err
		}
		if raw[0] == 0x0d {
			return fields, nil
		}
		if _, err := io.ReadFull(r, raw[1:]); err != nil {
			return nil, err
		}
		var f Field
		if err := binary.Read(bytes.NewReader(raw[:]), binary.LittleEndian, &f); err != nil {
			return nil, err
		}
		fields = append(fields, f)
	}
}

// memoFieldOffset validates that field is a length-4 M field and returns
// its offset within a DBF record (including the deletion flag byte).
func memoFieldOffset(fields []Field, field int) (int64, error) {
	if field < 0 || field >= len(fields) {
		return 0, fmt.Errorf("field index %d out of range: %w", field, ErrMemoField)
	}
	if f := fields[field]; f.Fieldtype != 'M' || f.Size != 4 {
		return 0, fmt.Errorf("field %s is not a length-4 M field: %w", f, ErrMemoField)
	}
	off := int64(1)
	for i := 0; i < field; i++ {
		off += int64(fields[i].Size)
	}
	return off, nil
}

// fptHeaderSize is the minimum size of an FPT file: the header occupies at
// least the first 512 bytes.
const fptHeaderSize = 512

// memoFile provides read-only access to the memo blocks of an FPT file.
type memoFile struct {
	ra        io.ReaderAt
	size      int64
	blockSize int64
}

// newMemoFile parses the FPT header of ra which has the given total size.
func newMemoFile(ra io.ReaderAt, size int64) (*memoFile, error) {
	if size < fptHeaderSize {
		return nil, fmt.Errorf("FPT file is only %d bytes, need at least %d: %w",
			size, fptHeaderSize, ErrMemoBlockHeader)
	}
	var hdr [8]byte
	if _, err := ra.ReadAt(hdr[:], 0); err != nil {
		return nil, fmt.Errorf("cannot read FPT header: %w", ErrMemoBlockHeader)
	}
	blockSize := int64(binary.BigEndian.Uint16(hdr[6:8]))
	if blockSize <= 0 {
		return nil, fmt.Errorf("FPT file declares block size 0: %w", ErrMemoBlockHeader)
	}
	return &memoFile{ra: ra, size: size, blockSize: blockSize}, nil
}

// read returns the memo value stored in the given block. A block number of
// zero means "no memo" and yields a MemoValue with Present == false. Every
// call returns a freshly allocated Data slice.
func (m *memoFile) read(block uint32) (MemoValue, error) {
	if block == 0 {
		return MemoValue{}, nil
	}
	// block is at most 2^32-1 and blockSize at most 2^16-1, so the
	// product always fits into an int64.
	off := int64(block) * m.blockSize
	if off > m.size-8 {
		return MemoValue{}, fmt.Errorf("memo block %d starts beyond FPT file: %w",
			block, ErrMemoBlockHeader)
	}
	var hdr [8]byte
	if _, err := m.ra.ReadAt(hdr[:], off); err != nil {
		return MemoValue{}, fmt.Errorf("cannot read header of memo block %d: %w",
			block, ErrMemoBlockHeader)
	}
	kind := binary.BigEndian.Uint32(hdr[0:4])
	length := binary.BigEndian.Uint32(hdr[4:8])
	if kind != MemoKindBinary && kind != MemoKindText {
		return MemoValue{}, fmt.Errorf("memo block %d has unsupported type %d: %w",
			block, kind, ErrMemoType)
	}
	// Check the declared length against the remaining file size before
	// allocating anything.
	if int64(length) > m.size-off-8 {
		return MemoValue{}, fmt.Errorf("memo block %d declares %d bytes, only %d available: %w",
			block, length, m.size-off-8, ErrMemoLength)
	}
	data := make([]byte, length)
	if n, err := m.ra.ReadAt(data, off+8); err != nil || n != len(data) {
		return MemoValue{}, fmt.Errorf("cannot read body of memo block %d: %w",
			block, ErrMemoBody)
	}
	return MemoValue{Present: true, Kind: kind, Data: data}, nil
}
