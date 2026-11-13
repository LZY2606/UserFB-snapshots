'use strict'

// Task target cases. Each exported function receives a `check` object exposing
// real assertions (check.ok / equal / deepEqual / throws / rejects / notEqual).

const assert = require('node:assert/strict')
const WAAClock = require('../index')

// --------------------------------------------------------------------------
// Helpers : independent analytic model
// --------------------------------------------------------------------------

// Seconds elapsed while travelling `u` beats inside the segment that starts at
// point `p` and ends at point `next` (or forever when there is no next point).
function segmentElapsed(p, next, u) {
  if (u <= 0) return 0
  if (!next || p.curve === 'step' || next.bpm === p.bpm)
    return 60 * u / p.bpm
  const k = (next.bpm - p.bpm) / (next.beat - p.beat)
  return 60 / k * Math.log1p(k * u / p.bpm)
}

function analyticTime(points, originBeat, originTime, beat) {
  let b = originBeat
    , t = originTime
  for (let i = 0; i < points.length; i++) {
    const p = points[i]
      , next = points[i + 1]
    if (beat <= b) return t
    if (!next || beat <= next.beat)
      return t + segmentElapsed(p, next, beat - b)
    t += segmentElapsed(p, next, next.beat - b)
    b = next.beat
  }
  return t
}

// BPM at a beat coordinate, derived independently from the point list.
function bpmAtPoints(points, beat) {
  if (beat <= points[0].beat) return points[0].bpm
  let i = 0
  while (i + 1 < points.length && points[i + 1].beat <= beat) i++
  const p = points[i]
    , next = points[i + 1]
  if (!next || p.curve === 'step') return p.bpm
  const frac = (beat - p.beat) / (next.beat - p.beat)
  return p.bpm + (next.bpm - p.bpm) * frac
}

// Independent numerical integration. Midpoint rule inside each piece, so that
// samples never sit exactly on a breakpoint where the BPM jumps.
function midpoint(f, a, b, n) {
  if (a === b) return 0
  const h = (b - a) / n
  let sum = 0
  for (let i = 0; i < n; i++) sum += f(a + (i + 0.5) * h)
  return sum * h
}

function numericTime(points, originBeat, originTime, targetBeat, n) {
  n = n || 4000
  const breaks = [originBeat]
  for (let i = 0; i < points.length; i++)
    if (points[i].beat > originBeat && points[i].beat < targetBeat) breaks.push(points[i].beat)
  breaks.push(targetBeat)

  let t = originTime
  for (let i = 0; i + 1 < breaks.length; i++) {
    const a = breaks[i]
      , b = breaks[i + 1]
    if (b > a) t += midpoint(function(x) { return 60 / bpmAtPoints(points, x) }, a, b, n)
  }
  return t
}

function secondsClose(check, actual, expected, what) {
  const tol = 1e-9 * (1 + Math.abs(expected))
  check.ok(Math.abs(actual - expected) <= tol, what + ' : ' + actual + ' vs ' + expected + ' (tol ' + tol + ')')
}

function beatsClose(check, actual, expected, what) {
  const tol = 1e-9 * (1 + Math.abs(expected))
  check.ok(Math.abs(actual - expected) <= tol, what + ' : ' + actual + ' vs ' + expected + ' (tol ' + tol + ')')
}

function setup(opts) {
  const context = { currentTime: 0 }
  const clock = new WAAClock(context, Object.assign({ tickMethod: 'manual' }, opts || {}))
  clock.start()
  return { context: context, clock: clock }
}

function at(context, clock, t) {
  context.currentTime = t
  clock.tick()
}

// The points used by most mapping checks. Curves apply from a point to the
// next one; the last point is a constant tail.
const MAPPING_POINTS = [
  { beat: 2, bpm: 120, curve: 'step' },
  { beat: 4, bpm: 60, curve: 'linear' },
  { beat: 8, bpm: 240, curve: 'linear' },
  { beat: 10, bpm: 30, curve: 'step' }
]

const NEAR_CONSTANT_POINTS = [
  { beat: 0, bpm: 120, curve: 'linear' },
  { beat: 2, bpm: 120.000001, curve: 'linear' },
  { beat: 5, bpm: 119.999999, curve: 'linear' },
  { beat: 9, bpm: 120.0000005, curve: 'step' }
]

// --------------------------------------------------------------------------
// mapping
// --------------------------------------------------------------------------

function mapping(check) {
  // --- Validate the bpm range and the origin domain up front. ---
  const ctxA = { currentTime: 0 }
    , clockA = new WAAClock(ctxA, { tickMethod: 'manual' })
  clockA.start()

  check.throws(function() { clockA.createTransport({ bpm: 0 }) }, /bpm/)
  check.throws(function() { clockA.createTransport({ bpm: 1000.0001 }) }, /bpm/)
  check.throws(function() { clockA.createTransport({ bpm: NaN }) }, /bpm/)
  check.throws(function() { clockA.createTransport({ bpm: Infinity }) }, /bpm/)
  check.throws(function() { clockA.createTransport({ bpm: 120, originTime: NaN }) }, /originTime/)
  check.throws(function() { clockA.createTransport({ bpm: 120, originBeat: Infinity }) }, /originBeat/)

  // Both ends of the accepted range are legal.
  const low = clockA.createTransport({ bpm: 1, originTime: 0, originBeat: 0 })
    , high = clockA.createTransport({ bpm: 1000, originTime: 0, originBeat: 0 })
  check.equal(low.timeAtBeat(0), 0)
  check.equal(low.timeAtBeat(60), 3600) // 60 beats at 1 BPM
  check.equal(high.timeAtBeat(0), 0)
  check.equal(high.timeAtBeat(1000), 60)

  // --- Constant-tempo map with a non-zero origin. ---
  const context = { currentTime: 0 }
    , clock = new WAAClock(context, { tickMethod: 'manual' })
  clock.start()

  const constant = clock.createTransport({ bpm: 120, originTime: 10, originBeat: 3 })
  check.equal(constant.timeAtBeat(3), 10)
  check.equal(constant.timeAtBeat(7), 12)   // 4 beats at 120 BPM
  check.equal(constant.beatAtTime(10), 3)
  check.equal(constant.beatAtTime(12), 7)
  check.equal(constant.beatAtTime(11), 5)

  // Beyond a million beats, constant tempo must stay exact and finite.
  check.equal(constant.timeAtBeat(1000003), 10 + 500000)
  check.equal(constant.beatAtTime(10 + 500001), 1000005)

  // Origin domain is rejected explicitly, in both directions.
  check.throws(function() { constant.timeAtBeat(2.999999) }, /origin/)
  check.throws(function() { constant.beatAtTime(9.999999) }, /origin/)
  check.throws(function() { constant.timeAtBeat(-Infinity) }, /finite/)
  check.throws(function() { constant.timeAtBeat(NaN) }, /finite/)
  check.throws(function() { constant.beatAtTime(Infinity) }, /finite/)

  // --- Multi-segment map : step, linear, linear, tail. ---
  const transport = clock.createTransport({ bpm: 100, originTime: 5, originBeat: 2 })
  transport.replaceTempoFrom(2, MAPPING_POINTS)
  check.equal(transport.timeAtBeat(2), 5) // the anchor's absolute time is kept

  const probes = [2, 2.5, 3.99, 4, 4.25, 5, 6.75, 8, 8.5, 9.75, 10, 12, 20]
  for (const beat of probes) {
    const analytic = analyticTime(MAPPING_POINTS, 2, 5, beat)
      , numeric = numericTime(MAPPING_POINTS, 2, 5, beat, 64000)
      , actual = transport.timeAtBeat(beat)
    secondsClose(check, actual, analytic, 'timeAtBeat analytic beat ' + beat)
    secondsClose(check, actual, numeric, 'timeAtBeat numeric beat ' + beat)
    beatsClose(check, transport.beatAtTime(actual), beat, 'beatAtTime round-trip beat ' + beat)
  }

  // Linear means BPM linear in *beats*, not in seconds. For a 120 -> 240 BPM
  // segment spanning beats 4..8, the midpoint beat happens before the midpoint
  // in seconds, so those two times differ.
  const segStart = MAPPING_POINTS[1]
    , segEnd = MAPPING_POINTS[2]
  const midTime = transport.timeAtBeat((segStart.beat + segEnd.beat) / 2)
    , secsMid = (transport.timeAtBeat(segStart.beat) + transport.timeAtBeat(segEnd.beat)) / 2
  check.notEqual(midTime, secsMid)
  secondsClose(check, midTime, analyticTime(MAPPING_POINTS, 2, 5, 6), 'linear midpoint')

  // The tail keeps the last BPM.
  secondsClose(check, transport.timeAtBeat(10), analyticTime(MAPPING_POINTS, 2, 5, 10), 'tail start')
  secondsClose(check,
    transport.timeAtBeat(12) - transport.timeAtBeat(10),
    60 * 2 / 30,
    'constant tail')

  // --- Nearly constant speeds over several segments stay stable. ---
  const near = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
  near.replaceTempoFrom(0, NEAR_CONSTANT_POINTS)
  for (const beat of [0, 0.5, 2, 3.5, 5, 6, 9, 10, 15.25]) {
    const analytic = analyticTime(NEAR_CONSTANT_POINTS, 0, 0, beat)
      , numeric = numericTime(NEAR_CONSTANT_POINTS, 0, 0, beat, 64000)
      , actual = near.timeAtBeat(beat)
    secondsClose(check, actual, analytic, 'near-constant analytic beat ' + beat)
    secondsClose(check, actual, numeric, 'near-constant numeric beat ' + beat)
    beatsClose(check, near.beatAtTime(actual), beat, 'near-constant round-trip beat ' + beat)
  }

  // Inverse lookup directly from seconds on the multi-segment map.
  for (const time of [5, 5.5, 6.4, 6.5, 7.9, 8.2, 9.5, 11]) {
    const beat = transport.beatAtTime(time)
    secondsClose(check, transport.timeAtBeat(beat), time, 'beatAtTime seconds anchor ' + time)
  }

  // Invalid point lists are rejected before any commit.
  check.throws(function() {
    transport.replaceTempoFrom(4, [{ beat: 3.5, bpm: 120 }])
  }, /first tempo point/)
  check.throws(function() {
    transport.replaceTempoFrom(4, [{ beat: 4, bpm: 120 }, { beat: 4, bpm: 60 }])
  }, /increasing/)
  check.throws(function() {
    transport.replaceTempoFrom(4, [{ beat: 4, bpm: 120, curve: 'ease' }])
  }, /curve/)
  check.throws(function() {
    transport.replaceTempoFrom(4, [{ beat: 4, bpm: 0 }])
  }, /bpm/)
}

// --------------------------------------------------------------------------
// retiming
// --------------------------------------------------------------------------

function retiming(check) {
  // ---------- Future retiming, isolation, same-anchor replacement ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const a = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
      , b = clock.createTransport({ bpm: 60, originTime: 0, originBeat: 0 })

    const delivered = []
    const plain = clock.callbackAtTime(function(e) { delivered.push(['plain', e.deadline]) }, 3)
      , aEvent = a.callbackAtBeat(function(e) {
        delivered.push(['a', e.occurrenceBeat, e.deadline])
      }, 6)
      , bEvent = b.callbackAtBeat(function(e) {
        delivered.push(['b', e.occurrenceBeat, e.deadline])
      }, 3)

    check.equal(aEvent.deadline, 3) // 6 beats at 120 BPM
    check.equal(bEvent.deadline, 3) // 3 beats at 60 BPM
    check.equal(plain.deadline, 3)

    // Replace from beat 4 (2 seconds), where the old tempo is 120 BPM.
    a.replaceTempoFrom(4, [
      { beat: 4, bpm: 60, curve: 'step' },
      { beat: 6, bpm: 60, curve: 'step' }
    ])
    check.equal(a.timeAtBeat(4), 2)   // anchor second kept
    check.equal(a.timeAtBeat(3), 1.5) // left side untouched
    check.equal(aEvent.deadline, 4)   // 2s + 2 beats at 60 BPM
    check.equal(bEvent.deadline, 3)   // the other transport is untouched
    check.equal(plain.deadline, 3)    // plain second events are untouched

    // The clock queue is sorted again after retiming.
    for (let i = 1; i < clock._events.length; i++)
      check.ok(clock._events[i - 1]._earliestTime <= clock._events[i]._earliestTime)

    // Same anchor, different suffix : reorder again.
    a.replaceTempoFrom(4, [
      { beat: 4, bpm: 240, curve: 'step' },
      { beat: 6, bpm: 240, curve: 'step' }
    ])
    check.equal(a.timeAtBeat(4), 2)
    check.equal(aEvent.deadline, 2.5) // 2s + 2 beats at 240 BPM
    for (let i = 1; i < clock._events.length; i++)
      check.ok(clock._events[i - 1]._earliestTime <= clock._events[i]._earliestTime)

    // Invalid suffixes leave both the graph and the queue untouched.
    const beforeTime = a.timeAtBeat(6)
      , beforeDeadlines = clock._events.map(function(e) { return e.deadline })
      , beforeLength = clock._events.length
      , beforeHasA = clock._hasEvent(aEvent)

    check.throws(function() {
      a.replaceTempoFrom(4, [{ beat: 4, bpm: 120 }, { beat: 5, bpm: 0 }])
    }, /bpm/)
    check.throws(function() {
      a.replaceTempoFrom(4, [{ beat: 4, bpm: 120 }, { beat: 5, bpm: 60 }, { beat: 5, bpm: 240 }])
    }, /increasing/)
    check.throws(function() {
      a.replaceTempoFrom(4, [{ beat: 3.5, bpm: 120 }])
    }, /first tempo point/)
    check.throws(function() {
      a.replaceTempoFrom(4, [{ beat: 4, bpm: Infinity }])
    }, /bpm/)

    check.equal(a.timeAtBeat(6), beforeTime)
    check.deepEqual(clock._events.map(function(e) { return e.deadline }), beforeDeadlines)
    check.equal(clock._events.length, beforeLength)
    check.equal(clock._hasEvent(aEvent), beforeHasA)

    // Real delivery order across the two transports and the plain event.
    at(context, clock, 2)
    check.deepEqual(delivered, [])
    at(context, clock, 2.5)
    check.deepEqual(delivered, [['a', 6, 2.5]])
    at(context, clock, 3)
    check.deepEqual(delivered, [['a', 6, 2.5], ['b', 3, 3], ['plain', 3]])
  }

  // ---------- Future retiming, delivery order ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const a = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
      , b = clock.createTransport({ bpm: 60, originTime: 0, originBeat: 0 })
    const order = []

    const aEvent = a.callbackAtBeat(function(e) { order.push(['a', e.occurrenceBeat, e.deadline]) }, 6)
      , bEvent = b.callbackAtBeat(function(e) { order.push(['b', e.occurrenceBeat, e.deadline]) }, 2)
      , plain = clock.callbackAtTime(function(e) { order.push(['plain', e.deadline]) }, 2.5)

    check.equal(aEvent.deadline, 3)
    check.equal(bEvent.deadline, 2)
    check.equal(plain.deadline, 2.5)

    a.replaceTempoFrom(4, [{ beat: 4, bpm: 60, curve: 'step' }])
    check.equal(aEvent.deadline, 4)
    check.equal(bEvent.deadline, 2)
    check.equal(plain.deadline, 2.5)

    at(context, clock, 2)
    check.deepEqual(order, [['b', 2, 2]])
    at(context, clock, 2.5)
    check.deepEqual(order, [['b', 2, 2], ['plain', 2.5]])
    at(context, clock, 4)
    check.deepEqual(order, [['b', 2, 2], ['plain', 2.5], ['a', 6, 4]])
  }

  // ---------- Look-ahead delivery keeps its original deadline ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
    const occurrences = []
    const event = t.callbackAtBeat(function(e) {
      occurrences.push({ beat: e.occurrenceBeat, deadline: e.deadline })
    }, 4).repeat(1).tolerance({ early: 0.5, late: 0.1 })

    check.equal(event.deadline, 2)
    // At 1.6s, beat 4 (2s) is inside the early window and is delivered early.
    at(context, clock, 1.6)
    check.deepEqual(occurrences, [{ beat: 4, deadline: 2 }])
    check.equal(event.occurrenceBeat, 5)
    check.equal(event.deadline, 2.5)

    // Retime from beat 3.5, which is still ahead of the audio playhead (3.2).
    // The new suffix is much slower, so the pending occurrence moves later.
    t.replaceTempoFrom(3.5, [{ beat: 3.5, bpm: 30, curve: 'step' }])
    check.equal(t.timeAtBeat(3.5), 1.75)
    check.equal(t.timeAtBeat(4), 2.75) // the graph now says beat 4 is later
    check.equal(event.deadline, 4.75)  // the pending beat-5 occurrence moved

    // The already delivered occurrence is NOT retriggered and its deadline was
    // the one given to the sound source (2), even though at 1.6s beat 4 has
    // not really arrived yet.
    check.equal(occurrences.length, 1)
    at(context, clock, 2)
    check.equal(occurrences.length, 1)
    at(context, clock, 4)
    check.equal(occurrences.length, 1)
    at(context, clock, 4.75)
    check.deepEqual(occurrences, [
      { beat: 4, deadline: 2 },
      { beat: 5, deadline: 4.75 }
    ])
    check.equal(event.occurrenceBeat, 6)
    // Retiming must not duplicate an already played occurrence.
    const beats = occurrences.map(function(o) { return o.beat })
    check.deepEqual(beats, [4, 5])
  }

  // ---------- Changing the graph from inside a callback ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
    const sequence = []
    let depth = 0
      , maxDepth = 0

    const second = t.callbackAtBeat(function(e) {
      depth++
      maxDepth = Math.max(maxDepth, depth)
      sequence.push(['second', e.occurrenceBeat, e.deadline])
      depth--
    }, 2.2).tolerance({ early: 0.02, late: 0.1 })

    const first = t.callbackAtBeat(function(e) {
      depth++
      maxDepth = Math.max(maxDepth, depth)
      sequence.push(['first-start', e.occurrenceBeat, e.deadline])
      // Retime from the current audio beat. The new 1000 BPM suffix makes the
      // second event due immediately.
      t.replaceTempoFrom(2.1, [{ beat: 2.1, bpm: 1000, curve: 'step' }])
      sequence.push(['first-end'])
      depth--
    }, 2).tolerance({ early: 0.02, late: 0.1 })

    check.equal(first.deadline, 1)
    check.equal(second.deadline, 1.1)

    at(context, clock, 1.05)
    check.deepEqual(sequence, [
      ['first-start', 2, 1],
      ['first-end'],
      ['second', 2.2, 1.056]
    ])
    // No nested callback : newly due events are handed back to the running tick.
    check.equal(maxDepth, 1)
  }

  // ---------- Scheduling a beat event inside a callback ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
    const sequence = []

    const parent = t.callbackAtBeat(function(e) {
      sequence.push('parent')
      const child = t.callbackAtBeat(function(e2) {
        sequence.push(['child', e2.occurrenceBeat, e2.deadline])
      }, 2.11).tolerance({ early: 0.05 })
      check.equal(child.deadline, 1.055)
    }, 2).tolerance({ early: 0.02 })

    at(context, clock, 1.05)
    check.deepEqual(sequence, ['parent', ['child', 2.11, 1.055]])
  }

  // ---------- Anchor cannot be before the current audio beat ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 60, originTime: 0, originBeat: 0 })
    const event = t.callbackAtBeat(function() {}, 10)
    context.currentTime = 2 // current beat is 2
    check.throws(function() { t.replaceTempoFrom(1.5, [{ beat: 1.5, bpm: 120 }]) }, /current/)
    // Exactly on the current beat is fine.
    t.replaceTempoFrom(2, [{ beat: 2, bpm: 120, curve: 'step' }])
    check.equal(t.timeAtBeat(2), 2)
    check.equal(event.deadline, 2 + 8 * 60 / 120)
  }

  // ---------- Anchor inside a linear segment keeps the left slope ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 60, originTime: 0, originBeat: 0 })
    t.replaceTempoFrom(0, [
      { beat: 0, bpm: 60, curve: 'linear' },
      { beat: 4, bpm: 120, curve: 'linear' }
    ])
    const beforeAnchor = analyticTime(
      [{ beat: 0, bpm: 60, curve: 'linear' }, { beat: 4, bpm: 120, curve: 'linear' }],
      0, 0, 2
    )
    t.replaceTempoFrom(2, [{ beat: 2, bpm: 240, curve: 'step' }])
    // Left of the anchor is unchanged (same slope, same left endpoint).
    check.equal(t.timeAtBeat(1), analyticTime(
      [{ beat: 0, bpm: 60, curve: 'linear' }, { beat: 4, bpm: 120, curve: 'linear' }], 0, 0, 1))
    check.equal(t.timeAtBeat(2), beforeAnchor)
    // Right of the anchor uses the new speed, even though it jumps.
    check.equal(t.timeAtBeat(3), beforeAnchor + 60 / 240)
  }
}

// --------------------------------------------------------------------------
// repeat
// --------------------------------------------------------------------------

function repeat(check) {
  // ---------- Occurrence numbering is baseBeat + index * interval ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
    const seen = []
    const event = t.callbackAtBeat(function(e) {
      seen.push({ beat: e.occurrenceBeat, index: e.occurrenceIndex, deadline: e.deadline })
    }, 2).repeat(1).tolerance({ early: 0, late: 0.5 })

    at(context, clock, 1)
    check.deepEqual(seen, [{ beat: 2, index: 0, deadline: 1 }])
    at(context, clock, 1.5)
    check.deepEqual(seen, [
      { beat: 2, index: 0, deadline: 1 },
      { beat: 3, index: 1, deadline: 1.5 }
    ])
    at(context, clock, 2)
    check.deepEqual(seen.map(function(o) { return o.beat }), [2, 3, 4])
    check.deepEqual(seen.map(function(o) { return o.deadline }), [1, 1.5, 2])
    event.clear()
  }

  // ---------- Across a tempo boundary ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
    t.replaceTempoFrom(4, [
      { beat: 4, bpm: 120, curve: 'linear' },
      { beat: 6, bpm: 240, curve: 'step' }
    ])
    const seen = []
    const event = t.callbackAtBeat(function(e) {
      seen.push({ beat: e.occurrenceBeat, index: e.occurrenceIndex, deadline: e.deadline })
    }, 3).repeat(1).tolerance({ early: 0, late: 0.5 })

    const expected = [3, 4, 5, 6, 7, 8].map(function(beat) {
      return { beat: beat, index: beat - 3, deadline: t.timeAtBeat(beat) }
    })
    // Deadlines are recomputed from the map, not accumulated from seconds.
    check.equal(t.timeAtBeat(3), 1.5)
    check.equal(t.timeAtBeat(4), 2)
    // 120 -> 240 BPM linear over 2 beats : 60 * u / 120 * log1p(u / 2) / (u / 2)
    check.equal(t.timeAtBeat(5), 2 + Math.log1p(1 / 2))
    check.equal(t.timeAtBeat(6), 2 + Math.log1p(1))
    check.equal(t.timeAtBeat(7), t.timeAtBeat(6) + 60 / 240)
    // Independent check of the same two segments via the analytic model.
    const linearPoints = [
      { beat: 0, bpm: 120, curve: 'step' },
      { beat: 4, bpm: 120, curve: 'linear' },
      { beat: 6, bpm: 240, curve: 'step' }
    ]
    secondsClose(check, t.timeAtBeat(5), analyticTime(linearPoints, 0, 0, 5), 'linear beat 5')
    secondsClose(check, t.timeAtBeat(6.5), analyticTime(linearPoints, 0, 0, 6.5), 'tail beat 6.5')

    at(context, clock, expected[0].deadline)
    check.deepEqual(seen, [expected[0]])
    at(context, clock, expected[1].deadline)
    check.deepEqual(seen, [expected[0], expected[1]])
    at(context, clock, expected[2].deadline)
    check.deepEqual(seen, [expected[0], expected[1], expected[2]])
    at(context, clock, expected[3].deadline)
    check.deepEqual(seen, [expected[0], expected[1], expected[2], expected[3]])
    at(context, clock, expected[4].deadline)
    check.deepEqual(seen, [expected[0], expected[1], expected[2], expected[3], expected[4]])
    at(context, clock, expected[5].deadline)
    check.deepEqual(seen, expected)
    // No duplicate delivery.
    check.deepEqual(seen.map(function(o) { return o.beat }), [3, 4, 5, 6, 7, 8])
    event.clear()
  }

  // ---------- Huge time jump : skip expired occurrences in O(1) ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
    const seen = []
    const event = t.callbackAtBeat(function(e) {
      seen.push({ beat: e.occurrenceBeat, index: e.occurrenceIndex, deadline: e.deadline })
    }, 0).repeat(1).tolerance({ early: 0, late: 0.1 })

    // Created at t=0, the beat-0 occurrence runs straight away.
    check.deepEqual(seen, [{ beat: 0, index: 0, deadline: 0 }])

    // Jump half a million seconds. At 120 BPM that is a million beats.
    at(context, clock, 500000)
    check.ok(seen.length < 5, 'must not replay a million expired occurrences')
    check.deepEqual(seen[seen.length - 1], {
      beat: 1000000,
      index: 1000000,
      deadline: 500000
    })
    check.equal(seen.length, 2)

    at(context, clock, 500000.5)
    check.deepEqual(seen[seen.length - 1], {
      beat: 1000001,
      index: 1000001,
      deadline: 500000.5
    })
    check.equal(seen.length, 3)
    event.clear()
  }

  // ---------- Expiry boundary matches the old seconds rule ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 60, originTime: 0, originBeat: 0 })
    const calls = []
    const event = t.callbackAtBeat(function(e) {
      calls.push(['ok', e.occurrenceBeat, e.deadline])
    }, 1).repeat(1).tolerance({ early: 0, late: 0.2 })
    event.onexpired = function(e) { calls.push(['expired', e.occurrenceBeat, e.deadline]) }

    // deadline 1s + late 0.2s : reaching exactly that point expires it.
    at(context, clock, 1.2)
    check.deepEqual(calls, [['expired', 1, 1]])
    check.equal(event.occurrenceIndex, 1)

    // The next repeat is still eligible (old rule).
    at(context, clock, 2)
    check.deepEqual(calls, [['expired', 1, 1], ['ok', 2, 2]])

    // Early window delivers before the deadline.
    const earlyCalls = []
    const early = t.callbackAtBeat(function(e) {
      earlyCalls.push([e.occurrenceBeat, e.deadline])
    }, 3).tolerance({ early: 0.01, late: 0.2 })
    at(context, clock, 2.99)
    check.deepEqual(earlyCalls, [[3, 3]])
    event.clear()
  }

  // ---------- clear from inside the callback ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120, originTime: 0, originBeat: 0 })
    const seen = []
    const event = t.callbackAtBeat(function(e) {
      seen.push(e.occurrenceBeat)
      e.clear()
    }, 1).repeat(0.5).tolerance({ early: 0, late: 0.2 })

    at(context, clock, 0.5)
    check.deepEqual(seen, [1])
    check.equal(clock._hasEvent(event), false)
    check.equal(event._cleared, true)
    at(context, clock, 1)
    at(context, clock, 1.5)
    check.deepEqual(seen, [1])
  }

  // ---------- repeat interval validation ----------
  {
    const env = setup()
      , context = env.context
      , clock = env.clock
    const t = clock.createTransport({ bpm: 120 })
    const event = t.callbackAtBeat(function() {}, 1)
    check.throws(function() { event.repeat(0) }, /positive/)
    check.throws(function() { event.repeat(-1) }, /positive/)
    check.throws(function() { event.repeat(NaN) }, /positive/)
    check.throws(function() { event.repeat(Infinity) }, /positive/)
    check.throws(function() { event.repeat('1') }, /positive/)
    event.repeat(0.25)
    check.equal(event.repeatInterval, 0.25)
  }
}

module.exports = { mapping: mapping, retiming: retiming, repeat: repeat }
