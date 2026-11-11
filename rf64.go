package wav

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"time"

	"github.com/go-audio/riff"
)

// RF64 container support follows EBU Tech 3306.
// The 32-bit 0xFFFFFFFF sentinel marks lengths that are instead carried in
// the 64-bit ds64 chunk.
const rf64LengthSentinel uint32 = 0xffffffff

var (
	cidRF64 = [4]byte{'R', 'F', '6', '4'}
	cidDS64 = [4]byte{'d', 's', '6', '4'}
	cidJUNK = [4]byte{'J', 'U', 'N', 'K'}

	ds64FixedSize = 28
	ds64EntrySize = 12
)

// ds64Entry is one table entry of an RF64 ds64 chunk: a 4 byte chunk ID
// followed by the 64-bit replacement length.
type ds64Entry struct {
	id  [4]byte
	sz  uint64
	pos int64 // file offset of the entry's length field
}

// ds64Table holds the parsed content of the RF64 ds64 chunk.
// Entries are kept ordered so repeated, identically named chunks each
// consume their own table entry instead of collapsing into one map value.
type ds64Table struct {
	riffSize    uint64
	dataSize    uint64
	sampleCount uint64
	tableLength uint32
	entries     []ds64Entry
	consumed    int
	// original is the table as read from the wire, used to restart a pass
	// (header scanning and rewinding both begin from the full table).
	original []ds64Entry
}

// take returns the first not-yet-consumed table entry matching id.
// Ordinary (non-sentinel) lengths do not consume entries.
func (t *ds64Table) take(id [4]byte) (ds64Entry, error) {
	for i := t.consumed; i < len(t.entries); i++ {
		if t.entries[i].id == id {
			e := t.entries[i]
			// entries before i stay available, entries at i and after shift
			t.entries = append(t.entries[:i], t.entries[i+1:]...)
			return e, nil
		}
	}
	return ds64Entry{}, fmt.Errorf("RF64: chunk %q declares length 0xffffffff but no unused ds64 table entry matches it", string(id[:]))
}

// reset restarts table substitution from the beginning.
func (t *ds64Table) reset() {
	t.entries = append([]ds64Entry(nil), t.original...)
	t.consumed = 0
}

// rf64Container wraps the underlying ReadSeeker while an RF64 file is being
// decoded. It tracks the logical 64-bit container size and resolves chunk
// lengths through the ds64 data.
type rf64Container struct {
	r        io.ReadSeeker
	ds64     *ds64Table
	riffSize uint64 // logical RIFF size (file length - 8)
	// cursor is the current logical file offset, mirrors r's position.
	cursor int64
	// physicalSize is the underlying reader's total length as reported by
	// a seek to end; zero when it cannot be determined.
	physicalSize int64
	// firstChunkPos is the file offset of the first chunk after ds64.
	firstChunkPos int64
	// dataEnd is the file offset right after the data payload and its
	// padding; zero until the data chunk was seen.
	dataEnd int64
	// openEnd is the offset where the chunk most recently handed to the
	// caller ends (payload plus padding); zero when nothing is open.
	openEnd int64
}

func newRF64Container(r io.ReadSeeker) *rf64Container {
	return &rf64Container{r: r}
}

// readFull reads exactly len(buf) bytes or returns an error mentioning the
// logical offset where the short read happened. A clean end of file keeps
// being io.EOF so callers can distinguish it from truncated structures.
func (c *rf64Container) readFull(buf []byte, what string) error {
	if _, err := io.ReadFull(c.r, buf); err != nil {
		if errors.Is(err, io.EOF) {
			return io.EOF
		}
		if errors.Is(err, io.ErrUnexpectedEOF) {
			return fmt.Errorf("RF64: truncated %s at file offset %d", what, c.cursor)
		}
		return fmt.Errorf("RF64: failed reading %s at file offset %d: %w", what, c.cursor, err)
	}
	c.cursor += int64(len(buf))
	return nil
}

func (c *rf64Container) seek(offset int64, whence int) (int64, error) {
	pos, err := c.r.Seek(offset, whence)
	if err != nil {
		return 0, fmt.Errorf("RF64: seek to offset %d (whence %d) failed: %w", offset, whence, err)
	}
	c.cursor = pos
	return pos, nil
}

// measurePhysicalSize records the underlying reader length by seeking to the
// end and restoring the previous position. Readers that cannot report their
// end leave the size unknown and are validated against the logical size only.
func (c *rf64Container) measurePhysicalSize() error {
	saved := c.cursor
	end, err := c.r.Seek(0, io.SeekEnd)
	if err != nil {
		c.cursor = saved
		return nil
	}
	c.physicalSize = end
	if _, err := c.seek(saved, io.SeekStart); err != nil {
		return err
	}
	return nil
}

// parseDS64 reads the ds64 chunk which must be the first sub chunk of an
// RF64 container. The reader must be positioned right after the "RF64"/size
// and "WAVE" header bytes.
func (c *rf64Container) parseDS64() error {
	var hdr [8]byte
	headerPos := c.cursor
	if err := c.readFull(hdr[:], "ds64 chunk header"); err != nil {
		if errors.Is(err, io.EOF) {
			return fmt.Errorf("RF64: truncated or missing ds64 chunk at file offset %d", headerPos)
		}
		return err
	}
	var id [4]byte
	copy(id[:], hdr[:4])
	if id != cidDS64 {
		return fmt.Errorf("RF64: first sub chunk must be ds64 at file offset %d, got %q", headerPos, string(id[:]))
	}
	declared := binary.LittleEndian.Uint32(hdr[4:8])
	if int64(declared) < int64(ds64FixedSize) {
		return fmt.Errorf("RF64: ds64 payload of %d bytes at file offset %d is smaller than the %d byte fixed part", declared, headerPos, ds64FixedSize)
	}
	if declared > math.MaxInt32 {
		return fmt.Errorf("RF64: ds64 payload length %d cannot be represented", declared)
	}

	body := make([]byte, declared)
	bodyPos := c.cursor
	if err := c.readFull(body, "ds64 payload"); err != nil {
		if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
			return fmt.Errorf("RF64: truncated ds64 payload at file offset %d", bodyPos)
		}
		return err
	}
	t := &ds64Table{
		riffSize:    binary.LittleEndian.Uint64(body[0:8]),
		dataSize:    binary.LittleEndian.Uint64(body[8:16]),
		sampleCount: binary.LittleEndian.Uint64(body[16:24]),
		tableLength: binary.LittleEndian.Uint32(body[24:28]),
	}
	tableBytes := int64(declared) - int64(ds64FixedSize)
	if int64(t.tableLength)*int64(ds64EntrySize) > tableBytes {
		return fmt.Errorf("RF64: ds64 declares %d table entries but only %d payload bytes remain at file offset %d", t.tableLength, tableBytes, c.cursor)
	}
	t.entries = make([]ds64Entry, 0, t.tableLength)
	entryBase := c.cursor - int64(declared) + int64(ds64FixedSize)
	for i := uint32(0); i < t.tableLength; i++ {
		off := int64(ds64FixedSize) + int64(i)*int64(ds64EntrySize)
		var eid [4]byte
		copy(eid[:], body[off:off+4])
		t.entries = append(t.entries, ds64Entry{
			id:  eid,
			sz:  binary.LittleEndian.Uint64(body[off+4 : off+12]),
			pos: entryBase + int64(i)*int64(ds64EntrySize) + 4,
		})
	}
	t.original = append([]ds64Entry(nil), t.entries...)
	// skip any trailing ds64 bytes (future extensions) and the padding byte
	if err := c.skipBytes(tableBytes-int64(t.tableLength)*int64(ds64EntrySize), "ds64 trailing bytes"); err != nil {
		return err
	}
	if err := c.skipPadding(uint64(declared), "ds64"); err != nil {
		return err
	}
	c.ds64 = t
	c.riffSize = t.riffSize
	c.firstChunkPos = c.cursor
	return nil
}

// closeOpen seeks past the remainder of the chunk last handed out, including
// its padding, so the next chunk header is read from the right place even if
// the caller did not finish the previous chunk.
func (c *rf64Container) closeOpen() error {
	if c.openEnd == 0 {
		return nil
	}
	if _, err := c.seek(0, io.SeekCurrent); err != nil {
		return err
	}
	if c.cursor < c.openEnd {
		if err := c.skipBytes(c.openEnd-c.cursor, "unread chunk data"); err != nil {
			return err
		}
	}
	c.openEnd = 0
	return nil
}

// skipBytes advances the logical cursor without reading the spanned region,
// so multi-GiB unknown chunks cost no I/O and no allocations.
func (c *rf64Container) skipBytes(n int64, what string) error {
	if n < 0 {
		return fmt.Errorf("RF64: negative skip (%d) for %s", n, what)
	}
	if n == 0 {
		return nil
	}
	if c.riffSize > 0 && uint64(c.cursor)+uint64(n) > c.riffSize+8 {
		return fmt.Errorf("RF64: %s would end at offset %d past the container end at offset %d", what, c.cursor+n, c.riffSize+8)
	}
	if c.physicalSize > 0 && c.cursor+n > c.physicalSize {
		return fmt.Errorf("RF64: %s would end at offset %d but the file is only %d bytes long", what, c.cursor+n, c.physicalSize)
	}
	if _, err := c.seek(c.cursor+n, io.SeekStart); err != nil {
		return err
	}
	return nil
}

// skipPadding skips the single word-alignment padding byte of an odd-sized
// chunk. Chunk lengths never include the padding byte. A final chunk written
// by tools that omit the trailing pad is tolerated: the pad is only skipped
// when it actually lies inside the container.
func (c *rf64Container) skipPadding(payloadLen uint64, what string) error {
	if payloadLen%2 == 0 {
		return nil
	}
	if c.riffSize > 0 && uint64(c.cursor)+1 > c.riffSize+8 {
		return nil
	}
	if c.physicalSize > 0 && c.cursor+1 > c.physicalSize {
		return nil
	}
	return c.skipBytes(1, what+" padding byte")
}

// nextChunkHeader reads the next chunk header and resolves its length,
// consuming a ds64 table entry only when the 32-bit length is the sentinel.
func (c *rf64Container) nextChunkHeader() ([4]byte, uint64, int64, error) {
	var hdr [8]byte
	hdrPos := c.cursor
	if err := c.readFull(hdr[:], "chunk header"); err != nil {
		if errors.Is(err, io.EOF) && c.riffSize > 0 && uint64(hdrPos) < c.riffSize+8 {
			return [4]byte{}, 0, hdrPos, fmt.Errorf("RF64: truncated chunk header at file offset %d (container ends at %d)", hdrPos, c.riffSize+8)
		}
		return [4]byte{}, 0, hdrPos, err
	}
	var id [4]byte
	copy(id[:], hdr[:4])
	declared := uint64(binary.LittleEndian.Uint32(hdr[4:8]))
	if declared == uint64(rf64LengthSentinel) {
		if id == riff.DataFormatID {
			if c.ds64.dataSize == 0 && c.ds64.tableLength == 0 {
				return id, 0, hdrPos, fmt.Errorf("RF64: data chunk at file offset %d uses 0xffffffff but ds64 data size is missing", hdrPos)
			}
			declared = c.ds64.dataSize
		} else {
			entry, err := c.ds64.take(id)
			if err != nil {
				return id, 0, hdrPos, err
			}
			declared = entry.sz
		}
	}
	if declared > math.MaxInt64-uint64(c.cursor) {
		return id, 0, hdrPos, fmt.Errorf("RF64: chunk %q at file offset %d has an unrepresentable length %d", string(id[:]), hdrPos, declared)
	}
	end := c.cursor + int64(declared)
	if c.riffSize > 0 && uint64(end) > c.riffSize+8 {
		return id, 0, hdrPos, fmt.Errorf("RF64: chunk %q at file offset %d ends at %d, past the container end at offset %d", string(id[:]), hdrPos, end, c.riffSize+8)
	}
	if c.physicalSize > 0 && end > c.physicalSize {
		return id, 0, hdrPos, fmt.Errorf("RF64: chunk %q at file offset %d would end at %d but the file is only %d bytes long", string(id[:]), hdrPos, end, c.physicalSize)
	}
	return id, declared, hdrPos, nil
}

// chunk returns a limited reader exposing exactly the payload of the chunk
// starting at the current cursor. Neither padding nor following chunks are
// reachable through it. Reading past the declared payload returns io.EOF.
func (c *rf64Container) chunk(id [4]byte, payloadLen uint64) *riff.Chunk {
	return &riff.Chunk{
		ID:   id,
		Size: int(payloadLen),
		R:    io.LimitReader(c.r, int64(payloadLen)),
	}
}

// finishChunk positions the cursor after the chunk payload and its padding.
// Callers must have fully consumed (or drained) the limited payload reader.
func (c *rf64Container) finishChunk(id [4]byte, payloadLen uint64) error {
	// chunk readers are bounded, so the underlying stream is exactly at the
	// end of the payload at this point; realign the bookkeeping anyway.
	if _, err := c.seek(0, io.SeekCurrent); err != nil {
		return err
	}
	return c.skipPadding(payloadLen, "chunk "+string(id[:]))
}

// drainChunk skips an entire chunk payload plus padding using seeks.
func (c *rf64Container) drainChunk(id [4]byte, payloadLen uint64) error {
	if err := c.skipBytes(int64(payloadLen), "chunk "+string(id[:])+" payload"); err != nil {
		return err
	}
	return c.skipPadding(payloadLen, "chunk "+string(id[:]))
}

// pcmFrameCount converts a PCM byte length into whole frames.
func pcmFrameCount(dataSize uint64, blockAlign int) (uint64, error) {
	if blockAlign <= 0 {
		return 0, fmt.Errorf("RF64: invalid block align %d", blockAlign)
	}
	if dataSize%uint64(blockAlign) != 0 {
		return 0, fmt.Errorf("RF64: data chunk holds %d bytes, which is not a whole number of %d-byte frames", dataSize, blockAlign)
	}
	return dataSize / uint64(blockAlign), nil
}

// durationFromFrames computes the playback duration from whole PCM frames
// rather than from the container length.
func durationFromFrames(frames uint64, sampleRate uint32) (time.Duration, error) {
	if sampleRate == 0 {
		return 0, errors.New("RF64: sample rate is zero")
	}
	if frames > math.MaxInt64/uint64(time.Second) {
		return 0, fmt.Errorf("RF64: frame count %d is too large to represent as a duration", frames)
	}
	return time.Duration(frames) * time.Second / time.Duration(sampleRate), nil
}
