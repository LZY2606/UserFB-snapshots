package fmp4_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"strings"
	"testing"

	amp4 "github.com/abema/go-mp4"

	"github.com/bluenviron/mediacommon/v2/pkg/codecs/mpeg4audio"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs"
	"github.com/bluenviron/mediacommon/v2/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// in-memory RecordingFile
// ---------------------------------------------------------------------------

type memFile struct {
	data []byte
	pos  int64

	// reads intersecting these absolute [start,end) ranges fail
	forbidden [][2]int64

	maxWrite      int
	writeErr      error
	writeErrAfter int // number of successful Write calls before writeErr starts
	writeCount    int
	syncErr       error
	syncErrAfter  int // number of successful Sync calls before syncErr starts
	syncCount     int
	truncateErr   error
	seekErr       error

	ops []string
}

func (f *memFile) Read(p []byte) (int, error) {
	end := f.pos + int64(len(p))
	for _, r := range f.forbidden {
		if f.pos < r[1] && end > r[0] {
			return 0, fmt.Errorf("read of media payload at %d is forbidden", f.pos)
		}
	}

	if f.pos >= int64(len(f.data)) {
		return 0, io.EOF
	}

	n := copy(p, f.data[f.pos:])
	f.pos += int64(n)
	return n, nil
}

func (f *memFile) Write(p []byte) (int, error) {
	f.ops = append(f.ops, fmt.Sprintf("write@%d:%d", f.pos, len(p)))

	if f.writeErr != nil {
		if f.writeCount < f.writeErrAfter {
			f.writeCount++
		} else {
			return 0, f.writeErr
		}
	}

	n := len(p)
	if f.maxWrite > 0 && n > f.maxWrite {
		n = f.maxWrite // short write without an error, like a broken file can do
	}

	end := f.pos + int64(n)
	if end > int64(len(f.data)) {
		grow := make([]byte, end-int64(len(f.data)))
		f.data = append(f.data, grow...)
	}
	copy(f.data[f.pos:end], p[:n])
	f.pos = end
	return n, nil
}

func (f *memFile) Seek(offset int64, whence int) (int64, error) {
	if f.seekErr != nil {
		return 0, f.seekErr
	}

	switch whence {
	case io.SeekStart:
		f.pos = offset
	case io.SeekCurrent:
		f.pos += offset
	case io.SeekEnd:
		f.pos = int64(len(f.data)) + offset
	}

	if f.pos < 0 {
		return 0, errors.New("negative position")
	}

	f.ops = append(f.ops, fmt.Sprintf("seek@%d", f.pos))
	return f.pos, nil
}

func (f *memFile) Truncate(size int64) error {
	f.ops = append(f.ops, fmt.Sprintf("truncate@%d", size))
	if f.truncateErr != nil {
		return f.truncateErr
	}

	if size < int64(len(f.data)) {
		f.data = append([]byte(nil), f.data[:size]...)
	} else {
		f.data = append(f.data, make([]byte, size-int64(len(f.data)))...)
	}
	if f.pos > size {
		f.pos = size
	}
	return nil
}

func (f *memFile) Sync() error {
	f.ops = append(f.ops, "sync")
	if f.syncErr != nil {
		if f.syncCount < f.syncErrAfter {
			f.syncCount++
			return nil
		}
		return f.syncErr
	}
	return nil
}

func (f *memFile) snapshot() []byte {
	return append([]byte(nil), f.data...)
}

func (f *memFile) opCount() int {
	return len(f.ops)
}

// ---------------------------------------------------------------------------
// independent box parsing helpers (no Recorder internals)
// ---------------------------------------------------------------------------

type rawBox struct {
	offset  uint64
	size    uint64
	hdrSize uint64
	typ     [4]byte
}

func readRawBoxAt(data []byte, offset int) (rawBox, bool) {
	if len(data)-offset < 8 {
		return rawBox{}, false
	}

	size32 := binary.BigEndian.Uint32(data[offset : offset+4])
	var typ [4]byte
	copy(typ[:], data[offset+4:offset+8])

	if size32 == 1 {
		if len(data)-offset < 16 {
			return rawBox{}, false
		}
		return rawBox{
			offset:  uint64(offset),
			size:    binary.BigEndian.Uint64(data[offset+8 : offset+16]),
			hdrSize: 16,
			typ:     typ,
		}, true
	}

	return rawBox{
		offset:  uint64(offset),
		size:    uint64(size32),
		hdrSize: 8,
		typ:     typ,
	}, true
}

type verifySample struct {
	duration        uint32
	ptsOffset       int32
	isNonSyncSample bool
	payload         []byte
}

type verifyTraf struct {
	id         uint32
	baseTime   uint64
	dataOffset int32
	samples    []verifySample
}

type verifyGroup struct {
	moof  rawBox
	mdat  rawBox
	seq   uint32
	trafs []verifyTraf
}

// firstMoofOffset returns the absolute offset of the first moof box, i.e. the
// size of the ftyp+moov initialization block.
func firstMoofOffset(t *testing.T, data []byte) int {
	t.Helper()

	pos := 0
	for pos < len(data) {
		box, ok := readRawBoxAt(data, pos)
		if !ok {
			t.Fatalf("truncated top-level box at %d", pos)
		}
		if string(box.typ[:]) == "moof" {
			return pos
		}
		pos += int(box.size)
	}
	t.Fatal("no moof found")
	return 0
}

// trafStart returns the absolute offset of the traf box with the given index
// inside a moof.
func trafStart(data []byte, g verifyGroup, trafIdx int) int {
	pos := int(g.moof.offset) + int(g.moof.hdrSize)
	mfhd, ok := readRawBoxAt(data, pos)
	if !ok {
		panic("mfhd not found")
	}
	pos += int(mfhd.size)

	for i := 0; i < trafIdx; i++ {
		box, ok := readRawBoxAt(data, pos)
		if !ok {
			panic("traf not found")
		}
		pos += int(box.size)
	}
	return pos
}

func mfhdSequenceOffset(g verifyGroup) int {
	// mfhd is the first box of the moof payload; 4-byte full-box header then seq
	return int(g.moof.offset) + int(g.moof.hdrSize) + 8 + 4
}

func tfhdTrackIDOffset(data []byte, g verifyGroup, trafIdx int) int {
	// traf header (8) + tfhd header (8) + full-box header (4)
	return trafStart(data, g, trafIdx) + 8 + 8 + 4
}

// parseGroups independently walks moof/mdat groups starting at offset.
func parseGroups(t *testing.T, data []byte, offset int) []verifyGroup {
	t.Helper()

	var groups []verifyGroup
	pos := offset

	for pos < len(data) {
		moof, ok := readRawBoxAt(data, pos)
		if !ok {
			t.Fatalf("truncated moof header at %d", pos)
		}
		if string(moof.typ[:]) != "moof" {
			t.Fatalf("expected moof at %d, got %q", pos, moof.typ)
		}

		mdatPos := pos + int(moof.size)
		mdat, ok := readRawBoxAt(data, mdatPos)
		if !ok {
			t.Fatalf("truncated mdat header at %d", mdatPos)
		}
		if string(mdat.typ[:]) != "mdat" {
			t.Fatalf("expected mdat at %d, got %q", mdatPos, mdat.typ)
		}

		g := verifyGroup{moof: moof, mdat: mdat}
		moofBytes := data[pos : pos+int(moof.size)]

		var cur *verifyTraf
		_, err := amp4.ReadBoxStructure(bytes.NewReader(moofBytes), func(h *amp4.ReadHandle) (any, error) {
			switch h.BoxInfo.Type.String() {
			case "moof", "traf":
				if h.BoxInfo.Type.String() == "traf" {
					g.trafs = append(g.trafs, verifyTraf{})
					cur = &g.trafs[len(g.trafs)-1]
				}
				return h.Expand()

			case "mfhd":
				box, _, err := h.ReadPayload()
				if err != nil {
					return nil, err
				}
				g.seq = box.(*amp4.Mfhd).SequenceNumber

			case "tfhd":
				box, _, err := h.ReadPayload()
				if err != nil {
					return nil, err
				}
				cur.id = box.(*amp4.Tfhd).TrackID

			case "tfdt":
				box, _, err := h.ReadPayload()
				if err != nil {
					return nil, err
				}
				cur.baseTime = box.(*amp4.Tfdt).GetBaseMediaDecodeTime()

			case "trun":
				box, _, err := h.ReadPayload()
				if err != nil {
					return nil, err
				}
				trun := box.(*amp4.Trun)
				cur.dataOffset = trun.DataOffset

				payloadPos := int(moof.offset) + int(trun.DataOffset)
				for i, e := range trun.Entries {
					size := int(e.SampleSize)
					if payloadPos+size > len(data) {
						return nil, fmt.Errorf("sample %d runs past the file", i)
					}
					cur.samples = append(cur.samples, verifySample{
						duration:        e.SampleDuration,
						ptsOffset:       int32(trun.GetSampleCompositionTimeOffset(i)),
						isNonSyncSample: e.SampleFlags&(1<<16) != 0,
						payload:         append([]byte(nil), data[payloadPos:payloadPos+size]...),
					})
					payloadPos += size
				}
			}

			return nil, nil
		})
		if err != nil {
			t.Fatalf("independent moof parse failed: %v", err)
		}

		groups = append(groups, g)
		pos += int(moof.size) + int(mdat.size)
	}

	return groups
}

// ---------------------------------------------------------------------------
// test fixtures
// ---------------------------------------------------------------------------

func dualTrackInit() *fmp4.Init {
	return &fmp4.Init{
		Tracks: []*fmp4.InitTrack{
			{
				ID:        1,
				TimeScale: 90000,
				Codec: &codecs.H264{
					SPS: []byte{
						0x67, 0x42, 0xc0, 0x28, 0xd9, 0x00, 0x78, 0x02,
						0x27, 0xe5, 0x84, 0x00, 0x00, 0x03, 0x00, 0x04,
						0x00, 0x00, 0x03, 0x00, 0xf0, 0x3c, 0x60, 0xc9,
						0x20,
					},
					PPS: []byte{0x08},
				},
			},
			{
				ID:        2,
				TimeScale: 48000,
				Codec: &codecs.MPEG4Audio{
					Config: mpeg4audio.AudioSpecificConfig{
						Type:          2,
						SampleRate:    48000,
						ChannelConfig: 2,
					},
				},
			},
		},
	}
}

func audioOnlyInit() *fmp4.Init {
	return &fmp4.Init{
		Tracks: []*fmp4.InitTrack{
			{
				ID:        7,
				TimeScale: 44100,
				Codec: &codecs.MPEG4Audio{
					Config: mpeg4audio.AudioSpecificConfig{
						Type:          2,
						SampleRate:    44100,
						ChannelConfig: 2,
					},
				},
			},
		},
	}
}

func payload(tag byte, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = tag + byte(i)
	}
	return b
}

// buildHistory writes three groups and returns the file plus group boundaries.
func buildHistory(t *testing.T, file *memFile, init *fmp4.Init) (int, []int) {
	t.Helper()

	rec, err := fmp4.OpenRecorder(file, init)
	if err != nil {
		t.Fatalf("OpenRecorder: %v", err)
	}

	videoPayload1 := payload(0x10, 6)
	videoPayload2 := payload(0x20, 4)
	audioPayload1 := payload(0x30, 2)

	err = rec.WriteSample(1, 1000, &fmp4.Sample{Duration: 3000, Payload: videoPayload1})
	if err != nil {
		t.Fatalf("WriteSample v1: %v", err)
	}
	err = rec.WriteSample(1, 4000, &fmp4.Sample{
		Duration:        3000,
		PTSOffset:       -500,
		IsNonSyncSample: true,
		Payload:         videoPayload2,
	})
	if err != nil {
		t.Fatalf("WriteSample v2: %v", err)
	}
	err = rec.WriteSample(2, 0, &fmp4.Sample{Duration: 1024, Payload: audioPayload1})
	if err != nil {
		t.Fatalf("WriteSample a1: %v", err)
	}
	if err := rec.Commit(); err != nil {
		t.Fatalf("Commit 1: %v", err)
	}

	// single-sample commit, audio track absent: its endpoint must not advance
	err = rec.WriteSample(1, 7000, &fmp4.Sample{Duration: 3000, Payload: payload(0x40, 3)})
	if err != nil {
		t.Fatalf("WriteSample v3: %v", err)
	}
	if err := rec.Commit(); err != nil {
		t.Fatalf("Commit 2: %v", err)
	}

	boundaries := []int{len(file.data)} // start of group 3

	err = rec.WriteSample(1, 10000, &fmp4.Sample{Duration: 3000, Payload: payload(0x50, 5)})
	if err != nil {
		t.Fatalf("WriteSample v4: %v", err)
	}
	err = rec.WriteSample(2, 1024, &fmp4.Sample{Duration: 1024, Payload: payload(0x60, 7)})
	if err != nil {
		t.Fatalf("WriteSample a2: %v", err)
	}
	if err := rec.Commit(); err != nil {
		t.Fatalf("Commit 3: %v", err)
	}

	// locate every moof boundary independently
	var groupStarts []int
	pos := 0
	// skip ftyp + moov: first moof is found by scanning box types
	for pos < len(file.data) {
		box, ok := readRawBoxAt(file.data, pos)
		if !ok {
			t.Fatalf("cannot read box at %d", pos)
		}
		if string(box.typ[:]) == "moof" {
			groupStarts = append(groupStarts, pos)
			mdat, ok := readRawBoxAt(file.data, pos+int(box.size))
			if !ok {
				t.Fatalf("cannot read mdat at %d", pos+int(box.size))
			}
			pos += int(box.size) + int(mdat.size)
		} else {
			pos += int(box.size)
		}
	}

	if len(groupStarts) != 3 {
		t.Fatalf("expected 3 groups, got %d", len(groupStarts))
	}

	return groupStarts[0], append(boundaries[:0], groupStarts...)
}

// ---------------------------------------------------------------------------
// TestRecorderRecovery
// ---------------------------------------------------------------------------

func TestRecorderRecovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		initData := dualTrackInit()

		// -- build a three-group history -------------------------------------
		var file memFile
		_, groupStarts := buildHistory(t, &file, initData)
		fullBytes := file.snapshot()
		initLen := groupStarts[0]

		groups := parseGroups(t, fullBytes, initLen)
		c.Check(len(groups) == 3, "expected 3 parsed groups, got %d", len(groups))

		// sequence numbers start at 1 and increase
		for i, g := range groups {
			c.Check(g.seq == uint32(i+1), "group %d sequence = %d", i, g.seq)
		}

		// group 1: two video samples (one negative CTS, one non-sync) + audio
		g1 := groups[0]
		c.Check(len(g1.trafs) == 2, "group 1 has 2 trafs, got %d", len(g1.trafs))
		c.Check(g1.trafs[0].id == 1 && g1.trafs[1].id == 2, "traf order follows Init")
		c.Check(g1.trafs[0].baseTime == 1000, "video tfdt = %d", g1.trafs[0].baseTime)
		c.Check(g1.trafs[1].baseTime == 0, "audio tfdt = %d", g1.trafs[1].baseTime)
		c.Check(len(g1.trafs[0].samples) == 2, "group 1 video samples")
		s0 := g1.trafs[0].samples[0]
		c.Check(s0.duration == 3000 && s0.ptsOffset == 0 && !s0.isNonSyncSample, "video sample 0 fields")
		c.Check(bytes.Equal(s0.payload, payload(0x10, 6)), "video sample 0 payload")
		s1 := g1.trafs[0].samples[1]
		c.Check(s1.duration == 3000 && s1.ptsOffset == -500 && s1.isNonSyncSample, "video sample 1 fields")
		c.Check(bytes.Equal(s1.payload, payload(0x20, 4)), "video sample 1 payload")
		c.Check(len(g1.trafs[1].samples) == 1 && g1.trafs[1].samples[0].duration == 1024, "audio sample")
		c.Check(bytes.Equal(g1.trafs[1].samples[0].payload, payload(0x30, 2)), "audio payload")

		// group 2: single video sample, no audio traf
		g2 := groups[1]
		c.Check(len(g2.trafs) == 1 && g2.trafs[0].id == 1, "group 2 only carries video")
		c.Check(g2.trafs[0].baseTime == 7000, "group 2 tfdt = %d", g2.trafs[0].baseTime)
		c.Check(len(g2.trafs[0].samples) == 1, "group 2 single sample")
		c.Check(bytes.Equal(g2.trafs[0].samples[0].payload, payload(0x40, 3)), "group 2 payload")

		// group 3: both tracks, audio continues from its old endpoint
		g3 := groups[2]
		c.Check(len(g3.trafs) == 2, "group 3 has 2 trafs")
		c.Check(g3.trafs[0].baseTime == 10000, "group 3 video tfdt = %d", g3.trafs[0].baseTime)
		c.Check(g3.trafs[1].baseTime == 1024, "group 3 audio tfdt = %d", g3.trafs[1].baseTime)
		c.Check(bytes.Equal(g3.trafs[0].samples[0].payload, payload(0x50, 5)), "group 3 video payload")
		c.Check(bytes.Equal(g3.trafs[1].samples[0].payload, payload(0x60, 7)), "group 3 audio payload")

		// every trun data offset points inside its own mdat and ranges close it
		for _, g := range groups {
			mdatPayload := int(g.mdat.size - g.mdat.hdrSize)
			total := 0
			for _, tr := range g.trafs {
				absolute := int(g.moof.offset) + int(tr.dataOffset)
				c.Check(absolute >= int(g.mdat.offset)+int(g.mdat.hdrSize), "data offset inside mdat payload")
				for _, smp := range tr.samples {
					total += len(smp.payload)
				}
			}
			c.Check(total == mdatPayload, "sample sizes close mdat exactly: %d vs %d", total, mdatPayload)
		}

		// -- DTS above 32 bits survives tfdt v1 and a reopen ------------------
		var bigFile memFile
		bigRec, err := fmp4.OpenRecorder(&bigFile, initData)
		c.Check(err == nil, "open big-DTS recorder: %v", err)
		bigDTS := uint64(math.MaxUint32) + 9000
		err = bigRec.WriteSample(1, bigDTS, &fmp4.Sample{Duration: 3000, Payload: payload(0x70, 4)})
		c.Check(err == nil, "write big-DTS sample: %v", err)
		c.Check(bigRec.Commit() == nil, "commit big-DTS group")
		bigInitLen := firstMoofOffset(t, bigFile.data)
		bigGroups := parseGroups(t, bigFile.data, bigInitLen)
		c.Check(bigGroups[0].trafs[0].baseTime == bigDTS, "tfdt holds >32-bit DTS %d", bigGroups[0].trafs[0].baseTime)

		bigRec2, err := fmp4.OpenRecorder(&bigFile, initData)
		c.Check(err == nil, "reopen big-DTS file: %v", err)
		err = bigRec2.WriteSample(1, bigDTS+3000, &fmp4.Sample{Duration: 3000, Payload: payload(0x71, 4)})
		c.Check(err == nil, "continue big-DTS timeline: %v", err)
		c.Check(bigRec2.Commit() == nil, "commit second big-DTS group")
		bigGroups = parseGroups(t, bigFile.data, bigInitLen)
		c.Check(len(bigGroups) == 2 && bigGroups[1].trafs[0].baseTime == bigDTS+3000, "big-DTS continuation")

		// -- byte-by-byte truncation of the last group ------------------------
		lastStart := groupStarts[2]
		cuts := []int{
			lastStart,      // nothing of group 3 remains (clean boundary)
			lastStart + 1,  // single moof header byte
			lastStart + 4,  // half box header
			lastStart + 7,  // header minus one byte
			lastStart + 8,  // full moof header, no body
			lastStart + 12, // partial mfhd
		}

		moof3 := groups[2].moof
		mdat3 := groups[2].mdat
		cuts = append(cuts,
			lastStart+int(moof3.size)-1,                 // moof one byte short
			lastStart+int(moof3.size),                   // complete moof, mdat missing
			lastStart+int(moof3.size)+1,                 // one mdat header byte
			lastStart+int(moof3.size)+7,                 // mdat header minus one
			lastStart+int(moof3.size)+8,                 // full mdat header, no payload
			lastStart+int(moof3.size)+int(mdat3.size)-1, // mdat one payload byte short
			len(fullBytes),                              // full file, nothing to discard
		)

		stablePrefix := fullBytes[:lastStart]

		for _, cut := range cuts {
			var f memFile
			f.data = append([]byte(nil), fullBytes[:cut]...)

			rec, err := fmp4.OpenRecorder(&f, initData)
			c.Check(err == nil, "reopen at cut %d: %v", cut, err)
			if err != nil {
				continue
			}

			if cut == len(fullBytes) {
				c.Check(len(f.data) == len(fullBytes), "full reopen keeps file size")
				c.Check(f.pos == int64(len(fullBytes)), "cursor at file end")
			} else {
				c.Check(len(f.data) == lastStart, "cut %d truncated to group start %d, got %d", cut, lastStart, len(f.data))
				c.Check(bytes.Equal(f.data, stablePrefix), "cut %d kept every earlier byte", cut)
				c.Check(f.pos == int64(lastStart), "cut %d cursor at valid end", cut)
				if cut != lastStart {
					c.Check(hasOp(f.ops, "truncate"), "cut %d performed a truncate", cut)
					c.Check(hasOp(f.ops, "sync"), "cut %d synced after truncate", cut)
				}

				// recording continues with sequence number 3 and the old endpoints
				err = rec.WriteSample(1, 10000, &fmp4.Sample{Duration: 3000, Payload: payload(0x80, 2)})
				c.Check(err == nil, "cut %d resume video: %v", cut, err)
				err = rec.WriteSample(2, 1024, &fmp4.Sample{Duration: 1024, Payload: payload(0x81, 2)})
				c.Check(err == nil, "cut %d resume audio: %v", cut, err)
				c.Check(rec.Commit() == nil, "cut %d resume commit", cut)

				repaired := parseGroups(t, f.data, initLen)
				c.Check(len(repaired) == 3, "cut %d rebuilt three groups", cut)
				c.Check(repaired[2].seq == 3, "cut %d new group uses sequence 3, got %d", cut, repaired[2].seq)
				c.Check(repaired[2].trafs[0].baseTime == 10000, "cut %d video tfdt restored", cut)
				c.Check(repaired[2].trafs[1].baseTime == 1024, "cut %d audio tfdt restored", cut)
			}
		}

		// truncating inside an earlier group also discards exactly that group onward
		midCut := groupStarts[1] + 5
		var f memFile
		f.data = append([]byte(nil), fullBytes[:midCut]...)
		rec, err := fmp4.OpenRecorder(&f, initData)
		c.Check(err == nil, "reopen inside group 2: %v", err)
		c.Check(len(f.data) == groupStarts[1], "truncated at group 2 start")
		c.Check(bytes.Equal(f.data, fullBytes[:groupStarts[1]]), "group 1 bytes untouched")

		// -- two full reopen rounds, sequence numbers keep growing -------------
		var roundFile memFile
		roundFile.data = append([]byte(nil), fullBytes...)

		for round := 1; round <= 2; round++ {
			rec, err := fmp4.OpenRecorder(&roundFile, initData)
			c.Check(err == nil, "reopen round %d: %v", round, err)
			c.Check(roundFile.pos == int64(len(roundFile.data)), "round %d cursor at end", round)

			err = rec.WriteSample(1, uint64(13000+(round-1)*3000), &fmp4.Sample{
				Duration: 3000,
				Payload:  payload(byte(0x90+round), 3),
			})
			c.Check(err == nil, "round %d video: %v", round, err)
			c.Check(rec.Commit() == nil, "round %d commit", round)
		}

		finalGroups := parseGroups(t, roundFile.data, initLen)
		c.Check(len(finalGroups) == 5, "five groups after two rounds, got %d", len(finalGroups))
		for i, g := range finalGroups {
			c.Check(g.seq == uint32(i+1), "round group %d sequence = %d", i, g.seq)
		}
		c.Check(finalGroups[3].trafs[0].baseTime == 13000, "round 1 tfdt")
		c.Check(finalGroups[4].trafs[0].baseTime == 16000, "round 2 tfdt")
		c.Check(finalGroups[4].seq == 5, "last sequence is 5")

		// -- recovery never reads media payloads -------------------------------
		var guarded memFile
		guarded.data = append([]byte(nil), fullBytes...)
		for _, g := range groups {
			payloadStart := int(g.mdat.offset) + int(g.mdat.hdrSize)
			payloadEnd := int(g.mdat.offset) + int(g.mdat.size)
			guarded.forbidden = append(guarded.forbidden, [2]int64{int64(payloadStart), int64(payloadEnd)})
		}

		rec, err = fmp4.OpenRecorder(&guarded, initData)
		c.Check(err == nil, "recovery with payload reads forbidden: %v", err)
		c.Check(guarded.pos == int64(len(fullBytes)), "guarded reopen cursor at end")
		err = rec.WriteSample(1, 13000, &fmp4.Sample{Duration: 3000, Payload: payload(0xa0, 2)})
		c.Check(err == nil, "guarded write: %v", err)
		c.Check(rec.Commit() == nil, "guarded commit")
		c.Check(hasOp(guarded.ops, "sync"), "guarded commit synced")

		// -- normal committed parts are readable by the production parser ------
		var parts fmp4.Parts
		err = parts.Unmarshal(roundFile.data[initLen:])
		c.Check(err == nil, "Parts.Unmarshal: %v", err)
		c.Check(len(parts) == 5, "unmarshaled 5 parts, got %d", len(parts))
		c.Check(parts[0].SequenceNumber == 1, "first part sequence")
		c.Check(parts[4].SequenceNumber == 5, "last part sequence")
		c.Check(len(parts[0].Tracks) == 2, "first part tracks")
		c.Check(parts[0].Tracks[0].BaseTime == 1000, "part video base time")
		c.Check(parts[0].Tracks[1].BaseTime == 0, "part audio base time")
		c.Check(parts[0].Tracks[0].Samples[1].PTSOffset == -500, "negative CTS preserved")
		c.Check(parts[0].Tracks[0].Samples[1].IsNonSyncSample, "non-sync flag preserved")
		c.Check(bytes.Equal(parts[0].Tracks[0].Samples[0].Payload, payload(0x10, 6)), "unmarshaled payload")
	})
}

func hasOp(ops []string, prefix string) bool {
	for _, op := range ops {
		if strings.HasPrefix(op, prefix) {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// TestRecorderTimeline
// ---------------------------------------------------------------------------

func TestRecorderTimeline(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// -- Init validation ---------------------------------------------------
		good := dualTrackInit()

		badCases := []struct {
			name string
			init *fmp4.Init
		}{
			{"nil", nil},
			{"no tracks", &fmp4.Init{}},
			{"three tracks", &fmp4.Init{Tracks: []*fmp4.InitTrack{
				{ID: 1, TimeScale: 1, Codec: &codecs.H264{}},
				{ID: 2, TimeScale: 1, Codec: &codecs.MPEG4Audio{}},
				{ID: 3, TimeScale: 1, Codec: &codecs.MPEG4Audio{}},
			}}},
			{"zero ID", &fmp4.Init{Tracks: []*fmp4.InitTrack{
				{ID: 0, TimeScale: 90000, Codec: &codecs.H264{}},
				{ID: 2, TimeScale: 48000, Codec: &codecs.MPEG4Audio{}},
			}}},
			{"duplicate ID", &fmp4.Init{Tracks: []*fmp4.InitTrack{
				{ID: 1, TimeScale: 90000, Codec: &codecs.H264{}},
				{ID: 1, TimeScale: 48000, Codec: &codecs.MPEG4Audio{}},
			}}},
			{"zero timescale", &fmp4.Init{Tracks: []*fmp4.InitTrack{
				{ID: 1, TimeScale: 0, Codec: &codecs.H264{}},
			}}},
			{"unsupported codec", &fmp4.Init{Tracks: []*fmp4.InitTrack{
				{ID: 1, TimeScale: 90000, Codec: &codecs.AV1{}},
			}}},
			{"two videos", &fmp4.Init{Tracks: []*fmp4.InitTrack{
				{ID: 1, TimeScale: 90000, Codec: &codecs.H264{}},
				{ID: 2, TimeScale: 90000, Codec: &codecs.H264{}},
			}}},
			{"video only", &fmp4.Init{Tracks: []*fmp4.InitTrack{
				{ID: 1, TimeScale: 90000, Codec: &codecs.H264{}},
			}}},
		}

		for _, bc := range badCases {
			var f memFile
			rec, err := fmp4.OpenRecorder(&f, bc.init)
			c.Check(err != nil && rec == nil, "init case %q must fail", bc.name)
			c.Check(len(f.data) == 0, "failed init %q wrote nothing", bc.name)
		}

		// -- WriteSample validation, queue and endpoints stay unchanged --------
		var f memFile
		rec, err := fmp4.OpenRecorder(&f, good)
		c.Check(err == nil, "open: %v", err)

		c.Check(rec.WriteSample(9, 0, &fmp4.Sample{Duration: 1, Payload: []byte{1}}) != nil, "unknown track")
		c.Check(rec.WriteSample(0, 0, &fmp4.Sample{Duration: 1, Payload: []byte{1}}) != nil, "zero track ID")
		c.Check(rec.WriteSample(-1, 0, &fmp4.Sample{Duration: 1, Payload: []byte{1}}) != nil, "negative track ID")
		c.Check(rec.WriteSample(1, 0, nil) != nil, "nil sample")
		c.Check(rec.WriteSample(1, 0, &fmp4.Sample{Duration: 0, Payload: []byte{1}}) != nil, "zero duration")
		c.Check(rec.WriteSample(1, 0, &fmp4.Sample{Duration: 1, Payload: []byte{}}) != nil, "empty payload")
		c.Check(rec.WriteSample(1, math.MaxUint64-1, &fmp4.Sample{Duration: 10, Payload: []byte{1}}) != nil, "dts overflow")

		// first packet may start at an arbitrary DTS
		c.Check(rec.WriteSample(1, 50000, &fmp4.Sample{Duration: 3000, Payload: payload(1, 4)}) == nil, "arbitrary first DTS")

		// a rejected gap leaves the queue untouched: the correct DTS still works
		c.Check(rec.WriteSample(1, 99000, &fmp4.Sample{Duration: 3000, Payload: payload(2, 4)}) != nil, "gap rejected")
		c.Check(rec.WriteSample(1, 53000, &fmp4.Sample{
			Duration: 3000,
			Payload:  payload(3, 4),
		}) == nil, "contiguous sample accepted")

		// negative CTS is accepted
		c.Check(rec.WriteSample(1, 56000, &fmp4.Sample{
			Duration:  3000,
			PTSOffset: -1234,
			Payload:   payload(4, 4),
		}) == nil, "negative CTS")

		// the session owns an independent copy of the sample payload
		mutable := payload(5, 4)
		c.Check(rec.WriteSample(1, 59000, &fmp4.Sample{Duration: 3000, Payload: mutable}) == nil, "queue mutable sample")
		mutable[0] = 0xFF

		// the final file receives one sequential append: no trun backfill, no
		// backward seeks, and the write starts exactly at the old end
		endBefore := int64(len(f.data))
		f.ops = nil
		c.Check(rec.Commit() == nil, "commit timeline group")
		var writeOps []string
		var backwardSeek bool
		for _, op := range f.ops {
			if strings.HasPrefix(op, "write") {
				writeOps = append(writeOps, op)
			}
			if strings.HasPrefix(op, "seek@") {
				var p int64
				if _, err := fmt.Sscanf(op, "seek@%d", &p); err == nil && p < endBefore {
					backwardSeek = true
				}
			}
		}
		c.Check(len(writeOps) == 1, "exactly one write per commit, got %v", writeOps)
		c.Check(!backwardSeek, "commit never seeks into already written bytes")
		if len(writeOps) == 1 {
			expectedWrite := fmt.Sprintf(
				"write@%d:%d", endBefore, len(f.data)-int(endBefore))
			c.Check(writeOps[0] == expectedWrite, "write is a single append at the end")
		}
		c.Check(hasOp(f.ops, "sync"), "commit synced the append")

		timelineInitLen := firstMoofOffset(t, f.data)
		committed := parseGroups(t, f.data, timelineInitLen)
		c.Check(len(committed) == 1, "one timeline group")
		c.Check(bytes.Equal(committed[0].trafs[0].samples[3].payload, payload(5, 4)),
			"queued payload was copied, not referenced")
		c.Check(committed[0].trafs[0].samples[2].ptsOffset == -1234, "negative CTS round trip")

		// empty commit performs no I/O and consumes no sequence number
		opsBefore := f.opCount()
		c.Check(rec.Commit() == nil, "empty commit")
		c.Check(f.opCount() == opsBefore, "empty commit did no I/O")
		c.Check(rec.WriteSample(1, 62000, &fmp4.Sample{Duration: 3000, Payload: payload(6, 2)}) == nil, "after empty commit")
		c.Check(rec.Commit() == nil, "next commit")
		after := parseGroups(t, f.data, timelineInitLen)
		c.Check(len(after) == 2 && after[1].seq == 2, "empty commit skipped sequence 1 consumption")

		// missing tracks do not advance, and restarted sessions keep endpoints
		var f2 memFile
		rec2, err := fmp4.OpenRecorder(&f2, good)
		c.Check(err == nil, "open second session: %v", err)
		c.Check(rec2.WriteSample(1, 0, &fmp4.Sample{Duration: 3000, Payload: payload(7, 2)}) == nil, "video only group")
		c.Check(rec2.Commit() == nil, "video only commit")

		rec3, err := fmp4.OpenRecorder(&f2, good)
		c.Check(err == nil, "reopen: %v", err)
		// audio never appeared: its first packet may start at any DTS
		c.Check(rec3.WriteSample(2, 777, &fmp4.Sample{
			Duration: 1024,
			Payload:  payload(8, 2),
		}) == nil, "audio starts late: %v", err)
		// video must continue exactly at its committed endpoint
		c.Check(rec3.WriteSample(1, 1, &fmp4.Sample{
			Duration: 3000,
			Payload:  payload(9, 2),
		}) != nil, "video gap after restart rejected")
		c.Check(rec3.WriteSample(1, 3000, &fmp4.Sample{
			Duration: 3000,
			Payload:  payload(9, 2),
		}) == nil, "video continues at endpoint")
		c.Check(rec3.Commit() == nil, "reopened commit")

		reopenedInitLen := firstMoofOffset(t, f2.data)
		reopened := parseGroups(t, f2.data, reopenedInitLen)
		c.Check(len(reopened) == 2, "two groups across restart")
		c.Check(reopened[1].trafs[0].baseTime == 3000, "video tfdt after restart")
		c.Check(reopened[1].trafs[1].baseTime == 777, "audio first tfdt")

		// -- audio-only configuration ------------------------------------------
		var af memFile
		arec, err := fmp4.OpenRecorder(&af, audioOnlyInit())
		c.Check(err == nil, "audio-only open: %v", err)
		c.Check(arec.WriteSample(7, 100, &fmp4.Sample{Duration: 1024, Payload: payload(10, 3)}) == nil, "audio sample")
		c.Check(arec.Commit() == nil, "audio commit")
		arec2, err := fmp4.OpenRecorder(&af, audioOnlyInit())
		c.Check(err == nil, "audio-only reopen: %v", err)
		c.Check(arec2.WriteSample(7, 1124, &fmp4.Sample{
			Duration: 1024,
			Payload:  payload(11, 3),
		}) == nil, "audio continuation")
		c.Check(arec2.Commit() == nil, "audio second commit")
		aGroups := parseGroups(t, af.data, firstMoofOffset(t, af.data))
		c.Check(len(aGroups) == 2 && aGroups[0].seq == 1 && aGroups[1].seq == 2, "audio-only sequences")
		c.Check(aGroups[1].trafs[0].baseTime == 1124, "audio-only endpoint")

		// -- Init is copied too: mutating it after open changes nothing --------
		var cf memFile
		initCopy := dualTrackInit()
		_, err = fmp4.OpenRecorder(&cf, initCopy)
		c.Check(err == nil, "open for init copy: %v", err)
		initCopy.Tracks[0].TimeScale = 12345
		rec4, err := fmp4.OpenRecorder(&cf, dualTrackInit())
		c.Check(err == nil, "reopen with equivalent fresh Init: %v", err)
		c.Check(rec4.WriteSample(1, 0, &fmp4.Sample{Duration: 3000, Payload: payload(12, 2)}) == nil, "write after reopen")
	})
}

// ---------------------------------------------------------------------------
// TestRecorderFailures
// ---------------------------------------------------------------------------

func TestRecorderFailures(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		initData := dualTrackInit()

		// -- short write: io.ErrShortWrite, terminal state, no more file I/O ---
		var sw memFile
		sw.maxWrite = 3
		rec, err := fmp4.OpenRecorder(&sw, initData)
		c.Check(err == nil, "open short-write file: %v", err)
		err = rec.WriteSample(1, 0, &fmp4.Sample{Duration: 3000, Payload: payload(1, 8)})
		c.Check(err == nil, "queue sample: %v", err)
		err = rec.Commit()
		c.Check(errors.Is(err, io.ErrShortWrite), "Commit returns io.ErrShortWrite, got %v", err)
		opsAfterFail := sw.opCount()
		err = rec.WriteSample(1, 3000, &fmp4.Sample{Duration: 3000, Payload: payload(2, 8)})
		c.Check(errors.Is(err, io.ErrShortWrite), "WriteSample keeps returning ErrShortWrite, got %v", err)
		err = rec.Commit()
		c.Check(errors.Is(err, io.ErrShortWrite), "Commit keeps returning ErrShortWrite, got %v", err)
		c.Check(sw.opCount() == opsAfterFail, "terminal recorder never touches the file again")

		// -- write error: the original error is preserved ----------------------
		writeErr := errors.New("disk on fire")
		var we memFile
		we.writeErr = writeErr
		we.writeErrAfter = 1 // the initialization block is still written
		rec, err = fmp4.OpenRecorder(&we, initData)
		c.Check(err == nil, "open write-error file: %v", err)
		c.Check(rec.WriteSample(1, 0, &fmp4.Sample{Duration: 3000, Payload: payload(3, 4)}) == nil, "queue")
		err = rec.Commit()
		c.Check(errors.Is(err, writeErr), "write error propagated, got %v", err)
		c.Check(errors.Is(rec.Commit(), writeErr), "sticky write error")

		// -- sync failure: the fragment was written, the error is sticky -------
		syncErr := errors.New("sync failed")
		var se memFile
		se.syncErr = syncErr
		se.syncErrAfter = 1 // the initialization block still syncs successfully
		rec, err = fmp4.OpenRecorder(&se, initData)
		c.Check(err == nil, "open sync-error file: %v", err)
		c.Check(rec.WriteSample(1, 0, &fmp4.Sample{Duration: 3000, Payload: payload(4, 4)}) == nil, "queue")
		err = rec.Commit()
		c.Check(errors.Is(err, syncErr), "sync error propagated, got %v", err)
		c.Check(hasOp(se.ops, "write"), "fragment was written before sync")
		c.Check(hasOp(se.ops, "sync"), "sync was attempted")
		c.Check(errors.Is(rec.WriteSample(1, 3000, &fmp4.Sample{
			Duration: 1,
			Payload:  []byte{1},
		}), syncErr), "sticky on WriteSample")
		c.Check(errors.Is(rec.Commit(), syncErr), "sticky on Commit")

		// -- failure while writing the initialization block --------------------
		var ie memFile
		ie.writeErr = errors.New("init write blocked")
		rec, err = fmp4.OpenRecorder(&ie, initData)
		c.Check(err != nil && rec == nil, "empty-file init write failure")
		c.Check(ie.pos == 0, "cursor restored after init write failure, pos=%d", ie.pos)
		// -- complete but inconsistent groups: error, no truncation ------------
		var good memFile
		_, groupStarts := buildHistory(t, &good, initData)
		goodBytes := good.snapshot()
		initLen := groupStarts[0]

		mutate := func(mut func(data []byte, groups []verifyGroup)) []byte {
			data := append([]byte(nil), goodBytes...)
			groups := parseGroups(t, data, initLen)
			mut(data, groups)
			return data
		}

		// sequence number discontinuity
		data := mutate(func(d []byte, gs []verifyGroup) {
			binary.BigEndian.PutUint32(d[mfhdSequenceOffset(gs[1]):], 99)
		})
		var cf memFile
		cf.data = data
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil && strings.Contains(err.Error(), "sequence"), "seq discontinuity rejected: %v", err)
		c.Check(len(cf.data) == len(goodBytes), "seq error left file intact")
		c.Check(!hasOp(cf.ops, "truncate"), "seq error did not truncate")

		// unknown track
		data = mutate(func(d []byte, gs []verifyGroup) {
			binary.BigEndian.PutUint32(d[tfhdTrackIDOffset(d, gs[0], 0):], 99)
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil && strings.Contains(err.Error(), "unknown track"), "unknown track rejected: %v", err)
		c.Check(len(cf.data) == len(goodBytes) && !hasOp(cf.ops, "truncate"), "unknown track left file intact")

		// duplicated traf track ID
		data = mutate(func(d []byte, gs []verifyGroup) {
			first := tfhdTrackIDOffset(d, gs[0], 0)
			second := tfhdTrackIDOffset(d, gs[0], 1)
			binary.BigEndian.PutUint32(d[second:], binary.BigEndian.Uint32(d[first:]))
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil, "duplicate traf track rejected: %v", err)
		c.Check(!hasOp(cf.ops, "truncate"), "duplicate traf did not truncate")

		// missing required trun field: strip the sample-size-present flag
		data = mutate(func(d []byte, gs []verifyGroup) {
			trunOff := findTrunFlagsOffset(d, gs[0])
			flags := readTrunFlags(d, trunOff)
			writeTrunFlags(d, trunOff, flags&^0x0200)
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil, "trun without size flag rejected: %v", err)
		c.Check(!hasOp(cf.ops, "truncate"), "bad trun did not truncate")

		// sample range not precisely closed: move the data offset by one
		data = mutate(func(d []byte, gs []verifyGroup) {
			trunOff := findTrunDataOffsetOffset(d, gs[0])
			v := binary.BigEndian.Uint32(d[trunOff:])
			binary.BigEndian.PutUint32(d[trunOff:], v+1)
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil && strings.Contains(err.Error(), "mdat"), "range mismatch rejected: %v", err)
		c.Check(len(cf.data) == len(goodBytes) && !hasOp(cf.ops, "truncate"), "range mismatch left file intact")

		// non-contiguous decode time: change the second group's tfdt
		data = mutate(func(d []byte, gs []verifyGroup) {
			tfdtOff := findTfdtValueOffset(d, gs[1].trafs[0], gs[1])
			binary.BigEndian.PutUint64(d[tfdtOff:], 424242)
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil && strings.Contains(err.Error(), "non-contiguous"), "timeline gap rejected: %v", err)
		c.Check(!hasOp(cf.ops, "truncate"), "timeline gap did not truncate")

		// foreign tfhd layout: drop the default-base-is-moof flag
		data = mutate(func(d []byte, gs []verifyGroup) {
			payloadOff := tfhdTrackIDOffset(d, gs[0], 0) - 4 // full-box header precedes track ID
			flags := uint32(d[payloadOff+1])<<16 | uint32(d[payloadOff+2])<<8 | uint32(d[payloadOff+3])
			flags &^= 0x020000
			d[payloadOff+1] = byte(flags >> 16)
			d[payloadOff+2] = byte(flags >> 8)
			d[payloadOff+3] = byte(flags)
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil && strings.Contains(err.Error(), "default-base-is-moof"), "foreign tfhd rejected: %v", err)
		c.Check(!hasOp(cf.ops, "truncate"), "foreign tfhd did not truncate")

		// foreign trun layout: version 1 downgraded to version 0 with equal size
		data = mutate(func(d []byte, gs []verifyGroup) {
			trunOff := findTrunFlagsOffset(d, gs[0])
			d[trunOff] = 0 // version byte
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil && strings.Contains(err.Error(), "version 1"), "trun v0 rejected: %v", err)
		c.Check(!hasOp(cf.ops, "truncate"), "trun v0 did not truncate")

		// foreign tfdt layout: use an unsupported version
		data = mutate(func(d []byte, gs []verifyGroup) {
			tfdtPayloadOff := findTfdtValueOffset(d, gs[0].trafs[0], gs[0]) - 4
			d[tfdtPayloadOff] = 9
		})
		cf = memFile{data: data}
		_, err = fmp4.OpenRecorder(&cf, initData)
		c.Check(err != nil && strings.Contains(err.Error(), "tfdt"), "bad tfdt version rejected: %v", err)
		c.Check(!hasOp(cf.ops, "truncate"), "bad tfdt did not truncate")

		// -- initialization block mismatch: cursor and bytes untouched ---------
		mismatched := dualTrackInit()
		mismatched.Tracks[0].TimeScale = 60000
		cf = memFile{data: append([]byte(nil), goodBytes...)}
		cf.pos = 123
		_, err = fmp4.OpenRecorder(&cf, mismatched)
		c.Check(err != nil, "init mismatch rejected: %v", err)
		c.Check(cf.pos == 123, "cursor unchanged after init mismatch, got %d", cf.pos)
		c.Check(bytes.Equal(cf.data, goodBytes), "bytes unchanged after init mismatch")

		mismatchedCodec := dualTrackInit()
		mismatchedCodec.Tracks[0].Codec.(*codecs.H264).SPS[0] = 0x68
		cf = memFile{data: append([]byte(nil), goodBytes...)}
		_, err = fmp4.OpenRecorder(&cf, mismatchedCodec)
		c.Check(err != nil && strings.Contains(err.Error(), "initialization"), "codec config mismatch rejected: %v", err)
		c.Check(bytes.Equal(cf.data, goodBytes), "codec mismatch left bytes unchanged")

		// -- truncate and sync failures during recovery give no session --------
		partial := append([]byte(nil), goodBytes[:groupStarts[2]+11]...)

		var tf memFile
		tf.data = append([]byte(nil), partial...)
		tf.truncateErr = errors.New("truncate denied")
		rec, err = fmp4.OpenRecorder(&tf, initData)
		c.Check(err != nil && rec == nil, "truncate failure surfaces: %v", err)
		c.Check(len(tf.data) == len(partial), "failed truncate kept partial bytes")

		var sf memFile
		sf.data = append([]byte(nil), partial...)
		sf.syncErr = errors.New("post-truncate sync denied")
		rec, err = fmp4.OpenRecorder(&sf, initData)
		c.Check(err != nil && rec == nil, "post-truncate sync failure surfaces: %v", err)

		// -- seek failure during recovery is an error, not a usable session ----
		var kf memFile
		kf.data = append([]byte(nil), goodBytes...)
		kf.seekErr = errors.New("seek denied")
		rec, err = fmp4.OpenRecorder(&kf, initData)
		c.Check(err != nil && rec == nil, "seek failure surfaces: %v", err)

		// -- Parts.Unmarshal still reads a healthy file end to end -------------
		var parts fmp4.Parts
		err = parts.Unmarshal(goodBytes[initLen:])
		c.Check(err == nil, "production Unmarshal on recovered-style file: %v", err)
		c.Check(len(parts) == 3, "three parts, got %d", len(parts))
	})
}

// findTrunFlagsOffset returns the absolute offset of the trun version/flags
// word for the first traf of a group.
func findTrunFlagsOffset(data []byte, g verifyGroup) int {
	pos := int(g.moof.offset) + 8 // skip moof header
	mfhd, ok := readRawBoxAt(data, pos)
	if !ok {
		panic("mfhd not found")
	}
	pos += int(mfhd.size)
	traf, ok := readRawBoxAt(data, pos)
	if !ok {
		panic("traf not found")
	}
	trafEnd := pos + int(traf.size)
	pos += 8 // skip traf header
	for pos < trafEnd {
		box, ok := readRawBoxAt(data, pos)
		if !ok {
			panic("truncated traf")
		}
		if string(box.typ[:]) == "trun" {
			return pos + int(box.hdrSize)
		}
		pos += int(box.size)
	}
	panic("trun not found")
}

func findTrunDataOffsetOffset(data []byte, g verifyGroup) int {
	// version/flags (4) + sample count (4), then data offset
	return findTrunFlagsOffset(data, g) + 8
}

// readTrunFlags reads the 24-bit trun flags following the version byte.
func readTrunFlags(data []byte, off int) uint32 {
	return uint32(data[off+1])<<16 | uint32(data[off+2])<<8 | uint32(data[off+3])
}

func writeTrunFlags(data []byte, off int, flags uint32) {
	data[off+1] = byte(flags >> 16)
	data[off+2] = byte(flags >> 8)
	data[off+3] = byte(flags)
}

func findTfdtValueOffset(data []byte, tr verifyTraf, g verifyGroup) int {
	// locate the traf carrying this track, then its tfdt value
	for trafIdx, traf := range g.trafs {
		if traf.id != tr.id {
			continue
		}

		start := trafStart(data, g, trafIdx)
		box, ok := readRawBoxAt(data, start)
		if !ok {
			panic("truncated traf")
		}

		inner := start + 8 // skip traf header
		innerEnd := start + int(box.size)
		for inner < innerEnd {
			child, ok := readRawBoxAt(data, inner)
			if !ok {
				panic("truncated traf child")
			}
			if string(child.typ[:]) == "tfdt" {
				// version 1: 4-byte full-box header then 8-byte base time
				return inner + int(child.hdrSize) + 4
			}
			inner += int(child.size)
		}
	}
	panic("tfdt not found")
}
