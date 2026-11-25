// Copyright 2020, Chef.  All rights reserved.
// https://github.com/q191201771/lal
//
// Use of this source code is governed by a MIT-style license
// that can be found in the License file.

package rtsp_test

import (
	"encoding/binary"
	"fmt"

	"github.com/q191201771/lal/pkg/base"
	"github.com/q191201771/lal/pkg/rtprtcp"
	"github.com/q191201771/lal/pkg/rtsp"
	"github.com/q191201771/lal/pkg/sdp"
)

type exObserver struct {
	packets []base.AvPacket
}

func (o *exObserver) OnSdp(sdp.LogicContext)        {}
func (o *exObserver) OnRtpPacket(rtprtcp.RtpPacket) {}
func (o *exObserver) OnAvPacket(pkt base.AvPacket)  { o.packets = append(o.packets, pkt) }

type exWriter struct{}

func (w *exWriter) WriteInterleavedPacket(b []byte, channel int) error { return nil }

func exRtp(pt byte, seq uint16, ts, ssrc uint32, payload []byte) []byte {
	b := make([]byte, 12+len(payload))
	b[0] = 0x80
	b[1] = pt
	binary.BigEndian.PutUint16(b[2:4], seq)
	binary.BigEndian.PutUint32(b[4:8], ts)
	binary.BigEndian.PutUint32(b[8:12], ssrc)
	copy(b[12:], payload)
	return b
}

func exSr(ssrc, rtpTs, msw, frac uint32) []byte {
	b := make([]byte, 28)
	b[0] = 0x80
	b[1] = 200
	binary.BigEndian.PutUint16(b[2:4], 6)
	binary.BigEndian.PutUint32(b[4:8], ssrc)
	binary.BigEndian.PutUint32(b[8:12], msw)
	binary.BigEndian.PutUint32(b[12:16], frac)
	binary.BigEndian.PutUint32(b[16:20], rtpTs)
	binary.BigEndian.PutUint32(b[20:24], 1)
	binary.BigEndian.PutUint32(b[24:28], 1)
	return b
}

// ExampleBaseInSession_WithSenderReportClock 展示开启SR时钟模式后，
// 音视频首帧的250ms间隔不会再被逐轨归零抹掉。
func ExampleBaseInSession_WithSenderReportClock() {
	sdpCtx, err := sdp.ParseSdp2LogicContext([]byte(
		"v=0\r\n" +
			"o=- 1 1 IN IP4 127.0.0.1\r\n" +
			"s=clock\r\n" +
			"t=0 0\r\n" +
			"m=video 0 RTP/AVP 96\r\n" +
			"a=rtpmap:96 H264/90000\r\n" +
			"a=control:v\r\n" +
			"m=audio 0 RTP/AVP 8\r\n" +
			"a=rtpmap:8 PCMA/8000\r\n" +
			"a=control:a\r\n"))
	if err != nil {
		panic(err)
	}

	o := &exObserver{}
	s := rtsp.NewBaseInSessionWithObserver(base.SessionTypeRtspPub, &exWriter{}, o)

	// 必须在InitWithSdp之前开启，默认是关闭的
	s.WithSenderReportClock(true).InitWithSdp(sdpCtx)
	if err := s.SetupWithChannel("v", 0, 1); err != nil {
		panic(err)
	}
	if err := s.SetupWithChannel("a", 2, 3); err != nil {
		panic(err)
	}

	const ntpSec uint32 = 3900000000
	// 两轨首包RTP时间戳本来就可以不同；这里SR表明音频首帧比视频首帧晚1/4秒
	s.HandleInterleavedPacket(exRtp(96, 1, 90000, 111, []byte{0x65, 1, 2}), 0)
	s.HandleInterleavedPacket(exRtp(8, 1, 8000, 222, []byte{1, 2, 3}), 2)
	s.HandleInterleavedPacket(exSr(111, 90000, ntpSec, 0), 1)
	s.HandleInterleavedPacket(exSr(222, 8000, ntpSec, 1<<30), 3)

	// 后续帧：视频40ms、音频40ms，以及250ms处的视频帧用于归并出音频首帧
	s.HandleInterleavedPacket(exRtp(96, 2, 93600, 111, []byte{0x41, 3, 4}), 0)
	s.HandleInterleavedPacket(exRtp(8, 2, 8320, 222, []byte{4, 5, 6}), 2)
	s.HandleInterleavedPacket(exRtp(96, 3, 112500, 111, []byte{0x65, 5, 6}), 0)

	for _, p := range o.packets {
		if p.IsVideo() {
			fmt.Printf("video:%d\n", p.Timestamp)
		} else {
			fmt.Printf("audio:%d\n", p.Timestamp)
		}
	}
	// Output:
	// video:0
	// video:40
	// audio:250
	// video:250
}
