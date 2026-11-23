package oggvorbis

import (
	"errors"
	"io"

	"github.com/jfreymuth/vorbis"
)

// A Reader can read audio from an ogg/vorbis file, including a chained ogg
// stream made of several independently encoded vorbis logical streams.
//
// All positions and lengths are expressed in global frame coordinates across
// the whole chain: frame zero is the first frame of the first segment, and
// each following segment starts right after the previous one's last valid
// frame. Coordinates count frames, never interleaved float values.
type Reader struct {
	seeker io.ReadSeeker
	idx    *chainIndex

	r  oggReader
	ps packetStream

	dec vorbis.Decoder

	position       int64
	buffer         []float32
	originalBuffer []float32
	decodeBuf      []float32
	toSkip         int

	length int64
	atEnd  bool

	// File-level metadata is fixed by the first segment and must not change
	// when playback (or a seek) moves the decoder into another segment.
	fileBitrate vorbis.Bitrate
	fileComment vorbis.CommentHeader

	// switchPending is set when the last decoded packet ended a segment. The
	// actual decoder switch (and its preroll) is performed only after the
	// current buffer has been drained, so a preroll can never overwrite
	// samples that have not been handed to the caller yet.
	switchPending bool

	// global offset of the current segment's local frame zero, and the number
	// of frames the current decoder has produced (before tail trimming)
	segStart    int64
	segProduced int64
}

// NewReader creates a new Reader.
// Some of the returned reader's methods will only work if in also implements
// io.ReadSeeker.
//
// When in is an io.ReadSeeker, the whole container is scanned once up front:
// pages are parsed and CRC-checked, and an index of page/segment metadata is
// built. No audio is decoded ahead and no compressed page content is kept, so
// the index grows with the number of pages, segments and header packets, not
// with the duration of the audio.
func NewReader(in io.Reader) (*Reader, error) {
	r := new(Reader)
	r.seeker, _ = in.(io.ReadSeeker)

	if r.seeker != nil {
		idx, err := buildChainIndex(r.seeker)
		if err != nil {
			return nil, err
		}
		if err := validateChainFormats(idx); err != nil {
			return nil, err
		}
		r.idx = idx
		r.length = idx.length
		if _, err := r.seeker.Seek(0, io.SeekStart); err != nil {
			return nil, err
		}
	}

	r.r = oggReader{source: in}
	r.ps = &sequentialStream{r: &r.r}

	if err := r.startFirstSegment(); err != nil {
		return nil, err
	}
	return r, nil
}

// validateChainFormats makes sure every segment shares the first segment's
// sample rate and channel count. Resampling and channel conversion are not
// supported.
func validateChainFormats(idx *chainIndex) error {
	var first vorbis.Decoder
	if err := first.ReadHeader(idx.segments[0].id); err != nil {
		return err
	}
	for i := 1; i < len(idx.segments); i++ {
		var d vorbis.Decoder
		if err := d.ReadHeader(idx.segments[i].id); err != nil {
			return err
		}
		if d.SampleRate() != first.SampleRate() || d.Channels() != first.Channels() {
			return errors.New("oggvorbis: chained stream has inconsistent sample rate or channel count")
		}
	}
	return nil
}

// startFirstSegment reads the first segment's three headers and performs the
// initial preroll.
func (r *Reader) startFirstSegment() error {
	for i := 0; i < 3; i++ {
		packet, err := r.ps.NextPacket()
		if err != nil {
			return noEOF(err)
		}
		if err := r.dec.ReadHeader(packet); err != nil {
			return err
		}
	}
	r.originalBuffer = make([]float32, r.dec.BufferSize())
	// Snapshot the file-level metadata from the first segment; later segment
	// switches replace the decoder but never these values.
	r.fileBitrate = r.dec.Bitrate
	r.fileComment = r.dec.CommentHeader

	// Read the first audio packet, because the data might not start at
	// position zero, see:
	// https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-132000A.2
	if err := r.prerollSegment(); err != nil {
		return err
	}
	return nil
}

// prerollSegment decodes the new segment's first audio packet and aligns the
// position with the first page's granule when that granule is already known.
// The decoded samples stay in r.buffer and are drained by Read before any
// further packet is decoded, so a preroll triggered at a chain boundary can
// never reorder the output.
func (r *Reader) prerollSegment() error {
	if err := r.fillBuffer(); err != nil {
		return err
	}
	if gran, known, consumed := r.ps.pageState(); consumed && known {
		offset := gran - r.segProduced
		if offset < 0 {
			drop := int(-offset) * r.Channels()
			if drop > len(r.buffer) {
				drop = len(r.buffer)
			}
			r.buffer = r.buffer[drop:]
		}
		r.segProduced = gran
		r.position = r.segStart + gran
	}
	return nil
}

// SampleRate returns the sample rate of the vorbis stream.
// For chained files this is determined by the first segment and does not
// change when playback moves to another segment.
func (r *Reader) SampleRate() int { return r.dec.SampleRate() }

// Channels returns the number of channels of the vorbis stream.
// For chained files this is determined by the first segment.
func (r *Reader) Channels() int { return r.dec.Channels() }

// Bitrate returns a struct containing information about the bitrate.
// For chained files the information reflects the first segment and does not
// change when playback moves to another segment.
func (r *Reader) Bitrate() vorbis.Bitrate { return r.fileBitrate }

// CommentHeader returns a struct containing info from the comment header.
// For chained files the information reflects the first segment and does not
// change when playback moves to another segment.
func (r *Reader) CommentHeader() vorbis.CommentHeader { return r.fileComment }

// Position returns the current position in frames. It is always the global
// index (across every segment) of the next frame that will be returned; page
// readahead, decoder buffering and per-segment granule resets never make it
// move backwards.
func (r *Reader) Position() int64 {
	return r.position - int64(len(r.buffer)/r.Channels()) + int64(r.toSkip/r.Channels())
}

// Length returns the length of the audio data in frames, summed over every
// segment after per-segment end trimming.
// A return value of zero means the length is not confirmed yet. For a
// non-seekable input that happens before the physical end of file has been
// reached; seekable inputs are scanned completely by NewReader.
func (r *Reader) Length() int64 { return r.length }

// SetPosition seeks to a global frame position.
// It returns an error if the underlying reader is not seekable, if pos is
// negative, or if the indexed container data is malformed. A position at or
// past the end moves the reader to the end; the following Read then returns
// io.EOF.
func (r *Reader) SetPosition(pos int64) error {
	if r.seeker == nil {
		return errors.New("oggvorbis: reader is not seekable")
	}
	if pos < 0 {
		return errors.New("oggvorbis: negative position")
	}
	r.buffer = nil
	r.toSkip = 0
	r.atEnd = false
	r.switchPending = false
	if pos >= r.length {
		r.position = r.length
		r.atEnd = true
		return nil
	}
	start, err := r.prepareSeek(pos)
	if err != nil {
		return err
	}
	r.position = start
	r.toSkip = int(pos-start) * r.Channels()
	return nil
}

// Read reads and decodes audio data and stores the result in p.
//
// It returns the number of values decoded (number of frames * channels) and
// any error encountered, similarly to an io.Reader's Read method. A single
// call may cross a chain boundary; it may also return the samples decoded so
// far and report a boundary error on the next call.
//
// The number of values produced will always be a multiple of Channels().
func (r *Reader) Read(p []float32) (int, error) {
	if r.atEnd && len(r.buffer) == 0 {
		return 0, io.EOF
	}
	if len(p) == 0 {
		return 0, nil
	}
	if len(p)%r.Channels() != 0 {
		p = p[:len(p)/r.Channels()*r.Channels()]
	}
	out := p
	total := 0
	err := error(nil)
	if r.toSkip > 0 {
		err = r.skip()
		if err != nil {
			return 0, err
		}
	}
	for len(p) > 0 {
		// Always drain buffered samples (including a chain preroll) before
		// decoding another packet or switching decoders.
		if len(r.buffer) > 0 {
			n := copy(p, r.buffer)
			r.buffer = r.buffer[n:]
			total += n
			p = p[n:]
			continue
		}
		if r.switchPending {
			// The previous packet ended a segment; switch decoders now that
			// every buffered sample of the old segment has been delivered.
			r.switchPending = false
			if err = r.advanceSegment(); err != nil {
				break
			}
			continue
		}
		if r.atEnd {
			err = io.EOF
			break
		}
		if len(p) >= len(r.originalBuffer) {
			var n int
			n, err = r.read(p)
			total += n
			p = p[n:]
			if err != nil {
				break
			}
			if n == 0 && !r.switchPending {
				// A packet can decode to zero samples (e.g. the first packet
				// after Clear); decode the next one into the buffer.
				if err = r.fillBuffer(); err != nil {
					break
				}
			}
			continue
		}
		if err = r.fillBuffer(); err != nil {
			break
		}
	}
	for i := range out[:total] {
		if out[i] > 1 {
			out[i] = 1
		} else if out[i] < -1 {
			out[i] = -1
		}
	}
	return total, err
}

func (r *Reader) fillBuffer() error {
	n, err := r.read(r.originalBuffer)
	r.buffer = r.originalBuffer[:n]
	if err != nil {
		return err
	}
	if n == 0 && !r.switchPending {
		return r.fillBuffer()
	}
	return nil
}

// read decodes one vorbis packet, applies end-of-segment trimming and marks
// the reader for a decoder switch when the packet just decoded ended a
// segment. The switch itself is deferred so buffered samples are delivered
// first.
func (r *Reader) read(p []float32) (int, error) {
	packet, err := r.ps.NextPacket()
	if err != nil {
		return 0, err
	}
	out, err := r.dec.DecodeInto(packet, p)
	n := len(out)
	ch := r.Channels()
	r.position += int64(n / ch)
	r.segProduced += int64(n / ch)
	if err != nil {
		return n, err
	}
	if r.ps.SegmentEnd() {
		gran, known, _ := r.ps.pageState()
		if !known {
			return n, errNoGranule
		}
		if extra := r.segProduced - gran; extra > 0 {
			trimmed := n - int(extra)*ch
			if trimmed < 0 {
				trimmed = 0
			}
			n = trimmed
		}
		r.segProduced = gran
		r.position = r.segStart + gran
		r.switchPending = true
	}
	return n, nil
}

// advanceSegment closes the current segment and initializes the decoder for
// the next one, if any. The next segment must use the same sample rate and
// channel count as the first.
func (r *Reader) advanceSegment() error {
	id, comment, setup, err := r.ps.NextSegment()
	if err == io.EOF {
		r.length = r.position
		r.atEnd = true
		return nil
	}
	if err != nil {
		return err
	}
	var dec vorbis.Decoder
	if err := readThreeHeaders(&dec, id, comment, setup); err != nil {
		return err
	}
	if dec.SampleRate() != r.SampleRate() || dec.Channels() != r.Channels() {
		return errors.New("oggvorbis: chained stream has inconsistent sample rate or channel count")
	}
	r.segStart = r.position
	r.segProduced = 0
	r.dec = dec
	r.originalBuffer = make([]float32, dec.BufferSize())
	dec.Clear()
	return r.prerollSegment()
}

func (r *Reader) skip() error {
	for {
		if r.switchPending && len(r.buffer) == 0 {
			r.switchPending = false
			if err := r.advanceSegment(); err != nil {
				return err
			}
		}
		err := r.fillBuffer()
		if len(r.buffer) > r.toSkip {
			r.buffer = r.buffer[r.toSkip:]
			r.toSkip = 0
			return err
		}
		r.toSkip -= len(r.buffer)
		r.buffer = nil
		if err != nil {
			return err
		}
	}
}
