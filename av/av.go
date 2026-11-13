package av

import (
	"fmt"
	"github.com/notedit/rtmp/codec/hevc"
	"github.com/notedit/rtmp/codec/opus"
	"time"

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
	// HEVCDecoderConfig is one HEVCDecoderConfigurationRecord (SequenceStart).
	HEVCDecoderConfig
	// HEVC is one length-prefixed HEVC frame (CodedFrames/CodedFramesX).
	HEVC
	// HEVCEndOfSequence ends the configuration state of one track (SequenceEnd).
	HEVCEndOfSequence
)

var PacketTypeString = map[int]string{
	H264:              "H264",
	AAC:               "AAC",
	OPUS:              "OPUS",
	H264DecoderConfig: "H264DecoderConfig",
	H264SPSPPSNALU:    "H264SPSPPSNALU",
	AACDecoderConfig:  "AACDecoderConfig",
	Metadata:          "Metadata",
	HEVCDecoderConfig: "HEVCDecoderConfig",
	HEVC:              "HEVC",
	HEVCEndOfSequence: "HEVCEndOfSequence",
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
	OPUS       *opus.Codec
	AAC        *aac.Codec
	H264       *h264.Codec

	// TrackID identifies the HEVC track of an enhanced RTMP message.
	// It is zero for the legacy AVC/AAC/script packets and for track 0.
	TrackID uint8
	// HEVC is the independent configuration snapshot bound to an HEVC packet
	// at the moment it is handed out. Later configuration switches on the
	// same track, or reuse of the input buffer, never mutate it. It is set
	// on HEVCDecoderConfig and HEVC packets and nil for everything else.
	HEVC *hevc.Codec
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
