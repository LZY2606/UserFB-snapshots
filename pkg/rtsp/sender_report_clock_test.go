// Copyright 2020, Chef.  All rights reserved.
// https://github.com/q191201771/lal
//
// Use of this source code is governed by a MIT-style license
// that can be found in the License file.
//
// Author: Chef (191201771@qq.com)

package rtsp

import (
	"encoding/binary"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/q191201771/lal/pkg/base"
	"github.com/q191201771/lal/pkg/rtprtcp"
	"github.com/q191201771/lal/pkg/sdp"
	"github.com/q191201771/lal/task_support/taskcheck"
	"github.com/q191201771/naza/pkg/nazanet"
)

// ---------------------------------------------------------------------------------------------------------------------

const (
	srClockTestVideoSsrc = 111
	srClockTestAudioSsrc = 222
	srClockTestNtpSec    = 3900000000
	srClockTestNtpFrac   = 1 << 30 // 0.25秒
)

const (
	srClockTestDualSdp = "v=0\r\n" +
		"o=- 1 1 IN IP4 127.0.0.1\r\n" +
		"s=clock\r\n" +
		"t=0 0\r\n" +
		"m=video 0 RTP/AVP 96\r\n" +
		"a=rtpmap:96 H264/90000\r\n" +
		"a=control:v\r\n" +
		"m=audio 0 RTP/AVP 8\r\n" +
		"a=rtpmap:8 PCMA/8000\r\n" +
		"a=control:a\r\n"
	srClockTestVideoOnlySdp = "v=0\r\n" +
		"o=- 1 1 IN IP4 127.0.0.1\r\n" +
		"s=clock\r\n" +
		"t=0 0\r\n" +
		"m=video 0 RTP/AVP 96\r\n" +
		"a=rtpmap:96 H264/90000\r\n" +
		"a=control:v\r\n"
	srClockTestAudioOnlySdp = "v=0\r\n" +
		"o=- 1 1 IN IP4 127.0.0.1\r\n" +
		"s=clock\r\n" +
		"t=0 0\r\n" +
		"m=audio 0 RTP/AVP 8\r\n" +
		"a=rtpmap:8 PCMA/8000\r\n" +
		"a=control:a\r\n"
)

type srClockTestObserver struct {
	mu  sync.Mutex
	av  []base.AvPacket
	raw int
}

func (o *srClockTestObserver) OnSdp(sdp.LogicContext) {}

func (o *srClockTestObserver) OnRtpPacket(pkt rtprtcp.RtpPacket) {
	o.mu.Lock()
	o.raw++
	o.mu.Unlock()
}

func (o *srClockTestObserver) OnAvPacket(pkt base.AvPacket) {
	o.mu.Lock()
	defer o.mu.Unlock()
	// 拷贝一份，避免底层内存被复用
	p := pkt
	p.Payload = append([]byte(nil), pkt.Payload...)
	o.av = append(o.av, p)
}

func (o *srClockTestObserver) avPackets() []base.AvPacket {
	o.mu.Lock()
	defer o.mu.Unlock()
	return append([]base.AvPacket(nil), o.av...)
}

func (o *srClockTestObserver) rawCount() int {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.raw
}

type srClockTestWriter struct {
	mu sync.Mutex
	n  int
}

func (w *srClockTestWriter) WriteInterleavedPacket(b []byte, channel int) error {
	w.mu.Lock()
	w.n++
	w.mu.Unlock()
	return nil
}

func (w *srClockTestWriter) count() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.n
}

// ---------------------------------------------------------------------------------------------------------------------

func srClockTestRtp(pt byte, seq uint16, ts, ssrc uint32, payload []byte) []byte {
	b := make([]byte, 12+len(payload))
	b[0] = 0x80
	b[1] = pt
	binary.BigEndian.PutUint16(b[2:], seq)
	binary.BigEndian.PutUint32(b[4:], ts)
	binary.BigEndian.PutUint32(b[8:], ssrc)
	copy(b[12:], payload)
	return b
}

func srClockTestSr(ssrc, rtpTs, msw, lsw uint32) []byte {
	b := make([]byte, 28)
	b[0] = 0x80
	b[1] = rtprtcp.RtcpPacketTypeSr
	binary.BigEndian.PutUint16(b[2:], 6)
	for i, v := range []uint32{ssrc, msw, lsw, rtpTs, 1, 1} {
		binary.BigEndian.PutUint32(b[4+4*i:], v)
	}
	return b
}

// srClockTestRr 一个8字节的RR子包，用来验证复合datagram中非SR子包会被跳过
func srClockTestRr(ssrc uint32) []byte {
	b := make([]byte, 8)
	b[0] = 0x80
	b[1] = rtprtcp.RtcpPacketTypeRr
	binary.BigEndian.PutUint16(b[2:], 1)
	binary.BigEndian.PutUint32(b[4:], ssrc)
	return b
}

func srClockTestCompound(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

func srClockTestSession(c *taskcheck.C, sdpText string) (*BaseInSession, *srClockTestObserver, *srClockTestWriter) {
	ctx, err := sdp.ParseSdp2LogicContext([]byte(sdpText))
	c.Check(err == nil, "parse sdp failed. err=%+v", err)
	if err != nil {
		return nil, nil, nil
	}
	o := &srClockTestObserver{}
	w := &srClockTestWriter{}
	s := NewBaseInSessionWithObserver(base.SessionTypeRtspPub, w, o)
	s.WithSenderReportClock(true)
	s.InitWithSdp(ctx)
	return s, o, w
}

type srClockAvItem struct {
	video bool
	ts    int64
}

func srClockCheckAv(c *taskcheck.C, o *srClockTestObserver, expected []srClockAvItem) {
	av := o.avPackets()
	c.Check(len(av) == len(expected), "av packet count. actual=%d, expected=%d", len(av), len(expected))
	n := len(av)
	if len(expected) < n {
		n = len(expected)
	}
	for i := 0; i < n; i++ {
		c.Check(av[i].IsVideo() == expected[i].video && av[i].Timestamp == expected[i].ts,
			"av[%d] mismatch. actual=(video=%v, ts=%d), expected=(video=%v, ts=%d)",
			i, av[i].IsVideo(), av[i].Timestamp, expected[i].video, expected[i].ts)
	}
}

// ---------------------------------------------------------------------------------------------------------------------

func TestSenderReportAlignment(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// case. 双轨起播，音视频首包间隔250ms，SR在首帧之后到达
		func() {
			s, o, w := srClockTestSession(c, srClockTestDualSdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithChannel("v", 0, 1) == nil, "setup video channel failed")
			c.Check(s.SetupWithChannel("a", 2, 3) == nil, "setup audio channel failed")

			// 视频首帧对应NTP整秒，音频首帧对应同一秒加四分之一秒
			s.HandleInterleavedPacket(srClockTestRtp(96, 1, 90000, srClockTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(8, 1, 8000, srClockTestAudioSsrc, []byte{1, 2, 3}), 2)

			// 锚点没齐的时候只延后OnAvPacket，原始OnRtpPacket照常通知
			c.Check(o.rawCount() == 2, "raw count before anchors. actual=%d", o.rawCount())
			c.Check(len(o.avPackets()) == 0, "av should be delayed before anchors. actual=%d", len(o.avPackets()))

			// 另一轨的SR不能拿来代替本轨的锚点：只有音频SR时，零点不能建立
			s.HandleInterleavedPacket(srClockTestSr(srClockTestAudioSsrc, 8000, srClockTestNtpSec, srClockTestNtpFrac), 3)
			c.Check(len(o.avPackets()) == 0, "audio sr must not anchor video. actual=%d", len(o.avPackets()))
			c.Check(w.count() == 1, "rr count after audio sr. actual=%d", w.count())

			// 视频SR到达后，零点建立，暂存的帧按零点输出，音视频间隔保留
			s.HandleInterleavedPacket(srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0), 1)
			srClockCheckAv(c, o, []srClockAvItem{{true, 0}})
			c.Check(w.count() == 2, "rr count after video sr. actual=%d", w.count())

			// 继续喂帧，视频40ms一帧，音频首帧250ms、之后40ms一帧
			s.HandleInterleavedPacket(srClockTestRtp(96, 2, 93600, srClockTestVideoSsrc, []byte{0x41, 2}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(8, 2, 8320, srClockTestAudioSsrc, []byte{4, 5, 6}), 2)
			for i, ts := range []uint32{97200, 100800, 104400, 108000, 111600, 115200, 118800} {
				s.HandleInterleavedPacket(srClockTestRtp(96, uint16(3+i), ts, srClockTestVideoSsrc, []byte{0x41, byte(3 + i)}), 0)
			}
			srClockCheckAv(c, o, []srClockAvItem{
				{true, 0}, {true, 40}, {true, 80}, {true, 120}, {true, 160}, {true, 200}, {true, 240},
				{false, 250}, {true, 280}, {false, 290},
			})

			// 后来的SR照常参与RR路径，但不改已经建立的映射
			s.HandleInterleavedPacket(srClockTestSr(srClockTestVideoSsrc, 122400, srClockTestNtpSec+10, 0), 1)
			c.Check(w.count() == 3, "rr count after late sr. actual=%d", w.count())
			s.HandleInterleavedPacket(srClockTestRtp(8, 3, 8640, srClockTestAudioSsrc, []byte{7}), 2)
			s.HandleInterleavedPacket(srClockTestRtp(96, 10, 122400, srClockTestVideoSsrc, []byte{0x41, 10}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(8, 4, 8960, srClockTestAudioSsrc, []byte{8}), 2)
			s.HandleInterleavedPacket(srClockTestRtp(96, 11, 126000, srClockTestVideoSsrc, []byte{0x41, 11}), 0)
			av := o.avPackets()
			c.Check(len(av) == 14, "av count after late sr. actual=%d", len(av))
			if len(av) == 14 {
				c.Check(av[10].IsVideo() && av[10].Timestamp == 320, "av[10] should be video 320ms. actual=(video=%v, ts=%d)", av[10].IsVideo(), av[10].Timestamp)
				c.Check(av[11].IsAudio() && av[11].Timestamp == 330, "av[11] should be audio 330ms. actual=(video=%v, ts=%d)", av[11].IsVideo(), av[11].Timestamp)
				c.Check(av[12].IsVideo() && av[12].Timestamp == 360, "late sr must not move mapping. actual=(video=%v, ts=%d)", av[12].IsVideo(), av[12].Timestamp)
				c.Check(av[13].IsAudio() && av[13].Timestamp == 370, "av[13] should be audio 370ms. actual=(video=%v, ts=%d)", av[13].IsVideo(), av[13].Timestamp)
			}
			c.Check(o.rawCount() == 15, "raw count. actual=%d", o.rawCount())

			// 数据内容保留
			if len(av) >= 1 {
				c.Check(string(av[0].Payload) == string([]byte{0, 0, 0, 3, 0x65, 1, 2}), "first video payload. actual=%v", av[0].Payload)
			}
		}()

		// case. 单轨视频起播，SR先于RTP到，SSRC对不上的SR不能当锚点
		func() {
			s, o, w := srClockTestSession(c, srClockTestVideoOnlySdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithChannel("v", 0, 1) == nil, "setup video channel failed")

			s.HandleInterleavedPacket(srClockTestSr(999, 12345, srClockTestNtpSec, 0), 1)
			s.HandleInterleavedPacket(srClockTestRtp(96, 1, 90000, srClockTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			c.Check(len(o.avPackets()) == 0, "sr with unmatched ssrc must not anchor. actual=%d", len(o.avPackets()))

			s.HandleInterleavedPacket(srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0), 1)
			s.HandleInterleavedPacket(srClockTestRtp(96, 2, 93600, srClockTestVideoSsrc, []byte{0x41, 2}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(96, 3, 97200, srClockTestVideoSsrc, []byte{0x41, 3}), 0)
			srClockCheckAv(c, o, []srClockAvItem{{true, 0}, {true, 40}, {true, 80}})
			c.Check(o.rawCount() == 3, "raw count. actual=%d", o.rawCount())
			c.Check(w.count() == 1, "rr count. actual=%d", w.count())
		}()

		// case. 单轨音频起播，帧先于SR到
		func() {
			s, o, _ := srClockTestSession(c, srClockTestAudioOnlySdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithChannel("a", 2, 3) == nil, "setup audio channel failed")

			s.HandleInterleavedPacket(srClockTestRtp(8, 1, 8000, srClockTestAudioSsrc, []byte{1, 2, 3}), 2)
			s.HandleInterleavedPacket(srClockTestRtp(8, 2, 8320, srClockTestAudioSsrc, []byte{4, 5, 6}), 2)
			c.Check(len(o.avPackets()) == 0, "av should be delayed before anchor. actual=%d", len(o.avPackets()))

			s.HandleInterleavedPacket(srClockTestSr(srClockTestAudioSsrc, 8000, srClockTestNtpSec, srClockTestNtpFrac), 3)
			s.HandleInterleavedPacket(srClockTestRtp(8, 3, 8640, srClockTestAudioSsrc, []byte{7}), 2)

			av := o.avPackets()
			c.Check(len(av) == 3, "av count. actual=%d", len(av))
			if len(av) == 3 {
				c.Check(av[0].Timestamp == 0 && av[1].Timestamp == 40 && av[2].Timestamp == 80,
					"audio ts. actual=(%d, %d, %d)", av[0].Timestamp, av[1].Timestamp, av[2].Timestamp)
				c.Check(string(av[0].Payload) == string([]byte{1, 2, 3}) &&
					string(av[1].Payload) == string([]byte{4, 5, 6}) &&
					string(av[2].Payload) == string([]byte{7}), "audio payload content")
			}
			c.Check(o.rawCount() == 3, "raw count. actual=%d", o.rawCount())
		}()
	})
}

func TestSenderReportClockPackets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// case. FU-A重排、STAP-A共用包时刻、RTP时间戳回绕
		func() {
			s, o, _ := srClockTestSession(c, srClockTestVideoOnlySdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithChannel("v", 0, 1) == nil, "setup video channel failed")

			// 锚点贴着32位回绕边界
			anchor := uint32(0xFFFFFF00)
			s.HandleInterleavedPacket(srClockTestSr(srClockTestVideoSsrc, anchor, srClockTestNtpSec, 0), 1)

			// 单nalu帧，正好在锚点上
			s.HandleInterleavedPacket(srClockTestRtp(96, 1, anchor, srClockTestVideoSsrc, []byte{0x65, 0x01}), 0)

			// STAP-A，跨回绕，包内各NAL共用所属包的时刻
			ts2 := anchor + 9000 // 100ms，已回绕
			stapA := []byte{0x78, 0x00, 0x02, 0x67, 0xAA, 0x00, 0x03, 0x68, 0xBB, 0xCC}
			s.HandleInterleavedPacket(srClockTestRtp(96, 2, ts2, srClockTestVideoSsrc, stapA), 0)

			// FU-A，重排（start、end、middle），重排之后还是同一个媒体时刻
			ts3 := ts2 + 18000 // 300ms
			s.HandleInterleavedPacket(srClockTestRtp(96, 3, ts3, srClockTestVideoSsrc, []byte{0x7C, 0x85, 0xAA, 0xAA}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(96, 5, ts3, srClockTestVideoSsrc, []byte{0x7C, 0x45, 0xCC, 0xCC}), 0)
			c.Check(len(o.avPackets()) == 2, "fua frame should wait for middle packet. actual=%d", len(o.avPackets()))
			s.HandleInterleavedPacket(srClockTestRtp(96, 4, ts3, srClockTestVideoSsrc, []byte{0x7C, 0x05, 0xBB, 0xBB}), 0)

			// 回绕之后的普通帧，不能触发旧的逐轨归零
			ts4 := ts3 + 3600 // 340ms
			s.HandleInterleavedPacket(srClockTestRtp(96, 6, ts4, srClockTestVideoSsrc, []byte{0x41, 0x09}), 0)

			av := o.avPackets()
			c.Check(len(av) == 4, "av count. actual=%d", len(av))
			if len(av) == 4 {
				c.Check(av[0].Timestamp == 0, "frame0 ts. actual=%d", av[0].Timestamp)
				c.Check(av[1].Timestamp == 100, "stap-a ts after wrap. actual=%d", av[1].Timestamp)
				c.Check(av[2].Timestamp == 300, "fua ts. actual=%d", av[2].Timestamp)
				c.Check(av[3].Timestamp == 340, "frame3 ts after wrap. actual=%d", av[3].Timestamp)

				c.Check(string(av[1].Payload) == string([]byte{0, 0, 0, 2, 0x67, 0xAA, 0, 0, 0, 3, 0x68, 0xBB, 0xCC}),
					"stap-a payload. actual=%v", av[1].Payload)
				c.Check(string(av[2].Payload) == string([]byte{0, 0, 0, 7, 0x65, 0xAA, 0xAA, 0xBB, 0xBB, 0xCC, 0xCC}),
					"fua payload. actual=%v", av[2].Payload)
			}
			c.Check(o.rawCount() == 6, "raw count. actual=%d", o.rawCount())
		}()

		// case. 音频乱序，缓存帧的时间不能用最后一次收到的RTP头替代
		func() {
			s, o, _ := srClockTestSession(c, srClockTestAudioOnlySdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithChannel("a", 2, 3) == nil, "setup audio channel failed")

			s.HandleInterleavedPacket(srClockTestSr(srClockTestAudioSsrc, 8000, srClockTestNtpSec, 0), 3)
			s.HandleInterleavedPacket(srClockTestRtp(8, 1, 8000, srClockTestAudioSsrc, []byte{1}), 2)
			// seq3先到，被缓存；seq2到达时两帧一起解出
			s.HandleInterleavedPacket(srClockTestRtp(8, 3, 8640, srClockTestAudioSsrc, []byte{3}), 2)
			c.Check(len(o.avPackets()) == 1, "out-of-order packet should be cached. actual=%d", len(o.avPackets()))
			s.HandleInterleavedPacket(srClockTestRtp(8, 2, 8320, srClockTestAudioSsrc, []byte{2}), 2)

			av := o.avPackets()
			c.Check(len(av) == 3, "av count. actual=%d", len(av))
			if len(av) == 3 {
				c.Check(av[0].Timestamp == 0 && av[1].Timestamp == 40 && av[2].Timestamp == 80,
					"cached frame must keep its own rtp clock. actual=(%d, %d, %d)",
					av[0].Timestamp, av[1].Timestamp, av[2].Timestamp)
				c.Check(string(av[2].Payload) == string([]byte{3}), "cached frame payload. actual=%v", av[2].Payload)
			}
		}()
	})
}

func TestSenderReportClockIngress(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// case. TCP interleaved：复合datagram里有多个子包，SR先于RTP到
		func() {
			s, o, w := srClockTestSession(c, srClockTestDualSdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithChannel("v", 0, 1) == nil, "setup video channel failed")
			c.Check(s.SetupWithChannel("a", 2, 3) == nil, "setup audio channel failed")

			// 一个datagram里复合了RR子包和两轨的SR，且SR先于RTP到
			s.HandleInterleavedPacket(srClockTestCompound(
				srClockTestRr(srClockTestVideoSsrc),
				srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0),
				srClockTestSr(srClockTestAudioSsrc, 8000, srClockTestNtpSec, srClockTestNtpFrac),
			), 1)
			c.Check(w.count() == 0, "no rr before media ssrc confirmed. actual=%d", w.count())

			s.HandleInterleavedPacket(srClockTestRtp(96, 1, 90000, srClockTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			c.Check(len(o.avPackets()) == 0, "audio anchor not ready. actual=%d", len(o.avPackets()))
			s.HandleInterleavedPacket(srClockTestRtp(8, 1, 8000, srClockTestAudioSsrc, []byte{1, 2, 3}), 2)
			srClockCheckAv(c, o, []srClockAvItem{{true, 0}})

			s.HandleInterleavedPacket(srClockTestRtp(96, 2, 93600, srClockTestVideoSsrc, []byte{0x41, 2}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(8, 2, 8320, srClockTestAudioSsrc, []byte{4, 5, 6}), 2)
			s.HandleInterleavedPacket(srClockTestRtp(96, 3, 97200, srClockTestVideoSsrc, []byte{0x41, 3}), 0)
			srClockCheckAv(c, o, []srClockAvItem{{true, 0}, {true, 40}, {true, 80}})

			// 正常的RR回复保留；后来的SR不改映射
			s.HandleInterleavedPacket(srClockTestSr(srClockTestVideoSsrc, 97200, srClockTestNtpSec+5, 0), 1)
			c.Check(w.count() == 1, "rr count. actual=%d", w.count())
			s.HandleInterleavedPacket(srClockTestRtp(96, 4, 100800, srClockTestVideoSsrc, []byte{0x41, 4}), 0)
			srClockCheckAv(c, o, []srClockAvItem{{true, 0}, {true, 40}, {true, 80}, {true, 120}})
			c.Check(o.rawCount() == 6, "raw count. actual=%d", o.rawCount())
		}()

		// case. TCP interleaved：坏datagram（版本错、report-count长度错、尾包截断）不能发布映射或者释放帧
		func() {
			s, o, w := srClockTestSession(c, srClockTestDualSdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithChannel("v", 0, 1) == nil, "setup video channel failed")
			c.Check(s.SetupWithChannel("a", 2, 3) == nil, "setup audio channel failed")

			s.HandleInterleavedPacket(srClockTestRtp(96, 1, 90000, srClockTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(8, 1, 8000, srClockTestAudioSsrc, []byte{1, 2, 3}), 2)
			c.Check(o.rawCount() == 2, "raw count. actual=%d", o.rawCount())

			// 版本错
			badVersion := srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0)
			badVersion[0] = 0x40
			s.HandleInterleavedPacket(badVersion, 1)
			// SR的report-count长度错
			badRc := srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0)
			badRc[0] = 0x81
			s.HandleInterleavedPacket(badRc, 1)
			// 尾包截断：前面的SR也不能发布映射或者释放帧
			truncated := srClockTestSr(srClockTestAudioSsrc, 8000, srClockTestNtpSec, srClockTestNtpFrac)
			s.HandleInterleavedPacket(srClockTestCompound(
				srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0),
				truncated[:10],
			), 1)
			c.Check(w.count() == 0, "bad datagrams must not produce rr. actual=%d", w.count())

			// 音频SR单独到达，音频锚点建立，但视频锚点缺失，零点不能建立
			s.HandleInterleavedPacket(srClockTestSr(srClockTestAudioSsrc, 8000, srClockTestNtpSec, srClockTestNtpFrac), 3)
			c.Check(w.count() == 1, "rr count after audio sr. actual=%d", w.count())
			s.HandleInterleavedPacket(srClockTestRtp(96, 2, 93600, srClockTestVideoSsrc, []byte{0x41, 2}), 0)
			s.HandleInterleavedPacket(srClockTestRtp(8, 2, 8320, srClockTestAudioSsrc, []byte{4, 5, 6}), 2)
			c.Check(len(o.avPackets()) == 0, "bad datagram must not publish mapping or release frames. actual=%d", len(o.avPackets()))

			// 视频SR重新到达后，零点建立，暂存的帧按零点输出
			s.HandleInterleavedPacket(srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0), 1)
			c.Check(w.count() == 2, "rr count after video sr. actual=%d", w.count())
			srClockCheckAv(c, o, []srClockAvItem{{true, 0}, {true, 40}})

			// 尾帧的等待规则不变：音频帧要等视频轨有更新的帧才排出
			for i, ts := range []uint32{97200, 100800, 104400, 108000, 111600, 115200} {
				s.HandleInterleavedPacket(srClockTestRtp(96, uint16(3+i), ts, srClockTestVideoSsrc, []byte{0x41, byte(3 + i)}), 0)
			}
			srClockCheckAv(c, o, []srClockAvItem{
				{true, 0}, {true, 40}, {true, 80}, {true, 120}, {true, 160}, {true, 200}, {true, 240},
				{false, 250}, {true, 280},
			})
		}()

		// case. UDP接收：走实际的本地UDP收包，SR先于RTP到
		func() {
			listen := func() *net.UDPConn {
				conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.ParseIP("127.0.0.1")})
				if err != nil {
					c.Check(false, "listen udp failed. err=%+v", err)
					return nil
				}
				return conn
			}
			wrap := func(conn *net.UDPConn) *nazanet.UdpConnection {
				u, err := nazanet.NewUdpConnection(func(option *nazanet.UdpConnectionOption) {
					option.Conn = conn
				})
				if err != nil {
					c.Check(false, "new udp connection failed. err=%+v", err)
					return nil
				}
				return u
			}

			vRtp, vRtcp, aRtp, aRtcp := listen(), listen(), listen(), listen()
			client := listen()
			if vRtp == nil || vRtcp == nil || aRtp == nil || aRtcp == nil || client == nil {
				return
			}
			defer func() {
				_ = client.Close()
			}()

			s, o, _ := srClockTestSession(c, srClockTestDualSdp)
			if s == nil {
				return
			}
			c.Check(s.SetupWithConn("v", wrap(vRtp), wrap(vRtcp)) == nil, "setup video conn failed")
			c.Check(s.SetupWithConn("a", wrap(aRtp), wrap(aRtcp)) == nil, "setup audio conn failed")
			defer func() {
				_ = s.Dispose()
			}()

			vRtpAddr := vRtp.LocalAddr().(*net.UDPAddr)
			vRtcpAddr := vRtcp.LocalAddr().(*net.UDPAddr)
			aRtpAddr := aRtp.LocalAddr().(*net.UDPAddr)
			aRtcpAddr := aRtcp.LocalAddr().(*net.UDPAddr)

			sendTo := func(b []byte, addr *net.UDPAddr) {
				if _, err := client.WriteToUDP(b, addr); err != nil {
					c.Check(false, "udp write failed. err=%+v", err)
				}
			}
			waitCond := func(cond func() bool) bool {
				deadline := time.Now().Add(3 * time.Second)
				for time.Now().Before(deadline) {
					if cond() {
						return true
					}
					time.Sleep(5 * time.Millisecond)
				}
				return false
			}

			// SR先于RTP到
			sendTo(srClockTestSr(srClockTestVideoSsrc, 90000, srClockTestNtpSec, 0), vRtcpAddr)
			sendTo(srClockTestSr(srClockTestAudioSsrc, 8000, srClockTestNtpSec, srClockTestNtpFrac), aRtcpAddr)
			time.Sleep(50 * time.Millisecond)

			sendTo(srClockTestRtp(96, 1, 90000, srClockTestVideoSsrc, []byte{0x65, 1, 2}), vRtpAddr)
			sendTo(srClockTestRtp(8, 1, 8000, srClockTestAudioSsrc, []byte{1, 2, 3}), aRtpAddr)
			time.Sleep(50 * time.Millisecond)

			sendTo(srClockTestRtp(8, 2, 8320, srClockTestAudioSsrc, []byte{4, 5, 6}), aRtpAddr)
			for i, ts := range []uint32{93600, 97200, 100800, 104400, 108000, 111600, 115200} {
				sendTo(srClockTestRtp(96, uint16(2+i), ts, srClockTestVideoSsrc, []byte{0x41, byte(2 + i)}), vRtpAddr)
			}

			c.Check(waitCond(func() bool { return len(o.avPackets()) == 9 }),
				"udp av packet count not reached. actual=%d", len(o.avPackets()))
			srClockCheckAv(c, o, []srClockAvItem{
				{true, 0}, {true, 40}, {true, 80}, {true, 120}, {true, 160}, {true, 200}, {true, 240},
				{false, 250}, {true, 280},
			})
			c.Check(waitCond(func() bool { return o.rawCount() == 10 }), "udp raw count. actual=%d", o.rawCount())

			// 正常的RR回复保留：SSRC确认后再发SR，应收到两轨的RR
			rrCount := 0
			deadline := time.Now().Add(3 * time.Second)
			buf := make([]byte, 2048)
			for time.Now().Before(deadline) && rrCount < 2 {
				sendTo(srClockTestSr(srClockTestVideoSsrc, 115200, srClockTestNtpSec+1, 0), vRtcpAddr)
				sendTo(srClockTestSr(srClockTestAudioSsrc, 8320, srClockTestNtpSec+1, srClockTestNtpFrac), aRtcpAddr)
				_ = client.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
				for {
					n, _, err := client.ReadFromUDP(buf)
					if err != nil {
						break
					}
					if n >= 2 && buf[1] == rtprtcp.RtcpPacketTypeRr {
						rrCount++
					}
				}
			}
			c.Check(rrCount >= 2, "udp rr count. actual=%d", rrCount)
		}()
	})
}
