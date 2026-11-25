// Copyright 2020, Chef.  All rights reserved.
// https://github.com/q191201771/lal
//
// Use of this source code is governed by a MIT-style license
// that can be found in the License file.
//
// Author: Chef (191201771@qq.com)

package rtsp

import (
	"encoding/hex"
	"net"
	"sync"

	"github.com/q191201771/naza/pkg/nazaatomic"

	"github.com/q191201771/naza/pkg/nazabytes"
	"github.com/q191201771/naza/pkg/nazaerrors"

	"github.com/q191201771/lal/pkg/base"
	"github.com/q191201771/lal/pkg/rtprtcp"
	"github.com/q191201771/lal/pkg/sdp"
	"github.com/q191201771/naza/pkg/nazanet"
)

// 聚合PubSession和PullSession，也即流数据是输入类型的session

// IBaseInSessionObserver
//
// BaseInSession会向上层回调两种格式的数据(本质上是一份数据，业务方可自由选择使用)：
// 1. 原始的rtp packet
// 2. rtp合并后的av packet
type IBaseInSessionObserver interface {
	OnSdp(sdpCtx sdp.LogicContext)

	// OnRtpPacket 回调收到的RTP包
	//
	OnRtpPacket(pkt rtprtcp.RtpPacket)

	// OnAvPacket
	//
	// @param pkt: pkt结构体中字段含义见 rtprtcp.OnAvPacket
	//
	OnAvPacket(pkt base.AvPacket)
}

type BaseInSession struct {
	cmdSession IInterleavedPacketWriter

	observer IBaseInSessionObserver

	audioRtpConn     *nazanet.UdpConnection
	videoRtpConn     *nazanet.UdpConnection
	audioRtcpConn    *nazanet.UdpConnection
	videoRtcpConn    *nazanet.UdpConnection
	audioRtpChannel  int
	audioRtcpChannel int
	videoRtpChannel  int
	videoRtcpChannel int

	sessionStat base.BasicSessionStat

	mu              sync.Mutex
	sdpCtx          sdp.LogicContext // const after set
	avPacketQueue   *AvPacketQueue
	audioRrProducer *rtprtcp.RrProducer
	videoRrProducer *rtprtcp.RrProducer

	// withSrClock 是否开启Sender Report时钟模式，需在InitWithSdp前通过WithSenderReportClock设置
	withSrClock bool
	srClock     *senderReportClock

	audioUnpacker rtprtcp.IRtpUnpacker
	videoUnpacker rtprtcp.IRtpUnpacker

	audioSsrc nazaatomic.Uint32
	videoSsrc nazaatomic.Uint32

	disposeOnce sync.Once
	waitChan    chan error

	dumpReadAudioRtp base.LogDump
	dumpReadVideoRtp base.LogDump
	dumpReadRtcp     base.LogDump
	dumpReadSr       base.LogDump
}

func NewBaseInSession(sessionType base.SessionType, cmdSession IInterleavedPacketWriter) *BaseInSession {
	s := &BaseInSession{
		sessionStat:      base.NewBasicSessionStat(sessionType, ""),
		cmdSession:       cmdSession,
		waitChan:         make(chan error, 1),
		dumpReadAudioRtp: base.NewLogDump(Log, 1),
		dumpReadVideoRtp: base.NewLogDump(Log, 1),
		dumpReadSr:       base.NewLogDump(Log, 2),
	}
	Log.Infof("[%s] lifecycle new rtsp BaseInSession. session=%p", s.UniqueKey(), s)
	return s
}

func NewBaseInSessionWithObserver(sessionType base.SessionType, cmdSession IInterleavedPacketWriter, observer IBaseInSessionObserver) *BaseInSession {
	s := NewBaseInSession(sessionType, cmdSession)
	s.observer = observer
	return s
}

// WithSenderReportClock 开启或关闭Sender Report时钟模式。
//
// 必须在InitWithSdp之前调用。默认关闭，关闭时时间戳处理和回调行为与历史版本完全一致
// （音视频两轨各自归零，并经过AvPacketQueue归并）。
//
// 开启后，每一轨使用首个有效SR的NTP/RTP对应关系恢复公共媒体时间，
// 保留音视频首帧之间的真实间隔，详见sender_report_clock.go。
// 返回session自身，方便链式调用；也可以作为普通设置语句单独调用。
func (session *BaseInSession) WithSenderReportClock(enable bool) *BaseInSession {
	session.withSrClock = enable
	return session
}

func (session *BaseInSession) InitWithSdp(sdpCtx sdp.LogicContext) {
	session.mu.Lock()
	session.sdpCtx = sdpCtx
	session.mu.Unlock()

	if session.sdpCtx.IsAudioUnpackable() {
		if session.withSrClock {
			session.audioUnpacker = rtprtcp.DefaultRtpUnpackerFactoryWithOptions(session.sdpCtx.GetAudioPayloadTypeBase(), session.sdpCtx.AudioClockRate, unpackerItemMaxSize, session.onAvPacketUnpacked, rtprtcp.FactoryOptionRawRtpTimestamp())
		} else {
			session.audioUnpacker = rtprtcp.DefaultRtpUnpackerFactory(session.sdpCtx.GetAudioPayloadTypeBase(), session.sdpCtx.AudioClockRate, unpackerItemMaxSize, session.onAvPacketUnpacked)
		}
	} else {
		Log.Warnf("[%s] audio unpacker not support for this type yet. logicCtx=%+v", session.UniqueKey(), session.sdpCtx)
	}
	if session.sdpCtx.IsVideoUnpackable() {
		if session.withSrClock {
			session.videoUnpacker = rtprtcp.DefaultRtpUnpackerFactoryWithOptions(session.sdpCtx.GetVideoPayloadTypeBase(), session.sdpCtx.VideoClockRate, unpackerItemMaxSize, session.onAvPacketUnpacked, rtprtcp.FactoryOptionRawRtpTimestamp())
		} else {
			session.videoUnpacker = rtprtcp.DefaultRtpUnpackerFactory(session.sdpCtx.GetVideoPayloadTypeBase(), session.sdpCtx.VideoClockRate, unpackerItemMaxSize, session.onAvPacketUnpacked)
		}
	} else {
		Log.Warnf("[%s] video unpacker not support this type yet. logicCtx=%+v", session.UniqueKey(), session.sdpCtx)
	}

	session.audioRrProducer = rtprtcp.NewRrProducer(session.sdpCtx.AudioClockRate)
	session.videoRrProducer = rtprtcp.NewRrProducer(session.sdpCtx.VideoClockRate)

	if session.withSrClock {
		// SR时钟模式：时钟状态本身会按SDP中出现（以及SETUP确认）的轨道等待锚点。
		session.srClock = newSenderReportClock(session.sdpCtx, session.onClockAlignedAvPacket)
		// 双m-line时先建好归并队列；若随后只SETUP一轨，输出时会按mergeEnabled绕过它直接回调
		if session.srClock.mergeEnabled() {
			session.avPacketQueue = NewAvPacketQueue(session.onAvPacket)
		}
	} else if session.sdpCtx.IsAudioUnpackable() && session.sdpCtx.IsVideoUnpackable() {
		session.mu.Lock()
		if BaseInSessionTimestampFilterFlag {
			session.avPacketQueue = NewAvPacketQueue(session.onAvPacket)
		}
		session.mu.Unlock()
	}

	if session.observer != nil {
		session.observer.OnSdp(session.sdpCtx)
	}
}

// SetObserver 如果没有设置回调监听对象，可以通过该函数设置，调用方保证调用该函数发生在调用InitWithSdp之后
func (session *BaseInSession) SetObserver(observer IBaseInSessionObserver) {
	session.observer = observer

	// 避免在当前协程回调，降低业务方使用负担，不必担心设置监听对象和回调函数中锁重入 TODO(chef): 更好的方式
	go func() {
		session.observer.OnSdp(session.sdpCtx)
	}()
}

func (session *BaseInSession) SetupWithConn(uri string, rtpConn, rtcpConn *nazanet.UdpConnection) error {
	var rtcpTrack srTrackKind
	if session.sdpCtx.IsAudioUri(uri) {
		session.audioRtpConn = rtpConn
		session.audioRtcpConn = rtcpConn
		rtcpTrack = srTrackKindAudio
	} else if session.sdpCtx.IsVideoUri(uri) {
		session.videoRtpConn = rtpConn
		session.videoRtcpConn = rtcpConn
		rtcpTrack = srTrackKindVideo
	} else {
		return nazaerrors.Wrap(base.ErrRtsp)
	}

	session.markSrSetup(rtcpTrack)

	go rtpConn.RunLoop(session.onReadRtpPacket)
	go rtcpConn.RunLoop(func(b []byte, rAddr *net.UDPAddr, err error) bool {
		return session.onReadRtcpPacketTrack(b, rAddr, err, rtcpTrack)
	})

	return nil
}

func (session *BaseInSession) SetupWithChannel(uri string, rtpChannel, rtcpChannel int) error {
	if session.sdpCtx.IsAudioUri(uri) {
		session.audioRtpChannel = rtpChannel
		session.audioRtcpChannel = rtcpChannel
		session.markSrSetup(srTrackKindAudio)
		return nil
	} else if session.sdpCtx.IsVideoUri(uri) {
		session.videoRtpChannel = rtpChannel
		session.videoRtcpChannel = rtcpChannel
		session.markSrSetup(srTrackKindVideo)
		return nil
	}
	return nazaerrors.Wrap(base.ErrRtsp)
}

func (session *BaseInSession) markSrSetup(kind srTrackKind) {
	if session.srClock == nil {
		return
	}
	session.mu.Lock()
	session.srClock.markSetupLocked(kind)
	session.mu.Unlock()
}

// ---------------------------------------------------------------------------------------------------------------------
// IClientSessionLifecycle interface
// ---------------------------------------------------------------------------------------------------------------------

// Dispose 文档请参考： IClientSessionLifecycle interface
func (session *BaseInSession) Dispose() error {
	return session.dispose(nil)
}

// WaitChan 文档请参考： IClientSessionLifecycle interface
//
// 注意，目前只有一种情况，即上层主动调用Dispose函数，此时error为nil
func (session *BaseInSession) WaitChan() <-chan error {
	return session.waitChan
}

// ---------------------------------------------------------------------------------------------------------------------

func (session *BaseInSession) GetSdp() sdp.LogicContext {
	session.mu.Lock()
	defer session.mu.Unlock()
	return session.sdpCtx
}

func (session *BaseInSession) HandleInterleavedPacket(b []byte, channel int) {
	switch channel {
	case session.audioRtpChannel:
		fallthrough
	case session.videoRtpChannel:
		_ = session.handleRtpPacket(b)
	case session.audioRtcpChannel:
		fallthrough
	case session.videoRtcpChannel:
		_ = session.handleRtcpPacketTrack(b, nil, session.rtcpTrackKindByChannel(channel))
	default:
		Log.Errorf("[%s] read interleaved packet but channel invalid. channel=%d", session.UniqueKey(), channel)
	}
}

// WriteRtpRtcpDummy 发现pull时，需要先给对端发送数据，才能收到数据
func (session *BaseInSession) WriteRtpRtcpDummy() {
	if session.videoRtpConn != nil {
		_ = session.videoRtpConn.Write(dummyRtpPacket)
	}
	if session.videoRtcpConn != nil {
		_ = session.videoRtcpConn.Write(dummyRtcpPacket)
	}
	if session.audioRtpConn != nil {
		_ = session.audioRtpConn.Write(dummyRtpPacket)
	}
	if session.audioRtcpConn != nil {
		_ = session.audioRtcpConn.Write(dummyRtcpPacket)
	}
}

// ----- ISessionStat --------------------------------------------------------------------------------------------------

func (session *BaseInSession) GetStat() base.StatSession {
	return session.sessionStat.GetStat()
}

func (session *BaseInSession) UpdateStat(intervalSec uint32) {
	session.sessionStat.UpdateStat(intervalSec)
}

func (session *BaseInSession) IsAlive() (readAlive, writeAlive bool) {
	return session.sessionStat.IsAlive()
}

// ---------------------------------------------------------------------------------------------------------------------

func (session *BaseInSession) UniqueKey() string {
	return session.sessionStat.UniqueKey()
}

// callback by RTPUnpacker
func (session *BaseInSession) onAvPacketUnpacked(pkt base.AvPacket) {
	session.mu.Lock()
	defer session.mu.Unlock()

	if session.srClock != nil {
		// SR时钟模式：锚点/零点未齐时在这里面暂存，齐备后换算并回调
		session.srClock.onFrameLocked(pkt)
		return
	}

	if session.avPacketQueue != nil {
		session.avPacketQueue.Feed(pkt)
	} else {
		session.observer.OnAvPacket(pkt)
	}
}

// onClockAlignedAvPacket SR时钟换算完成后的帧出口，调用方持有session.mu
func (session *BaseInSession) onClockAlignedAvPacket(pkt base.AvPacket) {
	if session.srClock != nil && session.srClock.mergeEnabled() {
		if session.avPacketQueue == nil {
			session.avPacketQueue = NewAvPacketQueue(session.onAvPacket)
		}
		session.avPacketQueue.FeedAligned(pkt)
	} else {
		session.observer.OnAvPacket(pkt)
	}
}

// rtcpTrackKindByChannel 根据SETUP时记录的interleaved RTCP通道判断轨道，无法唯一确定时返回unknown
func (session *BaseInSession) rtcpTrackKindByChannel(channel int) srTrackKind {
	aCh := session.audioRtcpChannel
	vCh := session.videoRtcpChannel
	switch channel {
	case aCh:
		if aCh != vCh {
			return srTrackKindAudio
		}
	case vCh:
		if aCh != vCh {
			return srTrackKindVideo
		}
	}
	return srTrackKindUnknown
}

// callback by avpacket queue
func (session *BaseInSession) onAvPacket(pkt base.AvPacket) {
	session.observer.OnAvPacket(pkt)
}

// callback by UDPConnection
func (session *BaseInSession) onReadRtpPacket(b []byte, rAddr *net.UDPAddr, err error) bool {
	if err != nil {
		// TODO(chef):
		// read udp [::]:30008: use of closed network connection
		// 可以退出loop，看是在上层退还是下层退，但是要注意每次read都判断的开销
		Log.Warnf("[%s] read udp packet failed. err=%+v", session.UniqueKey(), err)
		return true
	}

	_ = session.handleRtpPacket(b)
	return true
}

// callback by UDPConnection
func (session *BaseInSession) onReadRtcpPacket(b []byte, rAddr *net.UDPAddr, err error) bool {
	return session.onReadRtcpPacketTrack(b, rAddr, err, srTrackKindUnknown)
}

// onReadRtcpPacketTrack 与onReadRtcpPacket相同，额外携带SETUP连接对应的轨道信息，用于早到SR的关联
func (session *BaseInSession) onReadRtcpPacketTrack(b []byte, rAddr *net.UDPAddr, err error, track srTrackKind) bool {
	if err != nil {
		Log.Warnf("[%s] read udp packet failed. err=%+v", session.UniqueKey(), err)
		return true
	}

	_ = session.handleRtcpPacketTrack(b, rAddr, track)
	return true
}

// @param rAddr 对端地址，往对端发送数据时使用，注意，如果nil，则表示是interleaved模式，我们直接往TCP连接发数据
func (session *BaseInSession) handleRtcpPacket(b []byte, rAddr *net.UDPAddr) error {
	return session.handleRtcpPacketTrack(b, rAddr, srTrackKindUnknown)
}

func (session *BaseInSession) handleRtcpPacketTrack(b []byte, rAddr *net.UDPAddr, track srTrackKind) error {
	session.sessionStat.AddReadBytes(len(b))

	if len(b) <= 0 {
		Log.Errorf("[%s] handleRtcpPacket but length invalid. len=%d", session.UniqueKey(), len(b))
		return nazaerrors.Wrap(base.ErrRtsp)
	}

	// SR时钟模式下，一个datagram可能包含多个子包，必须先整体校验边界/版本/长度/report-count，
	// 再逐个使用其中的SR，避免截断的尾包导致前面的SR提前发布映射或释放帧
	if session.srClock != nil {
		srs, valid := parseSrCompound(b)
		if !valid {
			Log.Errorf("[%s] handleRtcpPacket invalid compound packet. len=%d, hex=%s",
				session.UniqueKey(), len(b), hex.Dump(nazabytes.Prefix(b, 32)))
			return nazaerrors.Wrap(base.ErrRtsp)
		}
		for i := range srs {
			session.handleOneSr(&srs[i], b, rAddr, track)
		}
		return nil
	}

	packetType := b[1]

	switch packetType {
	case rtprtcp.RtcpPacketTypeSr:
		sr := rtprtcp.ParseSr(b)
		session.handleOneSr(&sr, b, rAddr, srTrackKindUnknown)
	default:
		Log.Warnf("[%s] handleRtcpPacket but type unknown. type=%d, len=%d, hex=%s",
			session.UniqueKey(), b[1], len(b), hex.Dump(nazabytes.Prefix(b, 32)))
		return nazaerrors.Wrap(base.ErrRtsp)
	}

	return nil
}

// handleOneSr 处理一个SR：既走原有RR回复路径，也在SR时钟模式下更新锚点
//
// @param rawDatagram SR所在的原始datagram，仅用于日志和写字节统计
func (session *BaseInSession) handleOneSr(sr *rtprtcp.Sr, rawDatagram []byte, rAddr *net.UDPAddr, track srTrackKind) {
	if session.dumpReadSr.ShouldDump() {
		session.dumpReadSr.Outf("[%s] READ_RTCP. sr=%+v", session.UniqueKey(), sr)
	}

	// RR回复严格保持原有按SSRC匹配音视频producer的行为
	var rrBuf []byte
	var rrTrack srTrackKind
	session.mu.Lock()
	switch sr.SenderSsrc {
	case session.audioSsrc.Load():
		rrBuf = session.audioRrProducer.Produce(sr.GetMiddleNtp())
		rrTrack = srTrackKindAudio
	case session.videoSsrc.Load():
		rrBuf = session.videoRrProducer.Produce(sr.GetMiddleNtp())
		rrTrack = srTrackKindVideo
	default:
		// noop
		//
		// ffmpeg推流时，会在发送第一个RTP包之前就发送一个SR，所以关闭这个警告日志
		//Log.Warnf("[%s] read rtcp sr but senderSsrc invalid. senderSsrc=%d, audio=%d, video=%d",
		//	p.uniqueKey, sr.SenderSsrc, p.audioSsrc, p.videoSsrc)
	}

	// 时钟锚点与RR相互独立，SSRC暂未确认时可借助SETUP轨道暂存早到SR
	if session.srClock != nil {
		session.srClock.offerSrLocked(track, *sr)
	}
	session.mu.Unlock()

	if rrBuf == nil {
		return
	}

	if rAddr != nil {
		switch rrTrack {
		case srTrackKindAudio:
			_ = session.audioRtcpConn.Write2Addr(rrBuf, rAddr)
		case srTrackKindVideo:
			_ = session.videoRtcpConn.Write2Addr(rrBuf, rAddr)
		}
	} else {
		switch rrTrack {
		case srTrackKindAudio:
			_ = session.cmdSession.WriteInterleavedPacket(rrBuf, session.audioRtcpChannel)
		case srTrackKindVideo:
			_ = session.cmdSession.WriteInterleavedPacket(rrBuf, session.videoRtcpChannel)
		}
	}
	session.sessionStat.AddWriteBytes(len(rawDatagram))
}

func (session *BaseInSession) handleRtpPacket(b []byte) error {
	session.sessionStat.AddReadBytes(len(b))

	if len(b) < rtprtcp.RtpFixedHeaderLength {
		Log.Errorf("[%s] handleRtpPacket but length invalid. len=%d", session.UniqueKey(), len(b))
		return nazaerrors.Wrap(base.ErrRtsp)
	}

	packetType := int(b[1] & 0x7F)
	if !session.sdpCtx.IsPayloadTypeOrigin(packetType) {
		//Log.Errorf("[%s] handleRtpPacket but type invalid. type=%d", session.UniqueKey(), packetType)
		return nazaerrors.Wrap(base.ErrRtsp)
	}

	h, err := rtprtcp.ParseRtpHeader(b)
	if err != nil {
		Log.Errorf("[%s] handleRtpPacket invalid rtp packet. err=%+v", session.UniqueKey(), err)
		return err
	}

	var pkt rtprtcp.RtpPacket
	pkt.Header = h
	pkt.Raw = b

	// 接收数据时，保证了sdp的原始类型对应
	if session.sdpCtx.IsAudioPayloadTypeOrigin(packetType) {
		if session.dumpReadAudioRtp.ShouldDump() {
			session.dumpReadAudioRtp.Outf("[%s] READ_RTP. audio, h=%+v, len=%d, hex=%s",
				session.UniqueKey(), h, len(b), hex.Dump(nazabytes.Prefix(b, 32)))
		}

		session.audioSsrc.Store(h.Ssrc)
		session.observer.OnRtpPacket(pkt)
		session.mu.Lock()
		session.audioRrProducer.FeedRtpPacket(h.Seq)
		if session.srClock != nil {
			session.srClock.confirmSsrcLocked(srTrackKindAudio, h.Ssrc)
		}
		session.mu.Unlock()

		if session.audioUnpacker != nil {
			session.audioUnpacker.Feed(pkt)
		}
	} else if session.sdpCtx.IsVideoPayloadTypeOrigin(packetType) {
		if session.dumpReadVideoRtp.ShouldDump() {
			session.dumpReadVideoRtp.Outf("[%s] READ_RTP. video, h=%+v, len=%d, hex=%s",
				session.UniqueKey(), h, len(b), hex.Dump(nazabytes.Prefix(b, 32)))
		}

		session.videoSsrc.Store(h.Ssrc)
		session.observer.OnRtpPacket(pkt)
		session.mu.Lock()
		session.videoRrProducer.FeedRtpPacket(h.Seq)
		if session.srClock != nil {
			session.srClock.confirmSsrcLocked(srTrackKindVideo, h.Ssrc)
		}
		session.mu.Unlock()

		if session.videoUnpacker != nil {
			session.videoUnpacker.Feed(pkt)
		}
	} else {
		// noop 因为前面已经判断过type了，所以永远不会走到这
	}

	return nil
}

func (session *BaseInSession) dispose(err error) error {
	var retErr error
	session.disposeOnce.Do(func() {
		Log.Infof("[%s] lifecycle dispose rtsp BaseInSession. session=%p", session.UniqueKey(), session)
		var e1, e2, e3, e4 error
		if session.audioRtpConn != nil {
			e1 = session.audioRtpConn.Dispose()
		}
		if session.audioRtcpConn != nil {
			e2 = session.audioRtcpConn.Dispose()
		}
		if session.videoRtpConn != nil {
			e3 = session.videoRtpConn.Dispose()
		}
		if session.videoRtcpConn != nil {
			e4 = session.videoRtcpConn.Dispose()
		}

		session.waitChan <- nil

		retErr = nazaerrors.CombineErrors(e1, e2, e3, e4)
	})
	return retErr
}
