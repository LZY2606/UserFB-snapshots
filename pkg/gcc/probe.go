// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package gcc

import (
	"errors"
	"sync"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/internal/cc"
	"github.com/pion/interceptor/pkg/twcc"
	"github.com/pion/rtp"
)

// Padding probe limits and wire format constants.
const (
	probeMinPackets     = 4
	probeMaxPackets     = 64
	probeMinPadding     = 1
	probeMaxPadding     = 255
	probeRTPHeaderBytes = 20 // 12 byte RTP header + 8 byte one-byte header extension block carrying TWCC
	probeMinTWCCExtID   = 1
	probeMaxTWCCExtID   = 14
)

var (
	// ErrProbeNotEnabled is returned when a probe operation is requested but
	// no probe stream was enabled with SendSideBWE.EnableProbeStream.
	ErrProbeNotEnabled = errors.New("padding probe stream is not enabled")
	// ErrProbeStreamInvalid is returned when a probe stream configuration is invalid.
	ErrProbeStreamInvalid = errors.New("invalid padding probe stream configuration")
	// ErrProbeSSRCConflict is returned when the probe SSRC collides with a media stream.
	ErrProbeSSRCConflict = errors.New("padding probe SSRC conflicts with a media stream")
	// ErrProbePacerUnsupported is returned when the configured Pacer cannot schedule probes.
	ErrProbePacerUnsupported = errors.New("configured pacer does not support padding probes")
	// ErrProbeActive is returned when a probe cluster is already in flight.
	ErrProbeActive = errors.New("a padding probe cluster is already in flight")
	// ErrProbeNotActive is returned when cancelling but no probe cluster is in flight.
	ErrProbeNotActive = errors.New("no padding probe cluster is in flight")
	// ErrProbeConfigInvalid is returned when a probe cluster configuration is invalid.
	ErrProbeConfigInvalid = errors.New("invalid padding probe cluster configuration")
)

// ProbeStatus describes how a padding probe cluster ended.
type ProbeStatus int

const (
	// ProbeStatusSuccess means the cluster gathered enough receptions to publish a bitrate sample.
	ProbeStatusSuccess ProbeStatus = iota + 1
	// ProbeStatusCanceled means the cluster was canceled with SendSideBWE.CancelProbe.
	ProbeStatusCanceled
	// ProbeStatusTimeout means the cluster reached its timeout before succeeding.
	ProbeStatusTimeout
)

// String returns a human readable representation of the probe status.
func (s ProbeStatus) String() string {
	switch s {
	case ProbeStatusSuccess:
		return "success"
	case ProbeStatusCanceled:
		return "canceled"
	case ProbeStatusTimeout:
		return "timeout"
	default:
		return "unknown"
	}
}

// ProbeStreamConfig configures the reserved padding probe stream. The stream
// uses an application reserved SSRC, payload type and RTP clock that must be
// negotiated with the peer out of band; the receiver must not expect decodable
// media on it.
type ProbeStreamConfig struct {
	// SSRC of the probe stream. Must not collide with any media stream SSRC.
	SSRC uint32
	// PayloadType of probe packets.
	PayloadType uint8
	// ClockRate of the probe RTP timestamps in Hz (ticks per second, e.g. 90000). Must be positive.
	ClockRate uint32
	// InitialSequenceNumber is the RTP sequence number of the first probe packet.
	InitialSequenceNumber uint16
	// RTPTimeBase is the RTP timestamp that corresponds to LocalTimeBase.
	RTPTimeBase uint32
	// LocalTimeBase is the local monotonic time that corresponds to RTPTimeBase.
	LocalTimeBase time.Time
	// TWCCExtensionID is the negotiated transport-wide CC header extension ID (1-14).
	TWCCExtensionID uint8
	// HeaderExtension is the twcc.HeaderExtensionInterceptor shared with the
	// media streams, so probe and media packets allocate transport sequence
	// numbers from the same space.
	HeaderExtension *twcc.HeaderExtensionInterceptor
	// Writer is the underlying writer probe packets are sent to after the
	// header extension interceptor assigned a transport sequence number.
	Writer interceptor.RTPWriter
	// Now is the monotonic clock used for probe scheduling. Defaults to time.Now.
	Now func() time.Time
}

// ProbeConfig configures a single one-shot padding probe cluster.
type ProbeConfig struct {
	// TargetBitrate is the bitrate in bit/s the probe stream aims at. Must be positive.
	TargetBitrate int
	// PaddingBytes is the number of RTP padding bytes per probe packet (1-255).
	PaddingBytes int
	// Packets is the number of planned probe packet attempts (4-64).
	Packets int
	// Timeout after which the cluster ends without success. Must be positive.
	Timeout time.Duration
}

// ProbeResult is the outcome of a finished padding probe cluster.
type ProbeResult struct {
	// ClusterID identifies the cluster; it increases by one per started cluster.
	ClusterID uint64
	// Status is how the cluster ended.
	Status ProbeStatus
	// Attempts is the number of probe packet attempts (consumed RTP sequence numbers).
	Attempts int
	// Sent is the number of probe packets that were successfully written.
	Sent int
	// Received is the number of uniquely received probe packets reported via TWCC.
	Received int
	// Bitrate is the measured bitrate in bit/s. Only set when Status is ProbeStatusSuccess.
	Bitrate int
}

// probePacer is implemented by pacers that can schedule padding probes. It is
// intentionally unexported: custom Pacers keep satisfying Pacer and get
// ErrProbePacerUnsupported when probing is requested.
type probePacer interface {
	setProbeEngine(*probeEngine)
}

// probeSentPacket is the in-cluster record of a successfully written probe packet.
type probeSentPacket struct {
	transportSequence uint64 // unwrapped transport-wide sequence number
	departure         time.Time
	wireLength        int
	received          bool
	arrival           time.Time
}

type probeCluster struct {
	id          uint64
	cfg         ProbeConfig
	started     time.Time
	deadline    time.Time
	interval    time.Duration
	attempts    int
	lastAttempt time.Time
	hasAttempt  bool

	sent                 map[uint16]*probeSentPacket
	sentCount            int
	sentBytes            int
	firstDeparture       time.Time
	lastDeparture        time.Time
	lastDepartureWireLen int
	receivedCount        int
	receivedBytes        int
	firstArrival         time.Time
	firstArrivalWireLen  int
	lastArrival          time.Time
	referenceSequence    uint64
	hasReference         bool
}

// probeResultFire carries a finished result out of the locked section so the
// result callback is never invoked while holding the cluster state lock.
type probeResultFire struct {
	result   ProbeResult
	callback func(ProbeResult)
}

func (f *probeResultFire) invoke() {
	if f != nil && f.callback != nil {
		f.callback(f.result)
	}
}

// probeEngine owns the probe stream state and at most one in-flight cluster.
type probeEngine struct {
	lock    sync.Mutex
	stream  ProbeStreamConfig
	writer  interceptor.RTPWriter
	adapter *cc.FeedbackAdapter
	nowFn   func() time.Time

	nextRTPSequence uint16
	nextClusterID   uint64
	cluster         *probeCluster
	latest          ProbeResult
	hasLatest       bool
	// probeSequences holds the transport sequence numbers of every probe
	// packet ever sent on this engine; it is used to keep probe ACKs out of
	// the media loss/delay samples even after a cluster ended.
	probeSequences map[uint16]struct{}
	onResult       func(ProbeResult)
	closed         bool
}

func newProbeEngine(
	stream ProbeStreamConfig,
	writer interceptor.RTPWriter,
	adapter *cc.FeedbackAdapter,
	nowFn func() time.Time,
) *probeEngine {
	return &probeEngine{
		stream:          stream,
		writer:          writer,
		adapter:         adapter,
		nowFn:           nowFn,
		nextRTPSequence: stream.InitialSequenceNumber,
		nextClusterID:   1,
		probeSequences:  map[uint16]struct{}{},
	}
}

// probeReceiveThreshold is the number of unique receptions required for a
// successful cluster: ceil(0.8 * packets), but at least 4.
func probeReceiveThreshold(packets int) int {
	return max((4*packets+4)/5, 4)
}

// unwrapForward extends a 16 bit transport sequence number into the active
// sequence space. Sequence numbers are allocated monotonically, so wrapping
// only ever moves forward across 65535/0 and never reuses an identity within
// a full circle.
func unwrapForward(reference uint64, sequence uint16) uint64 {
	candidate := reference&^0xffff | uint64(sequence)
	if candidate < reference {
		candidate += 1 << 16
	}

	return candidate
}

func (e *probeEngine) setOnResult(f func(ProbeResult)) {
	e.lock.Lock()
	defer e.lock.Unlock()
	e.onResult = f
}

func (e *probeEngine) hasActiveCluster() bool {
	e.lock.Lock()
	defer e.lock.Unlock()

	return e.cluster != nil
}

func (e *probeEngine) latestResult() (ProbeResult, bool) {
	e.lock.Lock()
	defer e.lock.Unlock()

	return e.latest, e.hasLatest
}

// rtpTimestamp maps a local monotonic time onto the probe RTP timeline:
// RTPTimeBase + floor((now - LocalTimeBase) * ClockRate / 1e9), mod 2^32.
func (e *probeEngine) rtpTimestamp(now time.Time) uint32 {
	delta := now.Sub(e.stream.LocalTimeBase)
	ticks := uint64(delta.Nanoseconds()) * uint64(e.stream.ClockRate) / uint64(time.Second) //nolint:gosec // G115

	return uint32(uint64(e.stream.RTPTimeBase) + ticks) //nolint:gosec // G115
}

// start begins a new cluster. On any failure the previous state is preserved.
func (e *probeEngine) start(cfg ProbeConfig) (uint64, error) {
	if cfg.TargetBitrate <= 0 ||
		cfg.PaddingBytes < probeMinPadding || cfg.PaddingBytes > probeMaxPadding ||
		cfg.Packets < probeMinPackets || cfg.Packets > probeMaxPackets ||
		cfg.Timeout <= 0 {
		return 0, ErrProbeConfigInvalid
	}

	e.lock.Lock()
	defer e.lock.Unlock()
	if e.closed {
		return 0, ErrSendSideBWEClosed
	}
	if e.cluster != nil {
		return 0, ErrProbeActive
	}

	wireLength := probeRTPHeaderBytes + cfg.PaddingBytes
	// ceil(8 * wireLength * 1e9 / targetBitrate) nanoseconds between attempts.
	interval := time.Duration(
		(8*int64(wireLength)*int64(time.Second) + int64(cfg.TargetBitrate) - 1) / int64(cfg.TargetBitrate),
	)
	now := e.nowFn()
	id := e.nextClusterID
	e.nextClusterID++
	e.cluster = &probeCluster{
		id:       id,
		cfg:      cfg,
		started:  now,
		deadline: now.Add(cfg.Timeout),
		interval: interval,
		sent:     map[uint16]*probeSentPacket{},
	}

	return id, nil
}

func (e *probeEngine) cancel() error {
	e.lock.Lock()
	if e.cluster == nil {
		e.lock.Unlock()

		return ErrProbeNotActive
	}
	fire := e.finishLocked(ProbeStatusCanceled, 0)
	e.lock.Unlock()
	fire.invoke()

	return nil
}

// close releases in-flight records. The latest result is kept for GetStats.
func (e *probeEngine) close() {
	e.lock.Lock()
	defer e.lock.Unlock()
	e.closed = true
	e.cluster = nil
	e.probeSequences = map[uint16]struct{}{}
}

// maybeAttempt runs one scheduling step at tick time now. It sends at most one
// probe packet; late wakeups never catch up with a burst.
func (e *probeEngine) maybeAttempt(now time.Time) {
	e.lock.Lock()
	cluster := e.cluster
	if cluster == nil || e.closed {
		e.lock.Unlock()

		return
	}
	if !now.Before(cluster.deadline) {
		fire := e.finishLocked(ProbeStatusTimeout, 0)
		e.lock.Unlock()
		fire.invoke()

		return
	}
	if fire := e.maybeSucceedLocked(); fire != nil {
		e.lock.Unlock()
		fire.invoke()

		return
	}
	if cluster.attempts >= cluster.cfg.Packets {
		e.lock.Unlock()

		return
	}
	if cluster.hasAttempt && now.Before(cluster.lastAttempt.Add(cluster.interval)) {
		e.lock.Unlock()

		return
	}

	// Only an actual attempt consumes an RTP sequence number; failed writes
	// are not rolled back either.
	rtpSequence := e.nextRTPSequence
	e.nextRTPSequence++
	timestamp := e.rtpTimestamp(now)
	cluster.attempts++
	cluster.lastAttempt = now
	cluster.hasAttempt = true
	clusterID := cluster.id
	paddingLength := cluster.cfg.PaddingBytes
	e.lock.Unlock()

	header := &rtp.Header{
		Version:        2,
		Padding:        true,
		Marker:         false,
		PayloadType:    e.stream.PayloadType,
		SequenceNumber: rtpSequence,
		Timestamp:      timestamp,
		SSRC:           e.stream.SSRC,
	}
	// The packet carries no payload, only RTP padding; the last padding byte
	// holds the padding length, so the wire length is 20 + padding bytes.
	header.PaddingSize = byte(paddingLength) //nolint:gosec // G115
	var payload []byte
	attributes := interceptor.Attributes{}
	attributes.Set(cc.TwccExtensionAttributesKey, e.stream.TWCCExtensionID)

	// The writer assigns the transport sequence number via the shared header
	// extension interceptor and then hands the packet to the underlying
	// writer. Neither the writer nor any callback runs under the cluster lock.
	n, err := e.writer.Write(header, payload, attributes)
	wireLength := header.MarshalSize() + len(payload) + int(header.PaddingSize)
	if err != nil || n < wireLength {
		// Writer errors and short writes are not recorded as sent.
		return
	}
	if err = e.adapter.OnSent(now, header, len(payload)+int(header.PaddingSize), attributes); err != nil {
		return
	}
	var tccExt rtp.TransportCCExtension
	if err = tccExt.Unmarshal(header.GetExtension(e.stream.TWCCExtensionID)); err != nil {
		return
	}
	e.recordSent(clusterID, tccExt.TransportSequence, now, wireLength)
}

// recordSent retains a successfully written probe packet in its cluster. A
// write that completes after its cluster ended (or after a new cluster
// started) is dropped and must not modify the finished or new state.
func (e *probeEngine) recordSent(clusterID uint64, sequence uint16, departure time.Time, wireLength int) {
	e.lock.Lock()
	cluster := e.cluster
	if cluster == nil || cluster.id != clusterID {
		e.lock.Unlock()

		return
	}
	extended := uint64(sequence)
	if cluster.hasReference {
		extended = unwrapForward(cluster.referenceSequence, sequence)
	} else {
		cluster.hasReference = true
	}
	cluster.referenceSequence = extended
	cluster.sent[sequence] = &probeSentPacket{
		transportSequence: extended,
		departure:         departure,
		wireLength:        wireLength,
	}
	cluster.sentCount++
	cluster.sentBytes += wireLength
	if cluster.sentCount == 1 || departure.Before(cluster.firstDeparture) {
		cluster.firstDeparture = departure
	}
	if cluster.sentCount == 1 || !departure.Before(cluster.lastDeparture) {
		cluster.lastDeparture = departure
		cluster.lastDepartureWireLen = wireLength
	}
	e.probeSequences[sequence] = struct{}{}
	fire := e.maybeSucceedLocked()
	e.lock.Unlock()
	fire.invoke()
}

// filterAcks feeds TWCC acknowledgments into the in-flight cluster and returns
// the acknowledgments that do not belong to the probe stream, so media loss
// and delay control only ever see media ACKs.
func (e *probeEngine) filterAcks(acks []cc.Acknowledgment) []cc.Acknowledgment {
	e.lock.Lock()
	now := e.nowFn()
	var fire *probeResultFire
	cluster := e.cluster
	if cluster != nil && !now.Before(cluster.deadline) {
		fire = e.finishLocked(ProbeStatusTimeout, 0)
		cluster = nil
	}

	remaining := make([]cc.Acknowledgment, 0, len(acks))
	for _, ack := range acks {
		if _, ok := e.probeSequences[ack.SequenceNumber]; !ok {
			remaining = append(remaining, ack)

			continue
		}
		if cluster == nil || ack.Arrival.IsZero() {
			// Loss states never revoke a recorded reception and may be
			// overwritten by a later reception report.
			continue
		}
		record, ok := cluster.sent[ack.SequenceNumber]
		if !ok || record.received {
			// Not a probe packet of the in-flight cluster, or a duplicate
			// report: duplicates never count twice.
			continue
		}
		record.received = true
		record.arrival = ack.Arrival
		cluster.receivedCount++
		cluster.receivedBytes += record.wireLength
		if cluster.receivedCount == 1 || ack.Arrival.Before(cluster.firstArrival) {
			cluster.firstArrival = ack.Arrival
			cluster.firstArrivalWireLen = record.wireLength
		}
		if cluster.receivedCount == 1 || ack.Arrival.After(cluster.lastArrival) {
			cluster.lastArrival = ack.Arrival
		}
	}
	if cluster != nil {
		if f := e.maybeSucceedLocked(); f != nil {
			fire = f
		}
	}
	e.lock.Unlock()
	fire.invoke()

	return remaining
}

// maybeSucceedLocked publishes a success result once all planned attempts are
// done, enough unique receptions were reported and both the send and receive
// spans are positive.
func (e *probeEngine) maybeSucceedLocked() *probeResultFire {
	cluster := e.cluster
	if cluster == nil || cluster.attempts < cluster.cfg.Packets {
		return nil
	}
	if cluster.receivedCount < probeReceiveThreshold(cluster.cfg.Packets) {
		return nil
	}
	sendSpan := cluster.lastDeparture.Sub(cluster.firstDeparture)
	receiveSpan := cluster.lastArrival.Sub(cluster.firstArrival)
	if sendSpan <= 0 || receiveSpan <= 0 {
		return nil
	}
	sendRate := 8 * int64(cluster.sentBytes-cluster.lastDepartureWireLen) *
		int64(time.Second) / sendSpan.Nanoseconds()
	receiveRate := 8 * int64(cluster.receivedBytes-cluster.firstArrivalWireLen) *
		int64(time.Second) / receiveSpan.Nanoseconds()

	return e.finishLocked(ProbeStatusSuccess, int(min(sendRate, receiveRate)))
}

// finishLocked ends the in-flight cluster, stores the result and returns it
// for callback invocation outside the lock.
func (e *probeEngine) finishLocked(status ProbeStatus, bitrate int) *probeResultFire {
	cluster := e.cluster
	if cluster == nil {
		return nil
	}
	result := ProbeResult{
		ClusterID: cluster.id,
		Status:    status,
		Attempts:  cluster.attempts,
		Sent:      cluster.sentCount,
		Received:  cluster.receivedCount,
		Bitrate:   bitrate,
	}
	e.latest = result
	e.hasLatest = true
	e.cluster = nil

	return &probeResultFire{result: result, callback: e.onResult}
}
