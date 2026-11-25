// Copyright 2020, Chef.  All rights reserved.
// https://github.com/q191201771/lal
//
// Use of this source code is governed by a MIT-style license
// that can be found in the License file.

package rtsp

import (
	"bytes"
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

// 本文件通过实际回调和输出内容验证Sender Report时钟模式：
//   - TestSenderReportAlignment: 双轨/单轨起播间隔、早到SR、SSRC关联
//   - TestSenderReportClockPackets: FU-A/STAP-A、32位回绕、暂存帧保持各自时刻
//   - TestSenderReportClockIngress: TCP interleaved和实际本地UDP两种接收方式、坏尾包

const (
	srTestDualSdp = "v=0\r\n" +
		"o=- 1 1 IN IP4 127.0.0.1\r\n" +
		"s=clock\r\n" +
		"t=0 0\r\n" +
		"m=video 0 RTP/AVP 96\r\n" +
		"a=rtpmap:96 H264/90000\r\n" +
		"a=control:v\r\n" +
		"m=audio 0 RTP/AVP 8\r\n" +
		"a=rtpmap:8 PCMA/8000\r\n" +
		"a=control:a\r\n"

	srTestVideoSdp = "v=0\r\n" +
		"o=- 1 1 IN IP4 127.0.0.1\r\n" +
		"s=clock\r\n" +
		"t=0 0\r\n" +
		"m=video 0 RTP/AVP 96\r\n" +
		"a=rtpmap:96 H264/90000\r\n" +
		"a=control:v\r\n"

	srTestAudioSdp = "v=0\r\n" +
		"o=- 1 1 IN IP4 127.0.0.1\r\n" +
		"s=clock\r\n" +
		"t=0 0\r\n" +
		"m=audio 0 RTP/AVP 8\r\n" +
		"a=rtpmap:8 PCMA/8000\r\n" +
		"a=control:a\r\n"
)

// 与task_support/baseline中的NTP取值保持同一设计：
// 视频首帧对应NTP整秒，音频首帧对应同一秒加1/4秒，期望输出0ms和250ms
const (
	srTestNtpMsw         uint32 = 3900000000
	srTestNtpFracQuarter uint32 = 1 << 30

	srTestVideoSsrc uint32 = 111
	srTestAudioSsrc uint32 = 222
)

type srTestObserver struct {
	mu  sync.Mutex
	av  []base.AvPacket
	raw int
}

func (o *srTestObserver) OnSdp(sdp.LogicContext) {}

func (o *srTestObserver) OnRtpPacket(rtprtcp.RtpPacket) {
	o.mu.Lock()
	o.raw++
	o.mu.Unlock()
}

func (o *srTestObserver) OnAvPacket(pkt base.AvPacket) {
	o.mu.Lock()
	o.av = append(o.av, pkt)
	o.mu.Unlock()
}

func (o *srTestObserver) snapshot() ([]base.AvPacket, int) {
	o.mu.Lock()
	defer o.mu.Unlock()
	out := make([]base.AvPacket, len(o.av))
	copy(out, o.av)
	return out, o.raw
}

func (o *srTestObserver) avCount() int {
	o.mu.Lock()
	defer o.mu.Unlock()
	return len(o.av)
}

func (o *srTestObserver) waitAv(n int, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if o.avCount() >= n {
			return true
		}
		time.Sleep(2 * time.Millisecond)
	}
	return o.avCount() >= n
}

func (o *srTestObserver) rawCount() int {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.raw
}

func (o *srTestObserver) waitRaw(n int, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if o.rawCount() >= n {
			return true
		}
		time.Sleep(2 * time.Millisecond)
	}
	return o.rawCount() >= n
}

type srTestWriter struct {
	mu       sync.Mutex
	n        int
	channels []int
}

func (w *srTestWriter) WriteInterleavedPacket(packet []byte, channel int) error {
	w.mu.Lock()
	w.n++
	w.channels = append(w.channels, channel)
	w.mu.Unlock()
	return nil
}

func (w *srTestWriter) count() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.n
}

type srClockHarness struct {
	c   *taskcheck.C
	s   *BaseInSession
	o   *srTestObserver
	w   *srTestWriter
	ctx sdp.LogicContext
}

func newSrClockHarness(c *taskcheck.C, sdpText string) *srClockHarness {
	ctx, err := sdp.ParseSdp2LogicContext([]byte(sdpText))
	c.Check(err == nil, "parse sdp failed: %v", err)
	o := &srTestObserver{}
	w := &srTestWriter{}
	s := NewBaseInSessionWithObserver(base.SessionTypeRtspPub, w, o)
	return &srClockHarness{c: c, s: s, o: o, w: w, ctx: ctx}
}

func (h *srClockHarness) initClock() {
	h.s.WithSenderReportClock(true)
	h.s.InitWithSdp(h.ctx)
}

func (h *srClockHarness) setupDualChannels() {
	h.c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video channel failed")
	h.c.Check(h.s.SetupWithChannel("a", 2, 3) == nil, "setup audio channel failed")
}

func srRtpPacket(pt byte, seq uint16, ts, ssrc uint32, payload []byte) []byte {
	b := make([]byte, 12+len(payload))
	b[0] = 0x80
	b[1] = pt
	binary.BigEndian.PutUint16(b[2:4], seq)
	binary.BigEndian.PutUint32(b[4:8], ts)
	binary.BigEndian.PutUint32(b[8:12], ssrc)
	copy(b[12:], payload)
	return b
}

func srSrPacket(ssrc, rtpTs, msw, frac uint32) []byte {
	b := make([]byte, 28)
	b[0] = 0x80 // V=2 RC=0
	b[1] = 200  // PT=SR
	binary.BigEndian.PutUint16(b[2:4], 6)
	binary.BigEndian.PutUint32(b[4:8], ssrc)
	binary.BigEndian.PutUint32(b[8:12], msw)
	binary.BigEndian.PutUint32(b[12:16], frac)
	binary.BigEndian.PutUint32(b[16:20], rtpTs)
	binary.BigEndian.PutUint32(b[20:24], 1)
	binary.BigEndian.PutUint32(b[24:28], 1)
	return b
}

// srAvccPayload H264解包器输出AVCC格式：每个NAL前4字节长度
func srAvccPayload(nals ...[]byte) []byte {
	var out []byte
	for _, n := range nals {
		l := make([]byte, 4)
		binary.BigEndian.PutUint32(l, uint32(len(n)))
		out = append(out, l...)
		out = append(out, n...)
	}
	return out
}

// srCheckCanonicalDualOutput 校验规范双轨序列的输出：
// v1(0ms), v2(40ms), a1(250ms), v3(250ms)，其中音频首帧保持250ms间隔
func srCheckCanonicalDualOutput(c *taskcheck.C, o *srTestObserver, expectedRaw int) {
	pkts, raw := o.snapshot()
	c.Check(raw == expectedRaw, "OnRtpPacket count mismatch, want=%d got=%d", expectedRaw, raw)
	c.Check(len(pkts) == 4, "OnAvPacket count mismatch, want=4 got=%d pkts=%+v", len(pkts), pkts)
	if len(pkts) < 4 {
		return
	}

	c.Check(pkts[0].IsVideo() && pkts[0].Timestamp == 0, "pkt0 want video 0ms, got video=%v ts=%d", pkts[0].IsVideo(), pkts[0].Timestamp)
	c.Check(pkts[1].IsVideo() && pkts[1].Timestamp == 40, "pkt1 want video 40ms, got video=%v ts=%d", pkts[1].IsVideo(), pkts[1].Timestamp)
	c.Check(pkts[2].IsAudio() && pkts[2].Timestamp == 250, "pkt2 want audio 250ms, got audio=%v ts=%d", pkts[2].IsAudio(), pkts[2].Timestamp)
	c.Check(pkts[3].IsVideo() && pkts[3].Timestamp == 250, "pkt3 want video 250ms, got video=%v ts=%d", pkts[3].IsVideo(), pkts[3].Timestamp)

	// 轨内顺序：视频0<40<250
	var vts []int64
	var ats []int64
	for _, p := range pkts {
		if p.IsVideo() {
			vts = append(vts, p.Timestamp)
		} else if p.IsAudio() {
			ats = append(ats, p.Timestamp)
		}
	}
	c.Check(len(vts) == 3 && vts[0] == 0 && vts[1] == 40 && vts[2] == 250, "video order/timestamps wrong: %v", vts)
	c.Check(len(ats) == 1 && ats[0] == 250, "audio order/timestamps wrong: %v", ats)

	// 数据内容保留
	wantV1 := srAvccPayload([]byte{0x65, 1, 2})
	c.Check(bytes.Equal(pkts[0].Payload, wantV1), "v1 payload mismatch, want=%x got=%x", wantV1, pkts[0].Payload)
	c.Check(bytes.Equal(pkts[2].Payload, []byte{1, 2, 3}), "a1 payload mismatch, got=%x", pkts[2].Payload)
}

func TestSenderReportAlignment(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// case1: 双轨，RTP先到、SR后到（与baseline相同的到达顺序）
		{
			h := newSrClockHarness(c, srTestDualSdp)
			h.initClock()
			h.setupDualChannels()

			h.s.HandleInterleavedPacket(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 1, 8000, srTestAudioSsrc, []byte{1, 2, 3}), 2)
			c.Check(h.o.avCount() == 0, "anchors missing, OnAvPacket must be deferred")

			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			c.Check(h.o.avCount() == 0, "one track anchor only, zero point must not open")
			h.s.HandleInterleavedPacket(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter), 3)

			// 补齐后续帧
			h.s.HandleInterleavedPacket(srRtpPacket(96, 2, 93600, srTestVideoSsrc, []byte{0x41, 3, 4}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 2, 8320, srTestAudioSsrc, []byte{4, 5, 6}), 2)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 3, 112500, srTestVideoSsrc, []byte{0x65, 5, 6}), 0)

			srCheckCanonicalDualOutput(c, h.o, 5)
			c.Check(h.w.count() == 2, "two valid SRs should produce two RR writes, got=%d", h.w.count())
		}

		// case2: 双轨，SR先于RTP到达；之后重复SR不改映射但仍回复RR
		{
			h := newSrClockHarness(c, srTestDualSdp)
			h.initClock()
			h.setupDualChannels()

			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			h.s.HandleInterleavedPacket(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter), 3)
			c.Check(h.w.count() == 0, "early SR before any RTP cannot produce RR yet")

			h.s.HandleInterleavedPacket(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 1, 8000, srTestAudioSsrc, []byte{1, 2, 3}), 2)

			// SSRC确认后重发SR，映射不变，但RR正常回复
			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			h.s.HandleInterleavedPacket(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter), 3)
			c.Check(h.w.count() == 2, "later SRs should still produce RR writes, got=%d", h.w.count())

			h.s.HandleInterleavedPacket(srRtpPacket(96, 2, 93600, srTestVideoSsrc, []byte{0x41, 3, 4}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 2, 8320, srTestAudioSsrc, []byte{4, 5, 6}), 2)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 3, 112500, srTestVideoSsrc, []byte{0x65, 5, 6}), 0)

			srCheckCanonicalDualOutput(c, h.o, 5)
		}

		// case3: SSRC对不上的SR和另一轨的SR都不能作为本轨锚点
		{
			h := newSrClockHarness(c, srTestDualSdp)
			h.initClock()
			h.setupDualChannels()

			h.s.HandleInterleavedPacket(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 1, 8000, srTestAudioSsrc, []byte{1, 2, 3}), 2)

			// 不存在的SSRC
			h.s.HandleInterleavedPacket(srSrPacket(9999, 90000, srTestNtpMsw, 0), 1)
			// 音频SSRC的SR发到视频RTCP通道，属于另一轨的SR
			h.s.HandleInterleavedPacket(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter), 1)

			pkts, _ := h.o.snapshot()
			c.Check(len(pkts) == 0, "mismatched/other-track SR must not open zero point, got=%+v", pkts)

			// 正确的两轨SR
			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			h.s.HandleInterleavedPacket(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter), 3)

			// seq1的首帧已在前面送入，这里补seq2/3完成规范序列
			h.s.HandleInterleavedPacket(srRtpPacket(96, 2, 93600, srTestVideoSsrc, []byte{0x41, 3, 4}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 2, 8320, srTestAudioSsrc, []byte{4, 5, 6}), 2)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 3, 112500, srTestVideoSsrc, []byte{0x65, 5, 6}), 0)

			srCheckCanonicalDualOutput(c, h.o, 5)
		}

		// case4: 只有视频轨（SDP中没有音频），单轨直接输出并从0起播
		{
			h := newSrClockHarness(c, srTestVideoSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video only failed")

			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 2, 93600, srTestVideoSsrc, []byte{0x41, 3, 4}), 0)

			pkts, raw := h.o.snapshot()
			c.Check(raw == 2, "video-only raw count want=2 got=%d", raw)
			c.Check(len(pkts) == 2, "video-only av count want=2 got=%d", len(pkts))
			if len(pkts) == 2 {
				c.Check(pkts[0].IsVideo() && pkts[0].Timestamp == 0, "video first ts want=0 got=%d", pkts[0].Timestamp)
				c.Check(pkts[1].IsVideo() && pkts[1].Timestamp == 40, "video second ts want=40 got=%d", pkts[1].Timestamp)
				c.Check(bytes.Equal(pkts[0].Payload, srAvccPayload([]byte{0x65, 1, 2})), "video payload mismatch got=%x", pkts[0].Payload)
			}
		}

		// case5: 只有音频轨（PCMA/8000）
		{
			h := newSrClockHarness(c, srTestAudioSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("a", 0, 1) == nil, "setup audio only failed")

			h.s.HandleInterleavedPacket(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter), 1)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 1, 8000, srTestAudioSsrc, []byte{1, 2, 3}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 2, 8320, srTestAudioSsrc, []byte{4, 5, 6}), 0)

			pkts, raw := h.o.snapshot()
			c.Check(raw == 2, "audio-only raw count want=2 got=%d", raw)
			c.Check(len(pkts) == 2, "audio-only av count want=2 got=%d", len(pkts))
			if len(pkts) == 2 {
				c.Check(pkts[0].IsAudio() && pkts[0].Timestamp == 0, "audio first ts want=0 got=%d", pkts[0].Timestamp)
				c.Check(pkts[1].IsAudio() && pkts[1].Timestamp == 40, "audio second ts want=40 got=%d", pkts[1].Timestamp)
				c.Check(bytes.Equal(pkts[0].Payload, []byte{1, 2, 3}), "audio payload mismatch got=%x", pkts[0].Payload)
			}
		}

		// case6: SDP虽然有两轨，但只SETUP/发送视频一轨时，不应等待音频锚点
		{
			h := newSrClockHarness(c, srTestDualSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video only failed")

			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 2, 93600, srTestVideoSsrc, []byte{0x41, 3, 4}), 0)

			pkts, _ := h.o.snapshot()
			c.Check(len(pkts) == 2, "dual sdp single setup should not wait audio, got=%d", len(pkts))
			if len(pkts) == 2 {
				c.Check(pkts[0].Timestamp == 0 && pkts[1].Timestamp == 40, "single setup timestamps wrong: %d,%d", pkts[0].Timestamp, pkts[1].Timestamp)
			}
		}
	})
}

func TestSenderReportClockPackets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// case1: FU-A start/middle/end重排后恢复同一个NAL，时间戳为该帧的媒体时刻
		{
			h := newSrClockHarness(c, srTestVideoSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video failed")
			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)

			// 先送一个首帧建立唯一零点，FU-A帧在锚点之后40ms
			h.s.HandleInterleavedPacket(srRtpPacket(96, 9, 90000, srTestVideoSsrc, []byte{0x65, 0x00}), 0)
			const fuTs uint32 = 93600 // 相对零点90000为40ms
			h.s.HandleInterleavedPacket(srRtpPacket(96, 10, fuTs, srTestVideoSsrc, []byte{0x7c, 0x85, 0x11}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 11, fuTs, srTestVideoSsrc, []byte{0x7c, 0x05, 0x22}), 0)
			c.Check(h.o.avCount() == 1, "FU-A start+middle must not produce a full frame")
			h.s.HandleInterleavedPacket(srRtpPacket(96, 12, fuTs, srTestVideoSsrc, []byte{0x7c, 0x45, 0x33, 0x44}), 0)

			pkts, _ := h.o.snapshot()
			c.Check(len(pkts) == 2, "base + FU-A should produce two frames, got=%d", len(pkts))
			if len(pkts) == 2 {
				c.Check(pkts[0].Timestamp == 0, "base frame ts want=0 got=%d", pkts[0].Timestamp)
				c.Check(pkts[1].Timestamp == 40, "FU-A frame ts want=40 got=%d", pkts[1].Timestamp)
				want := srAvccPayload([]byte{0x65, 0x11, 0x22, 0x33, 0x44})
				c.Check(bytes.Equal(pkts[1].Payload, want), "FU-A reassembled payload want=%x got=%x", want, pkts[1].Payload)
			}
		}

		// case2: STAP-A中的两个NAL共用所属RTP包的时刻，合并为一个AvPacket
		{
			h := newSrClockHarness(c, srTestVideoSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video failed")
			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)

			// 先送首帧建立零点，STAP-A在40ms处
			h.s.HandleInterleavedPacket(srRtpPacket(96, 19, 90000, srTestVideoSsrc, []byte{0x65, 0x00}), 0)

			nal1 := []byte{0x67, 0xaa}
			nal2 := []byte{0x68, 0xbb, 0xcc}
			stap := []byte{0x78}
			l := make([]byte, 2)
			binary.BigEndian.PutUint16(l, uint16(len(nal1)))
			stap = append(stap, l...)
			stap = append(stap, nal1...)
			binary.BigEndian.PutUint16(l, uint16(len(nal2)))
			stap = append(stap, l...)
			stap = append(stap, nal2...)

			h.s.HandleInterleavedPacket(srRtpPacket(96, 20, 93600, srTestVideoSsrc, stap), 0)

			pkts, _ := h.o.snapshot()
			c.Check(len(pkts) == 2, "base + STAP-A should produce two AvPackets, got=%d", len(pkts))
			if len(pkts) == 2 {
				c.Check(pkts[1].Timestamp == 40, "STAP-A shared ts want=40 got=%d", pkts[1].Timestamp)
				want := srAvccPayload(nal1, nal2)
				c.Check(bytes.Equal(pkts[1].Payload, want), "STAP-A payload want=%x got=%x", want, pkts[1].Payload)
			}
		}

		// case3: 32位RTP时间戳向前回绕，模32位有符号tick差后输出0/50/100ms，不触发旧的逐轨归零
		{
			h := newSrClockHarness(c, srTestVideoSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video failed")

			const anchor uint32 = 0xfffff000
			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, anchor, srTestNtpMsw, 0), 1)

			h.s.HandleInterleavedPacket(srRtpPacket(96, 30, anchor, srTestVideoSsrc, []byte{0x65, 0xa0}), 0)
			// anchor+4500=0x100000194 -> 回绕后0x194，4500/90000s=50ms
			h.s.HandleInterleavedPacket(srRtpPacket(96, 31, uint32(0x194), srTestVideoSsrc, []byte{0x65, 0xa1}), 0)
			// anchor+9000 -> 回绕后0x1328，100ms
			h.s.HandleInterleavedPacket(srRtpPacket(96, 32, uint32(0x1328), srTestVideoSsrc, []byte{0x65, 0xa2}), 0)

			pkts, _ := h.o.snapshot()
			c.Check(len(pkts) == 3, "wrap case frame count want=3 got=%d", len(pkts))
			if len(pkts) == 3 {
				c.Check(pkts[0].Timestamp == 0, "wrap ts0 want=0 got=%d", pkts[0].Timestamp)
				c.Check(pkts[1].Timestamp == 50, "wrap ts1 want=50 got=%d", pkts[1].Timestamp)
				c.Check(pkts[2].Timestamp == 100, "wrap ts2 want=100 got=%d", pkts[2].Timestamp)
				c.Check(bytes.Equal(pkts[2].Payload, srAvccPayload([]byte{0x65, 0xa2})), "wrap payload order/content lost: %x", pkts[2].Payload)
			}
		}

		// case4: SR在若干完整帧之后才到，暂存帧必须使用各自原始RTP时刻，不能用最后收到的RTP头替代
		{
			h := newSrClockHarness(c, srTestVideoSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video failed")

			h.s.HandleInterleavedPacket(srRtpPacket(96, 40, 90000, srTestVideoSsrc, []byte{0x65, 0xb0}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 41, 93600, srTestVideoSsrc, []byte{0x65, 0xb1}), 0)
			c.Check(h.o.avCount() == 0, "frames before SR must stay staged")

			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)

			pkts, _ := h.o.snapshot()
			c.Check(len(pkts) == 2, "staged frames should flush, got=%d", len(pkts))
			if len(pkts) == 2 {
				c.Check(pkts[0].Timestamp == 0 && pkts[1].Timestamp == 40,
					"staged frames must keep their own rtp moment, got=%d,%d", pkts[0].Timestamp, pkts[1].Timestamp)
				c.Check(bytes.Equal(pkts[0].Payload, srAvccPayload([]byte{0x65, 0xb0})), "staged frame0 content mismatch")
				c.Check(bytes.Equal(pkts[1].Payload, srAvccPayload([]byte{0x65, 0xb1})), "staged frame1 content mismatch")
			}
		}
	})
}

func TestSenderReportClockIngress(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// ---------- TCP interleaved: 坏尾包不得让前面的SR发布映射或释放帧 ----------
		{
			h := newSrClockHarness(c, srTestVideoSdp)
			h.initClock()
			c.Check(h.s.SetupWithChannel("v", 0, 1) == nil, "setup video failed")
			h.s.HandleInterleavedPacket(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1}), 0)

			// 合法SR后面跟一个截断的RR子包：length=2声明12字节，实际只有4字节头
			truncatedTail := []byte{0x80, 201, 0x00, 0x02}
			badCompound := append(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), truncatedTail...)
			h.s.HandleInterleavedPacket(badCompound, 1)

			pkts, _ := h.o.snapshot()
			c.Check(len(pkts) == 0, "truncated tail must prevent earlier SR from releasing frames, got=%d", len(pkts))

			// SR的report-count声明了report block但包体缺失
			badRcSr := srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0)
			badRcSr[0] = 0x81 // V=2 RC=1，但length仍为6(28字节)，缺少24字节report block
			h.s.HandleInterleavedPacket(badRcSr, 1)
			pkts, _ = h.o.snapshot()
			c.Check(len(pkts) == 0, "SR with missing report blocks must be rejected, got=%d", len(pkts))

			// 随后一个完整合法的SR可以正常建立映射并释放帧，RR回复保留
			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			pkts, _ = h.o.snapshot()
			c.Check(len(pkts) == 1 && pkts[0].Timestamp == 0, "valid SR after bad datagrams should release frame, got=%+v", pkts)
			c.Check(h.w.count() >= 1, "normal RR reply should be retained, got=%d", h.w.count())
		}

		// ---------- TCP interleaved: 早到SR ----------
		{
			h := newSrClockHarness(c, srTestDualSdp)
			h.initClock()
			h.setupDualChannels()

			h.s.HandleInterleavedPacket(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0), 1)
			h.s.HandleInterleavedPacket(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter), 3)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1, 2}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 1, 8000, srTestAudioSsrc, []byte{1, 2, 3}), 2)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 2, 93600, srTestVideoSsrc, []byte{0x41, 3, 4}), 0)
			h.s.HandleInterleavedPacket(srRtpPacket(8, 2, 8320, srTestAudioSsrc, []byte{4, 5, 6}), 2)
			h.s.HandleInterleavedPacket(srRtpPacket(96, 3, 112500, srTestVideoSsrc, []byte{0x65, 5, 6}), 0)

			srCheckCanonicalDualOutput(c, h.o, 5)
		}

		// ---------- 实际本地UDP收包：SetupWithConn + RunLoop ----------
		srRunUdpHarness(c)
	})
}

// srRunUdpHarness 起4个真实的本地UDP接收连接，验证UDP入口与interleaved共用同一套时钟逻辑
func srRunUdpHarness(c *taskcheck.C) {
	newServerConn := func(tag string) *net.UDPConn {
		pc, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 0})
		c.Check(err == nil, "listen udp %s failed: %v", tag, err)
		return pc
	}

	vRtpPC := newServerConn("video rtp")
	if vRtpPC == nil {
		return
	}
	defer vRtpPC.Close()
	vRtcpPC := newServerConn("video rtcp")
	if vRtcpPC == nil {
		return
	}
	defer vRtcpPC.Close()
	aRtpPC := newServerConn("audio rtp")
	if aRtpPC == nil {
		return
	}
	defer aRtpPC.Close()
	aRtcpPC := newServerConn("audio rtcp")
	if aRtcpPC == nil {
		return
	}
	defer aRtcpPC.Close()

	h := newSrClockHarness(c, srTestDualSdp)
	h.initClock()

	vRtpConn, err := nazanet.NewUdpConnection(func(o *nazanet.UdpConnectionOption) { o.Conn = vRtpPC })
	c.Check(err == nil, "new video rtp conn: %v", err)
	vRtcpConn, err := nazanet.NewUdpConnection(func(o *nazanet.UdpConnectionOption) { o.Conn = vRtcpPC })
	c.Check(err == nil, "new video rtcp conn: %v", err)
	aRtpConn, err := nazanet.NewUdpConnection(func(o *nazanet.UdpConnectionOption) { o.Conn = aRtpPC })
	c.Check(err == nil, "new audio rtp conn: %v", err)
	aRtcpConn, err := nazanet.NewUdpConnection(func(o *nazanet.UdpConnectionOption) { o.Conn = aRtcpPC })
	c.Check(err == nil, "new audio rtcp conn: %v", err)
	if vRtpConn == nil || vRtcpConn == nil || aRtpConn == nil || aRtcpConn == nil {
		return
	}

	c.Check(h.s.SetupWithConn("v", vRtpConn, vRtcpConn) == nil, "setup video udp failed")
	c.Check(h.s.SetupWithConn("a", aRtpConn, aRtcpConn) == nil, "setup audio udp failed")
	defer h.s.Dispose()

	dial := func(addr *net.UDPAddr, tag string) *net.UDPConn {
		pc, err := net.DialUDP("udp", nil, addr)
		c.Check(err == nil, "dial udp %s failed: %v", tag, err)
		return pc
	}
	cvRtp := dial(vRtpPC.LocalAddr().(*net.UDPAddr), "video rtp")
	if cvRtp == nil {
		return
	}
	defer cvRtp.Close()
	cvRtcp := dial(vRtcpPC.LocalAddr().(*net.UDPAddr), "video rtcp")
	if cvRtcp == nil {
		return
	}
	defer cvRtcp.Close()
	caRtp := dial(aRtpPC.LocalAddr().(*net.UDPAddr), "audio rtp")
	if caRtp == nil {
		return
	}
	defer caRtp.Close()
	caRtcp := dial(aRtcpPC.LocalAddr().(*net.UDPAddr), "audio rtcp")
	if caRtcp == nil {
		return
	}
	defer caRtcp.Close()

	// 早到SR：先分别发往两轨的RTCP端口
	_, err = cvRtcp.Write(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0))
	c.Check(err == nil, "write early video sr: %v", err)
	_, err = caRtcp.Write(srSrPacket(srTestAudioSsrc, 8000, srTestNtpMsw, srTestNtpFracQuarter))
	c.Check(err == nil, "write early audio sr: %v", err)

	// 再发RTP和后续帧
	_, _ = cvRtp.Write(srRtpPacket(96, 1, 90000, srTestVideoSsrc, []byte{0x65, 1, 2}))
	_, _ = caRtp.Write(srRtpPacket(8, 1, 8000, srTestAudioSsrc, []byte{1, 2, 3}))
	_, _ = cvRtp.Write(srRtpPacket(96, 2, 93600, srTestVideoSsrc, []byte{0x41, 3, 4}))
	_, _ = caRtp.Write(srRtpPacket(8, 2, 8320, srTestAudioSsrc, []byte{4, 5, 6}))
	_, _ = cvRtp.Write(srRtpPacket(96, 3, 112500, srTestVideoSsrc, []byte{0x65, 5, 6}))

	c.Check(h.o.waitAv(4, 3*time.Second), "udp aligned packets not delivered in time, count=%d", h.o.avCount())
	// a2不参与第4个包的释放，这里额外等待所有RTP都完成通知，避免raw计数竞态
	c.Check(h.o.waitRaw(5, 2*time.Second), "udp raw rtp notify count mismatch, got=%d", h.o.rawCount())
	srCheckCanonicalDualOutput(c, h.o, 5)

	// RTP之后重发SR，RR应通过UDP地址回复到发送端
	_, _ = cvRtcp.Write(srSrPacket(srTestVideoSsrc, 90000, srTestNtpMsw, 0))
	_ = cvRtcp.SetReadDeadline(time.Now().Add(2 * time.Second))
	rrBuf := make([]byte, 1500)
	n, err := cvRtcp.Read(rrBuf)
	c.Check(err == nil, "read RR reply over udp failed: %v", err)
	if err == nil && n >= 2 {
		c.Check(rrBuf[1] == rtprtcp.RtcpPacketTypeRr, "reply should be RR pt=%d, got byte1=%d", rtprtcp.RtcpPacketTypeRr, rrBuf[1])
	}
}
