package flv

import (
	"fmt"

	"github.com/notedit/rtmp/av"
	"github.com/notedit/rtmp/codec/hevc"
	"github.com/notedit/rtmp/format/flv/flvio"
	"github.com/notedit/rtmp/utils/bits/pio"
)

// EnhancedPacketReader turns enhanced RTMP v2 video tags into av.Packets.
// One reader owns the track-to-configuration state of exactly one FLV
// demuxer or one RTMP connection.
type EnhancedPacketReader struct {
	readTag func() (flvio.Tag, error)

	// tracks is the committed configuration state. Values are never
	// mutated in place: a SequenceStart replaces the pointer, a
	// SequenceEnd removes it.
	tracks map[uint8]*hevc.Codec

	// queue holds the remaining packets of an enhanced message that
	// carried more than one track.
	queue []av.Packet
}

// NewEnhancedPacketReader creates a reader that pulls tags through readTag.
func NewEnhancedPacketReader(readTag func() (flvio.Tag, error)) *EnhancedPacketReader {
	return &EnhancedPacketReader{
		readTag: readTag,
		tracks:  map[uint8]*hevc.Codec{},
	}
}

type enhancedEntry struct {
	trackID    uint8
	packetType uint8
	content    []byte
}

// ReadPacket returns the next packet. When an enhanced message contains
// several tracks, the remaining tracks are queued and returned by the
// following calls before another message is read.
func (r *EnhancedPacketReader) ReadPacket() (pkt av.Packet, err error) {
	if len(r.queue) > 0 {
		pkt = r.queue[0]
		r.queue = r.queue[1:]
		return
	}

	for {
		var tag flvio.Tag
		if tag, err = r.readTag(); err != nil {
			return
		}

		if tag.Type != flvio.TAG_VIDEO || !tag.Enhanced {
			var ok bool
			if pkt, ok = legacyPacket(tag); ok {
				return
			}
			continue
		}

		var pkts []av.Packet
		if pkts, err = r.parseEnhancedVideo(tag); err != nil {
			return
		}
		pkt = pkts[0]
		r.queue = pkts[1:]
		return
	}
}

// parseEnhancedVideo fully validates one enhanced message, commits the
// configuration changes only when every member is valid, and returns the
// packets in member order.
func (r *EnhancedPacketReader) parseEnhancedVideo(tag flvio.Tag) (pkts []av.Packet, err error) {
	var hdrn int
	var frameType, packetType uint8
	var fourcc string
	var mtMode, innerPacketType uint8
	if hdrn, frameType, packetType, fourcc, mtMode, innerPacketType, err =
		flvio.ParseEnhancedVideoHeader(tag.EnhancedHeader); err != nil {
		return
	}
	if hdrn != len(tag.EnhancedHeader) {
		err = fmt.Errorf("EnhancedVideoHeaderLengthInvalid(got=%d,want=%d)", len(tag.EnhancedHeader), hdrn)
		return
	}
	if fourcc != flvio.FourCCHVC1 {
		err = fmt.Errorf("EnhancedVideoFourCCUnknown(%q)", fourcc)
		return
	}

	entries, err := splitEnhancedEntries(tag.Data, packetType, mtMode, innerPacketType)
	if err != nil {
		return
	}

	// Stage every configuration change on a copy first. The committed
	// state is only replaced once the whole message has passed both the
	// structural and the configuration association checks.
	staging := make(map[uint8]*hevc.Codec, len(r.tracks)+1)
	for id, codec := range r.tracks {
		staging[id] = codec
	}

	type resolved struct {
		entry enhancedEntry
		codec *hevc.Codec
		ctime int32
		frame []byte
	}
	rs := make([]resolved, 0, len(entries))

	isKey := frameType == flvio.FRAME_KEY
	for _, entry := range entries {
		rv := resolved{entry: entry}

		switch entry.packetType {
		case flvio.EnhancedSequenceStart:
			var codec *hevc.Codec
			if codec, err = hevc.ParseDecoderConfig(entry.content); err != nil {
				return
			}
			staging[entry.trackID] = codec
			rv.codec = codec

		case flvio.EnhancedSequenceEnd:
			if len(entry.content) != 0 {
				err = fmt.Errorf("EnhancedSequenceEndBodyInvalid(track=%d,len=%d)", entry.trackID, len(entry.content))
				return
			}
			delete(staging, entry.trackID)

		case flvio.EnhancedCodedFrames:
			if len(entry.content) < 3 {
				err = fmt.Errorf("EnhancedCodedFramesCTimeTruncated(track=%d,len=%d)", entry.trackID, len(entry.content))
				return
			}
			rv.ctime = pio.I24BE(entry.content[:3])
			rv.frame = entry.content[3:]
			rv.codec = staging[entry.trackID]
			if rv.codec == nil {
				err = fmt.Errorf("EnhancedFrameTrackNotConfigured(track=%d)", entry.trackID)
				return
			}
			if err = rv.codec.CheckFrame(rv.frame); err != nil {
				return
			}

		case flvio.EnhancedCodedFramesX:
			rv.ctime = 0
			rv.frame = entry.content
			rv.codec = staging[entry.trackID]
			if rv.codec == nil {
				err = fmt.Errorf("EnhancedFrameTrackNotConfigured(track=%d)", entry.trackID)
				return
			}
			if err = rv.codec.CheckFrame(rv.frame); err != nil {
				return
			}

		default:
			err = fmt.Errorf("EnhancedPacketTypeUnsupported(%d)", entry.packetType)
			return
		}

		rs = append(rs, rv)
	}

	// All checks passed: commit the staged configuration atomically.
	r.tracks = staging

	for _, rv := range rs {
		pkt := av.Packet{
			Time:       flvio.TsToTime(int64(tag.Time)),
			IsKeyFrame: isKey,
			TrackID:    rv.entry.trackID,
		}

		switch rv.entry.packetType {
		case flvio.EnhancedSequenceStart:
			pkt.Type = av.HEVCDecoderConfig
			pkt.Data = append([]byte(nil), rv.entry.content...)
			pkt.HEVC = rv.codec.Snapshot()

		case flvio.EnhancedSequenceEnd:
			pkt.Type = av.HEVCEndOfSequence

		case flvio.EnhancedCodedFrames, flvio.EnhancedCodedFramesX:
			pkt.Type = av.HEVC
			pkt.Data = append([]byte(nil), rv.frame...)
			pkt.CTime = flvio.TsToTime(int64(rv.ctime))
			pkt.HEVC = rv.codec.Snapshot()
		}

		pkts = append(pkts, pkt)
	}
	return
}

func splitEnhancedEntries(data []byte, outerPacketType, mtMode, innerPacketType uint8) (entries []enhancedEntry, err error) {
	if outerPacketType == flvio.EnhancedMultitrack {
		switch mtMode {
		case flvio.EnhancedOneTrack:
			if len(data) < 1 {
				err = fmt.Errorf("EnhancedOneTrackIDTruncated")
				return
			}
			trackID := data[0]
			entries = []enhancedEntry{{
				trackID:    trackID,
				packetType: innerPacketType,
				content:    data[1:],
			}}
			return

		case flvio.EnhancedManyTracks:
			n := 0
			for n < len(data) {
				if len(data)-n < 4 {
					err = fmt.Errorf("EnhancedManyTracksItemHeaderTruncated(left=%d)", len(data)-n)
					return
				}
				trackID := data[n]
				itemLen := int(pio.U24BE(data[n+1:]))
				n += 4
				if itemLen > len(data)-n {
					err = fmt.Errorf("EnhancedManyTracksItemOutOfBounds(track=%d,len=%d,left=%d)", trackID, itemLen, len(data)-n)
					return
				}
				entries = append(entries, enhancedEntry{
					trackID:    trackID,
					packetType: innerPacketType,
					content:    data[n : n+itemLen],
				})
				n += itemLen
			}
			if len(entries) == 0 {
				err = fmt.Errorf("EnhancedManyTracksEmpty")
			}
			return

		default:
			err = fmt.Errorf("EnhancedMultitrackModeUnsupported(%d)", mtMode)
			return
		}
	}

	switch outerPacketType {
	case flvio.EnhancedSequenceStart, flvio.EnhancedCodedFrames,
		flvio.EnhancedSequenceEnd, flvio.EnhancedCodedFramesX:
	default:
		err = fmt.Errorf("EnhancedPacketTypeUnsupported(%d)", outerPacketType)
		return
	}

	entries = []enhancedEntry{{
		trackID:    0,
		packetType: outerPacketType,
		content:    data,
	}}
	return
}

// legacyPacket converts a non-enhanced tag exactly like the historical FLV
// reader. Unknown tags report ok=false so the caller reads the next tag.
func legacyPacket(tag flvio.Tag) (pkt av.Packet, ok bool) {
	switch tag.Type {
	case flvio.TAG_AMF0, flvio.TAG_AMF3:
		data := convertToAMF0Metadata(tag.Data, tag.Type == flvio.TAG_AMF3)
		if data != nil {
			return av.Packet{
				Type: av.Metadata,
				Data: data,
				Time: flvio.TsToTime(int64(tag.Time)),
			}, true
		}

	case flvio.TAG_VIDEO:
		switch tag.VideoFormat {
		case flvio.VIDEO_H264:
			switch tag.AVCPacketType {
			case flvio.AVC_SEQHDR:
				return av.Packet{Type: av.H264DecoderConfig, Data: tag.Data}, true
			case flvio.AVC_NALU:
				return av.Packet{
					Type:       av.H264,
					Data:       tag.Data,
					Time:       flvio.TsToTime(int64(tag.Time)),
					CTime:      flvio.TsToTime(int64(tag.CTime)),
					IsKeyFrame: tag.FrameType == flvio.FRAME_KEY,
				}, true
			}
		}

	case flvio.TAG_AUDIO:
		switch tag.SoundFormat {
		case flvio.SOUND_OPUS:
			return av.Packet{
				Type: av.OPUS,
				Data: tag.Data,
				Time: flvio.TsToTime(int64(tag.Time)),
			}, true
		case flvio.SOUND_AAC:
			switch tag.AACPacketType {
			case flvio.AAC_SEQHDR:
				return av.Packet{Type: av.AACDecoderConfig, Data: tag.Data}, true
			case flvio.AAC_RAW:
				return av.Packet{
					Type: av.AAC,
					Data: tag.Data,
					Time: flvio.TsToTime(int64(tag.Time)),
				}, true
			}
		}
	}
	return
}

// enhancedVideoTag builds the enhanced RTMP v2 video tag for one HEVC packet.
// Track 0 uses a plain enhanced message, non-zero tracks use OneTrack.
func enhancedVideoTag(pkt av.Packet) (tag flvio.Tag, err error) {
	frameType := uint8(flvio.FRAME_INTER)
	if pkt.IsKeyFrame {
		frameType = flvio.FRAME_KEY
	}

	tag = flvio.Tag{
		Type: flvio.TAG_VIDEO,
		Time: uint32(flvio.TimeToTs(pkt.Time)),
	}

	appendHdr := func(b []byte) []byte {
		tag.EnhancedHeader = append(tag.EnhancedHeader, b...)
		return tag.EnhancedHeader
	}

	switch pkt.Type {
	case av.HEVCDecoderConfig:
		if pkt.TrackID == 0 {
			appendHdr([]byte{flvio.EnhancedVideoMarker | frameType<<4 | flvio.EnhancedSequenceStart})
			appendHdr([]byte(flvio.FourCCHVC1))
		} else {
			appendHdr([]byte{flvio.EnhancedVideoMarker | frameType<<4 | flvio.EnhancedMultitrack})
			appendHdr([]byte(flvio.FourCCHVC1))
			appendHdr([]byte{flvio.EnhancedOneTrack<<4 | flvio.EnhancedSequenceStart})
			appendHdr([]byte{pkt.TrackID})
		}
		tag.Data = pkt.Data

	case av.HEVC:
		buf := make([]byte, 0, 3+len(pkt.Data))
		var b3 [3]byte
		pio.PutI24BE(b3[:], int32(flvio.TimeToTs(pkt.CTime)))
		buf = append(buf, b3[:]...)
		buf = append(buf, pkt.Data...)

		if pkt.TrackID == 0 {
			appendHdr([]byte{flvio.EnhancedVideoMarker | frameType<<4 | flvio.EnhancedCodedFrames})
			appendHdr([]byte(flvio.FourCCHVC1))
		} else {
			appendHdr([]byte{flvio.EnhancedVideoMarker | frameType<<4 | flvio.EnhancedMultitrack})
			appendHdr([]byte(flvio.FourCCHVC1))
			appendHdr([]byte{flvio.EnhancedOneTrack<<4 | flvio.EnhancedCodedFrames})
			appendHdr([]byte{pkt.TrackID})
		}
		tag.Data = buf

	case av.HEVCEndOfSequence:
		if pkt.TrackID == 0 {
			appendHdr([]byte{flvio.EnhancedVideoMarker | frameType<<4 | flvio.EnhancedSequenceEnd})
			appendHdr([]byte(flvio.FourCCHVC1))
		} else {
			appendHdr([]byte{flvio.EnhancedVideoMarker | frameType<<4 | flvio.EnhancedMultitrack})
			appendHdr([]byte(flvio.FourCCHVC1))
			appendHdr([]byte{flvio.EnhancedOneTrack<<4 | flvio.EnhancedSequenceEnd})
			appendHdr([]byte{pkt.TrackID})
		}

	default:
		err = fmt.Errorf("NotAnEnhancedHEVCPacket(%d)", pkt.Type)
		return
	}

	tag.Enhanced = true
	return
}
