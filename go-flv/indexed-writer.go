package flv

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math"

	"github.com/yapingcat/gomedia/go-codec"
)

// AMF0 marker bytes used by the indexed writer's onMetaData tag.
const (
	amf0Number      = 0x00
	amf0Boolean     = 0x01
	amf0String      = 0x02
	amf0Object      = 0x03
	amf0EcmaArray   = 0x08
	amf0ObjectEnd   = 0x09
	amf0StrictArray = 0x0A
	amf0LongString  = 0x0C
)

const (
	indexedMinKeyframes = 1
	indexedMaxKeyframes = 65535
	// Every keyframe costs two AMF0 Number values (times + filepositions),
	// 9 bytes each. The base size covers the FLV-independent AMF overhead
	// including the _padding entry.
	indexedScriptBaseBodySize = 256
	indexedScriptPerKeyframe  = 18
)

var (
	errIndexedCapacity  = errors.New("flv: indexed writer keyframe capacity exceeded")
	errIndexedDtsBack   = errors.New("flv: indexed writer dts must not decrease or wrap around uint32")
	errIndexedLifecycle = errors.New("flv: indexed writer lifecycle violation")
	errIndexedFailed    = errors.New("flv: indexed writer is in a permanent failure state")
	errIndexedCodec     = errors.New("flv: indexed writer only supports H.264 video and AAC audio")
	errFinalizeIndex    = errors.New("flv: Finalize is only supported by indexed writers")
	errIndexedSeekPos   = errors.New("flv: seek returned an unexpected position")
)

type indexedState int

const (
	indexedStateInit indexedState = iota + 1
	indexedStateActive
	indexedStateFinalized
	indexedStateFailed
)

// keyframeEntry is one row of the onMetaData keyframes index.
type keyframeEntry struct {
	dts          uint32
	filePosition uint64 // offset of the tag header's first byte, relative to the FLV header
}

// indexedRecorder holds all indexed-mode state. A normal FlvWriter leaves
// this pointer nil and keeps its original byte-for-byte behavior.
type indexedRecorder struct {
	ws io.WriteSeeker

	state        indexedState
	maxKeyframes uint32

	baseOffset   int64 // absolute position of the FLV header
	scriptOffset int64 // absolute position of the reserved script tag header
	bodyCap      int   // fixed DataSize of the reserved script tag
	tailOffset   int64 // current end of the bytes written for this FLV

	videoMuxer *AVCMuxer
	audioMuxer *AACMuxer

	hasVideo bool
	hasAudio bool
	maxPts   uint32
	lastDts  uint32
	haveDts  bool

	keyframes []keyframeEntry
}

// preparedTag is one fully encoded tag (11-byte header + body + PreviousTagSize)
// staged for an all-or-nothing commit.
type preparedTag struct {
	data    []byte
	isMedia bool
	isKey   bool
	dts     uint32
}

// CreateIndexedFlvWriter creates an FLV writer that reserves a fixed-size
// onMetaData script tag in front of the media and fills it in Finalize with
// a keyframes index. Already written media bytes are never moved.
// maxKeyframes must be between 1 and 65535.
func CreateIndexedFlvWriter(w io.WriteSeeker, maxKeyframes uint32) (*FlvWriter, error) {
	if w == nil {
		return nil, errors.New("flv: nil writer")
	}
	if maxKeyframes < indexedMinKeyframes || maxKeyframes > indexedMaxKeyframes {
		return nil, errors.New("flv: maxKeyframes must be between 1 and 65535")
	}
	pos, err := w.Seek(0, io.SeekCurrent)
	if err != nil {
		return nil, err
	}
	bodyCap := indexedScriptBaseBodySize + indexedScriptPerKeyframe*int(maxKeyframes)
	fw := &FlvWriter{
		writer: w,
		muxer:  new(FlvMuxer),
		indexed: &indexedRecorder{
			ws:           w,
			state:        indexedStateInit,
			maxKeyframes: maxKeyframes,
			baseOffset:   pos,
			tailOffset:   pos,
			bodyCap:      bodyCap,
			keyframes:    make([]keyframeEntry, 0, maxKeyframes),
		},
	}
	return fw, nil
}

// writeRaw drains p at the current cursor without touching the session tail.
func (ix *indexedRecorder) writeRaw(p []byte) error {
	if ix.state == indexedStateFailed {
		return errIndexedFailed
	}
	for len(p) > 0 {
		n, err := ix.ws.Write(p)
		if err != nil {
			ix.state = indexedStateFailed
			return err
		}
		if n <= 0 || n > len(p) {
			ix.state = indexedStateFailed
			return io.ErrShortWrite
		}
		p = p[n:]
	}
	return nil
}

// writeFull writes the whole buffer at the session tail or puts the session
// into the permanent failure state. Short writes are retried until drained.
func (ix *indexedRecorder) writeFull(p []byte) error {
	if err := ix.writeRaw(p); err != nil {
		return err
	}
	ix.tailOffset += int64(len(p))
	return nil
}

// seekAbs seeks to an absolute position and verifies the reported offset.
func (ix *indexedRecorder) seekAbs(pos int64) (int64, error) {
	if ix.state == indexedStateFailed {
		return 0, errIndexedFailed
	}
	got, err := ix.ws.Seek(pos, io.SeekStart)
	if err != nil {
		ix.state = indexedStateFailed
		return 0, err
	}
	if got != pos {
		ix.state = indexedStateFailed
		return got, errIndexedSeekPos
	}
	return got, nil
}

// writeAt backfills p at absPos without moving the session tail, restoring
// the cursor to the file end afterwards.
func (ix *indexedRecorder) writeAt(p []byte, absPos int64) error {
	if ix.state == indexedStateFailed {
		return errIndexedFailed
	}
	tail := ix.tailOffset
	if _, err := ix.seekAbs(absPos); err != nil {
		return err
	}
	if err := ix.writeRaw(p); err != nil {
		return err
	}
	if _, err := ix.seekAbs(tail); err != nil {
		return err
	}
	return nil
}

func (ix *indexedRecorder) checkActive() error {
	switch ix.state {
	case indexedStateFailed:
		return errIndexedFailed
	case indexedStateActive:
		return nil
	default:
		return errIndexedLifecycle
	}
}

// writeIndexedFlvHeader writes the FLV header, PreviousTagSize0 and the
// fixed-size reserved script tag region.
func (ix *indexedRecorder) writeIndexedFlvHeader() error {
	if ix.state == indexedStateFailed {
		return errIndexedFailed
	}
	if ix.state != indexedStateInit {
		return errIndexedLifecycle
	}

	flvhdr := [9]byte{'F', 'L', 'V', 0x01, 0x05, 0, 0, 0, 9}
	if err := ix.writeFull(flvhdr[:]); err != nil {
		return err
	}
	var previousTagSize0 [4]byte
	if err := ix.writeFull(previousTagSize0[:]); err != nil {
		return err
	}

	ix.scriptOffset = ix.tailOffset
	reserve := make([]byte, int(FLVTAG_SIZE)+ix.bodyCap+4)
	reserve[0] = uint8(SCRIPT_TAG)
	PutUint24(reserve[1:4], uint32(ix.bodyCap))
	// timestamp and stream id stay zero; the body bytes stay zero until
	// Finalize backfills them.
	binary.BigEndian.PutUint32(reserve[int(FLVTAG_SIZE)+ix.bodyCap:], FLVTAG_SIZE+uint32(ix.bodyCap))
	if err := ix.writeFull(reserve); err != nil {
		return err
	}

	ix.state = indexedStateActive
	return nil
}

// encodeFlvTag builds the exact tag bytes the legacy FlvMuxer would produce,
// including the trailing PreviousTagSize.
func encodeFlvTag(tagType TagType, dts uint32, body []byte) []byte {
	var hdr FlvTag
	hdr.TagType = uint8(tagType)
	hdr.DataSize = uint32(len(body))
	hdr.Timestamp = dts & 0x00FFFFFF
	hdr.TimestampExtended = uint8((dts >> 24) & 0xFF)
	tag := hdr.Encode()
	tag = append(tag, body...)
	var prev [4]byte
	binary.BigEndian.PutUint32(prev[:], FLVTAG_SIZE+uint32(len(body)))
	return append(tag, prev[:]...)
}

// prepareVideo turns one WriteH264 call into a staged tag batch without
// touching the output.
func (ix *indexedRecorder) prepareVideo(data []byte, pts, dts uint32) ([]preparedTag, bool, error) {
	if ix.videoMuxer == nil {
		ix.videoMuxer = NewAVCMuxer()
	}
	// Reject before the muxer sees the data so that a refused call neither
	// consumes parameter sets nor pending NAL units.
	wouldBeKey := false
	codec.SplitFrameWithStartCode(data, func(nalu []byte) bool {
		if codec.H264NaluType(nalu) == codec.H264_NAL_I_SLICE {
			wouldBeKey = true
		}
		return true
	})
	if wouldBeKey && uint32(len(ix.keyframes))+1 > ix.maxKeyframes {
		return nil, false, errIndexedCapacity
	}

	// Own the bytes: a rejected call must never retain or mutate caller data
	// through the muxer's pending-frame cache.
	owned := make([]byte, len(data))
	copy(owned, data)
	bodies := ix.videoMuxer.Write(owned, pts, dts)
	if len(bodies) == 0 {
		return nil, false, nil
	}
	batch := make([]preparedTag, 0, len(bodies))
	keyCount := 0
	for _, body := range bodies {
		isSeq := body[1] == AVC_SEQUENCE_HEADER
		isKey := !isSeq && body[1] == AVC_NALU && body[0]>>4 == uint8(KEY_FRAME)
		if isKey {
			keyCount++
		}
		batch = append(batch, preparedTag{
			data:    encodeFlvTag(VIDEO_TAG, dts, body),
			isMedia: !isSeq,
			isKey:   isKey,
			dts:     dts,
		})
	}
	// Defensive invariant: the whole batch must fit before any byte is output.
	if uint32(len(ix.keyframes)+keyCount) > ix.maxKeyframes {
		return nil, false, errIndexedCapacity
	}
	return batch, keyCount > 0, nil
}

// prepareAudio turns one WriteAAC call into a staged tag batch.
func (ix *indexedRecorder) prepareAudio(data []byte, pts, dts uint32) ([]preparedTag, error) {
	if ix.audioMuxer == nil {
		ix.audioMuxer = NewAACMuxer()
	}
	bodies := ix.audioMuxer.Write(data, pts, dts)
	if len(bodies) == 0 {
		return nil, nil
	}
	batch := make([]preparedTag, 0, len(bodies))
	for _, body := range bodies {
		isSeq := body[1] == AAC_SEQUENCE_HEADER
		batch = append(batch, preparedTag{
			data:    encodeFlvTag(AUDIO_TAG, dts, body),
			isMedia: !isSeq,
		})
	}
	return batch, nil
}

// commit writes the staged batch as one contiguous region and only then
// updates the session/index state.
func (ix *indexedRecorder) commit(batch []preparedTag, pts, dts uint32, trackVideo bool) error {
	total := 0
	for _, t := range batch {
		total += len(t.data)
	}
	buf := make([]byte, 0, total)
	positions := make([]uint64, len(batch))
	cur := ix.tailOffset
	for i, t := range batch {
		positions[i] = uint64(cur - ix.baseOffset)
		cur += int64(len(t.data))
		buf = append(buf, t.data...)
	}
	if err := ix.writeFull(buf); err != nil {
		return err
	}

	hasMedia := false
	for i, t := range batch {
		if t.isMedia {
			hasMedia = true
			if trackVideo {
				ix.hasVideo = true
			} else {
				ix.hasAudio = true
			}
		}
		if t.isKey {
			ix.keyframes = append(ix.keyframes, keyframeEntry{dts: t.dts, filePosition: positions[i]})
		}
	}
	if hasMedia && pts > ix.maxPts {
		ix.maxPts = pts
	}
	ix.lastDts = dts
	ix.haveDts = true
	return nil
}

func (ix *indexedRecorder) checkDts(dts uint32) error {
	if ix.haveDts && dts < ix.lastDts {
		return errIndexedDtsBack
	}
	return nil
}

func (f *FlvWriter) writeIndexedH264(data []byte, pts, dts uint32) error {
	ix := f.indexed
	if err := ix.checkActive(); err != nil {
		return err
	}
	if err := ix.checkDts(dts); err != nil {
		return err
	}
	batch, _, err := ix.prepareVideo(data, pts, dts)
	if err != nil {
		return err
	}
	if len(batch) == 0 {
		ix.lastDts = dts
		ix.haveDts = true
		return nil
	}
	return ix.commit(batch, pts, dts, true)
}

func (f *FlvWriter) writeIndexedAAC(data []byte, pts, dts uint32) error {
	ix := f.indexed
	if err := ix.checkActive(); err != nil {
		return err
	}
	if err := ix.checkDts(dts); err != nil {
		return err
	}
	batch, err := ix.prepareAudio(data, pts, dts)
	if err != nil {
		return err
	}
	if len(batch) == 0 {
		ix.lastDts = dts
		ix.haveDts = true
		return nil
	}
	return ix.commit(batch, pts, dts, false)
}

// Finalize backfills the reserved onMetaData script tag. It is only valid on
// writers created with CreateIndexedFlvWriter. A second successful call does
// nothing and leaves the cursor at the file end.
func (f *FlvWriter) Finalize() error {
	if f.indexed == nil {
		return errFinalizeIndex
	}
	ix := f.indexed
	switch ix.state {
	case indexedStateFailed:
		return errIndexedFailed
	case indexedStateFinalized:
		return nil
	case indexedStateInit:
		return errIndexedLifecycle
	}

	body, err := ix.buildMetadata()
	if err != nil {
		return err
	}
	if len(body) > ix.bodyCap {
		// Cannot happen given the reserve formula; refuse rather than
		// corrupting the following media tag.
		return errors.New("flv: metadata exceeds reserved script tag size")
	}
	if err := ix.writeAt(body, ix.scriptOffset+int64(FLVTAG_SIZE)); err != nil {
		return err
	}
	ix.state = indexedStateFinalized
	return nil
}

func putAMFString(b *bytes.Buffer, s string) {
	var l [2]byte
	binary.BigEndian.PutUint16(l[:], uint16(len(s)))
	b.Write(l[:])
	b.WriteString(s)
}

func putAMFNumber(b *bytes.Buffer, v float64) {
	b.WriteByte(amf0Number)
	var buf [8]byte
	binary.BigEndian.PutUint64(buf[:], math.Float64bits(v))
	b.Write(buf[:])
}

func putAMFBoolean(b *bytes.Buffer, v bool) {
	b.WriteByte(amf0Boolean)
	if v {
		b.WriteByte(1)
	} else {
		b.WriteByte(0)
	}
}

// buildMetadata encodes AMF0 String "onMetaData" followed by an ECMA Array.
// A Long String _padding field fills the fixed script body exactly.
func (ix *indexedRecorder) buildMetadata() ([]byte, error) {
	var b bytes.Buffer
	b.Grow(ix.bodyCap)

	b.WriteByte(amf0String)
	putAMFString(&b, "onMetaData")

	b.WriteByte(amf0EcmaArray)
	var cnt [4]byte
	binary.BigEndian.PutUint32(cnt[:], 6)
	b.Write(cnt[:])

	putAMFString(&b, "duration")
	putAMFNumber(&b, float64(ix.maxPts)/1000.0)

	putAMFString(&b, "filesize")
	putAMFNumber(&b, float64(ix.tailOffset-ix.baseOffset))

	putAMFString(&b, "hasVideo")
	putAMFBoolean(&b, ix.hasVideo)

	putAMFString(&b, "hasAudio")
	putAMFBoolean(&b, ix.hasAudio)

	putAMFString(&b, "keyframes")
	b.WriteByte(amf0Object)

	putAMFString(&b, "times")
	b.WriteByte(amf0StrictArray)
	binary.BigEndian.PutUint32(cnt[:], uint32(len(ix.keyframes)))
	b.Write(cnt[:])
	for _, kf := range ix.keyframes {
		putAMFNumber(&b, float64(kf.dts)/1000.0)
	}

	putAMFString(&b, "filepositions")
	b.WriteByte(amf0StrictArray)
	binary.BigEndian.PutUint32(cnt[:], uint32(len(ix.keyframes)))
	b.Write(cnt[:])
	for _, kf := range ix.keyframes {
		putAMFNumber(&b, float64(kf.filePosition))
	}
	b.Write([]byte{0x00, 0x00, amf0ObjectEnd}) // end keyframes object

	// Fill the remainder of the fixed body with an AMF0 Long String.
	used := b.Len()
	overhead := len("_padding") + 2 + 1 + 4 + 3 // key + key length + marker + string length + ECMA object end
	remain := ix.bodyCap - used - overhead
	if remain < 0 {
		return nil, errors.New("flv: metadata exceeds reserved script tag size")
	}
	putAMFString(&b, "_padding")
	b.WriteByte(amf0LongString)
	binary.BigEndian.PutUint32(cnt[:], uint32(remain))
	b.Write(cnt[:])
	b.Write(make([]byte, remain))
	b.Write([]byte{0x00, 0x00, amf0ObjectEnd}) // end the ECMA array

	if b.Len() != ix.bodyCap {
		return nil, errors.New("flv: metadata did not fill reserved script tag size")
	}
	return b.Bytes(), nil
}
