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
	"github.com/q191201771/lal/pkg/sdp"
)

// Sender Report时钟模式
//
// 默认模式下，AvPacketQueue会把音视频两轨各自的首帧归零，从而丢失两轨首包RTP时间戳本来就可能存在的间隔。
// 开启本模式后：
//  1. 每一轨使用首个有效SR的<NTP, RTP>对应关系恢复媒体的公共时间；
//  2. 关联以SETUP确定的轨道以及随后由RTP确认的SSRC为准；
//  3. 所有SDP中出现的轨道都拿到锚点，并且至少解出一个完整帧后，
//     取各轨首个完整帧媒体时刻的最小值作为唯一零点，之后不再移动；
//  4. 零点确定前到达的完整帧先暂存，确定后按公共时间换算并交给既有双队列归并逻辑。
//
// 本文件中的所有状态均由BaseInSession.mu保护，方法命名带Locked后缀提醒调用方持锁。

type srTrackKind int

const (
	srTrackKindUnknown srTrackKind = iota
	srTrackKindAudio
	srTrackKindVideo
)

// srClockTrack 单轨的SR锚点和首帧信息
type srClockTrack struct {
	kind      srTrackKind
	clockRate int

	// setUp 该轨是否已经通过SETUP(SetupWithChannel/SetupWithConn)显式建立
	setUp bool

	// ssrcKnown/ssrc 由SETUP之后收到的该轨RTP包确认
	ssrcKnown bool
	ssrc      uint32

	// ssrc尚未确认时，先按SETUP轨道暂存SR，确认后挑选SSRC一致的最早一个作为锚点
	pendingSr []rtprtcp.Sr

	anchored    bool
	anchor      rtprtcp.Sr
	anchorNtpNs int64

	hasFirst   bool
	firstRtpTs uint32
}

// senderReportClock 维护所有已知轨道的锚点、唯一零点和零点前的暂存帧
type senderReportClock struct {
	tracks map[srTrackKind]*srClockTrack

	// anySetup 是否已有轨道完成SETUP。
	// 一旦发生过SETUP，需要等待的轨道以SETUP为准；否则退化为SDP中出现的全部轨道。
	// 这样“SDP里没出现的轨道不会去等”，同时兼容双m-line SDP但只SETUP/发送一轨的用法。
	anySetup bool

	zeroSet bool
	zeroNs  int64

	// staged 保存零点确定前的完整帧，按到达顺序排列。
	// 此模式下base.AvPacket.Timestamp中存放的是原始uint32 RTP tick（见rtprtcp.FactoryOptionRawRtpTimestamp）
	staged []base.AvPacket

	// sink 零点换算后的帧出口，由BaseInSession注入
	sink func(pkt base.AvPacket)
}

func newSenderReportClock(sdpCtx sdp.LogicContext, sink func(pkt base.AvPacket)) *senderReportClock {
	c := &senderReportClock{
		tracks: make(map[srTrackKind]*srClockTrack),
		sink:   sink,
	}

	if sdpCtx.HasAudio() {
		rate := sdpCtx.AudioClockRate
		if rate <= 0 {
			rate = 8000
		}
		c.tracks[srTrackKindAudio] = &srClockTrack{kind: srTrackKindAudio, clockRate: rate}
	}
	if sdpCtx.HasVideo() {
		rate := sdpCtx.VideoClockRate
		if rate <= 0 {
			rate = 90000
		}
		c.tracks[srTrackKindVideo] = &srClockTrack{kind: srTrackKindVideo, clockRate: rate}
	}
	return c
}

// markSetupLocked 记录某轨已经完成SETUP
func (c *senderReportClock) markSetupLocked(kind srTrackKind) {
	tr := c.tracks[kind]
	if tr == nil {
		return
	}
	c.anySetup = true
	tr.setUp = true
}

// expectedTracks 返回零点前必须拿到锚点的轨道
func (c *senderReportClock) expectedTracks() []*srClockTrack {
	var out []*srClockTrack
	for _, tr := range c.tracks {
		if !c.anySetup || tr.setUp {
			out = append(out, tr)
		}
	}
	return out
}

// mergeEnabled 两个预期轨道时启用双队列归并，单轨直接顺序输出
func (c *senderReportClock) mergeEnabled() bool {
	return len(c.expectedTracks()) == 2
}

// confirmSsrcLocked RTP包到达时确认该轨的媒体SSRC，并兑现早到的匹配SR
func (c *senderReportClock) confirmSsrcLocked(kind srTrackKind, ssrc uint32) {
	tr := c.tracks[kind]
	if tr == nil || tr.ssrcKnown {
		return
	}

	tr.ssrcKnown = true
	tr.ssrc = ssrc

	if tr.anchored {
		tr.pendingSr = nil
		return
	}

	// 只采用SSRC对得上的最早一个暂存SR
	for i := range tr.pendingSr {
		if tr.pendingSr[i].SenderSsrc == ssrc {
			c.setAnchorLocked(tr, tr.pendingSr[i])
			break
		}
	}
	tr.pendingSr = nil

	if c.tryOpenZeroLocked() {
		c.flushStagedLocked()
	}
}

// offerSrLocked 处理一个已经通过完整性校验的SR
//
// @param hint 该SR由哪条SETUP轨道的通道/连接收到，UDP和TCP interleaved均可确定
func (c *senderReportClock) offerSrLocked(hint srTrackKind, sr rtprtcp.Sr) {
	// 先按已经确认的SSRC找轨道
	var ssrcKind srTrackKind = srTrackKindUnknown
	for kind, tr := range c.tracks {
		if tr.ssrcKnown && tr.ssrc == sr.SenderSsrc {
			ssrcKind = kind
			break
		}
	}

	var target *srClockTrack
	if ssrcKind != srTrackKindUnknown {
		// SSRC已经表明它属于另一轨，而接收路径又明确指向本轨，认为是串轨SR，两轨的锚点都不能使用它
		if hint != srTrackKindUnknown && hint != ssrcKind {
			return
		}
		target = c.tracks[ssrcKind]
	} else if hint != srTrackKindUnknown {
		tr := c.tracks[hint]
		if tr == nil {
			return
		}
		if tr.ssrcKnown {
			// 本轨SSRC已确认，但SR的SSRC对不上，不能作为本轨锚点
			if tr.ssrc != sr.SenderSsrc {
				return
			}
			target = tr
		} else {
			// SSRC尚未确认（SR可能先于RTP到达），先按SETUP轨道暂存，等待SSRC确认
			tr.pendingSr = append(tr.pendingSr, sr)
			return
		}
	} else {
		// 既没有轨道提示，SSRC也对不上任何已知轨道，无法关联
		return
	}

	if target == nil || target.anchored {
		// 零点锚点每轨只定一次，后续SR仍走原有RR路径，但不改映射
		return
	}

	c.setAnchorLocked(target, sr)
	if c.tryOpenZeroLocked() {
		c.flushStagedLocked()
	}
}

// onFrameLocked 接收解包器输出的一个完整帧
func (c *senderReportClock) onFrameLocked(pkt base.AvPacket) {
	kind := srTrackKindAudio
	if pkt.IsVideo() {
		kind = srTrackKindVideo
	}
	tr := c.tracks[kind]
	if tr == nil {
		// SDP中没有该轨，理论上不会走到，直接透传避免吞数据
		c.sink(pkt)
		return
	}

	rawTs := uint32(pkt.Timestamp)
	if !tr.hasFirst {
		tr.hasFirst = true
		tr.firstRtpTs = rawTs
	}

	if !c.zeroSet {
		// 锚点未齐或零点尚未确定，延后OnAvPacket，保留帧自己的原始RTP时刻，
		// 不能在输出时用该轨最后收到的RTP头重新计算
		c.staged = append(c.staged, pkt)
		if c.tryOpenZeroLocked() {
			c.flushStagedLocked()
		}
		return
	}

	c.emitAlignedLocked(tr, pkt, rawTs)
}

func (c *senderReportClock) setAnchorLocked(tr *srClockTrack, sr rtprtcp.Sr) {
	tr.anchored = true
	tr.anchor = sr
	tr.anchorNtpNs = ntpToNs(sr.Msw, sr.Lsw)
}

// tryOpenZeroLocked 所有已知轨道都有锚点且至少有一个完整帧时，确定唯一零点
func (c *senderReportClock) tryOpenZeroLocked() bool {
	if c.zeroSet {
		return true
	}
	tracks := c.expectedTracks()
	if len(tracks) == 0 {
		return false
	}

	for _, tr := range tracks {
		if !tr.anchored {
			return false
		}
	}

	var zeroNs int64
	found := false
	for _, tr := range tracks {
		if !tr.hasFirst {
			continue
		}
		mediaNs := tr.mediaTimeNs(tr.firstRtpTs)
		if !found || mediaNs < zeroNs {
			zeroNs = mediaNs
			found = true
		}
	}
	if !found {
		// 至少要有一个完整帧
		return false
	}

	c.zeroNs = zeroNs
	c.zeroSet = true
	return true
}

func (c *senderReportClock) flushStagedLocked() {
	if !c.zeroSet || len(c.staged) == 0 {
		return
	}
	pending := c.staged
	c.staged = nil
	for i := range pending {
		pkt := pending[i]
		kind := srTrackKindAudio
		if pkt.IsVideo() {
			kind = srTrackKindVideo
		}
		tr := c.tracks[kind]
		if tr == nil {
			c.sink(pkt)
			continue
		}
		c.emitAlignedLocked(tr, pkt, uint32(pkt.Timestamp))
	}
}

// emitAlignedLocked 用帧自己的原始RTP钟与本轨锚点求模32位有符号tick差，
// 换算到NTP公共时间后，相对唯一零点向下取整到毫秒
func (c *senderReportClock) emitAlignedLocked(tr *srClockTrack, pkt base.AvPacket, rawTs uint32) {
	// uint32回绕差值再转int32，即模2^32的有符号tick差。
	// 约定输入RTP时刻与锚点相距小于半个回绕周期，因此歧义不存在。
	deltaTicks := int32(rawTs - tr.anchor.Timestamp)

	deltaNs := floorDiv(int64(deltaTicks)*1e9, int64(tr.clockRate))
	mediaNs := tr.anchorNtpNs + deltaNs

	pkt.Timestamp = floorDiv(mediaNs-c.zeroNs, 1e6)
	c.sink(pkt)
}

// mediaTimeNs 计算某原始RTP时刻对应的NTP公共时间（纳秒，向下取整）
func (tr *srClockTrack) mediaTimeNs(rawTs uint32) int64 {
	deltaTicks := int32(rawTs - tr.anchor.Timestamp)
	return tr.anchorNtpNs + floorDiv(int64(deltaTicks)*1e9, int64(tr.clockRate))
}

// ntpToNs 将NTP的64位定点时间（高32位秒，低32位小数秒）换算为纳秒
// 只用于两轨之间的相对差值，NTP纪元起点本身不影响结果
func ntpToNs(msw, lsw uint32) int64 {
	fractionNs := floorDiv(int64(lsw)*1_000_000_000, int64(1)<<32)
	return int64(msw)*1_000_000_000 + fractionNs
}

// floorDiv 数学意义上的向下取整除法（b必须为正数）
func floorDiv(a, b int64) int64 {
	q := a / b
	r := a % b
	if r != 0 && ((a < 0) != (b < 0)) {
		q--
	}
	return q
}

// parseSrCompound 校验整个RTCP datagram并取出其中的SR子包
//
// 一个datagram可以包含多个子包。必须依次检查：
// 子包头完整边界、版本号、length声明的边界，以及SR按report-count声明的长度。
// 只要有一个子包（包括尾部子包）不合法，就返回ok=false，
// 调用方不得发布其中任何SR的映射，也不得因此释放暂存帧。
func parseSrCompound(b []byte) (srs []rtprtcp.Sr, ok bool) {
	offset := 0
	for offset < len(b) {
		if len(b)-offset < rtprtcp.RtcpHeaderLength {
			return nil, false
		}

		h := rtprtcp.ParseRtcpHeader(b[offset:])
		if h.Version != rtprtcp.RtcpVersion {
			return nil, false
		}

		// length单位为32bit字，且整个子包长度=(length+1)*4
		blockLen := (int(h.Length) + 1) * 4
		if blockLen < rtprtcp.RtcpHeaderLength || offset+blockLen > len(b) {
			return nil, false
		}

		block := b[offset : offset+blockLen]
		if h.PacketType == rtprtcp.RtcpPacketTypeSr {
			reportCount := int(h.CountOrFormat)
			// SR: 4字节头 + 4字节SSRC + 20字节sender info + 每个report block 24字节
			if blockLen < 28+24*reportCount {
				return nil, false
			}
			srs = append(srs, rtprtcp.ParseSr(block))
		}

		offset += blockLen
	}

	if offset != len(b) {
		return nil, false
	}
	return srs, true
}
