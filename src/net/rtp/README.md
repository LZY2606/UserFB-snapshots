# GoRTP

GoRTP implements RTP/RTCP (RFC 3550) sessions, streams, and transports in Go.

## Manually pumped media sending (no sockets, no timers)

`ConfigurePacing`, `QueuePaced`, `PumpPaced`, and `CancelPaced` add a send queue
that the caller drives by hand. Each queued packet gets its deadline from the
configured media anchor, and `PumpPaced(now)` sends everything that is due —
useful to replay recorded audio/video through any `TransportWrite`, including a
pure in-memory transport, without opening sockets or starting a timer goroutine.

```go
package main

import (
	"fmt"
	"net"
	"time"

	rtp "github.com/wernerd/GoRTP/src/net/rtp/v2"
)

// memTransport is an in-memory TransportWrite/TransportRecv: it just records
// the RTP bytes it would put on the wire.
type memTransport struct{ wire [][]byte }

func (m *memTransport) WriteDataTo(p *rtp.DataPacket, a *rtp.Address) (int, error) {
	m.wire = append(m.wire, append([]byte(nil), p.Buffer()[:p.InUse()]...))
	return p.InUse(), nil
}
func (m *memTransport) WriteCtrlTo(p *rtp.CtrlPacket, a *rtp.Address) (int, error) {
	return p.InUse(), nil
}
func (m *memTransport) SetToLower(rtp.TransportWrite)      {}
func (m *memTransport) CloseWrite()                        {}
func (m *memTransport) ListenOnTransports() error          { return nil }
func (m *memTransport) OnRecvData(*rtp.DataPacket) bool    { return true }
func (m *memTransport) OnRecvCtrl(*rtp.CtrlPacket) bool    { return true }
func (m *memTransport) SetCallUpper(rtp.TransportRecv)     {}
func (m *memTransport) CloseRecv()                         {}
func (m *memTransport) SetEndChannel(rtp.TransportEnd)     {}

func main() {
	tp := &memTransport{}
	rs := rtp.NewSession(tp, tp)

	own := &rtp.Address{IPAddr: net.IPv4(127, 0, 0, 1), DataPort: 5004, CtrlPort: 5005}
	idx, _ := rs.NewSsrcStreamOut(own, 0x01020304, 1)
	rs.SsrcStreamOutForIndex(idx).SetPayloadType(0) // PCMU, 8000 Hz
	rs.AddRemote(&rtp.Address{IPAddr: net.IPv4(10, 0, 0, 1), DataPort: 6000, CtrlPort: 6001})

	// Anchor the media timeline: RTP timestamp 0 corresponds to baseTime.
	baseTime := time.Now()
	if err := rs.ConfigurePacing(idx, 8000, 0, baseTime, 16); err != nil {
		panic(err)
	}

	// Queue one 20ms PCMU frame (160 samples) after the other. QueuePaced
	// stores an independent copy, the packet may be freed right away.
	for i := 0; i < 5; i++ {
		p := rs.NewDataPacketForStream(idx, 0)
		p.SetTimestamp(uint32(i * 160))
		p.SetSequence(uint16(1 + i))
		p.SetPayload(make([]byte, 160))
		if err := rs.QueuePaced(idx, p); err != nil {
			panic(err)
		}
		p.FreePacket()
	}

	// The caller drives the clock. A late pump catches up all due packets at
	// once; PumpPaced never sleeps.
	for i := 0; i < 5; i++ {
		now := baseTime.Add(time.Duration(i+1) * 20 * time.Millisecond)
		retired, err := rs.PumpPaced(now)
		fmt.Printf("pump %d: retired=%d err=%v\n", i, retired, err)
	}
	fmt.Printf("captured %d RTP packets in memory\n", len(tp.wire))

	rs.CancelPaced(idx)
	rs.CloseSession()
}
```

Key rules:

- Deadlines are `baseTime + int64(int32(packet.Timestamp()-baseTimestamp)) * time.Second / clockRate`;
  negative media offsets are rejected, timestamp and sequence number may wrap.
- Sequence numbers must strictly advance (mod 2^16, step < 32768, gaps allowed);
  timestamps must not move backwards (equal timestamps cover fragments).
- `PumpPaced` arbitrates by head-of-queue deadline, then by ascending stream
  index; a packet whose targets are not all done blocks only its own stream.
- Sender statistics (`SenderPacketCnt`, `SenderOctectCnt`, RTCP SR contents)
  are updated exactly once per packet, on its first successful target write.
