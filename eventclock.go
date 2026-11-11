// Copyright © Brett Vickers.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package ntp

import (
	"errors"
	"fmt"
	"math"
	"math/bits"
	"time"
)

// EventClock sentinel errors. Callers can test for them with errors.Is.
var (
	ErrClockInvalidMaxAge    = errors.New("event clock: max age must be positive")
	ErrClockInvalidResponse  = errors.New("event clock: invalid calibration response")
	ErrClockInvalidMonotonic = errors.New("event clock: monotonic time must be non-negative")
	ErrClockNotInitialized   = errors.New("event clock: no calibration anchor")
	ErrClockNonMonotonic     = errors.New("event clock: monotonic time did not increase")
	ErrClockBeforeAnchor     = errors.New("event clock: monotonic time precedes calibration anchor")
	ErrClockExpired          = errors.New("event clock: calibration anchor is too old")
	ErrClockDiscontinuity    = errors.New("event clock: calibration discontinuity")
	ErrClockOverflow         = errors.New("event clock: time arithmetic overflow")
)

// An EventInterval is a UTC time interval derived from a monotonic clock
// reading. Earliest and Latest are inclusive bounds. Generation identifies the
// calibration epoch in which the interval was produced.
//
// An EventInterval is a value snapshot. It is not modified by later calls to
// EventClock.Observe or EventClock.Reset.
type EventInterval struct {
	Earliest   time.Time
	Latest     time.Time
	Generation uint64
}

type eventClockAnchor struct {
	mono   time.Duration
	center time.Time
	radius time.Duration
}

// An EventClock converts monotonic elapsed times into UTC time intervals
// using a sequence of NTP calibration responses. It does not modify the
// system clock and does not perform NTP queries of its own.
//
// An EventClock is intended for serial use by a single goroutine. It performs
// no locking and is neither persisted nor safe for concurrent use.
type EventClock struct {
	maxAge     time.Duration
	driftPPM   uint32
	generation uint64
	anchor     *eventClockAnchor
}

// NewEventClock returns an EventClock that projects calibration anchors at
// most maxAge into the future, widening intervals by driftPPM parts per
// million of elapsed monotonic time. maxAge must be positive.
func NewEventClock(maxAge time.Duration, driftPPM uint32) (*EventClock, error) {
	if maxAge <= 0 {
		return nil, ErrClockInvalidMaxAge
	}
	return &EventClock{
		maxAge:     maxAge,
		driftPPM:   driftPPM,
		generation: 1,
	}, nil
}

// Observe records a calibration anchor sampled at monotonic time mono.
// localUTC is the local wall-clock reading taken at the same instant, and
// response is the NTP response from which the offset is obtained.
//
// The anchor's center is localUTC + response.ClockOffset and its base radius
// is response.RootDistance. Only values are retained; response may be
// modified by the caller after Observe returns.
//
// If an anchor already exists, mono must be strictly greater than the
// previous anchor's monotonic time, and the new anchor's interval must
// intersect the previous anchor's interval projected to mono using the same
// drift bound. If it does not, ErrClockDiscontinuity is returned and the
// previous anchor is left unchanged. A previous anchor is used for this
// continuity check even if it has exceeded the clock's max age.
//
// Observe accepts only non-nil responses that pass Validate and have a
// non-negative RootDistance.
func (c *EventClock) Observe(mono time.Duration, localUTC time.Time, response *Response) error {
	if mono < 0 {
		return ErrClockInvalidMonotonic
	}
	if response == nil {
		return fmt.Errorf("%w: nil response", ErrClockInvalidResponse)
	}
	if err := response.Validate(); err != nil {
		return fmt.Errorf("%w: %w", ErrClockInvalidResponse, err)
	}
	if response.RootDistance < 0 {
		return fmt.Errorf("%w: negative root distance", ErrClockInvalidResponse)
	}

	center, err := checkedTimeAdd(localUTC.UTC(), response.ClockOffset)
	if err != nil {
		return err
	}
	radius := response.RootDistance

	if a := c.anchor; a != nil {
		if mono <= a.mono {
			return ErrClockNonMonotonic
		}

		// Project the previous anchor to the new sample's monotonic time.
		delta := mono - a.mono
		oldDrift, err := driftRadius(delta, c.driftPPM)
		if err != nil {
			return err
		}
		oldRadius, err := addDurations(a.radius, oldDrift)
		if err != nil {
			return err
		}
		oldCenter, err := checkedTimeAdd(a.center, delta)
		if err != nil {
			return err
		}
		oldEarliest, err := checkedTimeAdd(oldCenter, -oldRadius)
		if err != nil {
			return err
		}
		oldLatest, err := checkedTimeAdd(oldCenter, oldRadius)
		if err != nil {
			return err
		}

		earliest, err := checkedTimeAdd(center, -radius)
		if err != nil {
			return err
		}
		latest, err := checkedTimeAdd(center, radius)
		if err != nil {
			return err
		}

		// The intervals are closed, so touching endpoints intersect.
		if latest.Before(oldEarliest) || oldLatest.Before(earliest) {
			return ErrClockDiscontinuity
		}
	}

	c.anchor = &eventClockAnchor{
		mono:   mono,
		center: center,
		radius: radius,
	}
	return nil
}

// Stamp projects an interval for the monotonic time mono. It uses only the
// most recent anchor, and succeeds only when mono is at or after the anchor
// and at most maxAge beyond it.
//
// The interval center is the anchor center plus elapsed monotonic time. Its
// radius is the anchor radius plus ceil(elapsed nanoseconds * driftPPM /
// 1,000,000) nanoseconds. Earliest and Latest are inclusive.
//
// Stamp does not consult the current wall clock. It returns
// ErrClockNotInitialized, ErrClockBeforeAnchor, ErrClockExpired or
// ErrClockOverflow when the interval cannot be projected.
func (c *EventClock) Stamp(mono time.Duration) (EventInterval, error) {
	if mono < 0 {
		return EventInterval{}, ErrClockInvalidMonotonic
	}
	a := c.anchor
	if a == nil {
		return EventInterval{}, ErrClockNotInitialized
	}
	if mono < a.mono {
		return EventInterval{}, ErrClockBeforeAnchor
	}

	elapsed := mono - a.mono
	if elapsed > c.maxAge {
		return EventInterval{}, ErrClockExpired
	}

	drift, err := driftRadius(elapsed, c.driftPPM)
	if err != nil {
		return EventInterval{}, err
	}
	radius, err := addDurations(a.radius, drift)
	if err != nil {
		return EventInterval{}, err
	}
	center, err := checkedTimeAdd(a.center, elapsed)
	if err != nil {
		return EventInterval{}, err
	}
	earliest, err := checkedTimeAdd(center, -radius)
	if err != nil {
		return EventInterval{}, err
	}
	latest, err := checkedTimeAdd(center, radius)
	if err != nil {
		return EventInterval{}, err
	}

	return EventInterval{
		Earliest:   earliest,
		Latest:     latest,
		Generation: c.generation,
	}, nil
}

// Reset discards the current calibration anchor and increments the clock's
// generation. The initial generation is 1. After Reset, a discontinuous
// anchor may be established without a continuity check.
func (c *EventClock) Reset() {
	c.anchor = nil
	c.generation++
}

// driftRadius returns ceil(elapsed * driftPPM / 1,000,000) as a Duration,
// reporting ErrClockOverflow if the result cannot be represented.
func driftRadius(elapsed time.Duration, driftPPM uint32) (time.Duration, error) {
	if elapsed <= 0 || driftPPM == 0 {
		return 0, nil
	}

	// Compute the 128-bit product to avoid overflowing elapsed * driftPPM.
	hi, lo := bits.Mul64(uint64(elapsed), uint64(driftPPM))
	const divisor = uint64(1_000_000)
	if hi >= divisor {
		return 0, ErrClockOverflow
	}
	q, r := bits.Div64(hi, lo, divisor)
	if q > math.MaxInt64 {
		return 0, ErrClockOverflow
	}
	if r != 0 {
		if q == math.MaxInt64 {
			return 0, ErrClockOverflow
		}
		q++
	}
	return time.Duration(q), nil
}

// addDurations adds two non-negative Durations, reporting ErrClockOverflow on
// overflow.
func addDurations(a, b time.Duration) (time.Duration, error) {
	if a < 0 || b < 0 {
		return 0, ErrClockOverflow
	}
	if a > math.MaxInt64-b {
		return 0, ErrClockOverflow
	}
	return a + b, nil
}

// checkedTimeAdd adds d to t, reporting ErrClockOverflow if the time value
// wraps around.
func checkedTimeAdd(t time.Time, d time.Duration) (time.Time, error) {
	if d == 0 {
		return t, nil
	}
	result := t.Add(d)
	if d > 0 {
		if !result.After(t) {
			return time.Time{}, ErrClockOverflow
		}
	} else if !result.Before(t) {
		return time.Time{}, ErrClockOverflow
	}
	return result, nil
}
