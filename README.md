# lal — RTSP Sender Report 时钟模式

本仓库在 `BaseInSession` 上提供了一个可选的 **Sender Report 时钟模式**，用于保留发送端音视频首包之间真实的起播间隔。

## 背景

音视频首包的 RTP timestamp 本来就可以不同。默认模式下，`AvPacketQueue` 会把音视频两轨各自归零，因此即使接收端收到了 Sender Report（SR），两轨之间原本的时间间隔也会丢失（例如视频首帧在 `0ms`、音频首帧本应在 `250ms`，默认模式会把音频也抹成 `0ms`）。

通过 `WithSenderReportClock(true)` 开启 SR 时钟模式后：

- 每一轨使用**首个有效 SR** 的 `<NTP, RTP>` 对应关系恢复公共媒体时间，后续 SR 只继续参与原有 RR 回复，不再改动映射；
- SR 可以先于 RTP 到达，也可以在若干完整帧解包之后才到达；
- 关联以 `SETUP` 确定的轨道和随后由 RTP 确认的媒体 SSRC 为准，另一轨的 SR 或 SSRC 对不上的 SR 不会作为本轨锚点；
- 所有 SDP 中出现的轨道都拿到锚点、且至少有一个完整帧后，取这些轨道首个完整帧媒体时刻的最小值作为**唯一零点**，之后不再移动；零点前暂存的帧仍按该零点输出；
- 帧时间使用该完整帧自己的原始 RTP 时间戳，先与本轨锚点求模 32 位的有符号 tick 差，再按 clock rate 换算并向下取整到毫秒，正确处理时间戳回绕；
- 双轨时仍复用现有双队列互相等待的归并逻辑，轨内顺序和数据内容保持不变；
- TCP interleaved 与 UDP 接收走同一套时钟逻辑；RTCP compound datagram 会先整体校验边界、版本、长度和 SR 的 report-count 长度，截断的尾包不会让前面的 SR 发布映射或释放帧。

当前支持 H264/90000 与 PCMA/8000，可以只有其中一轨；SDP 中没有出现的轨道不会被等待。该选项默认关闭，已有构造方式、observer 接口和默认模式行为均保持不变。

## Go 调用示例

在 `InitWithSdp` 之前调用 `WithSenderReportClock(true)`：

```go
package main

import (
	"github.com/q191201771/lal/pkg/base"
	"github.com/q191201771/lal/pkg/rtprtcp"
	"github.com/q191201771/lal/pkg/rtsp"
	"github.com/q191201771/lal/pkg/sdp"
)

type observer struct{}

func (o *observer) OnSdp(sdp.LogicContext)                {}
func (o *observer) OnRtpPacket(pkt rtprtcp.RtpPacket)     {} // 原始RTP包照常通知
func (o *observer) OnAvPacket(pkt base.AvPacket) {
	// SR时钟模式下，pkt.Timestamp是相对唯一零点的毫秒值，
	// 音视频首帧之间的间隔（例如0ms与250ms）会被保留。
	if pkt.IsVideo() {
		// ...
	} else if pkt.IsAudio() {
		// ...
	}
}

type rrWriter struct{}

func (w *rrWriter) WriteInterleavedPacket(b []byte, channel int) error {
	// TCP interleaved模式下，RR等回复通过这里写回
	return nil
}

func main() {
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

	s := rtsp.NewBaseInSessionWithObserver(base.SessionTypeRtspPub, &rrWriter{}, &observer{})

	// 关键：在InitWithSdp之前开启，默认关闭；返回session自身，也可单独作为语句调用
	s.WithSenderReportClock(true).InitWithSdp(sdpCtx)

	// TCP interleaved
	if err := s.SetupWithChannel("v", 0, 1); err != nil {
		panic(err)
	}
	if err := s.SetupWithChannel("a", 2, 3); err != nil {
		panic(err)
	}

	// 之后照常把interleaved解帧后的RTP/RTCP包喂给：
	//   s.HandleInterleavedPacket(rtpOrRtcpPayload, channel)
	//
	// UDP模式则改用SetupWithConn(uri, rtpConn, rtcpConn)，
	// RTP/RTCP接收循环会进入完全相同的时钟与RR逻辑。
}
```

仓库的 `pkg/rtsp` 中还提供了可运行的 Go example：`ExampleBaseInSession_WithSenderReportClock`，以及三组测试：

```bash
go test ./pkg/rtsp -run '^TestSenderReport(Alignment|ClockPackets|ClockIngress)$' -count=1 -v
```

其中 `TestSenderReportClockIngress` 包含一项真实的本地 UDP 收发测试；如果运行环境禁止绑定本地 UDP 端口，需要允许本地 bind（例如在沙箱设置中打开 `sandbox.network.allowLocalBinding`）后再运行。
