package mp4

import (
	"bufio"
	"fmt"
	"io"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/codec/aacparser"
	"github.com/nareix/joy4/codec/h264parser"
	"github.com/nareix/joy4/format/mp4/mp4io"
	"github.com/nareix/joy4/utils/bits/pio"
)

const (
	defaultTimeScale  = 90000
	movieTimeScale    = 10000
	maxClockTimeScale = 1000000000
)

// Muxer writes an MP4 file. The default muxer (NewMuxer) keeps the historical
// behaviour: every sample duration is derived from the difference of two packet
// times and independently converted to a 90kHz tick. A clock muxer
// (NewClockMuxer) instead quantizes absolute endpoints on each track's own
// timescale, which avoids accumulating rounding error over long recordings.
type Muxer struct {
	w       io.WriteSeeker
	bufw    *bufio.Writer
	wpos    int64
	streams []*Stream

	clockMode  bool
	clockScale []uint32
	headerDone bool
}

func NewMuxer(w io.WriteSeeker) *Muxer {
	return &Muxer{
		w:    w,
		bufw: bufio.NewWriterSize(w, pio.RecommendBufioSize),
	}
}

// NewClockMuxer returns a muxer that times every track by an explicit media
// timescale. The timescales slice is copied; it must have one entry per track
// passed to WriteHeader, in the same order. Each entry must be in
// [1, 1000000000]. Invalid input is reported by WriteHeader before anything is
// written to w.
func NewClockMuxer(w io.WriteSeeker, timescales []uint32) *Muxer {
	m := &Muxer{
		w:         w,
		bufw:      bufio.NewWriterSize(w, pio.RecommendBufioSize),
		clockMode: true,
	}
	m.clockScale = append([]uint32(nil), timescales...)
	return m
}

func (self *Muxer) newStream(codec av.CodecData) (err error) {
	switch codec.Type() {
	case av.H264, av.AAC:

	default:
		err = fmt.Errorf("mp4: codec type=%v is not supported", codec.Type())
		return
	}
	stream := &Stream{CodecData: codec}

	stream.sample = &mp4io.SampleTable{
		SampleDesc:   &mp4io.SampleDesc{},
		TimeToSample: &mp4io.TimeToSample{},
		SampleToChunk: &mp4io.SampleToChunk{
			Entries: []mp4io.SampleToChunkEntry{
				{
					FirstChunk:      1,
					SampleDescId:    1,
					SamplesPerChunk: 1,
				},
			},
		},
		SampleSize:  &mp4io.SampleSize{},
		ChunkOffset: &mp4io.ChunkOffset{},
	}

	stream.trackAtom = &mp4io.Track{
		Header: &mp4io.TrackHeader{
			TrackId:  int32(len(self.streams) + 1),
			Flags:    0x0003, // Track enabled | Track in movie
			Duration: 0,      // fill later
			Matrix:   [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
		},
		Media: &mp4io.Media{
			Header: &mp4io.MediaHeader{
				TimeScale: 0, // fill later
				Duration:  0, // fill later
				Language:  21956,
			},
			Info: &mp4io.MediaInfo{
				Sample: stream.sample,
				Data: &mp4io.DataInfo{
					Refer: &mp4io.DataRefer{
						Url: &mp4io.DataReferUrl{
							Flags: 0x000001, // Self reference
						},
					},
				},
			},
		},
	}

	switch codec.Type() {
	case av.H264:
		stream.sample.SyncSample = &mp4io.SyncSample{}
	}

	stream.timeScale = defaultTimeScale
	stream.muxer = self
	self.streams = append(self.streams, stream)

	return
}

func (self *Stream) fillTrackAtom() (err error) {
	self.trackAtom.Media.Header.TimeScale = int32(self.timeScale)

	if self.Type() == av.H264 {
		codec := self.CodecData.(h264parser.CodecData)
		width, height := codec.Width(), codec.Height()
		self.sample.SampleDesc.AVC1Desc = &mp4io.AVC1Desc{
			DataRefIdx:           1,
			HorizontalResolution: 72,
			VorizontalResolution: 72,
			Width:                int16(width),
			Height:               int16(height),
			FrameCount:           1,
			Depth:                24,
			ColorTableId:         -1,
			Conf:                 &mp4io.AVC1Conf{Data: codec.AVCDecoderConfRecordBytes()},
		}
		self.trackAtom.Media.Handler = &mp4io.HandlerRefer{
			SubType: [4]byte{'v', 'i', 'd', 'e'},
			Name:    []byte("Video Media Handler"),
		}
		self.trackAtom.Media.Info.Video = &mp4io.VideoMediaInfo{
			Flags: 0x000001,
		}
		self.trackAtom.Header.TrackWidth = float64(width)
		self.trackAtom.Header.TrackHeight = float64(height)

	} else if self.Type() == av.AAC {
		codec := self.CodecData.(aacparser.CodecData)
		self.sample.SampleDesc.MP4ADesc = &mp4io.MP4ADesc{
			DataRefIdx:       1,
			NumberOfChannels: int16(codec.ChannelLayout().Count()),
			SampleSize:       int16(codec.SampleFormat().BytesPerSample()),
			SampleRate:       float64(codec.SampleRate()),
			Conf: &mp4io.ElemStreamDesc{
				DecConfig: codec.MPEG4AudioConfigBytes(),
			},
		}
		self.trackAtom.Header.Volume = 1
		self.trackAtom.Header.AlternateGroup = 1
		self.trackAtom.Media.Handler = &mp4io.HandlerRefer{
			SubType: [4]byte{'s', 'o', 'u', 'n'},
			Name:    []byte("Sound Handler"),
		}
		self.trackAtom.Media.Info.Sound = &mp4io.SoundMediaInfo{}

	} else {
		err = fmt.Errorf("mp4: codec type=%d invalid", self.Type())
	}

	return
}

func (self *Muxer) validateClockHeader(streams []av.CodecData) error {
	if len(self.clockScale) != len(streams) {
		return fmt.Errorf("mp4: clock timescales has %d entries, header has %d streams",
			len(self.clockScale), len(streams))
	}
	for i, ts := range self.clockScale {
		if ts < 1 || ts > maxClockTimeScale {
			return fmt.Errorf("mp4: clock timescale[%d]=%d out of range [1, %d]",
				i, ts, maxClockTimeScale)
		}
	}
	return nil
}

func (self *Muxer) WriteHeader(streams []av.CodecData) (err error) {
	if self.clockMode {
		if err = self.validateClockHeader(streams); err != nil {
			return
		}
	}

	self.streams = []*Stream{}
	for _, stream := range streams {
		if err = self.newStream(stream); err != nil {
			return
		}
	}

	if self.clockMode {
		for i, stream := range self.streams {
			stream.timeScale = int64(self.clockScale[i])
		}
	}

	taghdr := make([]byte, 8)
	pio.PutU32BE(taghdr[4:], uint32(mp4io.MDAT))
	if _, err = self.w.Write(taghdr); err != nil {
		return
	}
	self.wpos += 8

	for _, stream := range self.streams {
		if stream.Type().IsVideo() {
			stream.sample.CompositionOffset = &mp4io.CompositionOffset{}
		}
	}
	self.headerDone = true
	return
}

func (self *Muxer) WritePacket(pkt av.Packet) (err error) {
	if pkt.Idx < 0 || int(pkt.Idx) >= len(self.streams) {
		return fmt.Errorf("mp4: packet stream index %d out of range", pkt.Idx)
	}
	stream := self.streams[pkt.Idx]
	if self.clockMode {
		return stream.writeClockPacket(pkt)
	}
	if stream.lastpkt != nil {
		if err = stream.writePacket(*stream.lastpkt, pkt.Time-stream.lastpkt.Time); err != nil {
			return
		}
	}
	stream.lastpkt = &pkt
	return
}

// writeClockPacket commits a packet in clock mode. In clock mode every packet
// is written as soon as it is submitted; its end is the next packet's time, so
// the pending sample is only tracked to supply that end.
func (self *Stream) writeClockPacket(pkt av.Packet) (err error) {
	if self.lastpkt == nil {
		if pkt.Time != 0 {
			return fmt.Errorf("mp4: stream#%d first packet time=%v must be zero", pkt.Idx, pkt.Time)
		}
	} else {
		if pkt.Time <= self.lastpkt.Time {
			return fmt.Errorf("mp4: stream#%d time=%v is not greater than lasttime=%v",
				pkt.Idx, pkt.Time, self.lastpkt.Time)
		}
		startTs, err := nsToTicksRound(self.lastpkt.Time, self.timeScale)
		if err != nil {
			return err
		}
		endTs, err := nsToTicksRound(pkt.Time, self.timeScale)
		if err != nil {
			return err
		}
		if endTs <= startTs {
			return fmt.Errorf("mp4: stream#%d endpoints %v and %v map to the same clock tick",
				pkt.Idx, self.lastpkt.Time, pkt.Time)
		}
		if err = self.commitClockSample(*self.lastpkt, endTs-startTs); err != nil {
			return err
		}
	}
	self.lastpkt = &pkt
	return nil
}

func (self *Stream) commitClockSample(pkt av.Packet, dur uint64) (err error) {
	if dur > uint64(^uint32(0)) {
		return fmt.Errorf("mp4: stream#%d sample duration %d exceeds uint32", pkt.Idx, dur)
	}

	if _, err = self.muxer.bufw.Write(pkt.Data); err != nil {
		return
	}

	if pkt.IsKeyFrame && self.sample.SyncSample != nil {
		self.sample.SyncSample.Entries = append(self.sample.SyncSample.Entries, uint32(self.sampleIndex+1))
	}

	duration := uint32(dur)
	if self.sttsEntry == nil || duration != self.sttsEntry.Duration {
		self.sample.TimeToSample.Entries = append(self.sample.TimeToSample.Entries,
			mp4io.TimeToSampleEntry{Duration: duration})
		self.sttsEntry = &self.sample.TimeToSample.Entries[len(self.sample.TimeToSample.Entries)-1]
	}
	self.sttsEntry.Count++

	if self.sample.CompositionOffset != nil {
		offset := uint32(self.timeToTs(pkt.CompositionTime))
		if self.cttsEntry == nil || offset != self.cttsEntry.Offset {
			table := self.sample.CompositionOffset
			table.Entries = append(table.Entries, mp4io.CompositionOffsetEntry{Offset: offset})
			self.cttsEntry = &table.Entries[len(table.Entries)-1]
		}
		self.cttsEntry.Count++
	}

	self.duration += int64(duration)
	self.sampleIndex++
	self.sample.ChunkOffset.Entries = append(self.sample.ChunkOffset.Entries, uint32(self.muxer.wpos))
	self.sample.SampleSize.Entries = append(self.sample.SampleSize.Entries, uint32(len(pkt.Data)))

	self.muxer.wpos += int64(len(pkt.Data))
	return
}

func (self *Stream) writePacket(pkt av.Packet, rawdur time.Duration) (err error) {
	if rawdur < 0 {
		err = fmt.Errorf("mp4: stream#%d time=%v < lasttime=%v", pkt.Idx, pkt.Time, self.lastpkt.Time)
		return
	}

	if _, err = self.muxer.bufw.Write(pkt.Data); err != nil {
		return
	}

	if pkt.IsKeyFrame && self.sample.SyncSample != nil {
		self.sample.SyncSample.Entries = append(self.sample.SyncSample.Entries, uint32(self.sampleIndex+1))
	}

	duration := uint32(self.timeToTs(rawdur))
	if self.sttsEntry == nil || duration != self.sttsEntry.Duration {
		self.sample.TimeToSample.Entries = append(self.sample.TimeToSample.Entries, mp4io.TimeToSampleEntry{Duration: duration})
		self.sttsEntry = &self.sample.TimeToSample.Entries[len(self.sample.TimeToSample.Entries)-1]
	}
	self.sttsEntry.Count++

	if self.sample.CompositionOffset != nil {
		offset := uint32(self.timeToTs(pkt.CompositionTime))
		if self.cttsEntry == nil || offset != self.cttsEntry.Offset {
			table := self.sample.CompositionOffset
			table.Entries = append(table.Entries, mp4io.CompositionOffsetEntry{Offset: offset})
			self.cttsEntry = &table.Entries[len(table.Entries)-1]
		}
		self.cttsEntry.Count++
	}

	self.duration += int64(duration)
	self.sampleIndex++
	self.sample.ChunkOffset.Entries = append(self.sample.ChunkOffset.Entries, uint32(self.muxer.wpos))
	self.sample.SampleSize.Entries = append(self.sample.SampleSize.Entries, uint32(len(pkt.Data)))

	self.muxer.wpos += int64(len(pkt.Data))
	return
}

// WriteTrailer flushes a legacy (NewMuxer) file. In clock mode it always fails
// because the caller must supply explicit track ends through WriteTrailerAt.
func (self *Muxer) WriteTrailer() (err error) {
	if self.clockMode {
		return fmt.Errorf("mp4: clock muxer requires WriteTrailerAt")
	}

	for _, stream := range self.streams {
		if stream.lastpkt != nil {
			if err = stream.writePacket(*stream.lastpkt, 0); err != nil {
				return
			}
			stream.lastpkt = nil
		}
	}

	return self.writeTrailer()
}

// WriteTrailerAt finishes a clock muxer. ends holds the absolute end time of
// the last packet on each track, in the same order as WriteHeader. Empty tracks
// must pass zero. Every end must be later than the track's last packet time and
// must quantize to a positive duration. All tracks are validated before any
// buffered data is flushed, so a bad argument can be corrected and retried.
func (self *Muxer) WriteTrailerAt(ends []time.Duration) (err error) {
	if !self.clockMode {
		return fmt.Errorf("mp4: WriteTrailerAt requires a clock muxer")
	}
	if !self.headerDone {
		return fmt.Errorf("mp4: WriteHeader has not been called")
	}

	localEnds := append([]time.Duration(nil), ends...)
	if len(localEnds) != len(self.streams) {
		return fmt.Errorf("mp4: WriteTrailerAt got %d ends for %d streams", len(localEnds), len(self.streams))
	}

	// Validate every track before mutating or flushing anything.
	durs := make([]uint32, len(self.streams))
	for i, stream := range self.streams {
		if stream.lastpkt == nil {
			if localEnds[i] != 0 {
				return fmt.Errorf("mp4: stream#%d is empty, end must be zero (got %v)", i, localEnds[i])
			}
			continue
		}
		end := localEnds[i]
		if end <= stream.lastpkt.Time {
			return fmt.Errorf("mp4: stream#%d end=%v must be later than last packet %v",
				i, end, stream.lastpkt.Time)
		}
		startTs, err := nsToTicksRound(stream.lastpkt.Time, stream.timeScale)
		if err != nil {
			return err
		}
		endTs, err := nsToTicksRound(end, stream.timeScale)
		if err != nil {
			return err
		}
		if endTs <= startTs {
			return fmt.Errorf("mp4: stream#%d end=%v quantizes to a non-positive duration", i, end)
		}
		if d := endTs - startTs; d > uint64(^uint32(0)) {
			return fmt.Errorf("mp4: stream#%d final duration %d exceeds uint32", i, d)
		} else {
			durs[i] = uint32(d)
		}
	}

	for i, stream := range self.streams {
		if stream.lastpkt == nil {
			continue
		}
		if err = stream.commitClockSample(*stream.lastpkt, uint64(durs[i])); err != nil {
			return
		}
		stream.lastpkt = nil
	}

	return self.writeTrailer()
}

func (self *Muxer) writeTrailer() (err error) {
	moov := &mp4io.Movie{}
	moov.Header = &mp4io.MovieHeader{
		PreferredRate:   1,
		PreferredVolume: 1,
		Matrix:          [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
		NextTrackId:     2,
		TimeScale:       int32(movieTimeScale),
	}

	maxMovieDur := uint64(0)
	for _, stream := range self.streams {
		if err = stream.fillTrackAtom(); err != nil {
			return
		}
		// Track duration in the 10000 movie timebase. The legacy path keeps its
		// historical ticks->duration->ticks double truncation so old files stay
		// byte-identical; the clock path converts ticks directly, avoiding the
		// extra rounding step.
		var trackMovieDur uint64
		if self.clockMode {
			trackMovieDur = mulDivFloor(uint64(stream.duration), uint64(movieTimeScale), uint64(stream.timeScale))
		} else {
			dur := time.Duration(stream.duration) * time.Second / time.Duration(stream.timeScale)
			trackMovieDur = uint64(dur * time.Duration(movieTimeScale) / time.Second)
		}
		stream.trackAtom.Header.Duration = trackMovieDur
		stream.trackAtom.Media.Header.Duration = uint64(stream.duration)
		if trackMovieDur > maxMovieDur {
			maxMovieDur = trackMovieDur
		}
		moov.Tracks = append(moov.Tracks, stream.trackAtom)
	}
	moov.Header.Duration = maxMovieDur

	// Select version 1 for each box whose own duration no longer fits 32 bits.
	for _, stream := range self.streams {
		if stream.trackAtom.Media.Header.Duration > uint64(^uint32(0)) {
			stream.trackAtom.Media.Header.Version = 1
		}
		if stream.trackAtom.Header.Duration > uint64(^uint32(0)) {
			stream.trackAtom.Header.Version = 1
		}
	}
	if moov.Header.Duration > uint64(^uint32(0)) {
		moov.Header.Version = 1
	}

	if err = self.bufw.Flush(); err != nil {
		return
	}

	var mdatsize int64
	if mdatsize, err = self.w.Seek(0, 1); err != nil {
		return
	}
	if _, err = self.w.Seek(0, 0); err != nil {
		return
	}
	taghdr := make([]byte, 4)
	pio.PutU32BE(taghdr, uint32(mdatsize))
	if _, err = self.w.Write(taghdr); err != nil {
		return
	}

	if _, err = self.w.Seek(0, 2); err != nil {
		return
	}
	b := make([]byte, moov.Len())
	moov.Marshal(b)
	if _, err = self.w.Write(b); err != nil {
		return
	}

	return
}
