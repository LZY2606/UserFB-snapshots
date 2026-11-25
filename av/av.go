package av

import (
	"fmt"
	"time"

	"github.com/notedit/rtmp/codec/hevc"
	"github.com/notedit/rtmp/codec/opus"

	"github.com/notedit/rtmp/codec/aac"
	"github.com/notedit/rtmp/codec/h264"
)

const (
	H264 = 1 + iota
	AAC
	OPUS
	H264DecoderConfig
	H264SPSPPSNALU
	AACDecoderConfig
	Metadata
	HEVC
	HEVCDecoderConfig
	HEVCSequenceEnd
)

var PacketTypeString = map[int]string{
	H264:              "H264",
	AAC:               "AAC",
	OPUS:              "OPUS",
	H264DecoderConfig: "H264DecoderConfig",
	H264SPSPPSNALU:    "H264SPSPPSNALU",
	AACDecoderConfig:  "AACDecoderConfig",
	Metadata:          "Metadata",
	HEVC:              "HEVC",
	HEVCDecoderConfig: "HEVCDecoderConfig",
	HEVCSequenceEnd:   "HEVCSequenceEnd",
}

type Packet struct {
	Type       int
	IsKeyFrame bool
	CTime      time.Duration
	Time       time.Duration
	Data       []byte
	ASeqHdr    []byte
	VSeqHdr    []byte
	Metadata   []byte

	// TrackID is the Enhanced RTMP video track id. Legacy single-track
	// messages use zero.
	TrackID uint8

	// HEVC is an immutable-by-convention private snapshot of the
	// HEVCDecoderConfigurationRecord active when the packet was read.
	HEVC *hevc.Codec

	// HEVCConfig is an independent copy of the active configuration record.
	HEVCConfig []byte

	OPUS *opus.Codec
	AAC  *aac.Codec
	H264 *h264.Codec
}

func (p Packet) String() string {
	ret := ""

	typeStr := PacketTypeString[p.Type]
	if typeStr == "" {
		typeStr = "UnknownPacketType"
	}
	ret += typeStr

	if p.IsKeyFrame {
		ret += " K"
	}

	ret += " " + fmt.Sprint(p.Time)

	if p.CTime != 0 {
		ret += " " + fmt.Sprint(p.CTime)
	}

	ret += " " + fmt.Sprint(len(p.Data))

	return ret
}

type PacketReader interface {
	ReadPacket() (Packet, error)
}

type PacketReadCloser interface {
	PacketReader
	Close() error
}

type PacketWriter interface {
	WritePacket(Packet) error
}

type PacketWriteCloser interface {
	PacketWriter
	Close() error
}
