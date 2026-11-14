// Copyright 2020, Chef.  All rights reserved.
// https://github.com/q191201771/lal
//
// Use of this source code is governed by a MIT-style license
// that can be found in the License file.
//
// Author: Chef (191201771@qq.com)

package rtsp

import (
	"github.com/q191201771/lal/pkg/base"
	"github.com/q191201771/lal/pkg/rtprtcp"
)

// senderReportClock 可选的Sender Report时钟模式
//
// 默认模式下，AvPacketQueue会把音频和视频各自归零，丢掉发送端两轨之间的起播间隔。
// 开启本模式后：
// 1. 每一轨用首个有效SR的NTP/RTP对应关系，把帧的原始RTP时间戳恢复成公共媒体时间
// 2. 所有已知轨道都拿到锚点、并且都至少有一个完整帧以后，取各轨首个完整帧里最早的媒体时刻作为唯一零点
// 3. 零点建立前完整帧被暂存（只延后OnAvPacket，原始OnRtpPacket照常通知），零点建立后按零点统一起算输出
//
// 关联以SETUP的轨道和随后确认的媒体SSRC为准：
// 另一轨的SR，或者SSRC对不上的SR，不能拿来代替本轨的锚点。
// 锚点一旦建立，后来的SR不再修改映射（它们照常参与原有的RR路径）。
//
// 注意，本类型的所有函数都只允许在 BaseInSession 的互斥锁保护下调用。
type senderReportClock struct {
	audio srClockTrack
	video srClockTrack

	// stashedSrs 媒体SSRC确认前暂存的SR，每个SSRC只保留首个
	stashedSrs []rtprtcp.Sr

	zeroSet bool
	zeroMs  int64

	pending []srClockPendingFrame

	// emit 输出一个已经换算好公共时间戳的完整帧（双轨时进归并队列，单轨时直接向上回调）
	emit func(pkt base.AvPacket)
}

type srClockTrack struct {
	known     bool // sdp中存在且可解包
	clockRate int

	ssrc    uint32 // 媒体SSRC，由收到的RTP包确认
	hasSsrc bool

	anchorSet   bool
	anchorRtp   uint32
	anchorNtpNs int64 // 锚点NTP对应的unix时间，单位纳秒

	hasFirstFrame bool
	firstFrameRtp uint32
}

type srClockPendingFrame struct {
	track *srClockTrack
	pkt   base.AvPacket
}

func newSenderReportClock(audioKnown bool, audioClockRate int, videoKnown bool, videoClockRate int, emit func(pkt base.AvPacket)) *senderReportClock {
	c := &senderReportClock{emit: emit}
	c.audio.known = audioKnown
	c.audio.clockRate = audioClockRate
	c.video.known = videoKnown
	c.video.clockRate = videoClockRate
	return c
}

// feedRtpSsrc 确认某轨的媒体SSRC（由该轨收到的RTP包得到）
func (c *senderReportClock) feedRtpSsrc(isAudio bool, ssrc uint32) {
	t := c.track(isAudio)
	if t == nil || t.hasSsrc {
		return
	}
	t.ssrc = ssrc
	t.hasSsrc = true

	// SSRC确认后，暂存的SR里首个对得上的成为本轨锚点
	for i, sr := range c.stashedSrs {
		if sr.SenderSsrc == ssrc {
			c.setAnchor(t, sr)
			c.stashedSrs = append(c.stashedSrs[:i], c.stashedSrs[i+1:]...)
			break
		}
	}

	c.tryEstablishZero()
}

// feedSr 输入一个校验通过的SR
func (c *senderReportClock) feedSr(sr rtprtcp.Sr) {
	for _, isAudio := range []bool{true, false} {
		t := c.track(isAudio)
		if t == nil || !t.hasSsrc || t.ssrc != sr.SenderSsrc {
			continue
		}
		// 锚点只定一次，后来的SR不改已经建立的映射
		if !t.anchorSet {
			c.setAnchor(t, sr)
			c.tryEstablishZero()
		}
		return
	}

	// SSRC还没确认的轨道无法判断该SR归属，先暂存，每个SSRC只保留首个
	for _, s := range c.stashedSrs {
		if s.SenderSsrc == sr.SenderSsrc {
			return
		}
	}
	c.stashedSrs = append(c.stashedSrs, sr)
}

// onFrame 输入一个解包出来的完整帧，帧的原始RTP时间戳记录在pkt.RtpTimestamp中
func (c *senderReportClock) onFrame(pkt base.AvPacket) {
	t := c.trackOf(pkt)
	if t == nil {
		// 不应该发生，该轨不在sdp中，退化为直接输出
		c.emit(pkt)
		return
	}

	if !t.hasFirstFrame {
		t.firstFrameRtp = pkt.RtpTimestamp
		t.hasFirstFrame = true
	}

	if c.zeroSet {
		c.emitFrame(t, pkt)
		return
	}

	// 零点还没建立，暂存帧（只延后OnAvPacket）
	c.pending = append(c.pending, srClockPendingFrame{track: t, pkt: pkt})
	c.tryEstablishZero()
}

func (c *senderReportClock) setAnchor(t *srClockTrack, sr rtprtcp.Sr) {
	t.anchorSet = true
	t.anchorRtp = sr.Timestamp
	t.anchorNtpNs = int64(rtprtcp.MswLsw2UnixNano(uint64(sr.Msw), uint64(sr.Lsw)))
	Log.Infof("[SRCLOCK] anchor established. clockRate=%d, ssrc=%d, anchorRtp=%d, anchorNtp=%d:%d",
		t.clockRate, sr.SenderSsrc, sr.Timestamp, sr.Msw, sr.Lsw)
}

// tryEstablishZero 已知轨道都拿到锚点、并且都至少有一个完整帧以后，建立唯一零点
func (c *senderReportClock) tryEstablishZero() {
	if c.zeroSet {
		return
	}

	knownCount := 0
	zeroMs := int64(0)
	for _, isAudio := range []bool{true, false} {
		t := c.track(isAudio)
		if t == nil {
			continue
		}
		if !t.anchorSet || !t.hasFirstFrame {
			return
		}
		ms := c.mediaMs(t, t.firstFrameRtp)
		if knownCount == 0 || ms < zeroMs {
			zeroMs = ms
		}
		knownCount++
	}
	if knownCount == 0 {
		return
	}

	c.zeroMs = zeroMs
	c.zeroSet = true
	Log.Infof("[SRCLOCK] zero established. zeroMs=%d, pendingFrames=%d", c.zeroMs, len(c.pending))

	// 暂存的帧也按这个零点输出，音视频间隔保留
	pending := c.pending
	c.pending = nil
	for _, pf := range pending {
		c.emitFrame(pf.track, pf.pkt)
	}
}

// mediaMs 用本轨锚点的NTP/RTP对应关系，把帧的原始RTP时间戳恢复成公共媒体时间，向下取整到毫秒
func (c *senderReportClock) mediaMs(t *srClockTrack, rtpTs uint32) int64 {
	// 模32位的有符号tick差（输入和锚点相距小于半个回绕周期）
	diff := int32(rtpTs - t.anchorRtp)
	ns := t.anchorNtpNs + floorDiv(int64(diff)*1e9, int64(t.clockRate))
	return floorDiv(ns, 1e6)
}

func (c *senderReportClock) emitFrame(t *srClockTrack, pkt base.AvPacket) {
	pkt.Timestamp = c.mediaMs(t, pkt.RtpTimestamp) - c.zeroMs
	c.emit(pkt)
}

func (c *senderReportClock) track(isAudio bool) *srClockTrack {
	if isAudio {
		if c.audio.known {
			return &c.audio
		}
		return nil
	}
	if c.video.known {
		return &c.video
	}
	return nil
}

func (c *senderReportClock) trackOf(pkt base.AvPacket) *srClockTrack {
	return c.track(pkt.IsAudio())
}

// floorDiv 向下取整的除法（b必须为正数）
func floorDiv(a, b int64) int64 {
	q := a / b
	if a%b != 0 && a < 0 {
		q--
	}
	return q
}
