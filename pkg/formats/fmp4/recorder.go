package fmp4

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"

	amp4 "github.com/abema/go-mp4"

	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4/seekablebuffer"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs"
)

var (
	errInvalidInit       = errors.New("invalid initialization block")
	errInvalidTrack      = errors.New("invalid track ID")
	errInvalidSample     = errors.New("invalid sample")
	errTimeline          = errors.New("non-contiguous sample timeline")
	errSequenceExhausted = errors.New("sequence numbers exhausted")
	errUnsupportedLayout = errors.New("unsupported fragment layout")
)

// RecordingFile is the storage backend of a Recorder. It is the same contract
// as an *os.File opened for reading and writing.
type RecordingFile interface {
	io.ReadWriteSeeker

	// Truncate changes the size of the file.
	Truncate(size int64) error

	// Sync commits the current contents of the file to stable storage.
	Sync() error
}

type recorderTrack struct {
	id uint32
}

type queuedSample struct {
	dts    uint64
	sample *Sample
}

// sectionReadSeeker is an io.ReadSeeker over a fixed window of another
// ReadSeeker. It never reads past the window, so media payloads stay untouched.
type sectionReadSeeker struct {
	r    io.ReadSeeker
	base int64
	off  int64
	size int64
}

func newSectionReadSeeker(r io.ReadSeeker, base, size int64) *sectionReadSeeker {
	return &sectionReadSeeker{r: r, base: base, off: 0, size: size}
}

func (s *sectionReadSeeker) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		s.off = offset

	case io.SeekCurrent:
		s.off += offset

	case io.SeekEnd:
		s.off = s.size + offset
	}

	if s.off < 0 {
		return 0, fmt.Errorf("negative position")
	}

	return s.off, nil
}

func (s *sectionReadSeeker) Read(p []byte) (int, error) {
	if s.off >= s.size {
		return 0, io.EOF
	}

	if int64(len(p)) > s.size-s.off {
		p = p[:s.size-s.off]
	}

	_, err := s.r.Seek(s.base+s.off, io.SeekStart)
	if err != nil {
		return 0, err
	}

	n, err := s.r.Read(p)
	s.off += int64(n)
	return n, err
}

// Recorder writes fragmented MP4 samples to a file and can resume recording
// into the same file after a restart. The caller owns the file lifecycle and
// must close it.
type Recorder struct {
	file      RecordingFile
	tracks    []recorderTrack
	byID      map[uint32]int
	initBytes []byte

	queues   map[uint32][]queuedSample
	endpoint map[uint32]uint64
	nextSeq  uint32

	terminal    bool
	terminalErr error
}

// OpenRecorder opens a RecordingFile for recording. If the file is empty, the
// initialization block is written to it. Otherwise the existing content is
// recovered: complete moof/mdat groups rebuild the sequence number and per-track
// timelines, while a partially written final group is discarded.
func OpenRecorder(file RecordingFile, init *Init) (*Recorder, error) {
	tracks, err := validateInit(init)
	if err != nil {
		return nil, err
	}

	var initBuf seekablebuffer.Buffer
	err = init.Marshal(&initBuf)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", errInvalidInit, err)
	}
	initBytes := append([]byte(nil), initBuf.Bytes()...)

	// remember the entry cursor so it can be restored on every failure path
	startPos, err := file.Seek(0, io.SeekCurrent)
	if err != nil {
		return nil, err
	}
	restoreCursor := func() {
		_, _ = file.Seek(startPos, io.SeekStart)
	}

	size, err := file.Seek(0, io.SeekEnd)
	if err != nil {
		restoreCursor()
		return nil, err
	}

	r := &Recorder{
		file:      file,
		tracks:    tracks,
		byID:      make(map[uint32]int),
		initBytes: initBytes,
		queues:    make(map[uint32][]queuedSample),
		endpoint:  make(map[uint32]uint64),
		nextSeq:   1,
	}
	for i, t := range tracks {
		r.byID[t.id] = i
	}

	if size == 0 {
		_, err = file.Seek(0, io.SeekStart)
		if err != nil {
			return nil, err
		}

		err = writeAll(file, initBytes)
		if err != nil {
			restoreCursor()
			return nil, err
		}

		err = file.Sync()
		if err != nil {
			restoreCursor()
			return nil, err
		}

		_, err = file.Seek(0, io.SeekEnd)
		if err != nil {
			return nil, err
		}

		return r, nil
	}

	validEnd, nextSeq, endpoints, err := r.recover(size)
	if err != nil {
		restoreCursor()
		return nil, err
	}

	r.nextSeq = nextSeq
	r.endpoint = endpoints

	_, err = file.Seek(validEnd, io.SeekStart)
	if err != nil {
		return nil, err
	}

	return r, nil
}

func validateInit(init *Init) ([]recorderTrack, error) {
	if init == nil || len(init.Tracks) == 0 || len(init.Tracks) > 2 {
		return nil, fmt.Errorf("%w: one audio track or one video plus one audio track are supported", errInvalidInit)
	}

	out := make([]recorderTrack, len(init.Tracks))
	seen := make(map[uint32]bool)
	hasVideo := false
	hasAudio := false

	for i, t := range init.Tracks {
		if t == nil {
			return nil, fmt.Errorf("%w: track %d is nil", errInvalidInit, i)
		}

		if t.ID <= 0 || uint64(t.ID) > math.MaxUint32 || seen[uint32(t.ID)] {
			return nil, fmt.Errorf("%w: track ID must be a unique non-zero uint32", errInvalidInit)
		}
		seen[uint32(t.ID)] = true

		if t.TimeScale == 0 {
			return nil, fmt.Errorf("%w: track %d has a zero timescale", errInvalidInit, t.ID)
		}

		if t.Codec == nil {
			return nil, fmt.Errorf("%w: track %d has no codec", errInvalidInit, t.ID)
		}

		// only these combinations are accepted; the codec configuration itself
		// is preserved independently through the marshaled initialization bytes
		switch t.Codec.(type) {
		case *codecs.H264:
			if hasVideo {
				return nil, fmt.Errorf("%w: multiple video tracks are not supported", errInvalidInit)
			}
			hasVideo = true

		case *codecs.MPEG4Audio:
			if hasAudio {
				return nil, fmt.Errorf("%w: multiple audio tracks are not supported", errInvalidInit)
			}
			hasAudio = true

		default:
			return nil, fmt.Errorf("%w: unsupported codec on track %d", errInvalidInit, t.ID)
		}

		out[i] = recorderTrack{id: uint32(t.ID)}
	}

	if !hasAudio || (len(out) == 2 && !hasVideo) {
		return nil, fmt.Errorf(
			"%w: supported combinations are one MPEG-4 audio track, or one H.264 plus one MPEG-4 audio track",
			errInvalidInit,
		)
	}

	return out, nil
}

// WriteSample queues a sample for the next Commit. Samples of a track must form
// a contiguous timeline starting from an arbitrary DTS.
func (r *Recorder) WriteSample(trackID int, dts uint64, sample *Sample) error {
	if r.terminal {
		return r.terminalErr
	}

	if trackID <= 0 || uint64(trackID) > math.MaxUint32 {
		return fmt.Errorf("%w: %d", errInvalidTrack, trackID)
	}
	id := uint32(trackID)

	if _, ok := r.byID[id]; !ok {
		return fmt.Errorf("%w: %d is not part of the initialization block", errInvalidTrack, trackID)
	}

	if sample == nil {
		return fmt.Errorf("%w: sample is nil", errInvalidSample)
	}
	if sample.Duration == 0 {
		return fmt.Errorf("%w: duration must be positive", errInvalidSample)
	}
	if len(sample.Payload) == 0 {
		return fmt.Errorf("%w: payload must not be empty", errInvalidSample)
	}
	if dts > math.MaxUint64-uint64(sample.Duration) {
		return fmt.Errorf("%w: dts plus duration overflows", errInvalidSample)
	}

	queue := r.queues[id]
	if len(queue) == 0 {
		if committed, ok := r.endpoint[id]; ok && dts != committed {
			return fmt.Errorf("%w: track %d expected DTS %d, got %d", errTimeline, trackID, committed, dts)
		}
	} else {
		last := queue[len(queue)-1]
		expected := last.dts + uint64(last.sample.Duration)
		if dts != expected {
			return fmt.Errorf("%w: track %d expected DTS %d, got %d", errTimeline, trackID, expected, dts)
		}
	}

	// the session keeps its own copy of every input sample
	cp := &Sample{
		Duration:        sample.Duration,
		PTSOffset:       sample.PTSOffset,
		IsNonSyncSample: sample.IsNonSyncSample,
		Payload:         append([]byte(nil), sample.Payload...),
	}
	r.queues[id] = append(queue, queuedSample{dts: dts, sample: cp})

	return nil
}

// Commit writes all queued samples as a single moof/mdat group. An empty commit
// performs no I/O and does not consume a sequence number.
func (r *Recorder) Commit() error {
	if r.terminal {
		return r.terminalErr
	}

	hasSamples := false
	for _, q := range r.queues {
		if len(q) != 0 {
			hasSamples = true
			break
		}
	}
	if !hasSamples {
		return nil
	}

	if r.nextSeq == 0 {
		return errSequenceExhausted
	}

	part := &Part{
		SequenceNumber: r.nextSeq,
		Tracks:         make([]*PartTrack, 0, len(r.tracks)),
	}

	// tracks are emitted in initialization order
	for _, t := range r.tracks {
		queue := r.queues[t.id]
		if len(queue) == 0 {
			continue
		}

		pt := &PartTrack{
			ID:       int(t.id),
			BaseTime: queue[0].dts,
			Samples:  make([]*Sample, len(queue)),
		}
		for i, q := range queue {
			pt.Samples[i] = q.sample
		}
		part.Tracks = append(part.Tracks, pt)
	}

	// all offsets are computed on the temporary buffer first, so the final
	// file only ever receives a single sequential append
	var buf seekablebuffer.Buffer
	err := part.Marshal(&buf)
	if err != nil {
		return err
	}
	data := append([]byte(nil), buf.Bytes()...)

	_, err = r.file.Seek(0, io.SeekEnd)
	if err != nil {
		r.terminate(err)
		return err
	}

	n, err := r.file.Write(data)
	if err != nil {
		r.terminate(err)
		return err
	}
	if n != len(data) {
		r.terminate(io.ErrShortWrite)
		return io.ErrShortWrite
	}

	err = r.file.Sync()
	if err != nil {
		r.terminate(err)
		return err
	}

	// publication happens only after the data is synced
	for _, t := range r.tracks {
		queue := r.queues[t.id]
		if len(queue) == 0 {
			continue
		}

		last := queue[len(queue)-1]
		r.endpoint[t.id] = last.dts + uint64(last.sample.Duration)
		r.queues[t.id] = queue[:0]
	}

	if r.nextSeq == math.MaxUint32 {
		r.nextSeq = 0
	} else {
		r.nextSeq++
	}

	return nil
}

func (r *Recorder) terminate(err error) {
	r.terminal = true
	r.terminalErr = err
}

func writeAll(w io.Writer, p []byte) error {
	for len(p) > 0 {
		n, err := w.Write(p)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		p = p[n:]
	}
	return nil
}

// recover scans the file content after the initialization block. It returns the
// offset of the valid file end, the next sequence number and the per-track
// endpoints, without ever reading sample payloads.
func (r *Recorder) recover(fileSize int64) (int64, uint32, map[uint32]uint64, error) {
	if fileSize < int64(len(r.initBytes)) {
		return 0, 0, nil, fmt.Errorf("%w: file is smaller than its initialization block", errUnsupportedLayout)
	}

	header := make([]byte, len(r.initBytes))
	_, err := io.ReadFull(newSectionReadSeeker(r.file, 0, int64(len(header))), header)
	if err != nil {
		return 0, 0, nil, err
	}
	if !bytes.Equal(header, r.initBytes) {
		return 0, 0, nil, fmt.Errorf("%w: initialization block does not match the provided Init", errInvalidInit)
	}

	pos := int64(len(r.initBytes))
	nextSeq := uint32(1)
	endpoints := make(map[uint32]uint64)

	for pos < fileSize {
		groupStart := pos

		moofInfo, err := readTopLevelBox(r.file, pos, fileSize)
		if err != nil {
			if errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, io.EOF) {
				return r.discardTail(groupStart, nextSeq, endpoints)
			}
			return 0, 0, nil, err
		}

		if moofInfo.Type != amp4.BoxTypeMoof() {
			return 0, 0, nil, fmt.Errorf("%w: expected moof at offset %d, got %v", errUnsupportedLayout, pos, moofInfo.Type)
		}

		if moofInfo.ExtendToEOF {
			// a size-zero box claims the rest of the file: the group can never be complete
			return r.discardTail(groupStart, nextSeq, endpoints)
		}

		moofEnd := groupStart + int64(moofInfo.Size)
		if moofEnd > fileSize {
			// declared moof body was not fully written
			return r.discardTail(groupStart, nextSeq, endpoints)
		}

		inner := newSectionReadSeeker(r.file, groupStart, int64(moofInfo.Size))
		innerInfo := &amp4.BoxInfo{
			Offset:     0,
			Size:       moofInfo.Size,
			HeaderSize: moofInfo.HeaderSize,
			Type:       moofInfo.Type,
		}

		frag, err := parseMoof(inner, innerInfo)
		if err != nil {
			return 0, 0, nil, fmt.Errorf("%w: %v", errUnsupportedLayout, err)
		}

		err = r.validateFragment(frag, nextSeq, endpoints)
		if err != nil {
			return 0, 0, nil, err
		}

		if moofEnd == fileSize {
			// complete moof without its mdat
			return r.discardTail(groupStart, nextSeq, endpoints)
		}

		mdatInfo, err := readTopLevelBox(r.file, moofEnd, fileSize)
		if err != nil {
			if errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, io.EOF) {
				return r.discardTail(groupStart, nextSeq, endpoints)
			}
			return 0, 0, nil, err
		}

		if mdatInfo.Type != amp4.BoxTypeMdat() {
			return 0, 0, nil, fmt.Errorf("%w: moof at offset %d is not followed by mdat", errUnsupportedLayout, groupStart)
		}

		groupEnd := moofEnd + int64(mdatInfo.Size)
		if groupEnd > fileSize || mdatInfo.ExtendToEOF {
			return r.discardTail(groupStart, nextSeq, endpoints)
		}

		err = validateSampleRanges(frag, moofInfo, mdatInfo)
		if err != nil {
			return 0, 0, nil, err
		}

		// all checks passed: publish the group state
		for _, traf := range frag.trafs {
			end := traf.baseTime
			for _, s := range traf.samples {
				end += uint64(s.Duration)
			}
			endpoints[traf.trackID] = end
		}

		if nextSeq == math.MaxUint32 {
			nextSeq = 0
		} else {
			nextSeq++
		}

		pos = groupEnd
	}

	return pos, nextSeq, endpoints, nil
}

func (r *Recorder) validateFragment(frag *parsedFragment, nextSeq uint32, endpoints map[uint32]uint64) error {
	if frag.sequence != nextSeq {
		return fmt.Errorf("%w: expected sequence number %d, got %d", errUnsupportedLayout, nextSeq, frag.sequence)
	}

	if len(frag.trafs) == 0 || len(frag.trafs) > len(r.tracks) {
		return fmt.Errorf("%w: invalid number of trafs", errUnsupportedLayout)
	}

	seen := make(map[uint32]bool)
	lastInitIndex := -1

	for _, traf := range frag.trafs {
		idx, ok := r.byID[traf.trackID]
		if !ok {
			return fmt.Errorf("%w: unknown track ID %d", errUnsupportedLayout, traf.trackID)
		}
		if seen[traf.trackID] {
			return fmt.Errorf("%w: track %d appears twice in one moof", errUnsupportedLayout, traf.trackID)
		}
		if idx <= lastInitIndex {
			return fmt.Errorf("%w: trafs are not in initialization track order", errUnsupportedLayout)
		}
		lastInitIndex = idx
		seen[traf.trackID] = true

		if len(traf.samples) == 0 {
			return fmt.Errorf("%w: traf of track %d has no samples", errUnsupportedLayout, traf.trackID)
		}

		end := traf.baseTime
		for _, s := range traf.samples {
			if s.Duration == 0 {
				return fmt.Errorf("%w: sample duration must be positive", errUnsupportedLayout)
			}
			if s.Size == 0 {
				return fmt.Errorf("%w: sample size must be positive", errUnsupportedLayout)
			}
			if end > math.MaxUint64-uint64(s.Duration) {
				return fmt.Errorf("%w: decode time overflows on track %d", errUnsupportedLayout, traf.trackID)
			}
			end += uint64(s.Duration)
		}

		// continuity also applies to the first group of a track, whose base
		// time must equal the endpoint committed by earlier groups
		if committed, ok := endpoints[traf.trackID]; ok && traf.baseTime != committed {
			return fmt.Errorf("%w: track %d has a non-contiguous decode time", errUnsupportedLayout, traf.trackID)
		}
	}

	return nil
}

func validateSampleRanges(frag *parsedFragment, moofInfo, mdatInfo *amp4.BoxInfo) error {
	var cumul uint64

	for _, traf := range frag.trafs {
		expected := int64(moofInfo.Size) + int64(mdatInfo.HeaderSize) + int64(cumul)
		if int64(traf.dataOffset) != expected {
			return fmt.Errorf("%w: sample range of track %d is not closed inside its mdat", errUnsupportedLayout, traf.trackID)
		}

		for _, s := range traf.samples {
			if cumul > math.MaxUint64-uint64(s.Size) {
				return fmt.Errorf("%w: sample range overflows", errUnsupportedLayout)
			}
			cumul += uint64(s.Size)
		}
	}

	mdatPayload := mdatInfo.Size - mdatInfo.HeaderSize
	if cumul != mdatPayload {
		return fmt.Errorf("%w: sample ranges do not precisely close the mdat payload", errUnsupportedLayout)
	}

	return nil
}

// discardTail truncates the file at the start of an incomplete final group and
// returns the recovered state of the complete groups that precede it.
func (r *Recorder) discardTail(
	groupStart int64,
	nextSeq uint32,
	endpoints map[uint32]uint64,
) (int64, uint32, map[uint32]uint64, error) {
	err := r.file.Truncate(groupStart)
	if err != nil {
		return 0, 0, nil, err
	}

	err = r.file.Sync()
	if err != nil {
		return 0, 0, nil, err
	}

	return groupStart, nextSeq, endpoints, nil
}

// readTopLevelBox reads a single box header located at offset. It returns
// io.ErrUnexpectedEOF when the header is truncated or the declared box extends
// past the file end. Media payloads are never read.
func readTopLevelBox(file io.ReadSeeker, offset, fileSize int64) (*amp4.BoxInfo, error) {
	remaining := fileSize - offset
	if remaining < amp4.SmallHeaderSize {
		return nil, io.ErrUnexpectedEOF
	}

	sr := newSectionReadSeeker(file, offset, remaining)
	bi, err := amp4.ReadBoxInfo(sr)
	if err != nil {
		if errors.Is(err, io.EOF) {
			return nil, io.ErrUnexpectedEOF
		}
		return nil, err
	}

	if offset+int64(bi.Size) > fileSize {
		return nil, io.ErrUnexpectedEOF
	}

	return bi, nil
}
