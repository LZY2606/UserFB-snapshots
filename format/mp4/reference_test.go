package mp4

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"testing"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/codec/aacparser"
	"github.com/nareix/joy4/codec/h264parser"
	"github.com/nareix/joy4/format/mp4/mp4io"
	"github.com/nareix/joy4/task_support/taskcheck"
	"github.com/nareix/joy4/utils/bits/pio"
)

// memFile is an in-memory io.WriteSeeker/io.ReadSeeker used instead of
// real files so the tests stay inside the standard library.
type memFile struct {
	buf []byte
	pos int64
}

func (m *memFile) Write(p []byte) (int, error) {
	if int(m.pos)+len(p) > len(m.buf) {
		m.buf = append(m.buf, make([]byte, int(m.pos)+len(p)-len(m.buf))...)
	}
	copy(m.buf[m.pos:], p)
	m.pos += int64(len(p))
	return len(p), nil
}

func (m *memFile) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
	case io.SeekCurrent:
		offset += m.pos
	case io.SeekEnd:
		offset += int64(len(m.buf))
	}
	if offset < 0 {
		return 0, errors.New("negative position")
	}
	m.pos = offset
	return m.pos, nil
}

func (m *memFile) Read(p []byte) (int, error) {
	if m.pos >= int64(len(m.buf)) {
		return 0, io.EOF
	}
	n := copy(p, m.buf[m.pos:])
	m.pos += int64(n)
	return n, nil
}

func (m *memFile) reader() *bytes.Reader {
	return bytes.NewReader(m.buf)
}

// countReader records every seek and read performed on one resource.
type countReader struct {
	rs io.ReadSeeker

	seeks         []int64 // absolute positions requested
	relativeSeeks int
	seekFails     int
	reads         int
	bytesRead     int64
	shortReads    int
}

func (c *countReader) Seek(offset int64, whence int) (int64, error) {
	if c.seekFails > 0 {
		c.seekFails--
		return 0, errors.New("simulated seek failure")
	}
	if whence != io.SeekStart {
		c.relativeSeeks++
	}
	pos, err := c.rs.Seek(offset, whence)
	if err != nil {
		return 0, err
	}
	c.seeks = append(c.seeks, pos)
	return pos, nil
}

func (c *countReader) Read(p []byte) (int, error) {
	if c.shortReads > 0 {
		c.shortReads--
		// one byte plus EOF makes io.ReadFull fail with
		// ErrUnexpectedEOF instead of silently completing.
		if len(p) > 1 {
			c.reads++
			c.bytesRead++
			p[0] = 0
			return 1, io.EOF
		}
	}
	n, err := c.rs.Read(p)
	c.reads++
	c.bytesRead += int64(n)
	return n, err
}

// drefEntrySpec describes one dref url entry in file order.
type drefEntrySpec struct {
	selfContained bool
	location      string // stored NUL-terminated when external
}

// descSpec describes one stsd entry; dataRefIdx is 1-based.
type descSpec struct {
	dataRefIdx int16
}

type chunkSpec struct {
	desc    uint32 // 1-based stsd index
	samples [][]byte
}

type trackFixture struct {
	isVideo     bool
	timeScale   int32
	duration    uint32 // uniform stts duration per sample
	drefs       []drefEntrySpec
	descs       []descSpec
	chunks      []chunkSpec
	keySamples  []uint32 // 1-based stss entries
	configDirty bool     // second description gets mismatched codec config
}

// sampleLayout records where one flattened sample ended up.
type sampleLayout struct {
	self     bool
	location string // raw location including NUL when external
	offset   int64
	data     []byte
}

type builtTrack struct {
	fixture trackFixture
	layouts []sampleLayout
	samples int
}

type builtFixture struct {
	main      *memFile
	resources map[string][]byte
	tracks    []builtTrack
}

// sampleData builds distinctive variable-length sample bytes.
func sampleData(tag byte, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = tag + byte(i)
	}
	return b
}

func (f *builtFixture) build(tracks []trackFixture) error {
	f.main = &memFile{}
	f.resources = map[string][]byte{}

	// reserve the mdat header; self-contained bytes start at offset 8
	if _, err := f.main.Write(make([]byte, 8)); err != nil {
		return err
	}

	moov := &mp4io.Movie{
		Header: &mp4io.MovieHeader{
			TimeScale:   10000,
			NextTrackId: int32(len(tracks) + 1),
			Matrix:      [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
		},
		Tracks: []*mp4io.Track{},
	}

	aacCfg, err := aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12, 0x10})
	if err != nil {
		return err
	}
	avcCfg, err := h264parser.NewCodecDataFromSPSAndPPS(
		[]byte{0x67, 0x42, 0x00, 0x1e, 0xbc, 0xb0, 0x50, 0x1e, 0x88, 0x76, 0x80},
		[]byte{0x68, 0xce, 0x38, 0x80},
	)
	if err != nil {
		return err
	}

	maxDuration := int64(0)
	for ti, tf := range tracks {
		bt := builtTrack{fixture: tf}

		dref := &mp4io.DataRefer{Version: 0, EntryCount: int32(len(tf.drefs))}
		for _, e := range tf.drefs {
			url := &mp4io.DataReferUrl{Version: 0}
			if e.selfContained {
				url.Flags = dataReferenceFlagSelfContained
			} else {
				url.Location = append([]byte(e.location), 0)
			}
			dref.Urls = append(dref.Urls, url)
		}

		stsd := &mp4io.SampleDesc{EntryCount: int32(len(tf.descs))}
		for di, d := range tf.descs {
			if tf.isVideo {
				conf := avcCfg.AVCDecoderConfRecordBytes()
				if tf.configDirty && di == 1 {
					conf = append(append([]byte{}, conf...), 0x7f)
				}
				stsd.Descs = append(stsd.Descs, &mp4io.AVC1Desc{
					DataRefIdx:           d.dataRefIdx,
					HorizontalResolution: 72,
					VorizontalResolution: 72,
					Width:                int16(avcCfg.Width()),
					Height:               int16(avcCfg.Height()),
					FrameCount:           1,
					Depth:                24,
					ColorTableId:         -1,
					Conf:                 &mp4io.AVC1Conf{Data: conf},
				})
			} else {
				conf := aacCfg.MPEG4AudioConfigBytes()
				if tf.configDirty && di == 1 {
					conf = append(append([]byte{}, conf...), 0x00)
				}
				stsd.Descs = append(stsd.Descs, &mp4io.MP4ADesc{
					DataRefIdx:       d.dataRefIdx,
					NumberOfChannels: 2,
					SampleSize:       16,
					SampleRate:       44100,
					Conf:             &mp4io.ElemStreamDesc{DecConfig: conf},
				})
			}
		}

		stsc := &mp4io.SampleToChunk{}
		stsz := &mp4io.SampleSize{}
		stco := &mp4io.ChunkOffset{}
		totalSamples := 0
		for ci, ch := range tf.chunks {
			stsc.Entries = append(stsc.Entries, mp4io.SampleToChunkEntry{
				FirstChunk:      uint32(ci + 1),
				SamplesPerChunk: uint32(len(ch.samples)),
				SampleDescId:    ch.desc,
			})
			ref := tf.drefs[tf.descs[ch.desc-1].dataRefIdx-1]
			var target []byte
			var base int64
			if ref.selfContained {
				target = f.main.buf
				base = int64(len(f.main.buf))
			} else {
				loc := string(append([]byte(ref.location), 0))
				target = f.resources[loc]
				base = int64(len(target))
			}
			stco.Entries = append(stco.Entries, uint32(base))
			for _, s := range ch.samples {
				if ref.selfContained {
					if _, err := f.main.Write(s); err != nil {
						return err
					}
					bt.layouts = append(bt.layouts, sampleLayout{
						self: true, offset: base, data: append([]byte{}, s...),
					})
					base += int64(len(s))
				} else {
					loc := string(append([]byte(ref.location), 0))
					target = append(target, s...)
					f.resources[loc] = target
					bt.layouts = append(bt.layouts, sampleLayout{
						location: loc, offset: base, data: append([]byte{}, s...),
					})
					base += int64(len(s))
				}
				stsz.Entries = append(stsz.Entries, uint32(len(s)))
				totalSamples++
			}
		}

		stts := &mp4io.TimeToSample{Entries: []mp4io.TimeToSampleEntry{
			{Count: uint32(totalSamples), Duration: tf.duration},
		}}
		stbl := &mp4io.SampleTable{
			SampleDesc:    stsd,
			TimeToSample:  stts,
			SampleToChunk: stsc,
			ChunkOffset:   stco,
			SampleSize:    stsz,
		}
		if tf.isVideo {
			stbl.SyncSample = &mp4io.SyncSample{Entries: append([]uint32{}, tf.keySamples...)}
		}

		info := &mp4io.MediaInfo{
			Data:   &mp4io.DataInfo{Refer: dref},
			Sample: stbl,
		}
		if tf.isVideo {
			info.Video = &mp4io.VideoMediaInfo{Flags: 1}
		} else {
			info.Sound = &mp4io.SoundMediaInfo{}
		}
		handler := &mp4io.HandlerRefer{Name: []byte("ref-test")}
		if tf.isVideo {
			handler.SubType = [4]byte{'v', 'i', 'd', 'e'}
		} else {
			handler.SubType = [4]byte{'s', 'o', 'u', 'n'}
		}

		trackDuration := int64(totalSamples) * int64(tf.duration)
		track := &mp4io.Track{
			Header: &mp4io.TrackHeader{
				TrackId:  int32(ti + 1),
				Flags:    0x0003,
				Duration: int32(trackDuration),
				Matrix:   [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
			},
			Media: &mp4io.Media{
				Header: &mp4io.MediaHeader{
					TimeScale: tf.timeScale,
					Duration:  int32(trackDuration),
					Language:  21956,
				},
				Handler: handler,
				Info:    info,
			},
		}
		moov.Tracks = append(moov.Tracks, track)
		bt.samples = totalSamples
		f.tracks = append(f.tracks, bt)

		if d := time.Duration(trackDuration) * time.Second / time.Duration(tf.timeScale); int64(d) > maxDuration {
			maxDuration = int64(d)
		}
	}
	moov.Header.Duration = int32(maxDuration)

	// finalize mdat size now that all self-contained bytes are written
	pio.PutU32BE(f.main.buf[0:4], uint32(len(f.main.buf)))
	copy(f.main.buf[4:8], []byte("mdat"))

	moovBytes := make([]byte, moov.Len())
	moov.Marshal(moovBytes)
	if _, err := f.main.Write(moovBytes); err != nil {
		return err
	}
	return nil
}

// expectedPacket is one fully decoded sample the demuxer should emit.
type expectedPacket struct {
	idx      int
	time     time.Duration
	data     []byte
	key      bool
	location string
	self     bool
	offset   int64
}

func (f *builtFixture) expected() []expectedPacket {
	queues := make([][]expectedPacket, len(f.tracks))
	for ti, bt := range f.tracks {
		for si, lay := range bt.layouts {
			ts := int64(si) * int64(bt.fixture.duration)
			queues[ti] = append(queues[ti], expectedPacket{
				idx:      ti,
				time:     time.Duration(ts) * time.Second / time.Duration(bt.fixture.timeScale),
				data:     lay.data,
				key:      bt.fixture.isVideo && containsU32(bt.fixture.keySamples, uint32(si+1)),
				location: lay.location,
				self:     lay.self,
				offset:   lay.offset,
			})
		}
	}
	var out []expectedPacket
	pos := make([]int, len(queues))
	for {
		pick := -1
		for i := range queues {
			if pos[i] >= len(queues[i]) {
				continue
			}
			if pick == -1 || queues[i][pos[i]].time < queues[pick][pos[pick]].time {
				pick = i
			}
		}
		if pick == -1 {
			return out
		}
		out = append(out, queues[pick][pos[pick]])
		pos[pick]++
	}
}

func containsU32(xs []uint32, v uint32) bool {
	for _, x := range xs {
		if x == v {
			return true
		}
	}
	return false
}

// findNestedAtom returns the [start, end) byte range of the atom at the
// given tag path from the file root.
func findNestedAtom(data []byte, path []string) (int, int, error) {
	var walk func(start, end int, tags []string) (int, int, bool)
	walk = func(start, end int, tags []string) (int, int, bool) {
		pos := start
		for pos+8 <= end {
			size := int(pio.U32BE(data[pos:]))
			if size <= 0 || pos+size > end {
				return 0, 0, false
			}
			tag := string(data[pos+4 : pos+8])
			if tag == tags[0] {
				if len(tags) == 1 {
					return pos, pos + size, true
				}
				// dref/stsd carry an 8-byte version/entry-count header
				// before their child atoms.
				childStart := pos + 8
				if tag == "dref" || tag == "stsd" {
					childStart += 8
				}
				if s, e, ok := walk(childStart, pos+size, tags[1:]); ok {
					return s, e, ok
				}
			}
			pos += size
		}
		return 0, 0, false
	}
	s, e, ok := walk(0, len(data), path)
	if !ok {
		return 0, 0, fmt.Errorf("atom path not found: %v", path)
	}
	return s, e, nil
}

// replaceChildTag overwrites the nth child tag inside a container atom,
// keeping every atom size intact.
func replaceChildTag(data []byte, path []string, childIndex int, newTag string) error {
	start, end, err := findNestedAtom(data, path)
	if err != nil {
		return err
	}
	containerTag := path[len(path)-1]
	pos := start + 8
	if containerTag == "dref" || containerTag == "stsd" {
		pos += 8
	}
	for i := 0; ; i++ {
		if pos+8 > end {
			return fmt.Errorf("child %d not found in %v", childIndex, path)
		}
		size := int(pio.U32BE(data[pos:]))
		if i == childIndex {
			copy(data[pos+4:pos+8], []byte(newTag))
			return nil
		}
		pos += size
	}
}

// resolverInstrumentation counts resolve calls and owns resource
// counters; one instance is shared by all tracks of a Demuxer.
type resolverInstrumentation struct {
	calls    map[string]int
	failures map[string]int // failures still to emit per location
	readers  map[string]*countReader
}

func newInstrumentation() *resolverInstrumentation {
	return &resolverInstrumentation{
		calls:    map[string]int{},
		failures: map[string]int{},
		readers:  map[string]*countReader{},
	}
}

func (ins *resolverInstrumentation) resolver(res map[string][]byte) ReferenceResolver {
	return func(location string) (io.ReadSeeker, error) {
		ins.calls[location]++
		if ins.failures[location] > 0 {
			ins.failures[location]--
			return nil, fmt.Errorf("simulated resolution failure for %q", location)
		}
		raw, ok := res[location]
		if !ok {
			return nil, fmt.Errorf("unknown location %q", location)
		}
		cr := &countReader{rs: bytes.NewReader(raw)}
		ins.readers[location] = cr
		return cr, nil
	}
}

func routingTracks() []trackFixture {
	audio := trackFixture{
		isVideo:   false,
		timeScale: 1000,
		duration:  50,
		drefs: []drefEntrySpec{
			{selfContained: true},
			{location: "audio-A.bin"},
			{location: "audio-B.bin"},
		},
		descs: []descSpec{{dataRefIdx: 1}, {dataRefIdx: 2}, {dataRefIdx: 3}},
		chunks: []chunkSpec{
			{desc: 1, samples: [][]byte{sampleData(0x10, 3)}},
			{desc: 2, samples: [][]byte{sampleData(0x20, 2), sampleData(0x21, 4)}},
			{desc: 3, samples: [][]byte{sampleData(0x30, 5)}},
			{desc: 1, samples: [][]byte{sampleData(0x40, 1), sampleData(0x41, 3), sampleData(0x42, 2)}},
			{desc: 2, samples: [][]byte{sampleData(0x50, 4)}},
		},
	}
	video := trackFixture{
		isVideo:   true,
		timeScale: 1000,
		duration:  100,
		drefs: []drefEntrySpec{
			{selfContained: true},
			{location: "audio-A.bin"}, // shared with the audio track
			{location: "video-X.bin"},
		},
		descs: []descSpec{{dataRefIdx: 2}, {dataRefIdx: 3}, {dataRefIdx: 1}},
		chunks: []chunkSpec{
			{desc: 1, samples: [][]byte{sampleData(0x60, 6)}},
			{desc: 2, samples: [][]byte{sampleData(0x70, 2), sampleData(0x71, 3)}},
			{desc: 3, samples: [][]byte{sampleData(0x80, 4)}},
			{desc: 1, samples: [][]byte{sampleData(0x90, 5)}},
		},
		keySamples: []uint32{1, 3, 5},
	}
	return []trackFixture{audio, video}
}

func TestReferenceRouting(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		fx := &builtFixture{}
		if err := fx.build(routingTracks()); err != nil {
			t.Fatalf("build fixture: %v", err)
		}
		ins := newInstrumentation()
		mainCounter := &countReader{rs: fx.main.reader()}
		d := NewDemuxerWithResolver(mainCounter, ins.resolver(fx.resources))

		// Streams must only parse indices/config and never resolve.
		streams, err := d.Streams()
		c.Check(err == nil, "Streams returned error: %v", err)
		c.Check(len(streams) == 2, "expected 2 streams, got %d", len(streams))
		c.Check(len(ins.calls) == 0, "resolver called during Streams: %d calls", len(ins.calls))
		c.Check(streams[0].Type() == av.AAC && streams[1].Type() == av.H264,
			"unexpected codec types: %v %v", streams[0].Type(), streams[1].Type())

		expect := fx.expected()
		got := []av.Packet{}
		for {
			pkt, rerr := d.ReadPacket()
			if rerr == io.EOF {
				break
			}
			c.Check(rerr == nil, "ReadPacket error: %v", rerr)
			if rerr != nil {
				break
			}
			got = append(got, pkt)
		}
		c.Check(len(got) == len(expect), "packet count: got %d want %d", len(got), len(expect))

		for i, exp := range expect {
			if i >= len(got) {
				break
			}
			c.Check(int(got[i].Idx) == exp.idx, "packet %d idx: got %d want %d", i, got[i].Idx, exp.idx)
			c.Check(got[i].Time == exp.time, "packet %d time: got %v want %v", i, got[i].Time, exp.time)
			c.Check(bytes.Equal(got[i].Data, exp.data), "packet %d data mismatch (got %x want %x)", i, got[i].Data, exp.data)
			c.Check(got[i].IsKeyFrame == exp.key, "packet %d keyframe: got %v want %v", i, got[i].IsKeyFrame, exp.key)
		}

		// Independent resource-call assertions: each external location
		// resolves exactly once even with two tracks and many chunks.
		c.Check(ins.calls["audio-A.bin\x00"] == 1, "audio-A resolve calls: %d", ins.calls["audio-A.bin\x00"])
		c.Check(ins.calls["audio-B.bin\x00"] == 1, "audio-B resolve calls: %d", ins.calls["audio-B.bin\x00"])
		c.Check(ins.calls["video-X.bin\x00"] == 1, "video-X resolve calls: %d", ins.calls["video-X.bin\x00"])

		// Same offset (0) in two different locations holds different bytes.
		crA := ins.readers["audio-B.bin\x00"]
		crX := ins.readers["video-X.bin\x00"]
		c.Check(crA != nil && crX != nil, "external readers were created")
		c.Check(fx.resources["audio-B.bin\x00"][0] != fx.resources["video-X.bin\x00"][0],
			"offset-0 content must differ across locations")

		// Every external read is preceded by an absolute Seek to the
		// sample's resource offset; byte counts match sample sizes.
		for loc, cr := range ins.readers {
			c.Check(cr.relativeSeeks == 0, "location %q had %d relative seeks", loc, cr.relativeSeeks)
			c.Check(len(cr.seeks) == cr.reads, "location %q seeks %d != reads %d", loc, len(cr.seeks), cr.reads)
			var wantBytes int64
			for _, exp := range expect {
				if exp.location == loc {
					wantBytes += int64(len(exp.data))
				}
			}
			c.Check(cr.bytesRead == wantBytes, "location %q bytes: got %d want %d", loc, cr.bytesRead, wantBytes)
		}
		c.Check(len(crA.seeks) == 1 && crA.seeks[0] == 0, "audio-B must seek once to offset 0, got %v", crA.seeks)
		c.Check(len(crX.seeks) == 2 && crX.seeks[0] == 0, "video-X seeks must start at 0, got %v", crX.seeks)

		// Multi-sample chunk: audio-A holds a two-sample chunk, so its
		// seek history must contain the consecutive absolute offsets 0, 2
		// (second sample starts right after the first two-byte sample).
		audioA := ins.readers["audio-A.bin\x00"]
		sawConsecutive := false
		for i := 0; i+1 < len(audioA.seeks); i++ {
			if audioA.seeks[i] == 0 && audioA.seeks[i+1] == 2 {
				sawConsecutive = true
			}
		}
		c.Check(sawConsecutive, "multi-sample chunk must seek to offsets 0 then 2: %v", audioA.seeks)

		// DTS tie keeps stream order: locate the 100ms collision.
		var tieAudio, tieVideo int
		for i, exp := range expect {
			if exp.time == 100*time.Millisecond {
				if exp.idx == 0 && tieAudio == 0 {
					tieAudio = i
				}
				if exp.idx == 1 {
					tieVideo = i
				}
			}
		}
		c.Check(tieAudio > 0 && tieVideo > tieAudio, "DTS tie must favor lower stream index: audio@%d video@%d", tieAudio, tieVideo)

		// Main-file traffic includes every self-contained sample.
		var selfBytes int64
		for _, exp := range expect {
			if exp.self {
				selfBytes += int64(len(exp.data))
			}
		}
		c.Check(mainCounter.bytesRead >= selfBytes, "main reads %d bytes, self samples need %d", mainCounter.bytesRead, selfBytes)

		// Legacy demuxer keeps self-contained support but rejects external
		// samples with a recognizable error.
		legacy := NewDemuxer(fx.main.reader())
		_, err = legacy.Streams()
		c.Check(err == nil, "legacy Streams error: %v", err)
		var sawExternalErr bool
		for {
			_, rerr := legacy.ReadPacket()
			if rerr == io.EOF {
				break
			}
			if rerr != nil && bytes.Contains([]byte(rerr.Error()), []byte("NewDemuxerWithResolver")) {
				sawExternalErr = true
				break
			}
		}
		c.Check(sawExternalErr, "legacy demuxer must reject external samples")

		// Self-contained round trip through the regular muxer/demuxer.
		selfFile := &memFile{}
		mux := NewMuxer(selfFile)
		c.Check(mux.WriteHeader(streams[:1]) == nil, "mux WriteHeader")
		for i := 0; i < 5; i++ {
			perr := mux.WritePacket(av.Packet{Idx: 0, Time: time.Duration(i) * 20 * time.Millisecond, Data: []byte{byte(i), 0xa5, 0x5a}})
			c.Check(perr == nil, "mux WritePacket %d: %v", i, perr)
		}
		c.Check(mux.WriteTrailer() == nil, "mux WriteTrailer")
		round := NewDemuxer(selfFile.reader())
		rs, rerr := round.Streams()
		c.Check(rerr == nil && len(rs) == 1, "roundtrip Streams: %v", rerr)
		for i := 0; i < 5; i++ {
			pkt, perr := round.ReadPacket()
			c.Check(perr == nil, "roundtrip packet %d error: %v", i, perr)
			c.Check(bytes.Equal(pkt.Data, []byte{byte(i), 0xa5, 0x5a}), "roundtrip packet %d data", i)
			c.Check(pkt.Time == time.Duration(i)*20*time.Millisecond, "roundtrip packet %d time %v", i, pkt.Time)
		}
		_, rerr = round.ReadPacket()
		c.Check(rerr == io.EOF, "roundtrip expected EOF, got %v", rerr)

		// Same file via resolver demuxer: resolver must never be called.
		noCallIns := newInstrumentation()
		round2 := NewDemuxerWithResolver(selfFile.reader(), noCallIns.resolver(map[string][]byte{}))
		_, rerr = round2.Streams()
		c.Check(rerr == nil, "resolver demuxer on self-contained file: %v", rerr)
		for i := 0; i < 5; i++ {
			_, perr := round2.ReadPacket()
			c.Check(perr == nil, "resolver demuxer packet %d: %v", i, perr)
		}
		c.Check(len(noCallIns.calls) == 0, "self-contained file triggered resolver calls: %d", len(noCallIns.calls))
	})
}

func TestReferenceSeek(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		fx := &builtFixture{}
		if err := fx.build(routingTracks()); err != nil {
			t.Fatalf("build fixture: %v", err)
		}
		ins := newInstrumentation()
		d := NewDemuxerWithResolver(fx.main.reader(), ins.resolver(fx.resources))
		streams, err := d.Streams()
		c.Check(err == nil, "Streams: %v", err)
		c.Check(len(streams) == 2, "stream count %d", len(streams))

		// Drain to the tail first.
		for {
			if _, rerr := d.ReadPacket(); rerr == io.EOF {
				break
			} else if rerr != nil {
				t.Fatalf("drain: %v", rerr)
			}
		}
		callsAfterDrain := map[string]int{}
		for k, v := range ins.calls {
			callsAfterDrain[k] = v
		}

		// Seeking must not open or resolve any resource.
		c.Check(d.SeekToTime(100*time.Millisecond) == nil, "SeekToTime error")
		for loc, n := range callsAfterDrain {
			c.Check(ins.calls[loc] == n, "seek opened resource %q: %d -> %d", loc, n, ins.calls[loc])
		}

		// SeekToTime first snaps the video track to the keyframe at or
		// before 100ms (sample 0), then re-positions every audio track to
		// the snapped video dts as well. Both tracks now sit at dts 0 and
		// the scheduler breaks the tie by stream order: audio first, then
		// the snapped video keyframe.
		pkt, rerr := d.ReadPacket()
		c.Check(rerr == nil, "first post-seek packet: %v", rerr)
		c.Check(pkt.Idx == 0 && pkt.Time == 0,
			"first packet must be audio re-positioned to video dts 0, got idx=%d time=%v", pkt.Idx, pkt.Time)
		c.Check(bytes.Equal(pkt.Data, sampleData(0x10, 3)), "audio sample at 0 after seek: %x", pkt.Data)

		pkt, rerr = d.ReadPacket()
		c.Check(rerr == nil, "second post-seek packet: %v", rerr)
		c.Check(pkt.Idx == 1 && pkt.Time == 0 && pkt.IsKeyFrame,
			"second packet must be the snapped video keyframe, got idx=%d time=%v key=%v", pkt.Idx, pkt.Time, pkt.IsKeyFrame)
		c.Check(bytes.Equal(pkt.Data, sampleData(0x60, 6)), "snapped video sample bytes: %x", pkt.Data)

		// Audio@0 lives in the self-contained file; the external audio-A
		// chunk (holding audio samples 1 and 2) is reached again through
		// an absolute seek to offset 0.
		audioA := ins.readers["audio-A.bin\x00"]
		c.Check(audioA != nil, "audio-A reader exists")
		sawOffset0 := false
		for _, p := range audioA.seeks {
			if p == 0 {
				sawOffset0 = true
			}
		}
		c.Check(sawOffset0, "re-reads must use absolute offset 0 seeks: %v", audioA.seeks)

		// Continue draining: every track re-entered scheduling after seek.
		rest := 1
		for {
			_, rerr := d.ReadPacket()
			if rerr == io.EOF {
				break
			}
			c.Check(rerr == nil, "post-seek drain error: %v", rerr)
			rest++
		}
		c.Check(rest > 2, "both tracks must continue after seek, got %d more packets", rest)

		// Seek back to zero: both tracks reset to sample 0 and the DTS
		// tie is broken by stream order, so audio is emitted first with
		// identical bytes; re-reads reissue absolute seeks.
		seeksBefore := len(audioA.seeks)
		c.Check(d.SeekToTime(0) == nil, "SeekToTime(0)")
		pkt, rerr = d.ReadPacket()
		c.Check(rerr == nil, "read after SeekToTime(0): %v", rerr)
		c.Check(pkt.Idx == 0 && pkt.Time == 0, "first packet after zero seek: idx=%d time=%v", pkt.Idx, pkt.Time)
		c.Check(bytes.Equal(pkt.Data, sampleData(0x10, 3)), "first packet data after zero seek")
		for {
			if _, rerr := d.ReadPacket(); rerr == io.EOF {
				break
			} else if rerr != nil {
				t.Fatalf("drain after zero seek: %v", rerr)
			}
		}
		c.Check(len(audioA.seeks) > seeksBefore, "re-reads after seek must issue fresh absolute seeks")

		// All observed seeks stay inside their resource.
		for loc, cr := range ins.readers {
			for _, p := range cr.seeks {
				c.Check(p >= 0 && p <= int64(len(fx.resources[loc])),
					"location %q seek %d outside resource", loc, p)
			}
		}
	})
}

func retryTracks() []trackFixture {
	return []trackFixture{{
		isVideo:   false,
		timeScale: 1000,
		duration:  50,
		drefs:     []drefEntrySpec{{location: "retry.bin"}},
		descs:     []descSpec{{dataRefIdx: 1}},
		chunks: []chunkSpec{
			{desc: 1, samples: [][]byte{sampleData(0xa0, 3), sampleData(0xa1, 4), sampleData(0xa2, 2), sampleData(0xa3, 5)}},
		},
	}}
}

func TestReferenceRetry(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Case 1: resolution fails twice, then succeeds. Failures are not
		// cached and the failed sample stays current across retries.
		fx := &builtFixture{}
		if err := fx.build(retryTracks()); err != nil {
			t.Fatalf("build fixture: %v", err)
		}
		ins := newInstrumentation()
		ins.failures["retry.bin\x00"] = 2
		d := NewDemuxerWithResolver(fx.main.reader(), ins.resolver(fx.resources))
		_, err := d.Streams()
		c.Check(err == nil, "Streams: %v", err)

		_, err = d.ReadPacket()
		c.Check(err != nil, "first read must fail")
		c.Check(ins.calls["retry.bin\x00"] == 1, "resolve call after first failure: %d", ins.calls["retry.bin\x00"])
		_, err = d.ReadPacket()
		c.Check(err != nil, "second read must fail")
		c.Check(ins.calls["retry.bin\x00"] == 2, "resolve call after second failure: %d", ins.calls["retry.bin\x00"])
		c.Check(len(ins.readers) == 0, "failed resolutions must not cache a reader")

		pkt, err := d.ReadPacket()
		c.Check(err == nil, "third read must succeed: %v", err)
		c.Check(ins.calls["retry.bin\x00"] == 3, "resolve call after recovery: %d", ins.calls["retry.bin\x00"])
		c.Check(bytes.Equal(pkt.Data, sampleData(0xa0, 3)), "retried sample data: %x", pkt.Data)
		c.Check(pkt.Time == 0, "retried sample time: %v", pkt.Time)

		// State did not advance during failures: subsequent samples are
		// still the original second, third and fourth ones.
		for i, want := range [][]byte{sampleData(0xa1, 4), sampleData(0xa2, 2), sampleData(0xa3, 5)} {
			pkt, perr := d.ReadPacket()
			c.Check(perr == nil, "continuation packet %d: %v", i, perr)
			c.Check(bytes.Equal(pkt.Data, want), "continuation packet %d data", i)
			c.Check(pkt.Time == time.Duration(i+1)*50*time.Millisecond, "continuation packet %d time %v", i, pkt.Time)
		}
		_, err = d.ReadPacket()
		c.Check(err == io.EOF, "expected EOF after all samples, got %v", err)
		c.Check(ins.calls["retry.bin\x00"] == 3, "successful resolve stays cached: %d", ins.calls["retry.bin\x00"])

		// Case 2: seek failure on the cached reader must not advance state
		// and must not trigger another resolution.
		fx2 := &builtFixture{}
		if err := fx2.build(retryTracks()); err != nil {
			t.Fatalf("build fixture 2: %v", err)
		}
		ins2 := newInstrumentation()
		d2 := NewDemuxerWithResolver(fx2.main.reader(), func(location string) (io.ReadSeeker, error) {
			ins2.calls[location]++
			cr := &countReader{rs: bytes.NewReader(fx2.resources[location]), seekFails: 1}
			ins2.readers[location] = cr
			return cr, nil
		})
		_, err = d2.Streams()
		c.Check(err == nil, "Streams case2: %v", err)
		_, err = d2.ReadPacket()
		c.Check(err != nil, "seek failure must surface")
		c.Check(ins2.calls["retry.bin\x00"] == 1, "seek failure must not re-resolve: %d", ins2.calls["retry.bin\x00"])
		pkt, err = d2.ReadPacket()
		c.Check(err == nil, "retry after seek failure: %v", err)
		c.Check(bytes.Equal(pkt.Data, sampleData(0xa0, 3)), "sample after seek retry: %x", pkt.Data)
		c.Check(pkt.Time == 0, "sample time unchanged after seek retry")

		// Case 3: short read must surface, not advance state, and the next
		// absolute-seek retry returns the complete sample.
		fx3 := &builtFixture{}
		if err := fx3.build(retryTracks()); err != nil {
			t.Fatalf("build fixture 3: %v", err)
		}
		ins3 := newInstrumentation()
		d3 := NewDemuxerWithResolver(fx3.main.reader(), func(location string) (io.ReadSeeker, error) {
			ins3.calls[location]++
			cr := &countReader{rs: bytes.NewReader(fx3.resources[location]), shortReads: 1}
			ins3.readers[location] = cr
			return cr, nil
		})
		_, err = d3.Streams()
		c.Check(err == nil, "Streams case3: %v", err)
		_, err = d3.ReadPacket()
		c.Check(err != nil, "short read must surface as error")
		pkt, err = d3.ReadPacket()
		c.Check(err == nil, "retry after short read: %v", err)
		c.Check(len(pkt.Data) == 3, "full sample length after retry: %d", len(pkt.Data))
		c.Check(bytes.Equal(pkt.Data, sampleData(0xa0, 3)), "full sample bytes after short read")
		c.Check(pkt.Time == 0, "short read must not advance time")
		c.Check(ins3.calls["retry.bin\x00"] == 1, "short read reuses cached resolver: %d", ins3.calls["retry.bin\x00"])
		cr3 := ins3.readers["retry.bin\x00"]
		c.Check(len(cr3.seeks) == 2, "retry re-seeks absolutely before reading: %d seeks", len(cr3.seeks))

		// Malformed tables produce recognizable errors during Streams and
		// never reach the resolver. Every case starts from a valid fixture
		// and mutates the serialized atoms in place.
		drefPath := []string{"moov", "trak", "mdia", "minf", "dinf", "dref"}
		urlPath := []string{"moov", "trak", "mdia", "minf", "dinf", "dref", "url "}
		stblPath := []string{"moov", "trak", "mdia", "minf", "stbl"}
		stsdPath := append(append([]string{}, stblPath...), "stsd")
		stscPath := append(append([]string{}, stblPath...), "stsc")
		mp4aPath := append(append([]string{}, stsdPath...), "mp4a")

		patchU32 := func(path []string, off int, val uint32) func(*builtFixture) error {
			return func(f *builtFixture) error {
				s, _, perr := findNestedAtom(f.main.buf, path)
				if perr != nil {
					return perr
				}
				pio.PutU32BE(f.main.buf[s+off:], val)
				return nil
			}
		}
		patchI16 := func(path []string, off int, val int16) func(*builtFixture) error {
			return func(f *builtFixture) error {
				s, _, perr := findNestedAtom(f.main.buf, path)
				if perr != nil {
					return perr
				}
				pio.PutI16BE(f.main.buf[s+off:], val)
				return nil
			}
		}
		malformed := []struct {
			name   string
			patch  func(*builtFixture) error
			mutate func(*[]trackFixture)
		}{
			{name: "dref count mismatch", patch: patchU32(drefPath, 12, 0)},
			{name: "stsd count mismatch", patch: patchU32(stsdPath, 12, 0)},
			{name: "empty stsc", patch: patchU32(stscPath, 12, 0)},
			{name: "stsc index out of range", patch: patchU32(stscPath, 24, 9)},
			{name: "data ref index out of range", patch: patchI16(mp4aPath, 14, 9)},
			{name: "url version unsupported", patch: func(f *builtFixture) error {
				s, _, perr := findNestedAtom(f.main.buf, urlPath)
				if perr != nil {
					return perr
				}
				f.main.buf[s+8] = 1
				return nil
			}},
			{name: "location missing zero terminator", patch: func(f *builtFixture) error {
				_, e, perr := findNestedAtom(f.main.buf, urlPath)
				if perr != nil {
					return perr
				}
				f.main.buf[e-1] = 'x'
				return nil
			}},
			{name: "unsupported dref entry", patch: func(f *builtFixture) error {
				return replaceChildTag(f.main.buf, drefPath, 0, "urn ")
			}},
			{name: "unsupported stsd entry", patch: func(f *builtFixture) error {
				return replaceChildTag(f.main.buf, stsdPath, 0, "xxxx")
			}},
			{name: "different codec config", mutate: func(t *[]trackFixture) {
				(*t)[0].configDirty = true
				(*t)[0].descs = append((*t)[0].descs, descSpec{dataRefIdx: 1})
			}},
		}
		for _, m := range malformed {
			tf := retryTracks()
			if m.mutate != nil {
				m.mutate(&tf)
			}
			fxm := &builtFixture{}
			if berr := fxm.build(tf); berr != nil {
				c.Check(berr != nil, "%s: expected build rejection", m.name)
				continue
			}
			if m.patch != nil {
				if perr := m.patch(fxm); perr != nil {
					t.Fatalf("%s patch: %v", m.name, perr)
				}
			}
			insm := newInstrumentation()
			dm := NewDemuxerWithResolver(fxm.main.reader(), insm.resolver(fxm.resources))
			_, serr := dm.Streams()
			c.Check(serr != nil, "%s: expected Streams error", m.name)
			c.Check(len(insm.calls) == 0, "%s: resolver must not be called on bad tables", m.name)
		}
	})
}
