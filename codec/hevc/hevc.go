package hevc

import (
	"fmt"

	"github.com/notedit/rtmp/utils/bits/pio"
)

// HEVCDecoderConfigurationRecord fixed header size, up to and including
// the numOfArrays byte (ISO/IEC 14496-15 8.3.3.1.2).
const hvccFixedHeaderSize = 23

// Codec is the parsed state of one HEVC track. Every track keeps its own
// instance and a frame may only be validated against the track that owns it.
type Codec struct {
	// ConfigBytes is an independent copy of the full
	// HEVCDecoderConfigurationRecord.
	ConfigBytes []byte

	// NALLengthSize is the width in bytes of the length prefix in front of
	// every NAL unit of this track. Legal values are 1, 2 and 4.
	NALLengthSize int
}

// Snapshot returns an independent copy of the codec. Later configuration
// switches on the same track can never mutate an earlier snapshot.
func (c *Codec) Snapshot() *Codec {
	if c == nil {
		return nil
	}
	cp := &Codec{NALLengthSize: c.NALLengthSize}
	if c.ConfigBytes != nil {
		cp.ConfigBytes = append([]byte(nil), c.ConfigBytes...)
	}
	return cp
}

// ParseDecoderConfig validates an HEVCDecoderConfigurationRecord and returns
// the per-track codec state. The compressed NAL content is never decoded, the
// original bytes are preserved.
func ParseDecoderConfig(b []byte) (c *Codec, err error) {
	if len(b) < hvccFixedHeaderSize {
		return nil, fmt.Errorf("HEVCDecoderConfigTruncated(got=%d,want>=%d)", len(b), hvccFixedHeaderSize)
	}

	if b[0] != 1 {
		return nil, fmt.Errorf("HEVCDecoderConfigVersionInvalid(%d)", b[0])
	}

	// Reserved fixed-pattern bits of the record.
	if b[13]&0xf0 != 0xf0 {
		return nil, fmt.Errorf("HEVCDecoderConfigSpatialSegmentationReservedInvalid(%#x)", b[13])
	}
	if b[15]&0xfc != 0xfc {
		return nil, fmt.Errorf("HEVCDecoderConfigParallelismReservedInvalid(%#x)", b[15])
	}
	if b[16]&0xfc != 0xfc {
		return nil, fmt.Errorf("HEVCDecoderConfigChromaFormatReservedInvalid(%#x)", b[16])
	}
	if b[17]&0xf8 != 0xf8 {
		return nil, fmt.Errorf("HEVCDecoderConfigBitDepthLumaReservedInvalid(%#x)", b[17])
	}
	if b[18]&0xf8 != 0xf8 {
		return nil, fmt.Errorf("HEVCDecoderConfigBitDepthChromaReservedInvalid(%#x)", b[18])
	}
	if b[21]&0xfc != 0xfc {
		return nil, fmt.Errorf("HEVCDecoderConfigLengthSizeReservedInvalid(%#x)", b[21])
	}

	lengthSizeMinusOne := int(b[21] & 0x03)
	var nallengthSize int
	switch lengthSizeMinusOne {
	case 0:
		nallengthSize = 1
	case 1:
		nallengthSize = 2
	case 3:
		nallengthSize = 4
	default:
		return nil, fmt.Errorf("HEVCDecoderConfigLengthSizeInvalid(%d)", lengthSizeMinusOne+1)
	}

	n := hvccFixedHeaderSize
	numArrays := int(b[22])
	for j := 0; j < numArrays; j++ {
		if len(b)-n < 3 {
			return nil, fmt.Errorf("HEVCDecoderConfigArrayHeaderTruncated(index=%d)", j)
		}
		// array_completeness(1) + reserved(1) + NAL_unit_type(6)
		n++
		numNalus := int(pio.U16BE(b[n:]))
		n += 2
		if numNalus == 0 {
			return nil, fmt.Errorf("HEVCDecoderConfigArrayEmpty(index=%d)", j)
		}
		for i := 0; i < numNalus; i++ {
			if len(b)-n < 2 {
				return nil, fmt.Errorf("HEVCDecoderConfigNALLengthTruncated(array=%d,nal=%d)", j, i)
			}
			nalLen := int(pio.U16BE(b[n:]))
			n += 2
			if nalLen == 0 {
				return nil, fmt.Errorf("HEVCDecoderConfigNALEmpty(array=%d,nal=%d)", j, i)
			}
			if nalLen > len(b)-n {
				return nil, fmt.Errorf("HEVCDecoderConfigNALOutOfBounds(array=%d,nal=%d,len=%d,left=%d)", j, i, nalLen, len(b)-n)
			}
			n += nalLen
		}
	}

	if n != len(b) {
		return nil, fmt.Errorf("HEVCDecoderConfigTrailingBytes(got=%d,consumed=%d)", len(b), n)
	}

	c = &Codec{
		ConfigBytes:   append([]byte(nil), b...),
		NALLengthSize: nallengthSize,
	}
	return c, nil
}

// CheckFrame validates every length-prefixed NAL unit of one frame using this
// track's configured length field width. The compressed NAL bytes themselves
// are not inspected or modified.
func (c *Codec) CheckFrame(b []byte) error {
	if c == nil {
		return fmt.Errorf("HEVCFrameWithoutConfig")
	}
	if len(b) == 0 {
		return fmt.Errorf("HEVCFrameEmpty")
	}

	width := c.NALLengthSize
	i := 0
	nalCount := 0
	for i < len(b) {
		if len(b)-i < width {
			return fmt.Errorf("HEVCFrameNALLengthTruncated(width=%d,left=%d)", width, len(b)-i)
		}

		var n int
		switch width {
		case 1:
			n = int(b[i])
		case 2:
			n = int(pio.U16BE(b[i:]))
		case 4:
			n = int(pio.U32BE(b[i:]))
		default:
			return fmt.Errorf("HEVCFrameLengthSizeUnsupported(%d)", width)
		}
		i += width

		if n == 0 {
			return fmt.Errorf("HEVCFrameNALEmpty(index=%d)", nalCount)
		}
		if n > len(b)-i {
			return fmt.Errorf("HEVCFrameNALOutOfBounds(index=%d,len=%d,left=%d)", nalCount, n, len(b)-i)
		}
		i += n
		nalCount++
	}

	return nil
}
