package mp4

import (
	"fmt"
	"time"
	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/codec/aacparser"
	"github.com/nareix/joy4/codec/h264parser"
	"github.com/nareix/joy4/format/mp4/mp4io"
	"github.com/nareix/joy4/utils/bits/pio"
	"io"
	"bufio"
)

const (
	// movieTimeScale is the mvhd time scale, kept identical to the legacy muxer.
	movieTimeScale int64 = 10000
	minClockTimeScale uint32 = 1
	maxClockTimeScale uint32 = 1000000000
)

type Muxer struct {
	w          io.WriteSeeker
	bufw       *bufio.Writer
	wpos       int64
	streams    []*Stream

	// clock mode: sample timing is derived from absolute packet times and
	// explicit per-track end times instead of adjacent packet deltas.
	clock       bool
	clockScales []uint32
}

func NewMuxer(w io.WriteSeeker) *Muxer {
	return &Muxer{
		w: w,
		bufw: bufio.NewWriterSize(w, pio.RecommendBufioSize),
	}
}

// NewClockMuxer creates a muxer that records sample timing against a precise
// per-track sample clock. timescales is copied and must line up, in order,
// with the CodecData slice passed to WriteHeader; each entry must be in
// [1, 1000000000]. The checks are performed by WriteHeader, before any file
// header byte is written.
func NewClockMuxer(w io.WriteSeeker, timescales []uint32) *Muxer {
	scales := make([]uint32, len(timescales))
	copy(scales, timescales)
	return &Muxer{
		w:           w,
		bufw:        bufio.NewWriterSize(w, pio.RecommendBufioSize),
		clock:       true,
		clockScales: scales,
	}
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
			TrackId:  int32(len(self.streams)+1),
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

	stream.timeScale = 90000
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
			SubType: [4]byte{'v','i','d','e'},
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
			SubType: [4]byte{'s','o','u','n'},
			Name:    []byte("Sound Handler"),
		}
		self.trackAtom.Media.Info.Sound = &mp4io.SoundMediaInfo{}

	} else {
		err = fmt.Errorf("mp4: codec type=%d invalid", self.Type())
	}

	return
}

func (self *Muxer) WriteHeader(streams []av.CodecData) (err error) {
	if self.clock {
		if len(streams) != len(self.clockScales) {
			err = fmt.Errorf("mp4: clock muxer got %d streams but %d timescales",
				len(streams), len(self.clockScales))
			return
		}
		for i, scale := range self.clockScales {
			if scale < minClockTimeScale || scale > maxClockTimeScale {
				err = fmt.Errorf("mp4: clock muxer timescale[%d]=%d out of range [%d,%d]",
					i, scale, minClockTimeScale, maxClockTimeScale)
				return
			}
		}
	}

	self.streams = []*Stream{}
	for _, stream := range streams {
		if err = self.newStream(stream); err != nil {
			self.streams = []*Stream{}
			return
		}
	}
	if self.clock {
		for i, scale := range self.clockScales {
			self.streams[i].timeScale = int64(scale)
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
	return
}

func (self *Muxer) WritePacket(pkt av.Packet) (err error) {
	if self.clock {
		return self.writeClockPacket(pkt)
	}

	stream := self.streams[pkt.Idx]
	if stream.lastpkt != nil {
		if err = stream.writePacket(*stream.lastpkt, pkt.Time-stream.lastpkt.Time); err != nil {
			return
		}
	}
	stream.lastpkt = &pkt
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
	self.appendSample(pkt, duration)

	self.duration += int64(duration)
	self.muxer.wpos += int64(len(pkt.Data))
	return
}

// appendSample records one fully resolved sample against the sample table.
// The sample duration is already expressed in track ticks.
func (self *Stream) appendSample(pkt av.Packet, duration uint32) {
	self.appendSampleTiming(pkt, duration)

	self.sampleIndex++
	self.sample.ChunkOffset.Entries = append(self.sample.ChunkOffset.Entries, uint32(self.muxer.wpos))
	self.sample.SampleSize.Entries = append(self.sample.SampleSize.Entries, uint32(len(pkt.Data)))
}

// appendSampleTiming appends the stts/ctts run entries for one resolved
// sample without touching chunk offsets or sizes, which in clock mode are
// already recorded at packet time.
func (self *Stream) appendSampleTiming(pkt av.Packet, duration uint32) {
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
}

func (self *Muxer) WriteTrailer() (err error) {
	if self.clock {
		err = fmt.Errorf("mp4: clock muxer requires WriteTrailerAt with per-track end times")
		return
	}

	for _, stream := range self.streams {
		if stream.lastpkt != nil {
			if err = stream.writePacket(*stream.lastpkt, 0); err != nil {
				return
			}
			stream.lastpkt = nil
		}
	}

	return self.writeMovieAtom(false)
}

// writeMovieAtom flushes buffered sample data, patches the mdat size and
// appends the moov atom. In clock mode the stts totals, mdhd/tkhd/mvhd
// durations are cross-checked against the quantized sample endpoints; in
// legacy mode the historical double time conversion is preserved exactly.
func (self *Muxer) writeMovieAtom(clockMode bool) (err error) {
	moov := &mp4io.Movie{}
	moov.Header = &mp4io.MovieHeader{
		PreferredRate:   1,
		PreferredVolume: 1,
		Matrix:          [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
		NextTrackId:     2,
	}

	maxMovieDur := uint64(0)
	for _, stream := range self.streams {
		if err = stream.fillTrackAtom(); err != nil {
			return
		}

		mdhd := stream.trackAtom.Media.Header
		tkhd := stream.trackAtom.Header

		if !clockMode {
			// Legacy path: keep the historical double conversion so that
			// NewMuxer output remains byte-for-byte compatible.
			mdhd.Duration = uint64(stream.duration)
			dur := stream.tsToTime(uint64(stream.duration))
			movieDur := uint64(timeToTs(dur, movieTimeScale))
			tkhd.Duration = movieDur
			if movieDur > maxMovieDur {
				maxMovieDur = movieDur
			}
			moov.Tracks = append(moov.Tracks, stream.trackAtom)
			continue
		}

		trackTicks := uint64(stream.clockEndTicks)

		// stts totals, mdhd duration, tkhd duration and mvhd duration all
		// come from the same set of actual sample endpoints.
		var sttsTotal uint64
		for _, entry := range stream.sample.TimeToSample.Entries {
			sttsTotal += uint64(entry.Count) * uint64(entry.Duration)
		}
		if sttsTotal != trackTicks {
			err = fmt.Errorf("mp4: stream#%d stts total %d != track duration %d",
				stream.idx, sttsTotal, trackTicks)
			return
		}

		mdhd.Duration = trackTicks
		if trackTicks > uint64(^uint32(0)) {
			mdhd.Version = 1
		}

		movieDur, derr := mulDivFloor(trackTicks, uint64(movieTimeScale), uint64(stream.timeScale))
		if derr != nil {
			err = derr
			return
		}
		tkhd.Duration = movieDur
		if movieDur > uint64(^uint32(0)) {
			tkhd.Version = 1
		}
		if movieDur > maxMovieDur {
			maxMovieDur = movieDur
		}
		moov.Tracks = append(moov.Tracks, stream.trackAtom)
	}

	moov.Header.TimeScale = int32(movieTimeScale)
	if clockMode {
		moov.Header.Duration = maxMovieDur
		if maxMovieDur > uint64(^uint32(0)) {
			moov.Header.Version = 1
		}
	} else {
		// Legacy movie duration: the same double conversion as before.
		maxDur := time.Duration(0)
		for _, stream := range self.streams {
			dur := stream.tsToTime(uint64(stream.duration))
			if dur > maxDur {
				maxDur = dur
			}
		}
		moov.Header.Duration = uint64(timeToTs(maxDur, movieTimeScale))
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
