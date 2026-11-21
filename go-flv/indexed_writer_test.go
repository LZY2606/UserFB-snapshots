package flv

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math"
	"testing"

	"github.com/yapingcat/gomedia/go-codec"
	"github.com/yapingcat/gomedia/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// In-memory controllable io.WriteSeeker
// ---------------------------------------------------------------------------

type memSeeker struct {
	data        []byte
	pos         int64
	writeLimit  int // <= 0 means every Write accepts the whole buffer
	failWriteOn int // 1-based Write call count that fails; 0 means never
	failSeekOn  int // 1-based Seek call count that fails; 0 means never
	writeCalls  int
	seekCalls   int
	failErr     error
}

func (m *memSeeker) Write(p []byte) (int, error) {
	m.writeCalls++
	if m.failWriteOn > 0 && m.writeCalls == m.failWriteOn {
		return 0, m.failErr
	}
	n := len(p)
	if m.writeLimit > 0 && m.writeLimit < n {
		n = m.writeLimit
	}
	end := m.pos + int64(n)
	if int64(len(m.data)) < end {
		grown := make([]byte, end)
		copy(grown, m.data)
		m.data = grown
	}
	copy(m.data[m.pos:end], p[:n])
	m.pos = end
	return n, nil
}

func (m *memSeeker) Seek(offset int64, whence int) (int64, error) {
	m.seekCalls++
	if m.failSeekOn > 0 && m.seekCalls == m.failSeekOn {
		return 0, m.failErr
	}
	var p int64
	switch whence {
	case 0:
		p = offset
	case 1:
		p = m.pos + offset
	case 2:
		p = int64(len(m.data)) + offset
	}
	m.pos = p
	return p, nil
}

func (m *memSeeker) ioCount() int { return m.writeCalls + m.seekCalls }

// ---------------------------------------------------------------------------
// Independent AMF0 decoder (only the markers emitted by the indexed writer)
// ---------------------------------------------------------------------------

type amfField struct {
	key string
	val amfVal
}

type amfVal struct {
	marker  byte
	num     float64
	boolean bool
	str     string
	count   uint32
	fields  []amfField // object / ecma array
	items   []amfVal   // strict array
}

func readAMF(data []byte) (amfVal, int, error) {
	if len(data) < 1 {
		return amfVal{}, 0, errors.New("amf: empty")
	}
	marker := data[0]
	off := 1
	switch marker {
	case amf0Number:
		if len(data) < 9 {
			return amfVal{}, 0, errors.New("amf: short number")
		}
		return amfVal{marker: marker, num: math.Float64frombits(binary.BigEndian.Uint64(data[1:9]))}, 9, nil
	case amf0Boolean:
		if len(data) < 2 {
			return amfVal{}, 0, errors.New("amf: short boolean")
		}
		return amfVal{marker: marker, boolean: data[1] != 0}, 2, nil
	case amf0String:
		if len(data) < 3 {
			return amfVal{}, 0, errors.New("amf: short string header")
		}
		n := int(binary.BigEndian.Uint16(data[1:3]))
		if len(data) < 3+n {
			return amfVal{}, 0, errors.New("amf: short string")
		}
		return amfVal{marker: marker, str: string(data[3 : 3+n])}, 3 + n, nil
	case amf0LongString:
		if len(data) < 5 {
			return amfVal{}, 0, errors.New("amf: short long string header")
		}
		n := int(binary.BigEndian.Uint32(data[1:5]))
		if len(data) < 5+n {
			return amfVal{}, 0, errors.New("amf: short long string")
		}
		return amfVal{marker: marker, str: string(data[5 : 5+n])}, 5 + n, nil
	case amf0Object, amf0EcmaArray:
		if marker == amf0EcmaArray {
			if len(data) < 5 {
				return amfVal{}, 0, errors.New("amf: short ecma header")
			}
			off = 5
		}
		v := amfVal{marker: marker}
		if marker == amf0EcmaArray {
			v.count = binary.BigEndian.Uint32(data[1:5])
		}
		for {
			if len(data) < off+3 {
				return amfVal{}, 0, errors.New("amf: short object key")
			}
			klen := int(binary.BigEndian.Uint16(data[off : off+2]))
			if klen == 0 && data[off+2] == amf0ObjectEnd {
				off += 3
				break
			}
			if len(data) < off+2+klen {
				return amfVal{}, 0, errors.New("amf: short object key name")
			}
			key := string(data[off+2 : off+2+klen])
			off += 2 + klen
			child, consumed, err := readAMF(data[off:])
			if err != nil {
				return amfVal{}, 0, err
			}
			off += consumed
			v.fields = append(v.fields, amfField{key: key, val: child})
		}
		return v, off, nil
	case amf0StrictArray:
		if len(data) < 5 {
			return amfVal{}, 0, errors.New("amf: short strict array header")
		}
		cnt := binary.BigEndian.Uint32(data[1:5])
		v := amfVal{marker: marker, count: cnt}
		off = 5
		for i := uint32(0); i < cnt; i++ {
			child, consumed, err := readAMF(data[off:])
			if err != nil {
				return amfVal{}, 0, err
			}
			off += consumed
			v.items = append(v.items, child)
		}
		return v, off, nil
	default:
		return amfVal{}, 0, errors.New("amf: unsupported marker")
	}
}

func (v amfVal) field(name string) (amfVal, bool) {
	for _, f := range v.fields {
		if f.key == name {
			return f.val, true
		}
	}
	return amfVal{}, false
}

// ---------------------------------------------------------------------------
// Independent FLV tag walker
// ---------------------------------------------------------------------------

type rawTag struct {
	off      int // offset of the tag header's first byte relative to the FLV header
	tagType  byte
	dataSize uint32
	ts       uint32
	tsExt    byte
	body     []byte
	prevSize uint32
}

func walkFLV(data []byte) (flags byte, tags []rawTag, err error) {
	if len(data) < 13 || data[0] != 'F' || data[1] != 'L' || data[2] != 'V' {
		return 0, nil, errors.New("flv: bad header")
	}
	flags = data[4]
	if binary.BigEndian.Uint32(data[9:13]) != 0 {
		return 0, nil, errors.New("flv: PreviousTagSize0 must be zero")
	}
	off := 13
	for off < len(data) {
		if len(data)-off < 11 {
			return 0, nil, errors.New("flv: truncated tag header")
		}
		hdr := data[off : off+11]
		t := rawTag{
			off:      off,
			tagType:  hdr[0] & 0x1F,
			dataSize: uint32(hdr[1])<<16 | uint32(hdr[2])<<8 | uint32(hdr[3]),
			ts:       uint32(hdr[4])<<16 | uint32(hdr[5])<<8 | uint32(hdr[6]),
			tsExt:    hdr[7],
		}
		end := off + 11 + int(t.dataSize)
		if len(data) < end+4 {
			return 0, nil, errors.New("flv: truncated tag body")
		}
		t.body = data[off+11 : end]
		t.prevSize = binary.BigEndian.Uint32(data[end : end+4])
		if t.prevSize != 11+t.dataSize {
			return 0, nil, errors.New("flv: bad PreviousTagSize")
		}
		tags = append(tags, t)
		off = end + 4
	}
	return flags, tags, nil
}

func signed24(b []byte) int32 {
	v := int32(b[0])<<16 | int32(b[1])<<8 | int32(b[2])
	if v&0x00800000 != 0 {
		v -= 0x01000000
	}
	return v
}

// ---------------------------------------------------------------------------
// Test media builders
// ---------------------------------------------------------------------------

// Frame builders return fresh Annex-B slices on every call. The underlying
// muxer rewrites 4-byte start codes into AVCC length prefixes in place, so a
// shared buffer reused across sessions would arrive pre-mutated.
func frameSPS() []byte  { return []byte{0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1e, 0xfb, 0xc8} }
func framePPS() []byte  { return []byte{0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80} }
func frameIDR1() []byte { return []byte{0, 0, 0, 1, 0x65, 0xb8, 0x80} }
func frameIDR2() []byte { return []byte{0, 0, 0, 1, 0x65, 0xb8, 0x90} }
func frameIDR3() []byte { return []byte{0, 0, 0, 1, 0x65, 0xb8, 0xa0} }
func frameP() []byte    { return []byte{0, 0, 0, 1, 0x41, 0xff, 0xff, 0xff} }
func frameP2() []byte   { return []byte{0, 0, 0, 1, 0x41, 0xee, 0xee, 0xee} }

func concatNALUs(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

func makeADTS(payload []byte) []byte {
	asc := &codec.AudioSpecificConfiguration{
		Audio_object_type:     2, // LC
		Sample_freq_index:     4, // 44100
		Channel_configuration: 2, // stereo
	}
	hdr, err := codec.ConvertASCToADTS(asc.Encode(), 7+len(payload))
	if err != nil {
		panic(err)
	}
	return append(hdr.Encode(), payload...)
}

func scriptRegionSize(maxKeyframes uint32) int {
	return int(FLVTAG_SIZE) + indexedScriptBaseBodySize + indexedScriptPerKeyframe*int(maxKeyframes) + 4
}

// ---------------------------------------------------------------------------
// TestIndexedFlvLayout
// ---------------------------------------------------------------------------

func TestIndexedFlvLayout(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const base = 123
		const maxKF uint32 = 16
		ws := &memSeeker{}
		ws.data = make([]byte, base)
		ws.pos = base

		w, err := CreateIndexedFlvWriter(ws, maxKF)
		c.Check(err == nil, "create indexed writer: %v", err)
		c.Check(w != nil, "writer returned")

		c.Check(w.WriteFlvHeader() == nil, "write header")
		region := scriptRegionSize(maxKF)
		firstMediaOff := base + 13 + region

		// Parameter sets alone: sequence header tag, no index entry.
		c.Check(w.WriteH264(concatNALUs(frameSPS(), framePPS()), 0, 0) == nil, "params only")
		// Positive composition offset keyframe.
		c.Check(w.WriteH264(frameIDR1(), 140, 100) == nil, "idr 1")
		c.Check(w.WriteH264(frameP(), 180, 140) == nil, "p 1")
		// Equal DTS keyframe: both must be retained in output order.
		c.Check(w.WriteH264(frameIDR2(), 140, 140) == nil, "idr 2 equal dts")
		// Negative composition offset (PTS < DTS), distinct NAL bytes.
		c.Check(w.WriteH264(frameIDR3(), 120, 180) == nil, "idr 3 negative cts")
		// AAC: first call emits sequence header + raw frame; DTS stays monotonic.
		c.Check(w.WriteAAC(makeADTS([]byte{0xDE, 0xAD}), 200, 200) == nil, "aac 1")
		c.Check(w.WriteAAC(makeADTS([]byte{0xBE, 0xEF}), 300, 300) == nil, "aac 2")
		// Timestamp that does not fit in 24 bits.
		bigDts := uint32(0x01000010)
		c.Check(w.WriteH264(frameP2(), bigDts+40, bigDts) == nil, "p 2 big timestamp")

		c.Check(w.Finalize() == nil, "finalize")

		raw := ws.data
		c.Check(len(raw) == int(ws.pos), "cursor at end after finalize")
		flags, tags, err := walkFLV(raw[base:])
		c.Check(err == nil, "independent flv walk: %v", err)
		c.Check(flags == 0x05, "audio+video flags")
		c.Check(len(tags) == 10, "10 tags (1 script, 6 video, 3 audio), got %d", len(tags))

		// Script tag layout.
		script := tags[0]
		c.Check(script.tagType == uint8(SCRIPT_TAG), "first tag is script")
		c.Check(script.off == 13, "script tag right after PreviousTagSize0")
		c.Check(int(script.dataSize) == indexedScriptBaseBodySize+indexedScriptPerKeyframe*int(maxKF), "fixed script DataSize")
		c.Check(script.ts == 0 && script.tsExt == 0, "script timestamp zero")

		// First media position must not have moved when backfilling.
		firstVideo := tags[1]
		c.Check(base+firstVideo.off == firstMediaOff, "first media tag position stable")

		// Independent AMF0 decode of the script body.
		nameVal, n, err := readAMF(script.body)
		c.Check(err == nil, "decode onMetaData name: %v", err)
		c.Check(nameVal.marker == amf0String && nameVal.str == "onMetaData", "AMF0 String onMetaData")
		meta, m, err := readAMF(script.body[n:])
		c.Check(err == nil, "decode ecma array: %v", err)
		c.Check(meta.marker == amf0EcmaArray, "ECMA Array marker")
		c.Check(meta.count == 6, "ecma count is 6 entries")
		c.Check(n+m == len(script.body), "AMF fills the whole script body")

		duration, ok := meta.field("duration")
		c.Check(ok && duration.marker == amf0Number, "duration number present")
		c.Check(duration.num == float64(bigDts+40)/1000.0, "duration uses max PTS (%.3f)", duration.num)
		c.Check(duration.num != float64(bigDts)/1000.0, "duration is not derived from DTS")

		filesize, ok := meta.field("filesize")
		c.Check(ok && filesize.marker == amf0Number, "filesize number present")
		c.Check(int(filesize.num) == len(raw)-base, "filesize spans FLV header to end")

		hasVideo, ok := meta.field("hasVideo")
		c.Check(ok && hasVideo.marker == amf0Boolean && hasVideo.boolean, "hasVideo true")
		hasAudio, ok := meta.field("hasAudio")
		c.Check(ok && hasAudio.marker == amf0Boolean && hasAudio.boolean, "hasAudio true")

		keyframes, ok := meta.field("keyframes")
		c.Check(ok && keyframes.marker == amf0Object, "keyframes object present")
		times, ok := keyframes.field("times")
		c.Check(ok && times.marker == amf0StrictArray, "times strict array")
		positions, ok := keyframes.field("filepositions")
		c.Check(ok && positions.marker == amf0StrictArray, "filepositions strict array")
		c.Check(times.count == positions.count && len(times.items) == len(positions.items), "equal length arrays")
		c.Check(times.count == 3, "three keyframes indexed, got %d", times.count)

		wantTimes := []float64{0.1, 0.14, 0.18}
		wantDts := []uint32{100, 140, 180}
		for i := range wantTimes {
			c.Check(times.items[i].marker == amf0Number, "times element is Number")
			c.Check(positions.items[i].marker == amf0Number, "filepositions element is Number")
			c.Check(times.items[i].num == wantTimes[i], "time %d = %.2f", i, times.items[i].num)

			pos := int64(positions.items[i].num)
			// Position is relative to this FLV header, not the file zero.
			c.Check(pos < int64(len(raw)-base), "position inside flv region")
			abs := int64(base) + pos
			c.Check(raw[abs] == uint8(VIDEO_TAG), "position lands on video tag header byte")
			tagHdr := raw[abs : abs+11]
			ds := uint32(tagHdr[1])<<16 | uint32(tagHdr[2])<<8 | uint32(tagHdr[3])
			body := raw[abs+11 : abs+11+int64(ds)]
			c.Check(body[0]>>4 == uint8(KEY_FRAME), "key frame marker")
			c.Check(body[1] == AVC_NALU, "AVC NALU packet (not sequence header)")
			c.Check(bytes.Contains(body, []byte{0x65}), "IDR NALU reachable via index")
			tagTs := uint32(tagHdr[7])<<24 | uint32(tagHdr[4])<<16 | uint32(tagHdr[5])<<8 | uint32(tagHdr[6])
			c.Check(tagTs == wantDts[i], "indexed tag DTS matches")
		}
		// No index entry points at the sequence header.
		seqTag := tags[1]
		c.Check(seqTag.body[1] == AVC_SEQUENCE_HEADER, "tag 1 is AVC sequence header")
		for _, p := range positions.items {
			c.Check(int64(p.num) != int64(seqTag.off), "sequence header not indexed")
		}

		// _padding Long String fills the remainder exactly.
		padding, ok := meta.field("_padding")
		c.Check(ok && padding.marker == amf0LongString, "_padding is AMF0 Long String")
		paddingBytes := 0
		if ok {
			paddingBytes = len(padding.str)
		}
		c.Check(paddingBytes > 0, "padding carries bytes")

		// Tag-level spot checks: negative CTS, >24 bit timestamp, audio tags.
		var negCtsTag, bigTsTag rawTag
		videoCount, audioCount := 0, 0
		for _, tg := range tags {
			switch tg.tagType {
			case uint8(VIDEO_TAG):
				videoCount++
				if tg.body[1] == AVC_NALU && signed24(tg.body[2:5]) == -60 {
					negCtsTag = tg
				}
				if tg.tsExt == 0x01 && tg.ts == 0x10 {
					bigTsTag = tg
				}
			case uint8(AUDIO_TAG):
				audioCount++
			}
		}
		c.Check(videoCount == 6, "six video tags (1 seq + 5 media)")
		c.Check(audioCount == 3, "three audio tags (1 seq + 2 raw)")
		c.Check(negCtsTag.tagType == uint8(VIDEO_TAG), "negative composition offset tag found")
		c.Check(bigTsTag.tagType == uint8(VIDEO_TAG), "extended timestamp tag found")
		fullBigTs := uint32(bigTsTag.tsExt)<<24 | bigTsTag.ts
		c.Check(fullBigTs == bigDts, "full 32-bit timestamp reconstructed")

		// FlvReader round trip over the complete file.
		reader := CreateFlvReader()
		vFrames, aFrames := 0, 0
		reader.OnFrame = func(cid codec.CodecID, frame []byte, pts, dts uint32) {
			switch cid {
			case codec.CODECID_VIDEO_H264:
				vFrames++
			case codec.CODECID_AUDIO_AAC:
				aFrames++
			}
		}
		c.Check(reader.Input(append([]byte{}, raw[base:]...)) == nil, "FlvReader accepts finalized file")
		c.Check(vFrames == 5, "five H.264 media frames read back, got %d", vFrames)
		c.Check(aFrames == 2, "two AAC frames read back, got %d", aFrames)

		// Config + media produced by a single WriteH264 call.
		combined := &memSeeker{}
		cw, err := CreateIndexedFlvWriter(combined, 4)
		c.Check(err == nil, "create combined writer")
		c.Check(cw.WriteFlvHeader() == nil, "combined header")
		c.Check(cw.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 140, 100) == nil, "config+media one call")
		c.Check(cw.WriteH264(frameP(), 180, 140) == nil, "p after combined")
		c.Check(cw.Finalize() == nil, "combined finalize")
		_, ctags, err := walkFLV(combined.data)
		c.Check(err == nil, "walk combined")
		c.Check(len(ctags) == 4, "script+seq+key+inter tags, got %d", len(ctags))
		c.Check(ctags[1].body[1] == AVC_SEQUENCE_HEADER, "combined emits seq header")
		c.Check(ctags[2].body[1] == AVC_NALU && ctags[2].body[0]>>4 == uint8(KEY_FRAME), "combined emits key media")
		_, cmeta := decodeScript(c, ctags[0].body)
		cKF, _ := cmeta.field("keyframes")
		cTimes, _ := cKF.field("times")
		c.Check(cTimes.count == 1, "only media keyframe indexed, seq header excluded")

		// Empty recording.
		empty := &memSeeker{}
		ew, err := CreateIndexedFlvWriter(empty, 2)
		c.Check(err == nil, "create empty writer")
		c.Check(ew.WriteFlvHeader() == nil && ew.Finalize() == nil, "empty header+finalize")
		_, etags, err := walkFLV(empty.data)
		c.Check(err == nil && len(etags) == 1 && etags[0].tagType == uint8(SCRIPT_TAG), "empty file has script only")
		_, emeta := decodeScript(c, etags[0].body)
		ed, _ := emeta.field("duration")
		c.Check(ed.num == 0, "empty duration is zero")
		ef, _ := emeta.field("filesize")
		c.Check(int(ef.num) == len(empty.data), "empty filesize is header+script region")
		ev, _ := emeta.field("hasVideo")
		ea, _ := emeta.field("hasAudio")
		c.Check(!ev.boolean && !ea.boolean, "empty has no tracks")
		eKF, _ := emeta.field("keyframes")
		eTimes, _ := eKF.field("times")
		ePos, _ := eKF.field("filepositions")
		c.Check(eTimes.count == 0 && ePos.count == 0, "empty index arrays")
		emptyReader := CreateFlvReader()
		emptyFrames := 0
		emptyReader.OnFrame = func(codec.CodecID, []byte, uint32, uint32) { emptyFrames++ }
		c.Check(emptyReader.Input(append([]byte{}, empty.data...)) == nil, "reader accepts empty recording")
		c.Check(emptyFrames == 0, "no frames in empty recording")

		// Audio-only recording.
		audioOnly := &memSeeker{}
		aw, err := CreateIndexedFlvWriter(audioOnly, 2)
		c.Check(err == nil, "create audio-only writer")
		c.Check(aw.WriteFlvHeader() == nil, "audio-only header")
		c.Check(aw.WriteAAC(makeADTS([]byte{1, 2, 3}), 100, 100) == nil, "audio-only aac")
		c.Check(aw.Finalize() == nil, "audio-only finalize")
		_, atags, err := walkFLV(audioOnly.data)
		c.Check(err == nil && len(atags) == 3, "script+audio seq+audio raw")
		_, ameta := decodeScript(c, atags[0].body)
		av, _ := ameta.field("hasVideo")
		aa, _ := ameta.field("hasAudio")
		c.Check(!av.boolean && aa.boolean, "audio-only flags")
		aKF, _ := ameta.field("keyframes")
		aTimes, _ := aKF.field("times")
		c.Check(aTimes.count == 0, "no keyframes without video")
		ar := CreateFlvReader()
		arCount := 0
		ar.OnFrame = func(cid codec.CodecID, frame []byte, pts, dts uint32) {
			c.Check(cid == codec.CODECID_AUDIO_AAC, "roundtrip codec is AAC")
			arCount++
		}
		c.Check(ar.Input(append([]byte{}, audioOnly.data...)) == nil, "reader accepts audio-only")
		c.Check(arCount == 1, "one raw AAC frame roundtripped")

		// Legacy writer byte comparison: same media calls, no script region.
		legacy := &memSeeker{}
		lw := CreateFlvWriter(legacy)
		c.Check(lw.WriteFlvHeader() == nil, "legacy header")
		c.Check(lw.WriteH264(concatNALUs(frameSPS(), framePPS()), 0, 0) == nil, "legacy params")
		c.Check(lw.WriteH264(frameIDR1(), 140, 100) == nil, "legacy idr")
		c.Check(lw.WriteAAC(makeADTS([]byte{0xDE, 0xAD}), 100, 100) == nil, "legacy aac")

		idxMedia := &memSeeker{}
		iw, err := CreateIndexedFlvWriter(idxMedia, 4)
		c.Check(err == nil, "create comparison writer")
		c.Check(iw.WriteFlvHeader() == nil, "comparison header")
		cmpRegion := scriptRegionSize(4)
		c.Check(iw.WriteH264(concatNALUs(frameSPS(), framePPS()), 0, 0) == nil, "indexed params")
		c.Check(iw.WriteH264(frameIDR1(), 140, 100) == nil, "indexed idr")
		c.Check(iw.WriteAAC(makeADTS([]byte{0xDE, 0xAD}), 100, 100) == nil, "indexed aac")
		c.Check(iw.Finalize() == nil, "comparison finalize")
		c.Check(bytes.Equal(legacy.data[:13], idxMedia.data[:13]), "FLV header bytes identical")
		c.Check(bytes.Equal(legacy.data[13:], idxMedia.data[13+cmpRegion:]), "media+prevsize bytes identical apart from script region")
	})
}

func decodeScript(c *taskcheck.C, body []byte) (amfVal, amfVal) {
	name, n, err := readAMF(body)
	c.Check(err == nil, "decode script name: %v", err)
	meta, _, err := readAMF(body[n:])
	c.Check(err == nil, "decode script meta: %v", err)
	return name, meta
}

// ---------------------------------------------------------------------------
// TestIndexedFlvTransactions
// ---------------------------------------------------------------------------

func TestIndexedFlvTransactions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ws := &memSeeker{}
		w, err := CreateIndexedFlvWriter(ws, 1)
		c.Check(err == nil, "capacity-1 writer")
		c.Check(w.WriteFlvHeader() == nil, "header")
		region := scriptRegionSize(1)

		// First call: sequence header + key media, fills the only slot.
		c.Check(w.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 100, 100) == nil, "first keyframe accepted")
		lenAfterFirst := len(ws.data)
		c.Check(lenAfterFirst > 13+region, "first call emitted tags")

		// Second keyframe exceeds capacity: whole call rejected, no bytes.
		err = w.WriteH264(frameIDR2(), 140, 140)
		c.Check(err != nil, "over-capacity keyframe rejected")
		c.Check(len(ws.data) == lenAfterFirst, "rejected call wrote no bytes")
		c.Check(ws.pos == int64(lenAfterFirst), "cursor unchanged after rejection")

		// A legal non-key frame afterwards still commits.
		c.Check(w.WriteH264(frameP(), 180, 140) == nil, "inter frame after rejection accepted")
		lenAfterP := len(ws.data)
		c.Check(lenAfterP > lenAfterFirst, "inter frame emitted")
		_, tags, err := walkFLV(ws.data)
		c.Check(err == nil, "walk after recovery")
		lastVideo := tags[len(tags)-1]
		c.Check(lastVideo.body[1] == AVC_NALU, "recovery tag is media")
		c.Check(bytes.Contains(lastVideo.body, []byte{0x41}), "recovery tag carries the new P NALU")
		c.Check(!bytes.Contains(lastVideo.body, []byte{0x65}), "rejected IDR content did not leak into P tag")

		// Audio is unaffected by keyframe capacity.
		c.Check(w.WriteAAC(makeADTS([]byte{1, 2}), 140, 140) == nil, "audio after rejection accepted")

		// DTS must not decrease.
		lenBeforeBack := len(ws.data)
		err = w.WriteH264(frameP(), 100, 120)
		c.Check(err != nil, "DTS regression rejected")
		c.Check(len(ws.data) == lenBeforeBack, "DTS regression wrote no bytes")
		// Equal DTS is allowed.
		c.Check(w.WriteH264(frameP(), 200, 140) == nil, "equal DTS accepted")
		// uint32 wrap-around is treated as regression.
		c.Check(w.WriteH264(frameP(), 0xFFFFFFFF, 0xFFFFFFFF) == nil, "high DTS accepted")
		lenBeforeWrap := len(ws.data)
		err = w.WriteAAC(makeADTS([]byte{3}), 0, 0)
		c.Check(err != nil, "DTS wrap rejected")
		c.Check(len(ws.data) == lenBeforeWrap && ws.pos == int64(lenBeforeWrap), "wrap rejection left output intact")
		c.Check(w.WriteAAC(makeADTS([]byte{3}), 0xFFFFFFFF, 0xFFFFFFFF) == nil, "same high DTS accepted again")
		c.Check(w.Finalize() == nil, "finalize after recovered rejections")
		_, ftags, err := walkFLV(ws.data)
		c.Check(err == nil, "walk finalized")
		_, meta := decodeScript(c, ftags[0].body)
		kf, _ := meta.field("keyframes")
		times, _ := kf.field("times")
		positions, _ := kf.field("filepositions")
		c.Check(times.count == 1 && positions.count == 1, "only the first keyframe is indexed")
		c.Check(times.items[0].num == 0.1, "indexed time is the first keyframe DTS")
		hv, _ := meta.field("hasVideo")
		ha, _ := meta.field("hasAudio")
		c.Check(hv.boolean && ha.boolean, "both tracks present after rejected calls")

		// Rejected config+media call must not leave NALs in the muxer cache.
		ws2 := &memSeeker{}
		w2, err := CreateIndexedFlvWriter(ws2, 1)
		c.Check(err == nil, "second transaction writer")
		c.Check(w2.WriteFlvHeader() == nil, "header 2")
		c.Check(w2.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 0, 0) == nil, "keyframe slot filled")
		mark := len(ws2.data)
		err = w2.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR2()), 40, 40)
		c.Check(err != nil, "second config+media call rejected")
		c.Check(len(ws2.data) == mark, "rejected config+media emitted nothing")
		c.Check(w2.WriteH264(frameP(), 80, 40) == nil, "later inter frame accepted")
		_, tags2, err := walkFLV(ws2.data)
		c.Check(err == nil, "walk writer 2")
		recovery := tags2[len(tags2)-1]
		c.Check(!bytes.Contains(recovery.body, []byte{0x65}), "no IDR NALU leaked from rejected batch")
		c.Check(bytes.Contains(recovery.body, []byte{0x41}), "recovery batch only has the P NALU")
		c.Check(w2.Finalize() == nil, "finalize writer 2")

		// Over-capacity rejection must not consume the AAC sequence header flow.
		ws3 := &memSeeker{}
		w3, err := CreateIndexedFlvWriter(ws3, 1)
		c.Check(err == nil, "third transaction writer")
		c.Check(w3.WriteFlvHeader() == nil, "header 3")
		c.Check(w3.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 0, 0) == nil, "slot filled 3")
		err = w3.WriteH264(frameIDR2(), 40, 40)
		c.Check(err != nil, "extra key rejected 3")
		c.Check(w3.WriteAAC(makeADTS([]byte{9}), 40, 40) == nil, "first AAC still emits sequence header+raw")
		_, tags3, err := walkFLV(ws3.data)
		c.Check(err == nil, "walk writer 3")
		audioSeq, audioRaw := 0, 0
		for _, tg := range tags3 {
			if tg.tagType == uint8(AUDIO_TAG) {
				if tg.body[1] == AAC_SEQUENCE_HEADER {
					audioSeq++
				} else {
					audioRaw++
				}
			}
		}
		c.Check(audioSeq == 1 && audioRaw == 1, "AAC sequence header preserved after rejection")
	})
}

// ---------------------------------------------------------------------------
// TestIndexedFlvLifecycle
// ---------------------------------------------------------------------------

func TestIndexedFlvLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Constructor validation.
		_, err := CreateIndexedFlvWriter(&memSeeker{}, 0)
		c.Check(err != nil, "maxKeyframes 0 rejected")
		_, err = CreateIndexedFlvWriter(&memSeeker{}, 65536)
		c.Check(err != nil, "maxKeyframes 65536 rejected")
		w1, err := CreateIndexedFlvWriter(&memSeeker{}, 1)
		c.Check(err == nil && w1 != nil, "maxKeyframes 1 accepted")
		wMax, err := CreateIndexedFlvWriter(&memSeeker{}, 65535)
		c.Check(err == nil && wMax != nil, "maxKeyframes 65535 accepted")

		// Media before the header, finalize before the header.
		c.Check(w1.WriteH264(frameIDR1(), 0, 0) != nil, "media before header rejected")
		c.Check(w1.WriteAAC(makeADTS([]byte{1}), 0, 0) != nil, "audio before header rejected")
		c.Check(w1.Finalize() != nil, "finalize before header rejected")

		// Finalize on a legacy non-indexed writer is an error.
		legacy := &memSeeker{}
		lw := CreateFlvWriter(legacy)
		c.Check(lw.WriteFlvHeader() == nil, "legacy header still works")
		c.Check(lw.Finalize() != nil, "legacy Finalize returns error")
		c.Check(lw.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 0, 0) == nil, "legacy media still works")

		// Indexed mode only accepts H.264 video and AAC audio.
		codecWS := &memSeeker{}
		cw, err := CreateIndexedFlvWriter(codecWS, 2)
		c.Check(err == nil, "codec-restriction writer created")
		c.Check(cw.WriteFlvHeader() == nil, "codec-restriction header")
		c.Check(cw.WriteH265(frameIDR1(), 0, 0) != nil, "H.265 entry rejected")
		c.Check(cw.WriteG711A([]byte{1}, 0, 0) != nil, "G711A entry rejected")
		c.Check(cw.WriteG711U([]byte{1}, 0, 0) != nil, "G711U entry rejected")
		c.Check(cw.WriteMp3([]byte{1}, 0, 0) != nil, "MP3 entry rejected")
		c.Check(len(codecWS.data) == 13+scriptRegionSize(2), "rejected codec entries wrote no media")
		// Allowed entries still work.
		c.Check(cw.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 0, 0) == nil, "H.264 entry accepted")
		c.Check(cw.WriteAAC(makeADTS([]byte{1}), 0, 0) == nil, "AAC entry accepted")
		c.Check(cw.Finalize() == nil, "codec-restriction finalize")

		// The fixed reserve grows with the declared capacity, including the
		// 65535 boundary; its DataSize and first media position never move.
		boundaryWS := &memSeeker{}
		bndW, err := CreateIndexedFlvWriter(boundaryWS, 65535)
		c.Check(err == nil, "boundary writer created")
		c.Check(bndW.WriteFlvHeader() == nil, "boundary header")
		wantRegion := scriptRegionSize(65535)
		c.Check(len(boundaryWS.data) == 13+wantRegion, "boundary reserve size is %d", wantRegion)
		_, btags, err := walkFLV(boundaryWS.data)
		c.Check(err == nil, "boundary reserve parses")
		c.Check(btags[0].tagType == uint8(SCRIPT_TAG), "boundary first tag is script")
		c.Check(int(btags[0].dataSize) == indexedScriptBaseBodySize+indexedScriptPerKeyframe*65535, "boundary fixed DataSize")
		mediaMark := len(boundaryWS.data)
		c.Check(bndW.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 0, 0) == nil, "boundary media")
		c.Check(len(boundaryWS.data) > mediaMark, "boundary media appended after reserve")
		c.Check(bndW.Finalize() == nil, "boundary finalize")
		_, btags2, err := walkFLV(boundaryWS.data)
		c.Check(err == nil, "boundary output parses")
		c.Check(int(btags2[0].dataSize) == indexedScriptBaseBodySize+indexedScriptPerKeyframe*65535, "boundary DataSize unchanged after backfill")
		c.Check(btags2[1].off == btags[0].off+11+int(btags[0].dataSize)+4, "boundary first media position unchanged")

		// Header once; media rejected after finalize; double finalize is a no-op.
		ws := &memSeeker{}
		w, err := CreateIndexedFlvWriter(ws, 4)
		c.Check(err == nil, "lifecycle writer")
		c.Check(w.WriteFlvHeader() == nil, "first header")
		c.Check(w.WriteFlvHeader() != nil, "second header rejected")
		c.Check(w.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 100, 100) == nil, "media")
		c.Check(w.WriteAAC(makeADTS([]byte{1, 2}), 100, 100) == nil, "audio")
		c.Check(w.Finalize() == nil, "first finalize")
		lenFinalized := len(ws.data)
		c.Check(ws.pos == int64(lenFinalized), "cursor at file end after finalize")
		c.Check(w.WriteH264(frameP(), 200, 200) != nil, "media after finalize rejected")
		c.Check(w.WriteAAC(makeADTS([]byte{3}), 200, 200) != nil, "audio after finalize rejected")
		ioBeforeSecondFinalize := ws.ioCount()
		c.Check(w.Finalize() == nil, "second finalize is harmless")
		c.Check(len(ws.data) == lenFinalized, "second finalize wrote no bytes")
		c.Check(ws.ioCount() == ioBeforeSecondFinalize, "second finalize performed no I/O")
		c.Check(ws.pos == int64(lenFinalized), "cursor stays at file end")

		// Constructor seek failure.
		faultCtor := &memSeeker{failSeekOn: 1, failErr: errors.New("seek boom")}
		_, err = CreateIndexedFlvWriter(faultCtor, 4)
		c.Check(err != nil, "constructor surfaces seek failure")

		// Header-stage write failure: session dies, no later I/O happens.
		headerWS := &memSeeker{failWriteOn: 3, failErr: errors.New("write boom")}
		hw, err := CreateIndexedFlvWriter(headerWS, 4)
		c.Check(err == nil, "header-fault writer created")
		c.Check(hw.WriteFlvHeader() != nil, "header write failure surfaced")
		frozen := headerWS.ioCount()
		c.Check(hw.WriteFlvHeader() != nil, "header retry fails without I/O")
		c.Check(hw.WriteH264(frameIDR1(), 0, 0) != nil, "media fails after header fault")
		c.Check(hw.WriteAAC(makeADTS([]byte{1}), 0, 0) != nil, "audio fails after header fault")
		c.Check(hw.Finalize() != nil, "finalize fails after header fault")
		c.Check(headerWS.ioCount() == frozen, "no I/O after header-stage failure")

		// Media-stage write failure while splitting a multi-tag commit into
		// many short writes.
		mediaWS := &memSeeker{writeLimit: 5}
		mw, err := CreateIndexedFlvWriter(mediaWS, 4)
		c.Check(err == nil, "media-fault writer created")
		c.Check(mw.WriteFlvHeader() == nil, "media-fault header")
		mediaWS.failErr = errors.New("write boom")
		mediaWS.failWriteOn = mediaWS.writeCalls + 2
		err = mw.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 100, 100)
		c.Check(err != nil, "media write failure surfaced")
		frozen = mediaWS.ioCount()
		c.Check(mw.WriteH264(frameP(), 140, 140) != nil, "later media rejected")
		c.Check(mw.WriteAAC(makeADTS([]byte{1}), 140, 140) != nil, "later audio rejected")
		c.Check(mw.Finalize() != nil, "finalize rejected after media fault")
		c.Check(mediaWS.ioCount() == frozen, "no I/O after media-stage failure")

		// Backfill seek failure.
		backWS := &memSeeker{failErr: errors.New("seek boom")}
		bw, err := CreateIndexedFlvWriter(backWS, 4)
		c.Check(err == nil, "backfill writer created")
		c.Check(bw.WriteFlvHeader() == nil, "backfill header")
		c.Check(bw.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 100, 100) == nil, "backfill media")
		backWS.failSeekOn = backWS.seekCalls + 1
		c.Check(bw.Finalize() != nil, "backfill seek failure surfaced")
		frozen = backWS.ioCount()
		c.Check(bw.Finalize() != nil, "second finalize still fails")
		c.Check(backWS.ioCount() == frozen, "no I/O retried after backfill seek fault")

		// Backfill write failure.
		backWriteWS := &memSeeker{failErr: errors.New("write boom")}
		bww, err := CreateIndexedFlvWriter(backWriteWS, 4)
		c.Check(err == nil, "backfill-write writer created")
		c.Check(bww.WriteFlvHeader() == nil, "backfill-write header")
		c.Check(bww.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 100, 100) == nil, "backfill-write media")
		// The next Write call is the metadata backfill itself.
		backWriteWS.failWriteOn = backWriteWS.writeCalls + 1
		c.Check(bww.Finalize() != nil, "backfill write failure surfaced")
		frozen = backWriteWS.ioCount()
		c.Check(bww.Finalize() != nil, "finalize after write fault still errors")
		c.Check(backWriteWS.ioCount() == frozen, "no I/O after backfill write fault")

		// Persistent short writes never lose bytes.
		shortWS := &memSeeker{writeLimit: 3}
		sw, err := CreateIndexedFlvWriter(shortWS, 4)
		c.Check(err == nil, "short-write writer created")
		c.Check(sw.WriteFlvHeader() == nil, "short-write header")
		c.Check(sw.WriteH264(concatNALUs(frameSPS(), framePPS(), frameIDR1()), 140, 100) == nil, "short-write keyframe")
		c.Check(sw.WriteH264(frameP(), 180, 140) == nil, "short-write inter")
		c.Check(sw.WriteAAC(makeADTS([]byte{0xAB, 0xCD}), 140, 140) == nil, "short-write audio")
		c.Check(sw.Finalize() == nil, "short-write finalize")
		_, stags, err := walkFLV(shortWS.data)
		c.Check(err == nil, "short-write output parses")
		_, smeta := decodeScript(c, stags[0].body)
		skf, _ := smeta.field("keyframes")
		stimes, _ := skf.field("times")
		c.Check(stimes.count == 1, "short-write index intact")
		sr := CreateFlvReader()
		sv, sa := 0, 0
		sr.OnFrame = func(cid codec.CodecID, frame []byte, pts, dts uint32) {
			if cid == codec.CODECID_VIDEO_H264 {
				sv++
			} else if cid == codec.CODECID_AUDIO_AAC {
				sa++
			}
		}
		c.Check(sr.Input(append([]byte{}, shortWS.data...)) == nil, "short-write file roundtrips")
		c.Check(sv == 2 && sa == 1, "short-write frame counts %d/%d", sv, sa)
	})
}
