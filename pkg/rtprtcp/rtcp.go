// Copyright 2020, Chef.  All rights reserved.
// https://github.com/q191201771/lal
//
// Use of this source code is governed by a MIT-style license
// that can be found in the License file.
//
// Author: Chef (191201771@qq.com)

package rtprtcp

import (
	"github.com/q191201771/naza/pkg/bele"
	"github.com/q191201771/naza/pkg/nazaerrors"

	"github.com/q191201771/lal/pkg/base"
)

// -------------------------------------------
// rfc3550 6.4.1 SR: Sender Report RTCP Packet
// -------------------------------------------
//
//        0                   1                   2                   3
//        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// header |V=2|P|    RC   |   PT=SR=200   |             length            |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                         SSRC of sender                        |
//        +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// sender |              NTP timestamp, most significant word             |
// info   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |             NTP timestamp, least significant word             |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                         RTP timestamp                         |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                     sender's packet count                     |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                      sender's octet count                     |
//        +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// report |                 SSRC_1 (SSRC of first source)                 |
// block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   1    | fraction lost |       cumulative number of packets lost       |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |           extended highest sequence number received           |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                      interarrival jitter                      |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                         last SR (LSR)                         |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                   delay since last SR (DLSR)                  |
//        +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// report |                 SSRC_2 (SSRC of second source)                |
// block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   2    :                               ...                             :
//        +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//        |                  profile-specific extensions                  |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

// ---------------------------------------------
// rfc3550 6.4.2 RR: Receiver Report RTCP Packet
// ---------------------------------------------
//
//        0                   1                   2                   3
//        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// header |V=2|P|    RC   |   PT=RR=201   |             length            |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                     SSRC of packet sender                     |
//        +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// report |                 SSRC_1 (SSRC of first source)                 |
// block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   1    | fraction lost |       cumulative number of packets lost       |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |           extended highest sequence number received           |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                      interarrival jitter                      |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                         last SR (LSR)                         |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//        |                   delay since last SR (DLSR)                  |
//        +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// report |                 SSRC_2 (SSRC of second source)                |
// block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   2    :                               ...                             :
//        +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//        |                  profile-specific extensions                  |
//        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

const (
	RtcpPacketTypeSr  = 200 // 0xc8 Sender Report
	RtcpPacketTypeRr  = 201 // 0xc9 Receiver Report
	RtcpPacketTypeApp = 204

	RtcpHeaderLength = 4

	RtcpVersion = 2
)

type RtcpHeader struct {
	Version       uint8  // 2b
	Padding       uint8  // 1b
	CountOrFormat uint8  // 5b
	PacketType    uint8  // 8b
	Length        uint16 // 16b, whole packet byte length = (Length+1) * 4
}

type Sr struct {
	SenderSsrc uint32
	Msw        uint32 // NTP timestamp, most significant word
	Lsw        uint32 // NTP timestamp, least significant word
	Timestamp  uint32
	PktCnt     uint32
	OctetCnt   uint32
}

func ParseRtcpHeader(b []byte) RtcpHeader {
	var h RtcpHeader
	h.Version = b[0] >> 6
	h.Padding = (b[0] >> 5) & 0x1
	h.CountOrFormat = b[0] & 0x1F
	h.PacketType = b[1]
	h.Length = bele.BeUint16(b[2:])
	return h
}

// ParseSr rfc3550 6.4.1
//
// @param b rtcp包，包含包头
func ParseSr(b []byte) Sr {
	var s Sr
	s.SenderSsrc = bele.BeUint32(b[4:])
	s.Msw = bele.BeUint32(b[8:])
	s.Lsw = bele.BeUint32(b[12:])
	s.Timestamp = bele.BeUint32(b[16:])
	s.PktCnt = bele.BeUint32(b[20:])
	s.OctetCnt = bele.BeUint32(b[24:])
	return s
}

// PackTo @param out 传出参数，注意，调用方保证长度>=4
func (r *RtcpHeader) PackTo(out []byte) {
	out[0] = r.Version<<6 | r.Padding<<5 | r.CountOrFormat
	out[1] = r.PacketType
	bele.BePutUint16(out[2:], r.Length)
}

func (s *Sr) GetMiddleNtp() uint32 {
	return uint32(((uint64(s.Msw)<<32 | uint64(s.Lsw)) << 16) >> 32)
}

// ParseCompoundSr 校验一个rtcp datagram（可能复合了多个子包），并返回其中所有的SR。
//
// 校验内容包括：每个子包的完整边界、版本、长度，以及SR子包的report-count长度。
// 注意，只要任何一个子包校验失败（比如尾包子包被截断），整个datagram都不可用，
// 也即前面已经解析出的SR也不能被使用。
func ParseCompoundSr(b []byte) ([]Sr, error) {
	var srs []Sr

	for offset := 0; offset < len(b); {
		if len(b)-offset < RtcpHeaderLength {
			return nil, nazaerrors.Wrap(base.ErrRtpRtcpShortBuffer)
		}

		h := ParseRtcpHeader(b[offset:])
		if h.Version != RtcpVersion {
			return nil, nazaerrors.Wrap(base.ErrRtpRtcpVersionInvalid)
		}

		pktLen := (int(h.Length) + 1) * 4
		if pktLen < RtcpHeaderLength || offset+pktLen > len(b) {
			return nil, nazaerrors.Wrap(base.ErrRtpRtcpShortBuffer)
		}

		if h.PacketType == RtcpPacketTypeSr {
			// sr子包 = 4字节头 + 24字节sender info + report-count个24字节report block
			need := RtcpHeaderLength + 24 + int(h.CountOrFormat)*24
			if pktLen < need {
				return nil, nazaerrors.Wrap(base.ErrRtpRtcpShortBuffer)
			}
			srs = append(srs, ParseSr(b[offset:offset+pktLen]))
		}

		offset += pktLen
	}

	return srs, nil
}
