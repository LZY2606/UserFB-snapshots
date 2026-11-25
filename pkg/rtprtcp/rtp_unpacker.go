// Copyright 2020, Chef.  All rights reserved.
// https://github.com/q191201771/lal
//
// Use of this source code is governed by a MIT-style license
// that can be found in the License file.
//
// Author: Chef (191201771@qq.com)

package rtprtcp

import (
	"github.com/q191201771/lal/pkg/base"
	"github.com/q191201771/naza/pkg/nazalog"
)

// 传入RTP包，合成帧数据，并回调返回
// 一路音频或一路视频各对应一个对象

var (
	_ IRtpUnpacker         = &RtpUnpackContainer{}
	_ IRtpUnpackContainer  = &RtpUnpackContainer{}
	_ IRtpUnpackerProtocol = &RtpUnpackerAac{}
	_ IRtpUnpackerProtocol = &RtpUnpackerAvcHevc{}
	_ IRtpUnpackerProtocol = &RtpUnpackerRaw{}
)

type IRtpUnpacker interface {
	IRtpUnpackContainer
}

type IRtpUnpackContainer interface {
	Feed(pkt RtpPacket)
}

type IRtpUnpackerProtocol interface {
	// CalcPositionIfNeeded 计算rtp包处于帧中的位置
	CalcPositionIfNeeded(pkt *RtpPacket)

	// TryUnpackOne 尝试合成一个完整帧
	//
	// 从当前队列的第一个包开始合成
	// 如果一个rtp包对应一个完整帧，则合成一帧
	// 如果一个rtp包对应多个完整帧，则合成多帧
	// 如果多个rtp包对应一个完整帧，则尝试合成一帧
	//
	// @return unpackedFlag 本次调用是否成功合成
	// @return unpackedSeq  如果成功合成，合成使用的最后一个seq号；如果失败，则为0
	TryUnpackOne(list *RtpPacketList) (unpackedFlag bool, unpackedSeq uint16)
}

// OnAvPacket
//
// @param pkt:
//
//	  pkt.Timestamp:
//	    RTP包头中的时间戳(pts)经过clockrate换算后的时间戳，单位毫秒。
//		注意，不支持带B帧的视频流，pts和dts永远相同。
//
//	  pkt.PayloadType: base.AvPacketPTXXX。
//
//	  pkt.Payload:
//		AAC:
//		  返回的是raw frame，一个AvPacket只包含一帧。
//		  引用的是接收到的RTP包中的内存块。
//		AVC或HEVC:
//		  AVCC格式，每个NAL前包含4字节NAL的长度。
//		  新申请的内存块，回调结束后，内部不再使用该内存块。
//		  注意，这一层只做RTP包的合并，假如sps和pps是两个RTP single包，则合并结果为两个AvPacket，
//		  假如sps和pps是一个stapA包，则合并结果为一个AvPacket。
type OnAvPacket func(pkt base.AvPacket)

// DefaultRtpUnpackerFactory 目前支持AVC，HEVC和AAC MPEG4-GENERIC，业务方也可以自己实现IRtpUnpackerProtocol，甚至是IRtpUnpackContainer
func DefaultRtpUnpackerFactory(payloadType base.AvPacketPt, clockRate int, maxSize int, onAvPacket OnAvPacket) IRtpUnpacker {
	return DefaultRtpUnpackerFactoryWithOptions(payloadType, clockRate, maxSize, onAvPacket)
}

// FactoryOption 用于配置DefaultRtpUnpackerFactoryWithOptions
type FactoryOption func(*factoryOptions)

type factoryOptions struct {
	// rawRtpTimestamp 为true时，输出的base.AvPacket.Timestamp保留RTP头中的原始uint32时间戳，
	// 而不是默认的除以clockRate/1000后的毫秒值
	rawRtpTimestamp bool
}

// FactoryOptionRawRtpTimestamp 让解包后的AvPacket.Timestamp保留原始RTP时间戳（tick单位）
func FactoryOptionRawRtpTimestamp() FactoryOption {
	return func(o *factoryOptions) {
		o.rawRtpTimestamp = true
	}
}

// DefaultRtpUnpackerFactoryWithOptions 与DefaultRtpUnpackerFactory相同，支持额外选项
func DefaultRtpUnpackerFactoryWithOptions(payloadType base.AvPacketPt, clockRate int, maxSize int, onAvPacket OnAvPacket, opts ...FactoryOption) IRtpUnpacker {
	var option factoryOptions
	for _, op := range opts {
		op(&option)
	}

	nazalog.Debugf("DefaultRtpUnpackerFactory. type=%d, clockRate=%d, maxSize=%d, rawRtpTimestamp=%v", payloadType, clockRate, maxSize, option.rawRtpTimestamp)
	var protocol IRtpUnpackerProtocol
	switch payloadType {
	case base.AvPacketPtAac:
		p := NewRtpUnpackerAac(payloadType, clockRate, onAvPacket)
		p.rawTimestamp = option.rawRtpTimestamp
		protocol = p
	case base.AvPacketPtG711U:
		fallthrough
	case base.AvPacketPtG711A, base.AvPacketPtOpus:
		p := NewRtpUnpackerRaw(payloadType, clockRate, onAvPacket)
		p.rawTimestamp = option.rawRtpTimestamp
		protocol = p
	case base.AvPacketPtAvc:
		fallthrough
	case base.AvPacketPtHevc:
		p := NewRtpUnpackerAvcHevc(payloadType, clockRate, onAvPacket)
		p.rawTimestamp = option.rawRtpTimestamp
		protocol = p
	default:
		Log.Fatalf("payload type not support yet. payloadType=%d", payloadType)
	}
	return NewRtpUnpackContainer(maxSize, protocol)
}

// avPacketTimestamp 各解包器共用的时间戳换算：默认输出毫秒，rawTimestamp时保留原始RTP tick
func avPacketTimestamp(rtpTimestamp uint32, clockRate int, rawTimestamp bool) int64 {
	if rawTimestamp {
		return int64(rtpTimestamp)
	}
	return int64(rtpTimestamp / uint32(clockRate/1000))
}
