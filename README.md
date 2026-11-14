# lal

## RTSP 接收端 Sender Report 时钟模式

RTSP 接收端（`BaseInSession`）默认用 `AvPacketQueue` 把音频和视频两轨的时间戳各自归零，
发送端两轨之间的起播间隔会丢失。开启可选的 Sender Report 时钟模式后：

- 每一轨用首个有效 SR（Sender Report）的 NTP/RTP 对应关系，把帧的原始 RTP 时间戳恢复成公共媒体时间；
- 所有已知轨道都拿到锚点、并且都至少有一个完整帧以后，取各轨首个完整帧里最早的媒体时刻作为唯一零点，之后不再移动；
- 零点建立前的完整帧会被暂存（只延后 `OnAvPacket`，原始 `OnRtpPacket` 照常通知），零点建立后按零点统一起算输出，音视频首包的起播间隔被保留。
  比如视频首帧对应 NTP 的整秒、音频首帧对应同一秒加四分之一秒，输出时间戳是 0ms 和 250ms；
- 后来的 SR 照常参与原有的 RR（Receiver Report）路径，但不改已经建立的映射；
- 目前支持 H264/90000 和 PCMA/8000，可以只有其中一轨；TCP interleaved 和 UDP 两种接收方式都走同一套时钟逻辑。

该模式默认关闭，已有的构造方式、observer 接口和默认模式行为不变。

### Go 调用示例

```go
package main

import (
	"github.com/q191201771/lal/pkg/base"
	"github.com/q191201771/lal/pkg/rtsp"
	"github.com/q191201771/lal/pkg/sdp"
)

// cmdSession 实现 rtsp.IInterleavedPacketWriter，observer 实现 rtsp.IBaseInSessionObserver
func newInSession(sdpRaw []byte, cmdSession rtsp.IInterleavedPacketWriter, observer rtsp.IBaseInSessionObserver) (*rtsp.BaseInSession, error) {
	ctx, err := sdp.ParseSdp2LogicContext(sdpRaw)
	if err != nil {
		return nil, err
	}

	session := rtsp.NewBaseInSessionWithObserver(base.SessionTypeRtspPub, cmdSession, observer)

	// 开启 Sender Report 时钟模式，注意必须在 InitWithSdp 之前设置
	session.WithSenderReportClock(true)

	session.InitWithSdp(ctx)
	return session, nil
}
```
