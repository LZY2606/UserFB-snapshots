// Copyright © Brett Vickers.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package ntp

import (
	"errors"
	"math"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

var eventUTC = time.Date(2026, 11, 10, 0, 0, 0, 0, time.UTC)

// eventClockResponse builds a valid offline NTPv4 response for EventClock
// tests. No network access is involved.
func eventClockResponse(offset, rootDistance time.Duration) *Response {
	return &Response{
		ClockOffset:    offset,
		Time:           eventUTC,
		ReferenceTime:  eventUTC,
		Version:        4,
		Stratum:        1,
		RootDistance:   rootDistance,
		RootDelay:      time.Millisecond,
		RootDispersion: time.Millisecond,
		Leap:           LeapNoWarning,
	}
}

func assertEventInterval(t *testing.T, iv EventInterval, center time.Time, radius time.Duration) {
	t.Helper()
	assert.True(t, center.Add(-radius).Equal(iv.Earliest),
		"earliest = %s, want %s", iv.Earliest, center.Add(-radius))
	assert.True(t, center.Add(radius).Equal(iv.Latest),
		"latest = %s, want %s", iv.Latest, center.Add(radius))
}

func TestEventClockProjectionAndContinuation(t *testing.T) {
	c, err := NewEventClock(time.Minute, 100)
	require.NoError(t, err)

	// Observe at mono=10s with an offset of +2s and a 10ms root distance.
	err = c.Observe(10*time.Second, eventUTC, eventClockResponse(2*time.Second, 10*time.Millisecond))
	require.NoError(t, err)

	// At mono=12s the center is 00:00:04Z. The drift radius is
	// ceil(2s * 100 / 1e6) = 200us, for a total radius of 10.2ms.
	iv, err := c.Stamp(12 * time.Second)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC.Add(4*time.Second), 10*time.Millisecond+200*time.Microsecond)
	assert.Equal(t, uint64(1), iv.Generation)

	// Continue at mono=13s with localUTC 00:00:33Z and offset -28s. The new
	// center is 00:00:05Z, which intersects the old anchor projected to the
	// same instant.
	err = c.Observe(13*time.Second, eventUTC.Add(33*time.Second),
		eventClockResponse(-28*time.Second, 10*time.Millisecond))
	require.NoError(t, err)

	iv, err = c.Stamp(13 * time.Second)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC.Add(5*time.Second), 10*time.Millisecond)

	// The same wall-clock jump at mono=14s without the offset correction does
	// not intersect the old projected interval and must be rejected.
	err = c.Observe(14*time.Second, eventUTC.Add(33*time.Second),
		eventClockResponse(0, 10*time.Millisecond))
	assert.ErrorIs(t, err, ErrClockDiscontinuity)

	// The rejected sample did not replace the old anchor. Its projection at
	// mono=14s is unchanged: center 00:00:06Z, radius 10ms + 100us.
	iv, err = c.Stamp(14 * time.Second)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC.Add(6*time.Second), 10*time.Millisecond+100*time.Microsecond)
	assert.Equal(t, uint64(1), iv.Generation)
}

func TestEventClockDriftCeilingIsOneNanosecond(t *testing.T) {
	c, err := NewEventClock(time.Hour, 100)
	require.NoError(t, err)
	require.NoError(t, c.Observe(10*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond)))

	// 1ns * 100 / 1e6 = 0.0001ns, which rounds up to one nanosecond.
	iv, err := c.Stamp(10*time.Second + time.Nanosecond)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC.Add(time.Nanosecond), 10*time.Millisecond+time.Nanosecond)
}

func TestEventClockAgeAndOrderLimits(t *testing.T) {
	c, err := NewEventClock(2*time.Second, 0)
	require.NoError(t, err)

	// No calibration yet.
	_, err = c.Stamp(0)
	assert.ErrorIs(t, err, ErrClockNotInitialized)

	require.NoError(t, c.Observe(10*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond)))

	// Before the anchor.
	_, err = c.Stamp(9 * time.Second)
	assert.ErrorIs(t, err, ErrClockBeforeAnchor)

	// Exactly at the anchor.
	iv, err := c.Stamp(10 * time.Second)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC, 10*time.Millisecond)

	// Exactly at maxAge is allowed.
	iv, err = c.Stamp(12 * time.Second)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC.Add(2*time.Second), 10*time.Millisecond)

	// One nanosecond beyond maxAge is not.
	_, err = c.Stamp(12*time.Second + time.Nanosecond)
	assert.ErrorIs(t, err, ErrClockExpired)
}

func TestEventClockClosedIntervalsMayTouch(t *testing.T) {
	// With zero drift, the old anchor at mono=10s projects at mono=11s to
	// [00:00:01.010, 00:00:01.010] and a new sample centered at
	// 00:00:01.020 has earliest exactly 00:00:01.010. The closed intervals
	// share an endpoint and must be accepted.
	t.Run("touching", func(t *testing.T) {
		c, err := NewEventClock(time.Minute, 0)
		require.NoError(t, err)
		require.NoError(t, c.Observe(10*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond)))

		err = c.Observe(11*time.Second, eventUTC.Add(time.Second+20*time.Millisecond),
			eventClockResponse(0, 10*time.Millisecond))
		require.NoError(t, err)

		iv, err := c.Stamp(11 * time.Second)
		require.NoError(t, err)
		assertEventInterval(t, iv, eventUTC.Add(time.Second+20*time.Millisecond), 10*time.Millisecond)
	})

	// One nanosecond further apart leaves a gap between the closed intervals.
	t.Run("one nanosecond gap", func(t *testing.T) {
		c, err := NewEventClock(time.Minute, 0)
		require.NoError(t, err)
		require.NoError(t, c.Observe(10*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond)))

		err = c.Observe(11*time.Second, eventUTC.Add(time.Second+20*time.Millisecond+time.Nanosecond),
			eventClockResponse(0, 10*time.Millisecond))
		assert.ErrorIs(t, err, ErrClockDiscontinuity)

		// The old anchor is still usable.
		iv, err := c.Stamp(11 * time.Second)
		require.NoError(t, err)
		assertEventInterval(t, iv, eventUTC.Add(time.Second), 10*time.Millisecond)
	})
}

func TestEventClockObserveUsesCopiedFields(t *testing.T) {
	r := eventClockResponse(2*time.Second, 10*time.Millisecond)
	c, err := NewEventClock(time.Minute, 100)
	require.NoError(t, err)
	require.NoError(t, c.Observe(10*time.Second, eventUTC, r))

	// The caller mutates the response after Observe. The anchor must retain
	// the original values.
	r.ClockOffset = 100 * time.Second
	r.RootDistance = 5 * time.Second
	r.Time = eventUTC.Add(48 * time.Hour)
	r.ReferenceTime = eventUTC.Add(48 * time.Hour)

	iv, err := c.Stamp(10 * time.Second)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC.Add(2*time.Second), 10*time.Millisecond)
}

func TestEventClockRejectsInvalidResponses(t *testing.T) {
	c, err := NewEventClock(time.Minute, 0)
	require.NoError(t, err)

	// Nil response.
	err = c.Observe(0, eventUTC, nil)
	assert.ErrorIs(t, err, ErrClockInvalidResponse)

	// NTPv5 response that is not synchronized.
	unsync := eventClockResponse(0, 10*time.Millisecond)
	unsync.Version = 5
	unsync.Flags = 0
	err = c.Observe(0, eventUTC, unsync)
	assert.ErrorIs(t, err, ErrClockInvalidResponse)
	assert.ErrorIs(t, err, ErrServerNotSynchronized)

	// Negative root distance.
	negative := eventClockResponse(0, -time.Millisecond)
	err = c.Observe(0, eventUTC, negative)
	assert.ErrorIs(t, err, ErrClockInvalidResponse)

	// None of the rejected samples established an anchor.
	_, err = c.Stamp(0)
	assert.ErrorIs(t, err, ErrClockNotInitialized)
}

func TestEventClockMonotonicOrderIsStrict(t *testing.T) {
	c, err := NewEventClock(time.Minute, 0)
	require.NoError(t, err)
	require.NoError(t, c.Observe(10*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond)))

	err = c.Observe(9*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond))
	assert.ErrorIs(t, err, ErrClockNonMonotonic)
	err = c.Observe(10*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond))
	assert.ErrorIs(t, err, ErrClockNonMonotonic)

	// The original anchor is untouched.
	iv, err := c.Stamp(10 * time.Second)
	require.NoError(t, err)
	assertEventInterval(t, iv, eventUTC, 10*time.Millisecond)
}

func TestEventClockReset(t *testing.T) {
	c, err := NewEventClock(time.Minute, 0)
	require.NoError(t, err)
	require.NoError(t, c.Observe(10*time.Second, eventUTC, eventClockResponse(0, 10*time.Millisecond)))

	before, err := c.Stamp(10 * time.Second)
	require.NoError(t, err)
	assert.Equal(t, uint64(1), before.Generation)

	// Reset clears calibration and increments the generation.
	c.Reset()
	_, err = c.Stamp(10 * time.Second)
	assert.ErrorIs(t, err, ErrClockNotInitialized)

	// After a reset, a discontinuous anchor is accepted without a continuity
	// check.
	require.NoError(t, c.Observe(1*time.Second, eventUTC.Add(100*time.Second),
		eventClockResponse(-100*time.Second, 10*time.Millisecond)))
	after, err := c.Stamp(time.Second)
	require.NoError(t, err)
	assert.Equal(t, uint64(2), after.Generation)

	// The interval returned before the reset is a value snapshot and remains
	// unchanged.
	assert.Equal(t, uint64(1), before.Generation)
	assert.True(t, before.Earliest.Equal(eventUTC.Add(-10*time.Millisecond)))
	assert.True(t, before.Latest.Equal(eventUTC.Add(10*time.Millisecond)))

	c.Reset()
	require.NoError(t, c.Observe(0, eventUTC, eventClockResponse(0, 10*time.Millisecond)))
	final, err := c.Stamp(0)
	require.NoError(t, err)
	assert.Equal(t, uint64(3), final.Generation)
}

func TestEventClockConstructorRejectsNonPositiveMaxAge(t *testing.T) {
	_, err := NewEventClock(0, 0)
	assert.ErrorIs(t, err, ErrClockInvalidMaxAge)
	_, err = NewEventClock(-time.Second, 0)
	assert.ErrorIs(t, err, ErrClockInvalidMaxAge)
}

func TestEventClockOverflow(t *testing.T) {
	// A maximal elapsed time multiplied by a maximal drift bound cannot be
	// represented as a Duration.
	c, err := NewEventClock(time.Duration(math.MaxInt64), math.MaxUint32)
	require.NoError(t, err)
	require.NoError(t, c.Observe(0, eventUTC, eventClockResponse(0, 0)))

	_, err = c.Stamp(time.Duration(math.MaxInt64))
	assert.ErrorIs(t, err, ErrClockOverflow)

	// Adding the offset overflows the time.Time range. The internal count of
	// seconds since year 1 is at its maximum, so any positive addition wraps.
	const unixToInternal = int64(62135596800)
	huge := time.Unix(math.MaxInt64-unixToInternal, 0).UTC()
	c2, err := NewEventClock(time.Minute, 0)
	require.NoError(t, err)
	err = c2.Observe(0, huge, eventClockResponse(2*time.Second, 0))
	assert.ErrorIs(t, err, ErrClockOverflow)
}

func TestEventClockNegativeMonotonic(t *testing.T) {
	c, err := NewEventClock(time.Minute, 0)
	require.NoError(t, err)
	_, err = c.Stamp(-time.Second)
	assert.ErrorIs(t, err, ErrClockInvalidMonotonic)

	err = c.Observe(-time.Second, eventUTC, eventClockResponse(0, 0))
	assert.ErrorIs(t, err, ErrClockInvalidMonotonic)
}

// Ensure the sentinel errors remain individually identifiable.
func TestEventClockErrorsAreDistinct(t *testing.T) {
	sentinels := []error{
		ErrClockInvalidMaxAge,
		ErrClockInvalidResponse,
		ErrClockInvalidMonotonic,
		ErrClockNotInitialized,
		ErrClockNonMonotonic,
		ErrClockBeforeAnchor,
		ErrClockExpired,
		ErrClockDiscontinuity,
		ErrClockOverflow,
	}
	for i := range sentinels {
		for j := range sentinels {
			if i == j {
				assert.True(t, errors.Is(sentinels[i], sentinels[j]))
			} else {
				assert.False(t, errors.Is(sentinels[i], sentinels[j]))
			}
		}
	}
}
