// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package gcc

import (
	"errors"
	"math"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/internal/cc"
	"github.com/pion/interceptor/internal/ntp"
	"github.com/pion/logging"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
)

const (
	transportCCURI = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
	latestBitrate  = 10_000
	minBitrate     = 5_000
	maxBitrate     = 50_000_000
)

// ErrSendSideBWEClosed is raised when SendSideBWE.WriteRTCP is called after SendSideBWE.Close.
var ErrSendSideBWEClosed = errors.New("SendSideBwe closed")

// Pacer is the interface implemented by packet pacers.
type Pacer interface {
	interceptor.RTPWriter
	AddStream(ssrc uint32, writer interceptor.RTPWriter)
	SetTargetBitrate(int)
	Close() error
}

// Stats contains internal statistics of the bandwidth estimator.
type Stats struct {
	LossStats
	DelayStats
}

// SendSideBWE implements a combination of loss and delay based GCC.
type SendSideBWE struct {
	pacer           Pacer
	lossController  *lossBasedBandwidthEstimator
	delayController *delayController
	feedbackAdapter *cc.FeedbackAdapter

	onTargetBitrateChange func(bitrate int)

	lock          sync.Mutex
	latestStats   Stats
	latestBitrate int
	minBitrate    int
	maxBitrate    int

	mediaSSRCs    map[uint32]struct{}
	probe         atomic.Pointer[probeEngine]
	onProbeResult func(ProbeResult)

	close     chan struct{}
	closeLock sync.RWMutex

	loggerFactory logging.LoggerFactory
}

// Option configures a bandwidth estimator.
type Option func(*SendSideBWE) error

// SendSideBWEInitialBitrate sets the initial bitrate of new GCC interceptors.
func SendSideBWEInitialBitrate(rate int) Option {
	return func(e *SendSideBWE) error {
		e.latestBitrate = rate

		return nil
	}
}

// SendSideBWEMaxBitrate sets the initial bitrate of new GCC interceptors.
func SendSideBWEMaxBitrate(rate int) Option {
	return func(e *SendSideBWE) error {
		e.maxBitrate = rate

		return nil
	}
}

// SendSideBWEMinBitrate sets the initial bitrate of new GCC interceptors.
func SendSideBWEMinBitrate(rate int) Option {
	return func(e *SendSideBWE) error {
		e.minBitrate = rate

		return nil
	}
}

// SendSideBWEPacer sets the pacing algorithm to use.
func SendSideBWEPacer(p Pacer) Option {
	return func(e *SendSideBWE) error {
		e.pacer = p

		return nil
	}
}

// WithLoggerFactory sets the logger factory for the bandwidth estimator.
func WithLoggerFactory(factory logging.LoggerFactory) Option {
	return func(e *SendSideBWE) error {
		e.loggerFactory = factory

		return nil
	}
}

// NewSendSideBWE creates a new sender side bandwidth estimator.
func NewSendSideBWE(opts ...Option) (*SendSideBWE, error) {
	send := &SendSideBWE{
		pacer:                 nil,
		lossController:        nil,
		delayController:       nil,
		feedbackAdapter:       cc.NewFeedbackAdapter(),
		onTargetBitrateChange: nil,
		lock:                  sync.Mutex{},
		latestStats:           Stats{},
		latestBitrate:         latestBitrate,
		minBitrate:            minBitrate,
		maxBitrate:            maxBitrate,
		mediaSSRCs:            map[uint32]struct{}{},
		close:                 make(chan struct{}),
	}
	for _, opt := range opts {
		if err := opt(send); err != nil {
			return nil, err
		}
	}
	if send.loggerFactory == nil {
		send.loggerFactory = logging.NewDefaultLoggerFactory()
	}
	if send.pacer == nil {
		send.pacer = newLeakyBucketPacer(send.latestBitrate, send.loggerFactory)
	}
	send.lossController = newLossBasedBWE(send.latestBitrate, send.loggerFactory)
	send.delayController = newDelayController(delayControllerConfig{
		nowFn:          time.Now,
		initialBitrate: send.latestBitrate,
		minBitrate:     send.minBitrate,
		maxBitrate:     send.maxBitrate,
	}, send.loggerFactory)

	send.delayController.onUpdate(send.onDelayUpdate)

	return send, nil
}

// AddStream adds a new stream to the bandwidth estimator.
func (e *SendSideBWE) AddStream(info *interceptor.StreamInfo, writer interceptor.RTPWriter) interceptor.RTPWriter {
	var hdrExtID uint8
	for _, e := range info.RTPHeaderExtensions {
		if e.URI == transportCCURI {
			hdrExtID = uint8(e.ID) //nolint:gosec // G115

			break
		}
	}

	e.lock.Lock()
	e.mediaSSRCs[info.SSRC] = struct{}{}
	e.lock.Unlock()

	e.pacer.AddStream(info.SSRC, interceptor.RTPWriterFunc(
		func(header *rtp.Header, payload []byte, attributes interceptor.Attributes) (int, error) {
			if hdrExtID != 0 {
				if attributes == nil {
					attributes = make(interceptor.Attributes)
				}
				attributes.Set(cc.TwccExtensionAttributesKey, hdrExtID)
			}
			if err := e.feedbackAdapter.OnSent(time.Now(), header, len(payload), attributes); err != nil {
				return 0, err
			}

			return writer.Write(header, payload, attributes)
		},
	))

	return e.pacer
}

// EnableProbeStream reserves a dedicated stream for one-shot padding probe
// clusters. Probing is disabled until this is called. The probe stream shares
// the media HeaderExtensionInterceptor so transport sequence numbers come from
// a single space, and requires a Pacer that supports probe scheduling (the
// LeakyBucketPacer does).
func (e *SendSideBWE) EnableProbeStream(cfg ProbeStreamConfig) error {
	if cfg.ClockRate == 0 ||
		cfg.TWCCExtensionID < probeMinTWCCExtID || cfg.TWCCExtensionID > probeMaxTWCCExtID ||
		cfg.HeaderExtension == nil || cfg.Writer == nil {
		return ErrProbeStreamInvalid
	}

	e.lock.Lock()
	_, conflict := e.mediaSSRCs[cfg.SSRC]
	e.lock.Unlock()
	if conflict {
		return ErrProbeSSRCConflict
	}

	pacer, ok := e.pacer.(probePacer)
	if !ok {
		return ErrProbePacerUnsupported
	}
	if old := e.probe.Load(); old != nil && old.hasActiveCluster() {
		return ErrProbeActive
	}

	nowFn := cfg.Now
	if nowFn == nil {
		nowFn = time.Now
	}
	info := &interceptor.StreamInfo{
		SSRC:        cfg.SSRC,
		PayloadType: cfg.PayloadType,
		ClockRate:   cfg.ClockRate,
		RTPHeaderExtensions: []interceptor.RTPHeaderExtension{
			{URI: transportCCURI, ID: int(cfg.TWCCExtensionID)},
		},
	}
	writer := cfg.HeaderExtension.BindLocalStream(info, cfg.Writer)
	engine := newProbeEngine(cfg, writer, e.feedbackAdapter, nowFn)

	e.lock.Lock()
	engine.setOnResult(e.onProbeResult)
	e.lock.Unlock()

	pacer.setProbeEngine(engine)
	e.probe.Store(engine)

	return nil
}

// StartProbe starts a one-shot padding probe cluster and returns its cluster
// ID. At most one cluster is in flight at a time; on invalid configuration or
// an in-flight cluster an error is returned and the previous state is kept.
func (e *SendSideBWE) StartProbe(cfg ProbeConfig) (uint64, error) {
	engine := e.probe.Load()
	if engine == nil {
		return 0, ErrProbeNotEnabled
	}

	return engine.start(cfg)
}

// CancelProbe cancels the in-flight probe cluster and publishes a canceled
// result. A new cluster may be started immediately afterwards.
func (e *SendSideBWE) CancelProbe() error {
	engine := e.probe.Load()
	if engine == nil {
		return ErrProbeNotEnabled
	}

	return engine.cancel()
}

// LatestProbeResult returns the most recently finished probe cluster result.
func (e *SendSideBWE) LatestProbeResult() (ProbeResult, bool) {
	engine := e.probe.Load()
	if engine == nil {
		return ProbeResult{}, false
	}

	return engine.latestResult()
}

// OnProbeResult sets a callback that is invoked once whenever a probe cluster
// finishes. The callback never runs while the cluster state lock is held. The
// result is not used to change the GCC target bitrate automatically.
func (e *SendSideBWE) OnProbeResult(f func(ProbeResult)) {
	e.lock.Lock()
	e.onProbeResult = f
	e.lock.Unlock()
	if engine := e.probe.Load(); engine != nil {
		engine.setOnResult(f)
	}
}

// WriteRTCP adds some RTCP feedback to the bandwidth estimator.
//
//nolint:cyclop
func (e *SendSideBWE) WriteRTCP(pkts []rtcp.Packet, _ interceptor.Attributes) error {
	now := time.Now()
	e.closeLock.RLock()
	defer e.closeLock.RUnlock()

	if e.isClosed() {
		return ErrSendSideBWEClosed
	}

	for _, pkt := range pkts {
		var acks []cc.Acknowledgment
		var err error
		var feedbackSentTime time.Time
		switch fb := pkt.(type) {
		case *rtcp.TransportLayerCC:
			acks, err = e.feedbackAdapter.OnTransportCCFeedback(now, fb)
			if err != nil {
				return err
			}
			if engine := e.probe.Load(); engine != nil {
				// Probe ACKs feed the in-flight cluster and must not enter
				// the media loss/delay control samples.
				acks = engine.filterAcks(acks)
			}
			for i, ack := range acks {
				if i == 0 {
					feedbackSentTime = ack.Arrival

					continue
				}
				if ack.Arrival.After(feedbackSentTime) {
					feedbackSentTime = ack.Arrival
				}
			}
		case *rtcp.CCFeedbackReport:
			acks = e.feedbackAdapter.OnRFC8888Feedback(now, fb)
			feedbackSentTime = ntp.ToTime(uint64(fb.ReportTimestamp) << 16)
		default:
			continue
		}

		feedbackMinRTT := time.Duration(math.MaxInt)
		for _, ack := range acks {
			if ack.Arrival.IsZero() {
				continue
			}
			pendingTime := feedbackSentTime.Sub(ack.Arrival)
			rtt := now.Sub(ack.Departure) - pendingTime
			feedbackMinRTT = time.Duration(min(int(rtt), int(feedbackMinRTT)))
		}
		if feedbackMinRTT < math.MaxInt {
			e.delayController.updateRTT(feedbackMinRTT)
		}

		e.lossController.updateLossEstimate(acks)
		e.delayController.updateDelayEstimate(acks)
	}

	return nil
}

// GetTargetBitrate returns the current target bitrate in bits per second.
func (e *SendSideBWE) GetTargetBitrate() int {
	e.lock.Lock()
	defer e.lock.Unlock()

	return e.latestBitrate
}

// GetStats returns some internal statistics of the bandwidth estimator.
func (e *SendSideBWE) GetStats() map[string]any {
	e.lock.Lock()
	defer e.lock.Unlock()

	stats := map[string]any{
		"lossTargetBitrate":  e.latestStats.LossStats.TargetBitrate,
		"averageLoss":        e.latestStats.AverageLoss,
		"delayTargetBitrate": e.latestStats.DelayStats.TargetBitrate,
		"delayMeasurement":   float64(e.latestStats.Measurement.Microseconds()) / 1000.0,
		"delayEstimate":      float64(e.latestStats.Estimate.Microseconds()) / 1000.0,
		"delayThreshold":     float64(e.latestStats.Threshold.Microseconds()) / 1000.0,
		"usage":              e.latestStats.Usage.String(),
		"state":              e.latestStats.State.String(),
	}
	if engine := e.probe.Load(); engine != nil {
		if result, ok := engine.latestResult(); ok {
			stats["probeResult"] = result
		}
	}

	return stats
}

// OnTargetBitrateChange sets the callback that is called when the target
// bitrate in bits per second changes.
func (e *SendSideBWE) OnTargetBitrateChange(f func(bitrate int)) {
	e.onTargetBitrateChange = f
}

// isClosed returns true if SendSideBWE is closed.
func (e *SendSideBWE) isClosed() bool {
	select {
	case <-e.close:
		return true
	default:
		return false
	}
}

// Close stops and closes the bandwidth estimator.
func (e *SendSideBWE) Close() error {
	e.closeLock.Lock()
	defer e.closeLock.Unlock()

	if err := e.delayController.Close(); err != nil {
		return err
	}
	close(e.close)

	if engine := e.probe.Load(); engine != nil {
		engine.close()
	}

	return e.pacer.Close()
}

func (e *SendSideBWE) onDelayUpdate(delayStats DelayStats) {
	e.lock.Lock()
	defer e.lock.Unlock()

	lossStats := e.lossController.getEstimate(delayStats.TargetBitrate)
	bitrateChanged := false
	bitrate := min(delayStats.TargetBitrate, lossStats.TargetBitrate)
	if bitrate != e.latestBitrate {
		bitrateChanged = true
		e.latestBitrate = bitrate
		e.pacer.SetTargetBitrate(e.latestBitrate)
	}

	if bitrateChanged && e.onTargetBitrateChange != nil {
		go e.onTargetBitrateChange(bitrate)
	}

	e.latestStats = Stats{
		LossStats:  lossStats,
		DelayStats: delayStats,
	}
}
