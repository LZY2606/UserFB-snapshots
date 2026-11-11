package wav

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"time"

	"github.com/go-audio/audio"
	"github.com/go-audio/riff"
)

var (
	// CIDList is the chunk ID for a LIST chunk
	CIDList = [4]byte{'L', 'I', 'S', 'T'}
	// CIDSmpl is the chunk ID for a smpl chunk
	CIDSmpl = [4]byte{'s', 'm', 'p', 'l'}
	// CIDINFO is the chunk ID for an INFO chunk
	CIDInfo = []byte{'I', 'N', 'F', 'O'}
	// CIDCue is the chunk ID for the cue chunk
	CIDCue = [4]byte{'c', 'u', 'e', 0x20}
)

// Decoder handles the decoding of wav files.
type Decoder struct {
	r      io.ReadSeeker
	parser *riff.Parser

	NumChans   uint16
	BitDepth   uint16
	SampleRate uint32

	AvgBytesPerSec uint32
	WavAudioFormat uint16

	err             error
	PCMSize         int
	pcmDataAccessed bool
	// pcmChunk is available so we can use the LimitReader
	PCMChunk *riff.Chunk
	// Metadata for the current file
	Metadata *Metadata

	// RF64 (EBU Tech 3306) specific state. rf64 is nil for ordinary RIFF files.
	rf64       *rf64Container
	rf64Frames uint64
	dataSeen   bool
	isRF64     bool // an RF64 header was seen, even if parsing failed
}

// NewDecoder creates a decoder for the passed wav reader.
// Note that the reader doesn't get rewinded as the container is processed.
func NewDecoder(r io.ReadSeeker) *Decoder {
	return &Decoder{
		r:      r,
		parser: riff.New(r),
	}
}

// Seek provides access to the cursor position in the PCM data
func (d *Decoder) Seek(offset int64, whence int) (int64, error) {
	return d.r.Seek(offset, whence)
}

// Rewind allows the decoder to be rewound to the beginning of the PCM data.
// This is useful if you want to keep on decoding the same file in a loop.
func (d *Decoder) Rewind() error {

	_, err := d.r.Seek(0, io.SeekStart)
	if err != nil {
		return fmt.Errorf("failed to seek back to the start %w", err)
	}
	// we have to use a new parser since it's read only and can't be seeked
	d.parser = riff.New(d.r)
	d.pcmDataAccessed = false
	d.PCMChunk = nil
	d.err = nil
	d.NumChans = 0
	d.rf64 = nil
	d.rf64Frames = 0
	d.dataSeen = false
	d.isRF64 = false
	err = d.FwdToPCM()
	if err != nil {
		return fmt.Errorf("failed to seek to the PCM data: %w", err)
	}
	return nil
}

// SampleBitDepth returns the bit depth encoding of each sample.
func (d *Decoder) SampleBitDepth() int32 {
	if d == nil {
		return 0
	}
	return int32(d.BitDepth)
}

// PCMLen returns the total number of bytes in the PCM data chunk
func (d *Decoder) PCMLen() int64 {
	if d == nil {
		return 0
	}
	if d.rf64 != nil && !d.dataSeen && d.rf64.ds64 != nil {
		// the data chunk has not been located yet, but ds64 already carries
		// its real 64-bit length
		return int64(d.rf64.ds64.dataSize)
	}
	return int64(d.PCMSize)
}

// Err returns the first non-EOF error that was encountered by the Decoder.
func (d *Decoder) Err() error {
	if errors.Is(d.err, io.EOF) {
		return nil
	}
	return d.err
}

// EOF returns positively if the underlying reader reached the end of file.
func (d *Decoder) EOF() bool {
	if d == nil || errors.Is(d.err, io.EOF) {
		return true
	}
	return false
}

// IsValidFile verifies that the file is valid/readable.
func (d *Decoder) IsValidFile() bool {
	d.err = d.readHeaders()
	if d.err != nil {
		return false
	}
	if d.NumChans < 1 {
		return false
	}
	if d.BitDepth < 8 {
		return false
	}
	if d, err := d.Duration(); err != nil || d <= 0 {
		return false
	}

	return true
}

// ReadInfo reads the underlying reader until the comm header is parsed.
// This method is safe to call multiple times.
func (d *Decoder) ReadInfo() {
	d.err = d.readHeaders()
}

// ReadMetadata parses the file for extra metadata such as the INFO list chunk.
// The entire file will be read and should be rewinded if more data must be
// accessed.
func (d *Decoder) ReadMetadata() {
	if d.Metadata != nil {
		return
	}
	d.ReadInfo()
	if d.Err() != nil || d.Metadata != nil {
		return
	}
	if d.rf64 != nil {
		d.readRF64Metadata()
		return
	}
	var (
		chunk *riff.Chunk
		err   error
	)
	for err == nil {
		chunk, err = d.parser.NextChunk()
		if err != nil {
			break
		}

		switch chunk.ID {
		case CIDList:
			if err = DecodeListChunk(d, chunk); err != nil {
				if !errors.Is(err, io.EOF) {
					d.err = err
				}
			}
			if d.Metadata != nil && d.Metadata.SamplerInfo != nil {
				// we got everything we were looking for
				break
			}
		case CIDSmpl:
			if err = DecodeSamplerChunk(d, chunk); err != nil {
				if !errors.Is(err, io.EOF) {
					d.err = err
				}
			}
		case CIDCue:
			if err = DecodeCueChunk(d, chunk); err != nil {
				if !errors.Is(err, io.EOF) {
					d.err = err
				}
			}
		default:
			// fmt.Println(string(chunk.ID[:]))
			chunk.Drain()
		}
	}

}

// readRF64Metadata scans the chunks after data for LIST/INFO, smpl and cue
// metadata. It works even when PCM reading is in progress: the current read
// position is restored afterwards so the next PCM read returns the frame
// right after the previously read one.
func (d *Decoder) readRF64Metadata() {
	c := d.rf64
	// PCM reads go through the chunk's limit reader on the underlying seeker,
	// so resync the bookkeeping cursor before remembering the position.
	savedPos, err := c.seek(0, io.SeekCurrent)
	if err != nil {
		d.err = err
		return
	}
	alreadyAtData := d.PCMChunk != nil && d.dataSeen

	// If the caller already consumed part of the data chunk, jump over the
	// remaining PCM and its padding first.
	if alreadyAtData && c.dataEnd > 0 && c.cursor < c.dataEnd {
		if err := c.skipBytes(c.dataEnd-c.cursor, "remaining PCM while reading metadata"); err != nil {
			d.err = err
			return
		}
	}

	for {
		chunk, err := d.NextChunk()
		if err != nil {
			if errors.Is(err, io.EOF) {
				d.err = nil
			}
			break
		}
		switch chunk.ID {
		case riff.DataFormatID:
			// data already reached via FwdToPCM or being read; skip a second
			// encounter defensively (NextChunk itself rejects a second data)
			if err := d.finishRF64Chunk(chunk); err != nil {
				d.err = err
				goto restore
			}
		case CIDList:
			if err = DecodeListChunk(d, chunk); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
				goto restore
			}
			if err := d.finishRF64Chunk(chunk); err != nil {
				d.err = err
				goto restore
			}
		case CIDSmpl:
			if err = DecodeSamplerChunk(d, chunk); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
				goto restore
			}
			if err := d.finishRF64Chunk(chunk); err != nil {
				d.err = err
				goto restore
			}
		case CIDCue:
			if err = DecodeCueChunk(d, chunk); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
				goto restore
			}
			if err := d.finishRF64Chunk(chunk); err != nil {
				d.err = err
				goto restore
			}
		default:
			if err := d.finishRF64Chunk(chunk); err != nil {
				d.err = err
				goto restore
			}
		}
	}

restore:
	// put the underlying reader back exactly where the PCM stream was
	if alreadyAtData {
		if _, err := c.seek(savedPos, io.SeekStart); err != nil {
			d.err = err
		}
	}
}

// FwdToPCM forwards the underlying reader until the start of the PCM chunk.
// If the PCM chunk was already read, no data will be found (you need to rewind).
func (d *Decoder) FwdToPCM() error {
	if d == nil {
		return fmt.Errorf("PCM data not found")
	}
	d.err = d.readHeaders()
	if d.err != nil {
		if d.isRF64 {
			// structural RF64 errors must surface with their chunk/offset
			// context instead of looking like a missing PCM chunk.
			return d.err
		}
		// keep the historical RIFF behavior: the error stays on d.Err()
		return nil
	}
	if d.rf64 != nil {
		return d.fwdToRF64PCM()
	}

	var chunk *riff.Chunk
	for d.err == nil {
		chunk, d.err = d.NextChunk()
		if d.err != nil {
			return d.err
		}
		if chunk.ID == riff.DataFormatID {
			d.PCMSize = chunk.Size
			d.PCMChunk = chunk
			break
		}
		if chunk.ID == CIDList {
			DecodeListChunk(d, chunk)
		}
		chunk.Drain()
	}
	if chunk == nil {
		return fmt.Errorf("PCM data not found")
	}
	d.pcmDataAccessed = true

	return nil
}

// fwdToRF64PCM walks the RF64 chunks up to the data chunk. Unknown chunks
// are seeked over, never read or buffered, so multi-GiB fillers stay cheap.
func (d *Decoder) fwdToRF64PCM() error {
	for {
		chunk, err := d.NextChunk()
		if err != nil {
			if errors.Is(err, io.EOF) {
				d.err = ErrPCMChunkNotFound
				return ErrPCMChunkNotFound
			}
			d.err = err
			return err
		}
		if chunk.ID == riff.DataFormatID {
			d.PCMChunk = chunk
			d.pcmDataAccessed = true
			return nil
		}
		switch chunk.ID {
		case CIDList:
			if err := DecodeListChunk(d, chunk); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
				return err
			}
		case CIDSmpl:
			if err := DecodeSamplerChunk(d, chunk); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
				return err
			}
		case CIDCue:
			if err := DecodeCueChunk(d, chunk); err != nil && !errors.Is(err, io.EOF) {
				d.err = err
				return err
			}
		}
		if err := d.finishRF64Chunk(chunk); err != nil {
			return err
		}
	}
}

// WasPCMAccessed returns positively if the PCM data was previously accessed.
func (d *Decoder) WasPCMAccessed() bool {
	if d == nil {
		return false
	}
	return d.pcmDataAccessed
}

// FullPCMBuffer is an inefficient way to access all the PCM data contained in the
// audio container. The entire PCM data is held in memory.
// Consider using PCMBuffer() instead.
func (d *Decoder) FullPCMBuffer() (*audio.IntBuffer, error) {
	if !d.WasPCMAccessed() {
		err := d.FwdToPCM()
		if err != nil {
			return nil, d.err
		}
	}
	if d.PCMChunk == nil {
		return nil, errors.New("PCM chunk not found")
	}
	format := &audio.Format{
		NumChannels: int(d.NumChans),
		SampleRate:  int(d.SampleRate),
	}

	buf := &audio.IntBuffer{Data: make([]int, 4096), Format: format, SourceBitDepth: int(d.BitDepth)}
	bytesPerSample := (d.BitDepth-1)/8 + 1
	sampleBufData := make([]byte, bytesPerSample)
	decodeF, err := sampleDecodeFunc(int(d.BitDepth))
	if err != nil {
		return nil, fmt.Errorf("could not get sample decode func %w", err)
	}

	i := 0
	for err == nil {
		buf.Data[i], err = decodeF(d.PCMChunk, sampleBufData)
		if err != nil {
			break
		}
		i++
		// grow the underlying slice if needed
		if i == len(buf.Data) {
			buf.Data = append(buf.Data, make([]int, 4096)...)
		}
	}
	buf.Data = buf.Data[:i]

	if errors.Is(err, io.EOF) {
		err = nil
	}

	return buf, err
}

// PCMBuffer populates the passed PCM buffer
func (d *Decoder) PCMBuffer(buf *audio.IntBuffer) (n int, err error) {
	if buf == nil {
		return 0, nil
	}

	if !d.pcmDataAccessed {
		err := d.FwdToPCM()
		if err != nil {
			return 0, d.err
		}
	}
	if d.PCMChunk == nil {
		return 0, ErrPCMChunkNotFound
	}

	format := &audio.Format{
		NumChannels: int(d.NumChans),
		SampleRate:  int(d.SampleRate),
	}

	buf.SourceBitDepth = int(d.BitDepth)
	decodeF, err := sampleDecodeFunc(int(d.BitDepth))
	if err != nil {
		return 0, fmt.Errorf("could not get sample decode func %w", err)
	}

	bPerSample := bytesPerSample(int(d.BitDepth))
	// populate a file buffer to avoid multiple very small reads
	// we need to cap the buffer size to not be bigger than the pcm chunk.
	size := len(buf.Data) * bPerSample
	tmpBuf := make([]byte, size)
	var m int
	m, err = d.PCMChunk.R.Read(tmpBuf)
	if err != nil {
		if errors.Is(err, io.EOF) {
			return m, nil
		}
		return m, err
	}
	if m == 0 {
		return m, nil
	}
	bufR := bytes.NewReader(tmpBuf[:m])
	sampleBuf := make([]byte, bPerSample, bPerSample)
	var misaligned bool
	if m%bPerSample > 0 {
		misaligned = true
	}

	// Note that we populate the buffer even if the
	// size of the buffer doesn't fit an even number of frames.
	for n = 0; n < len(buf.Data); n++ {
		buf.Data[n], err = decodeF(bufR, sampleBuf)
		if err != nil {
			// the last sample isn't a full sample but just padding.
			if misaligned {
				n--
			}
			break
		}
	}
	buf.Format = format
	if errors.Is(err, io.EOF) {
		err = nil
	}

	return n, err
}

// Format returns the audio format of the decoded content.
func (d *Decoder) Format() *audio.Format {
	if d == nil {
		return nil
	}
	return &audio.Format{
		NumChannels: int(d.NumChans),
		SampleRate:  int(d.SampleRate),
	}
}

// NextChunk returns the next available chunk.
// The returned chunk's Size is the actual payload length: word-alignment
// padding and following chunks are not reachable through its reader.
// Callers are expected to finish reading a chunk before calling NextChunk.
func (d *Decoder) NextChunk() (*riff.Chunk, error) {
	if d.err = d.readHeaders(); d.err != nil {
		d.err = fmt.Errorf("failed to read header - %v", d.err)
		return nil, d.err
	}

	if d.rf64 != nil {
		return d.nextRF64Chunk()
	}

	var (
		id   [4]byte
		size uint32
	)

	id, size, d.err = d.parser.IDnSize()
	if d.err != nil {
		d.err = fmt.Errorf("error reading chunk header - %v", d.err)
		return nil, d.err
	}

	// TODO: any reason we don't use d.parser.NextChunk (riff.NextChunk) here?
	// It correctly handles the misaligned chunk.

	// TODO: copied over from riff.parser.NextChunk
	// all RIFF chunks (including WAVE "data" chunks) must be word aligned.
	// If the data uses an odd number of bytes, a padding byte with a value of zero must be placed at the end of the sample data.
	// The "data" chunk header's size should not include this byte.
	if size%2 == 1 {
		size++
	}

	c := &riff.Chunk{
		ID:   id,
		Size: int(size),
		R:    io.LimitReader(d.r, int64(size)),
	}
	return c, d.err
}

// nextRF64Chunk reads and resolves one RF64 chunk. ds64 table entries are
// only consumed by chunks carrying the 0xffffffff sentinel.
func (d *Decoder) nextRF64Chunk() (*riff.Chunk, error) {
	c := d.rf64
	if err := c.closeOpen(); err != nil {
		d.err = err
		return nil, err
	}
	id, payloadLen, hdrPos, err := c.nextChunkHeader()
	if err != nil {
		d.err = err
		return nil, err
	}
	switch id {
	case cidDS64:
		// the first ds64 was consumed while reading the headers; another one
		// anywhere else in the container is invalid.
		d.err = fmt.Errorf("RF64: second ds64 chunk at file offset %d is not allowed", hdrPos)
		return nil, d.err
	case riff.DataFormatID:
		if d.dataSeen {
			d.err = fmt.Errorf("RF64: second data chunk at file offset %d is not allowed", hdrPos)
			return nil, d.err
		}
		d.dataSeen = true
		blockAlign := int(d.NumChans) * int(d.BitDepth) / 8
		frames, ferr := pcmFrameCount(payloadLen, blockAlign)
		if ferr != nil {
			d.err = ferr
			return nil, ferr
		}
		if sc := c.ds64.sampleCount; sc != 0 && sc != frames {
			d.err = fmt.Errorf("RF64: ds64 sample count %d does not match the %d whole PCM frames in data at file offset %d", sc, frames, hdrPos)
			return nil, d.err
		}
		d.rf64Frames = frames
		d.PCMSize = int(payloadLen)
		// nextChunkHeader consumed the 8 byte header; cursor marks the payload
		c.dataEnd = c.cursor + int64(payloadLen) + int64(payloadLen%2)
	default:
		// other chunks, including repeated unknown chunks, are handled by the
		// same machinery and keep consuming only their own table entries.
	}
	ch := c.chunk(id, payloadLen)
	// nextChunkHeader consumed the 8 byte header; cursor marks the payload
	c.openEnd = c.cursor + int64(payloadLen) + int64(payloadLen%2)
	return ch, nil
}

// finishRF64Chunk is called once an RF64 chunk returned by NextChunk is no
// longer needed; it seeks over any unread payload and the padding byte,
// rather than reading potentially multi-GiB payloads.
func (d *Decoder) finishRF64Chunk(ch *riff.Chunk) error {
	if ch == nil || d.rf64 == nil {
		return nil
	}
	payloadLen := uint64(ch.Size)
	// chunk reads happen through the limit reader on the underlying seeker,
	// so resync the bookkeeping cursor before computing the skip distance.
	if _, err := d.rf64.seek(0, io.SeekCurrent); err != nil {
		d.err = err
		return err
	}
	if !ch.IsFullyRead() {
		remaining := int64(payloadLen) - int64(ch.Pos)
		if err := d.rf64.skipBytes(remaining, "chunk "+string(ch.ID[:])+" remaining payload"); err != nil {
			d.err = err
			return err
		}
	}
	if err := d.rf64.skipPadding(payloadLen, "chunk "+string(ch.ID[:])); err != nil {
		d.err = err
		return err
	}
	d.rf64.openEnd = 0
	return nil
}

// Duration returns the time duration for the current audio container.
// For RF64 files it is derived from whole PCM frames and the sample rate,
// never estimated from the total container length.
func (d *Decoder) Duration() (time.Duration, error) {
	if d == nil || d.parser == nil {
		return 0, errors.New("can't calculate the duration of a nil pointer")
	}
	if d.err = d.readHeaders(); d.err != nil {
		return 0, d.err
	}
	if d.rf64 != nil {
		if d.SampleRate == 0 {
			return 0, errors.New("can't calculate the duration without a sample rate")
		}
		if !d.dataSeen {
			// Locate the data chunk (seek-only for unknown chunks) so the
			// duration is derived from its real payload and the ds64 sample
			// count is validated against the actual frame count.
			if err := d.fwdToRF64PCM(); err != nil {
				return 0, err
			}
		}
		blockAlign := int(d.NumChans) * int(d.BitDepth) / 8
		frames, err := pcmFrameCount(uint64(d.PCMSize), blockAlign)
		if err != nil {
			return 0, err
		}
		return durationFromFrames(frames, d.SampleRate)
	}
	return d.parser.Duration()
}

// String implements the Stringer interface.
func (d *Decoder) String() string {
	if d == nil {
		return ""
	}
	if d.rf64 != nil {
		dur, _ := d.Duration()
		return fmt.Sprintf("Format: %s - %d channels @ %d / %d bits - Duration: %f seconds",
			string(riff.WavFormatID[:]), d.NumChans, d.SampleRate, d.BitDepth, dur.Seconds())
	}
	return d.parser.String()
}

// readHeaders is safe to call multiple times
func (d *Decoder) readHeaders() error {
	if d == nil || d.NumChans > 0 {
		return nil
	}

	var id [4]byte
	if _, err := io.ReadFull(d.r, id[:]); err != nil {
		return err
	}
	switch id {
	case cidRF64:
		d.isRF64 = true
		return d.readRF64Headers(id)
	case riff.RiffID:
		return d.readRIFFHeaders(id)
	default:
		return fmt.Errorf("%s - %s", string(id[:]), riff.ErrFmtNotSupported)
	}
}

// readRIFFHeaders contains the original RIFF-only header parsing.
func (d *Decoder) readRIFFHeaders(firstID [4]byte) error {
	var sizeBuf [4]byte
	if _, err := io.ReadFull(d.r, sizeBuf[:]); err != nil {
		return err
	}
	d.parser.ID = firstID
	d.parser.Size = binary.LittleEndian.Uint32(sizeBuf[:])
	if err := binary.Read(d.r, binary.BigEndian, &d.parser.Format); err != nil {
		return err
	}

	var chunk *riff.Chunk
	var rewindBytes int64
	var err error

	for err == nil {
		chunk, err = d.parser.NextChunk()
		if err != nil {
			break
		}

		if chunk.ID == riff.FmtID {
			chunk.DecodeWavHeader(d.parser)
			d.NumChans = d.parser.NumChannels
			d.BitDepth = d.parser.BitsPerSample
			d.SampleRate = d.parser.SampleRate
			d.WavAudioFormat = d.parser.WavAudioFormat
			d.AvgBytesPerSec = d.parser.AvgBytesPerSec

			if rewindBytes > 0 {
				d.r.Seek(-(rewindBytes + int64(chunk.Size) + 8), 1)
			}
			break
		} else if chunk.ID == CIDList {
			// The list chunk can be in the header or footer
			// because so many players don't support that chunk properly
			// it is recommended to have it at the end of the file.
			DecodeListChunk(d, chunk)
			// unexpected chunk order, might be a bext chunk
			rewindBytes += int64(chunk.Size) + 8
		} else if chunk.ID == CIDSmpl {
			DecodeSamplerChunk(d, chunk)
			rewindBytes += int64(chunk.Size) + 8
		} else {
			// unexpected chunk order, might be a bext chunk
			rewindBytes += int64(chunk.Size) + 8
			// drain the chunk
			io.CopyN(io.Discard, d.r, int64(chunk.Size))
		}
	}

	return d.err
}

// readRF64Headers parses the RF64 container header, its mandatory ds64 chunk
// and the fmt chunk (which may be preceded by other chunks).
func (d *Decoder) readRF64Headers(containerID [4]byte) error {
	c := newRF64Container(d.r)
	// the 4 container ID bytes were already consumed by readHeaders
	c.cursor = 4
	var sizeBuf [4]byte
	if err := c.readFull(sizeBuf[:], "RF64 size field"); err != nil {
		if errors.Is(err, io.EOF) {
			return fmt.Errorf("RF64: truncated container header at file offset 4")
		}
		return err
	}
	outerSize := binary.LittleEndian.Uint32(sizeBuf[:])
	var format [4]byte
	if err := c.readFull(format[:], "WAVE format ID"); err != nil {
		if errors.Is(err, io.EOF) {
			return fmt.Errorf("RF64: truncated container header at file offset 8")
		}
		return err
	}
	if format != riff.WavFormatID {
		return fmt.Errorf("RF64: %s - %s", string(format[:]), riff.ErrFmtNotSupported)
	}
	if err := c.parseDS64(); err != nil {
		return err
	}
	// The 64-bit riffSize replaces the outer field only when it carries the
	// sentinel; otherwise the ordinary 32-bit outer length is authoritative.
	if outerSize != rf64LengthSentinel {
		c.riffSize = uint64(outerSize)
	}
	if err := c.measurePhysicalSize(); err != nil {
		return err
	}

	d.parser.ID = containerID
	d.parser.Size = outerSize
	d.parser.Format = riff.WavFormatID

	var sawDS64 bool
	for {
		id, payloadLen, hdrPos, err := c.nextChunkHeader()
		if err != nil {
			return err
		}
		switch id {
		case cidDS64:
			if sawDS64 {
				return fmt.Errorf("RF64: second ds64 chunk at file offset %d is not allowed", hdrPos)
			}
			sawDS64 = true
			if err := c.drainChunk(id, payloadLen); err != nil {
				return err
			}
		case riff.FmtID:
			ch := c.chunk(id, payloadLen)
			if err := ch.DecodeWavHeader(d.parser); err != nil {
				return fmt.Errorf("RF64: failed decoding fmt chunk at file offset %d: %w", hdrPos, err)
			}
			d.NumChans = d.parser.NumChannels
			d.BitDepth = d.parser.BitsPerSample
			d.SampleRate = d.parser.SampleRate
			d.WavAudioFormat = d.parser.WavAudioFormat
			d.AvgBytesPerSec = d.parser.AvgBytesPerSec
			if !ch.IsFullyRead() {
				ch.Drain()
			}
			// rewind so the chunk following fmt can be parsed again from the wire
			if err := c.finishChunk(id, payloadLen); err != nil {
				return err
			}
			// rewind so the chunk following fmt can be parsed again from the wire.
			// The header scan may have consumed ds64 table entries while
			// resolving chunks before fmt, so restart substitution from the
			// beginning of the table.
			c.ds64.reset()
			if _, err := c.seek(c.firstChunkPos, io.SeekStart); err != nil {
				return err
			}
			d.rf64 = c
			return nil
		case CIDList:
			ch := c.chunk(id, payloadLen)
			if err := DecodeListChunk(d, ch); err != nil {
				return err
			}
			if err := c.finishChunk(id, payloadLen); err != nil {
				return err
			}
		case CIDSmpl:
			ch := c.chunk(id, payloadLen)
			if err := DecodeSamplerChunk(d, ch); err != nil {
				return err
			}
			if err := c.finishChunk(id, payloadLen); err != nil {
				return err
			}
		case CIDCue:
			ch := c.chunk(id, payloadLen)
			if err := DecodeCueChunk(d, ch); err != nil {
				return err
			}
			if err := c.finishChunk(id, payloadLen); err != nil {
				return err
			}
		default:
			if err := c.drainChunk(id, payloadLen); err != nil {
				return err
			}
		}
	}
}

func bytesPerSample(bitDepth int) int {
	return bitDepth / 8
}

// sampleDecodeFunc returns a function that can be used to convert
// a byte range into an int value based on the amount of bits used per sample.
// Note that 8bit samples are unsigned, all other values are signed.
func sampleDecodeFunc(bitsPerSample int) (func(io.Reader, []byte) (int, error), error) {
	// NOTE: WAV PCM data is stored using little-endian
	switch bitsPerSample {
	case 8:
		// 8bit values are unsigned
		return func(r io.Reader, buf []byte) (int, error) {
			_, err := r.Read(buf[:1])
			return int(buf[0]), err
		}, nil
	case 16:
		return func(r io.Reader, buf []byte) (int, error) {
			_, err := r.Read(buf[:2])
			return int(int16(binary.LittleEndian.Uint16(buf[:2]))), err
		}, nil
	case 24:
		// -34,359,738,367 (0x7FFFFF) to 34,359,738,368	(0x800000)
		return func(r io.Reader, buf []byte) (int, error) {
			_, err := r.Read(buf[:3])
			if err != nil {
				return 0, err
			}
			return int(audio.Int24LETo32(buf[:3])), nil
		}, nil
	case 32:
		return func(r io.Reader, buf []byte) (int, error) {
			_, err := r.Read(buf[:4])
			return int(int32(binary.LittleEndian.Uint32(buf[:4]))), err
		}, nil
	default:
		return nil, fmt.Errorf("unhandled byte depth:%d", bitsPerSample)
	}
}

// sampleDecodeFloat64Func returns a function that can be used to convert
// a byte range into a float64 value based on the amount of bits used per sample.
func sampleFloat64DecodeFunc(bitsPerSample int) (func([]byte) float64, error) {
	bytesPerSample := bitsPerSample / 8
	switch bytesPerSample {
	case 1:
		// 8bit values are unsigned
		return func(s []byte) float64 {
			return float64(uint8(s[0]))
		}, nil
	case 2:
		return func(s []byte) float64 {
			return float64(int(s[0]) + int(s[1])<<8)
		}, nil
	case 3:
		return func(s []byte) float64 {
			var output int32
			output |= int32(s[2]) << 0
			output |= int32(s[1]) << 8
			output |= int32(s[0]) << 16
			return float64(output)
		}, nil
	case 4:
		// TODO: fix the float64 conversion (current int implementation)
		return func(s []byte) float64 {
			return float64(int(s[0]) + int(s[1])<<8 + int(s[2])<<16 + int(s[3])<<24)
		}, nil
	default:
		return nil, fmt.Errorf("unhandled byte depth:%d", bitsPerSample)
	}
}
