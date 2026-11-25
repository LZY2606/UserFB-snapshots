// Package hevc parses the small part of HEVC configuration records needed by
// Enhanced RTMP. Parameter-set and NAL unit bytes are retained but not decoded.
package hevc

import (
	"fmt"

	"github.com/notedit/rtmp/utils/bits/pio"
)

// Codec contains the validated HEVCDecoderConfigurationRecord fields used by
// the FLV and RTMP demuxers. Raw is an independent copy.
type Codec struct {
	Raw []byte

	ConfigurationVersion uint8
	LengthSizeMinusOne   uint8
	NALLengthSize        int
	NumOfArrays          int
}

// Clone returns a deep copy so a packet snapshot cannot alias the active track
// configuration's byte slice.
func (c *Codec) Clone() *Codec {
	if c == nil {
		return nil
	}
	cp := *c
	if c.Raw != nil {
		cp.Raw = append([]byte(nil), c.Raw...)
	}
	return &cp
}

// ParseConfig validates the fixed header and every declared parameter-set NAL
// boundary. It does not decode the compressed parameter-set bytes.
func ParseConfig(b []byte) (*Codec, error) {
	const fixedHeaderLen = 23
	if len(b) < fixedHeaderLen {
		return nil, fmt.Errorf("HEVCConfigTruncated(header=%d,need=%d)", len(b), fixedHeaderLen)
	}
	if b[0] != 1 {
		return nil, fmt.Errorf("HEVCConfigVersionUnsupported(%d)", b[0])
	}

	// The 23-byte fixed header places lengthSizeMinusOne in the low two bits
	// of byte 21. Reserved/profile compatibility bits do not affect boundary
	// parsing and are intentionally not rejected here.
	lengthSizeMinusOne := b[21] & 0x03
	lengthSize := int(lengthSizeMinusOne) + 1
	switch lengthSize {
	case 1, 2, 4:
	default:
		return nil, fmt.Errorf("HEVCNALLengthSizeUnsupported(%d)", lengthSize)
	}

	numArrays := int(b[22])
	off := fixedHeaderLen
	for array := 0; array < numArrays; array++ {
		if len(b)-off < 3 {
			return nil, fmt.Errorf("HEVCConfigArrayHeaderTruncated(array=%d,offset=%d)", array, off)
		}
		// array completeness and NAL type do not affect boundary parsing.
		numNALUs := int(pio.U16BE(b[off+1 : off+3]))
		off += 3

		for nalu := 0; nalu < numNALUs; nalu++ {
			if len(b)-off < 2 {
				return nil, fmt.Errorf("HEVCConfigNALULengthTruncated(array=%d,nalu=%d)", array, nalu)
			}
			naluLen := int(pio.U16BE(b[off : off+2]))
			off += 2
			if naluLen == 0 {
				return nil, fmt.Errorf("HEVCConfigNALUEmpty(array=%d,nalu=%d)", array, nalu)
			}
			if naluLen > len(b)-off {
				return nil, fmt.Errorf("HEVCConfigNALUBoundaryInvalid(array=%d,nalu=%d,length=%d,remain=%d)", array, nalu, naluLen, len(b)-off)
			}
			off += naluLen
		}
	}

	if off != len(b) {
		return nil, fmt.Errorf("HEVCConfigTrailingBytes(%d)", len(b)-off)
	}

	return &Codec{
		Raw:                  append([]byte(nil), b...),
		ConfigurationVersion: b[0],
		LengthSizeMinusOne:   lengthSizeMinusOne,
		NALLengthSize:        lengthSize,
		NumOfArrays:          numArrays,
	}, nil
}

// ValidateNALUnits checks every length-prefixed NAL unit using the supplied
// track's configured length-field width. The bytes are not decoded.
func ValidateNALUnits(b []byte, lengthSize int) error {
	switch lengthSize {
	case 1, 2, 4:
	default:
		return fmt.Errorf("HEVCNALLengthSizeUnsupported(%d)", lengthSize)
	}

	off := 0
	count := 0
	for off < len(b) {
		if len(b)-off < lengthSize {
			return fmt.Errorf("HEVCFrameLengthPrefixTruncated(offset=%d,width=%d,remain=%d)", off, lengthSize, len(b)-off)
		}

		var naluLen uint64
		switch lengthSize {
		case 1:
			naluLen = uint64(b[off])
		case 2:
			naluLen = uint64(pio.U16BE(b[off : off+2]))
		case 4:
			naluLen = uint64(pio.U32BE(b[off : off+4]))
		}
		prefixEnd := off + lengthSize

		if naluLen == 0 {
			return fmt.Errorf("HEVCFrameNALUEmpty(offset=%d)", off)
		}
		remain := len(b) - prefixEnd
		if naluLen > uint64(remain) {
			return fmt.Errorf("HEVCFrameNALUBoundaryInvalid(offset=%d,length=%d,remain=%d)", off, naluLen, remain)
		}

		off = prefixEnd + int(naluLen)
		count++
	}

	if count == 0 {
		return fmt.Errorf("HEVCFrameContainsNoNALUnits")
	}
	return nil
}
