package flv

import (
	"fmt"
	"time"

	"github.com/notedit/rtmp/av"
	"github.com/notedit/rtmp/codec/hevc"
	"github.com/notedit/rtmp/format/flv/flvio"
)

// PacketDecoder owns the multitrack read queue and the per-track HEVC
// configuration state. A zero value is not usable; use NewPacketDecoder.
type PacketDecoder struct {
	pending []av.Packet
	configs map[uint8]*hevc.Codec
}

func NewPacketDecoder() *PacketDecoder {
	return &PacketDecoder{configs: make(map[uint8]*hevc.Codec)}
}

// ReadPacket returns one configuration or coded packet from one track. A
// multitrack FLV/RTMP message is parsed atomically; additional packets are
// queued and returned by the following calls before another tag is read.
func (d *PacketDecoder) ReadPacket(readTag func() (flvio.Tag, error)) (pkt av.Packet, err error) {
	if d == nil {
		d = NewPacketDecoder()
	}
	if d.configs == nil {
		d.configs = make(map[uint8]*hevc.Codec)
	}

	if len(d.pending) != 0 {
		pkt = d.pending[0]
		d.pending = d.pending[1:]
		return pkt, nil
	}

	for {
		var tag flvio.Tag
		if tag, err = readTag(); err != nil {
			return av.Packet{}, err
		}

		var pkts []av.Packet
		if pkts, err = d.parseTag(tag); err != nil {
			return av.Packet{}, err
		}
		if len(pkts) == 0 {
			continue
		}

		d.pending = append(d.pending, pkts[1:]...)
		return pkts[0], nil
	}
}

func (d *PacketDecoder) parseTag(tag flvio.Tag) ([]av.Packet, error) {
	if tag.Type == flvio.TAG_VIDEO && isEnhancedVideoTag(tag) {
		return d.parseEnhancedVideoTag(tag)
	}
	return parseLegacyTag(tag)
}

func isEnhancedVideoTag(tag flvio.Tag) bool {
	if tag.Enhanced {
		return true
	}
	if len(tag.Header) > 0 && tag.Header[0]&0x80 != 0 {
		return true
	}
	return len(tag.Data) > 0 && tag.Data[0]&0x80 != 0
}

func fullVideoTagPayload(tag flvio.Tag) []byte {
	if len(tag.Header) > 0 {
		out := make([]byte, 0, len(tag.Header)+len(tag.Data))
		out = append(out, tag.Header...)
		out = append(out, tag.Data...)
		return out
	}
	if tag.Enhanced && tag.Type == flvio.TAG_VIDEO {
		header := make([]byte, tag.MaxHeaderLen())
		n := tag.FillHeader(header)
		out := make([]byte, 0, n+len(tag.Data))
		out = append(out, header[:n]...)
		out = append(out, tag.Data...)
		return out
	}
	return tag.Data
}

func copyBytes(b []byte) []byte {
	if b == nil {
		return nil
	}
	return append([]byte(nil), b...)
}

func enhancedError(format string, args ...interface{}) error {
	return fmt.Errorf("enhanced RTMP video: "+format, args...)
}

// parseEnhancedVideoTag validates the complete message before mutating the
// active configuration map or returning any packet.
func (d *PacketDecoder) parseEnhancedVideoTag(tag flvio.Tag) ([]av.Packet, error) {
	raw := fullVideoTagPayload(tag)
	if len(raw) < 1 {
		return nil, enhancedError("empty header")
	}

	first := raw[0]
	frameType := (first >> 4) & 0x07
	packetType := first & 0x0f
	dts := flvio.TsToTime(int64(tag.Time))

	// Work on a copy. A malformed later member must not leave configuration
	// updates from an earlier member half applied.
	nextConfigs := make(map[uint8]*hevc.Codec, len(d.configs)+1)
	for track, cfg := range d.configs {
		nextConfigs[track] = cfg
	}

	var pkts []av.Packet

	parseOne := func(track uint8, innerPacketType uint8, content []byte) error {
		pkt, deliver, err := parseEnhancedTrackContent(nextConfigs, track, innerPacketType, content, frameType, dts)
		if err != nil {
			return err
		}
		if deliver {
			pkts = append(pkts, pkt)
		}
		return nil
	}

	switch packetType {
	case flvio.EX_PACKET_MULTITRACK:
		if len(raw) < 6 {
			return nil, enhancedError("truncated multitrack/FourCC header (%d bytes)", len(raw))
		}
		multitrackInfo := raw[1]
		mode := multitrackInfo >> 4
		innerPacketType := multitrackInfo & 0x0f
		fourcc := string(raw[2:6])
		if fourcc != flvio.ExFourCCHEVC {
			return nil, enhancedError("unknown FourCC %q", fourcc)
		}
		pos := 6

		switch mode {
		case flvio.EX_MULTITRACK_ONE_TRACK:
			if pos >= len(raw) {
				return nil, enhancedError("OneTrack is missing track id")
			}
			track := raw[pos]
			pos++
			if err := parseOne(track, innerPacketType, raw[pos:]); err != nil {
				return nil, err
			}

		case flvio.EX_MULTITRACK_MANY_TRACKS:
			for pos < len(raw) {
				if len(raw)-pos < 4 {
					return nil, enhancedError("ManyTracks item header truncated at offset %d", pos)
				}
				track := raw[pos]
				itemLen := int(pioU24(raw[pos+1 : pos+4]))
				contentStart := pos + 4
				contentEnd := contentStart + itemLen
				if contentEnd > len(raw) || contentEnd < contentStart {
					return nil, enhancedError("ManyTracks item length out of bounds(track=%d,length=%d,offset=%d,message=%d)", track, itemLen, pos, len(raw))
				}
				if err := parseOne(track, innerPacketType, raw[contentStart:contentEnd]); err != nil {
					return nil, err
				}
				pos = contentEnd
			}

		default:
			return nil, enhancedError("unsupported multitrack mode %d", mode)
		}

	case flvio.EX_PACKET_SEQUENCE_START,
		flvio.EX_PACKET_CODED_FRAMES,
		flvio.EX_PACKET_SEQUENCE_END,
		flvio.EX_PACKET_CODED_FRAMES_X:
		if len(raw) < 5 {
			return nil, enhancedError("truncated FourCC header for packet type %d", packetType)
		}
		fourcc := string(raw[1:5])
		if fourcc != flvio.ExFourCCHEVC {
			return nil, enhancedError("unknown FourCC %q", fourcc)
		}
		if err := parseOne(0, packetType, raw[5:]); err != nil {
			return nil, err
		}

	default:
		return nil, enhancedError("unsupported enhanced packet type %d", packetType)
	}

	// Commit only after every track member has passed structural and
	// configuration-association validation.
	d.configs = nextConfigs
	return pkts, nil
}

func pioU24(b []byte) uint32 {
	return uint32(b[0])<<16 | uint32(b[1])<<8 | uint32(b[2])
}

func parseEnhancedTrackContent(
	configs map[uint8]*hevc.Codec,
	track uint8,
	packetType uint8,
	content []byte,
	frameType uint8,
	dts time.Duration,
) (pkt av.Packet, deliver bool, err error) {
	switch packetType {
	case flvio.EX_PACKET_SEQUENCE_START:
		cfg, perr := hevc.ParseConfig(content)
		if perr != nil {
			return av.Packet{}, false, enhancedError("track %d: %w", track, perr)
		}
		configs[track] = cfg

		configSnapshot := cfg.Clone()
		pkt = av.Packet{
			Type:       av.HEVCDecoderConfig,
			Time:       dts,
			TrackID:    track,
			Data:       copyBytes(cfg.Raw),
			HEVC:       configSnapshot,
			HEVCConfig: copyBytes(configSnapshot.Raw),
		}
		return pkt, true, nil

	case flvio.EX_PACKET_SEQUENCE_END:
		if len(content) != 0 {
			return av.Packet{}, false, enhancedError("track %d: SequenceEnd has %d trailing bytes", track, len(content))
		}
		delete(configs, track)
		return av.Packet{}, false, nil

	case flvio.EX_PACKET_CODED_FRAMES, flvio.EX_PACKET_CODED_FRAMES_X:
		var cts time.Duration
		nalu := content
		if packetType == flvio.EX_PACKET_CODED_FRAMES {
			if len(content) < 3 {
				return av.Packet{}, false, enhancedError("track %d: truncated composition time", track)
			}
			cts = flvio.TsToTime(int64(pioI24(content[0:3])))
			nalu = content[3:]
		}

		cfg, ok := configs[track]
		if !ok || cfg == nil {
			return av.Packet{}, false, enhancedError("track %d: coded frame before SequenceStart", track)
		}
		if verr := hevc.ValidateNALUnits(nalu, cfg.NALLengthSize); verr != nil {
			return av.Packet{}, false, enhancedError("track %d: %w", track, verr)
		}

		snapshot := cfg.Clone()
		pkt = av.Packet{
			Type:       av.HEVC,
			IsKeyFrame: frameType == flvio.FRAME_KEY,
			CTime:      cts,
			Time:       dts,
			TrackID:    track,
			Data:       copyBytes(nalu),
			HEVC:       snapshot,
			HEVCConfig: copyBytes(snapshot.Raw),
			VSeqHdr:    copyBytes(snapshot.Raw),
		}
		return pkt, true, nil

	default:
		return av.Packet{}, false, enhancedError("track %d: unsupported inner packet type %d", track, packetType)
	}
}

func pioI24(b []byte) int32 {
	v := int32(int8(b[0]))
	v <<= 8
	v |= int32(b[1])
	v <<= 8
	v |= int32(b[2])
	return v
}

// HEVCVideoTag builds the FLV/RTMP video tag header and data for an HEVC
// packet. Track zero uses a plain enhanced header; nonzero tracks use
// OneTrack.
func HEVCVideoTag(pkt av.Packet) (flvio.Tag, error) {
	innerType := flvio.EX_PACKET_CODED_FRAMES
	frameType := flvio.FRAME_INTER
	data := pkt.Data

	switch pkt.Type {
	case av.HEVCDecoderConfig:
		innerType = flvio.EX_PACKET_SEQUENCE_START
		frameType = flvio.FRAME_KEY
	case av.HEVCSequenceEnd:
		innerType = flvio.EX_PACKET_SEQUENCE_END
		frameType = flvio.FRAME_KEY
	case av.HEVC:
		innerType = flvio.EX_PACKET_CODED_FRAMES
		if pkt.IsKeyFrame {
			frameType = flvio.FRAME_KEY
		}
	default:
		return flvio.Tag{}, fmt.Errorf("not an HEVC packet type: %d", pkt.Type)
	}

	tag := flvio.Tag{
		Type:          flvio.TAG_VIDEO,
		Time:          uint32(flvio.TimeToTs(pkt.Time)),
		FrameType:     uint8(frameType),
		Enhanced:      true,
		ExFourCC:      flvio.ExFourCCHEVC,
		ExTrackID:     pkt.TrackID,
		ExInnerPacket: uint8(innerType),
		Data:          data,
	}
	if pkt.TrackID == 0 {
		tag.ExPacketType = uint8(innerType)
		tag.ExMultitrack = false
	} else {
		tag.ExPacketType = flvio.EX_PACKET_MULTITRACK
		tag.ExMultitrack = true
		tag.ExManyTracks = false
	}
	if innerType == flvio.EX_PACKET_CODED_FRAMES {
		tag.CTime = int32(flvio.TimeToTs(pkt.CTime))
	}
	return tag, nil
}
