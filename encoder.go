package wav

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/go-audio/audio"
	"github.com/go-audio/riff"
)

// Encoder encodes LPCM data into a wav containter.
type Encoder struct {
	w   io.WriteSeeker
	buf *bytes.Buffer

	SampleRate int
	BitDepth   int
	NumChans   int

	// A number indicating the WAVE format category of the file. The content of
	// the <format-specific-fields> portion of the ‘fmt’ chunk, and the
	// interpretation of the waveform data, depend on this value. PCM = 1 (i.e.
	// Linear quantization) Values other than 1 indicate some form of
	// compression.
	WavAudioFormat int

	// Metadata contains metadata to inject in the file.
	Metadata *Metadata

	// AutoRF64 asks the encoder to reserve the RF64 ds64 area up front (as a
	// JUNK chunk) and to promote the container in place once the final RIFF
	// or data size reaches 0xffffffff. Set it before the first Write or
	// WriteFrame call; the mode is frozen for the rest of the encoding.
	AutoRF64 bool

	WrittenBytes    int
	frames          int
	pcmChunkStarted bool
	pcmChunkSizePos int
	wroteHeader     bool // true if we've written the header out

	// RF64 bookkeeping.
	rf64        bool  // effective mode, frozen at the first write
	modeLocked  bool  // true once the effective mode has been chosen
	pcmBytes    int64 // exact number of PCM payload bytes written
	rf64JunkPos int64 // file offset of the reserved JUNK chunk
	pcmPadded   bool  // true once the odd PCM alignment byte was written
}

// NewEncoder creates a new encoder to create a new wav file.
// Don't forget to add Frames to the encoder before writing.
func NewEncoder(w io.WriteSeeker, sampleRate, bitDepth, numChans, audioFormat int) *Encoder {
	return &Encoder{
		w:              w,
		buf:            bytes.NewBuffer(make([]byte, 0, bytesNumFromDuration(time.Minute, sampleRate, bitDepth)*numChans)),
		SampleRate:     sampleRate,
		BitDepth:       bitDepth,
		NumChans:       numChans,
		WavAudioFormat: audioFormat,
	}
}

// AddLE serializes and adds the passed value using little endian
func (e *Encoder) AddLE(src interface{}) error {
	return e.addEncoded(binary.LittleEndian, src)
}

// AddBE serializes and adds the passed value using big endian
func (e *Encoder) AddBE(src interface{}) error {
	return e.addEncoded(binary.BigEndian, src)
}

// addEncoded serializes src and writes it in one go, treating a short write
// as a failure instead of silently reporting success.
func (e *Encoder) addEncoded(order binary.ByteOrder, src interface{}) error {
	var b bytes.Buffer
	if err := binary.Write(&b, order, src); err != nil {
		return err
	}
	n, err := e.w.Write(b.Bytes())
	e.WrittenBytes += n
	if err != nil {
		return err
	}
	if n != b.Len() {
		return io.ErrShortWrite
	}
	return nil
}

func (e *Encoder) addBuffer(buf *audio.IntBuffer) error {
	if buf == nil {
		return fmt.Errorf("can't add a nil buffer")
	}

	frameCount := buf.NumFrames()
	// performance tweak: setup a buffer so we don't do too many writes
	var err error
	for i := 0; i < frameCount; i++ {
		for j := 0; j < buf.Format.NumChannels; j++ {
			v := buf.Data[i*buf.Format.NumChannels+j]
			switch e.BitDepth {
			case 8:
				if err = binary.Write(e.buf, binary.LittleEndian, uint8(v)); err != nil {
					return err
				}
			case 16:
				if err = binary.Write(e.buf, binary.LittleEndian, int16(v)); err != nil {
					return err
				}
			case 24:
				if err = binary.Write(e.buf, binary.LittleEndian, audio.Int32toInt24LEBytes(int32(v))); err != nil {
					return err
				}
			case 32:
				if err = binary.Write(e.buf, binary.LittleEndian, int32(v)); err != nil {
					return err
				}
			default:
				return fmt.Errorf("can't add frames of bit size %d", e.BitDepth)
			}
		}
		e.frames++
	}
	data := e.buf.Bytes()
	n, err := e.w.Write(data)
	if err != nil {
		e.WrittenBytes += n
		e.buf.Reset()
		return err
	}
	if n != len(data) {
		e.WrittenBytes += n
		e.buf.Reset()
		return fmt.Errorf("short write: wrote %d of %d PCM bytes", n, len(data))
	}
	e.WrittenBytes += n
	e.pcmBytes += int64(n)
	e.buf.Reset()

	return nil
}

func (e *Encoder) writeHeader() error {
	if e.wroteHeader {
		return errors.New("already wrote header")
	}
	e.wroteHeader = true
	if e == nil {
		return fmt.Errorf("can't write a nil encoder")
	}
	if e.w == nil {
		return fmt.Errorf("can't write to a nil writer")
	}

	if e.WrittenBytes > 0 {
		return nil
	}

	// riff ID
	if err := e.AddLE(riff.RiffID); err != nil {
		return err
	}
	// file size uint32, to update later on.
	if err := e.AddLE(uint32(4294967295)); err != nil {
		return err
	}
	// wave headers
	if err := e.AddLE(riff.WavFormatID); err != nil {
		return err
	}
	if e.rf64 {
		// Reserve the ds64 fixed area as a JUNK chunk right after WAVE.
		// It is rewritten in place if the container is promoted to RF64.
		e.rf64JunkPos = int64(e.WrittenBytes)
		if err := e.AddLE(cidJUNK); err != nil {
			return err
		}
		// JUNK chunk size: the 28 byte fixed ds64 part.
		if err := e.AddLE(uint32(ds64FixedSize)); err != nil {
			return err
		}
		if err := e.AddBE(make([]byte, ds64FixedSize)); err != nil {
			return err
		}
	}
	// form
	if err := e.AddLE(riff.FmtID); err != nil {
		return err
	}
	// chunk size
	if err := e.AddLE(uint32(16)); err != nil {
		return err
	}
	// wave format
	if err := e.AddLE(uint16(e.WavAudioFormat)); err != nil {
		return err
	}
	// num channels
	if err := e.AddLE(uint16(e.NumChans)); err != nil {
		return fmt.Errorf("error encoding the number of channels - %w", err)
	}
	// samplerate
	if err := e.AddLE(uint32(e.SampleRate)); err != nil {
		return fmt.Errorf("error encoding the sample rate - %w", err)
	}
	blockAlign := e.NumChans * e.BitDepth / 8
	// avg bytes per sec
	if err := e.AddLE(uint32(e.SampleRate * blockAlign)); err != nil {
		return fmt.Errorf("error encoding the avg bytes per sec - %w", err)
	}
	// block align
	if err := e.AddLE(uint16(blockAlign)); err != nil {
		return err
	}
	// bits per sample
	if err := e.AddLE(uint16(e.BitDepth)); err != nil {
		return fmt.Errorf("error encoding bits per sample - %w", err)
	}

	return nil
}

// lockMode freezes the RF64 mode at the first write, so later changes to
// AutoRF64 cannot affect an encoding that already started.
func (e *Encoder) lockMode() {
	if !e.modeLocked {
		e.rf64 = e.AutoRF64
		e.modeLocked = true
	}
}

// startPCMChunk writes the data chunk ID and reserves its size field.
func (e *Encoder) startPCMChunk() error {
	if err := e.AddLE(riff.DataFormatID); err != nil {
		return fmt.Errorf("error encoding sound header %w", err)
	}
	e.pcmChunkStarted = true

	// write a temporary chunksize
	e.pcmChunkSizePos = e.WrittenBytes
	if err := e.AddLE(uint32(4294967295)); err != nil {
		return fmt.Errorf("%w when writing wav data chunk size header", err)
	}
	return nil
}

// Write encodes and writes the passed buffer to the underlying writer.
// Don't forget to Close() the encoder or the file won't be valid.
func (e *Encoder) Write(buf *audio.IntBuffer) error {
	e.lockMode()
	if !e.wroteHeader {
		if err := e.writeHeader(); err != nil {
			return err
		}
	}

	if !e.pcmChunkStarted {
		if err := e.startPCMChunk(); err != nil {
			return err
		}
	}

	return e.addBuffer(buf)
}

// encodedSize returns the number of bytes addEncoded writes for src.
// A []byte is written verbatim by binary.Write and reports -1 from
// binary.Size, so it needs its length explicitly.
func encodedSize(src interface{}) int {
	if b, ok := src.([]byte); ok {
		return len(b)
	}
	return binary.Size(src)
}

// WriteFrame writes a single frame of data to the underlying writer.
func (e *Encoder) WriteFrame(value interface{}) error {
	e.lockMode()
	if e.rf64 {
		// The frame must serialize to exactly one blockAlign; reject bad
		// values before this frame contributes anything to the output.
		blockAlign := e.NumChans * e.BitDepth / 8
		n := encodedSize(value)
		if n < 0 {
			return fmt.Errorf("unsupported frame value type %T", value)
		}
		if n != blockAlign {
			return fmt.Errorf("frame serializes to %d bytes, want exactly blockAlign %d", n, blockAlign)
		}
	}
	if !e.wroteHeader {
		e.writeHeader()
	}
	if !e.pcmChunkStarted {
		if err := e.startPCMChunk(); err != nil {
			return err
		}
	}

	if err := e.AddLE(value); err != nil {
		return err
	}
	e.pcmBytes += int64(encodedSize(value))
	e.frames++
	return nil
}

func (e *Encoder) writeMetadata() error {
	chunkData := encodeInfoChunk(e)
	if err := e.AddBE(CIDList); err != nil {
		return fmt.Errorf("failed to write the LIST chunk ID: %w", err)
	}
	if err := e.AddLE(uint32(len(chunkData))); err != nil {
		return fmt.Errorf("failed to write the LIST chunk size: %w", err)
	}
	return e.AddBE(chunkData)
}

// Close flushes the content to disk, make sure the headers are up to date
// Note that the underlying writer is NOT being closed.
func (e *Encoder) Close() error {
	if e == nil || e.w == nil {
		return nil
	}
	e.lockMode()

	if e.rf64 && e.pcmChunkStarted && e.pcmBytes%2 == 1 {
		// Word-align the PCM data. The padding byte counts towards riffSize
		// but never towards dataSize or the frame count.
		if err := e.AddLE(uint8(0)); err != nil {
			return fmt.Errorf("failed to write the PCM alignment byte - %w", err)
		}
		e.pcmPadded = true
	}

	// inject metadata at the end to not trip implementation not supporting
	// metadata chunks
	if e.Metadata != nil {
		if err := e.writeMetadata(); err != nil {
			return fmt.Errorf("failed to write metadata - %w", err)
		}
	}

	if e.rf64 {
		return e.closeRF64()
	}

	// go back and write total size in header
	if _, err := e.w.Seek(4, 0); err != nil {
		return err
	}
	if err := e.AddLE(uint32(e.WrittenBytes) - 8); err != nil {
		return fmt.Errorf("%w when writing the total written bytes", err)
	}

	// rewrite the audio chunk length header
	if e.pcmChunkSizePos > 0 {
		if _, err := e.w.Seek(int64(e.pcmChunkSizePos), 0); err != nil {
			return err
		}
		chunksize := uint32((int(e.BitDepth) / 8) * int(e.NumChans) * e.frames)
		if err := e.AddLE(uint32(chunksize)); err != nil {
			return fmt.Errorf("%w when writing wav data chunk size header", err)
		}
	}

	// jump back to the end of the file.
	if _, err := e.w.Seek(0, 2); err != nil {
		return err
	}
	switch e.w.(type) {
	case *os.File:
		return e.w.(*os.File).Sync()
	}
	return nil
}

// closeRF64 finalizes a container opened with AutoRF64: either ordinary
// RIFF with real 32-bit lengths, or an in-place RF64 promotion where the
// reserved JUNK area becomes ds64 and the audio bytes are never copied.
func (e *Encoder) closeRF64() error {
	// total bytes written sequentially up to here (metadata and padding
	// included). The RIFF size is the whole container minus 8.
	total := int64(e.WrittenBytes)
	riffSize := uint64(total - 8)
	audioSize := uint64(e.pcmBytes)
	promote := riffSize >= uint64(rf64LengthSentinel) || audioSize >= uint64(rf64LengthSentinel)

	if !promote {
		if _, err := e.w.Seek(4, 0); err != nil {
			return err
		}
		if err := e.AddLE(uint32(riffSize)); err != nil {
			return fmt.Errorf("%w when writing the total written bytes", err)
		}
		if e.pcmChunkStarted {
			if _, err := e.w.Seek(int64(e.pcmChunkSizePos), 0); err != nil {
				return err
			}
			if err := e.AddLE(uint32(audioSize)); err != nil {
				return fmt.Errorf("%w when writing wav data chunk size header", err)
			}
		}
		if _, err := e.w.Seek(0, 2); err != nil {
			return err
		}
		return e.sync()
	}

	blockAlign := e.NumChans * e.BitDepth / 8
	if blockAlign <= 0 && audioSize != 0 {
		return fmt.Errorf("can't promote to RF64 with blockAlign %d", blockAlign)
	}
	var sampleCount uint64
	if blockAlign > 0 {
		sampleCount = audioSize / uint64(blockAlign)
	}

	// Rewrite the reserved JUNK chunk as the ds64 chunk. It is exactly the
	// 28 byte fixed part with an empty table.
	if _, err := e.w.Seek(e.rf64JunkPos, 0); err != nil {
		return err
	}
	if err := e.AddBE(cidDS64); err != nil {
		return fmt.Errorf("failed to write the ds64 chunk ID - %w", err)
	}
	if err := e.AddLE(uint32(ds64FixedSize)); err != nil {
		return fmt.Errorf("failed to write the ds64 chunk size - %w", err)
	}
	if err := e.AddLE(riffSize); err != nil {
		return fmt.Errorf("failed to write the ds64 riff size - %w", err)
	}
	if err := e.AddLE(audioSize); err != nil {
		return fmt.Errorf("failed to write the ds64 data size - %w", err)
	}
	if err := e.AddLE(sampleCount); err != nil {
		return fmt.Errorf("failed to write the ds64 sample count - %w", err)
	}
	if err := e.AddLE(uint32(0)); err != nil {
		return fmt.Errorf("failed to write the ds64 table length - %w", err)
	}

	// Replace the outer RIFF ID and write the 32-bit sentinel.
	if _, err := e.w.Seek(0, 0); err != nil {
		return err
	}
	if err := e.AddBE(cidRF64); err != nil {
		return fmt.Errorf("failed to write the RF64 container ID - %w", err)
	}
	if err := e.AddLE(uint32(rf64LengthSentinel)); err != nil {
		return fmt.Errorf("failed to write the RF64 outer size - %w", err)
	}

	// The data chunk also carries the sentinel; its real length is in ds64.
	if e.pcmChunkStarted {
		if _, err := e.w.Seek(int64(e.pcmChunkSizePos), 0); err != nil {
			return err
		}
		if err := e.AddLE(uint32(rf64LengthSentinel)); err != nil {
			return fmt.Errorf("failed to write the RF64 data size - %w", err)
		}
	}

	if _, err := e.w.Seek(0, 2); err != nil {
		return err
	}
	return e.sync()
}

func (e *Encoder) sync() error {
	switch e.w.(type) {
	case *os.File:
		return e.w.(*os.File).Sync()
	}
	return nil
}
