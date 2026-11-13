'use strict'

// A tempo map converts between musical beat coordinates and absolute seconds.
//
// The map is made of contiguous segments. Each segment represents the tempo
// between two points of the map:
//
//   { beat, bpm, curve: 'step' | 'linear' }
//
// `curve` describes the tempo from that point to the next one. The last
// segment is always a constant-tempo tail.
//
// Beats and seconds are plain finite doubles. A linear segment is linear in
// BPM as a function of *beats*, not of seconds. Converting a beat difference
// to seconds therefore means integrating 60 / bpm over the beat interval.
// For a linear segment this has a closed form:
//
//   dt = 60 * (b1 - b0) / bpm0 * log1p(x) / x,  x = (bpm1 - bpm0) / bpm0
//
// and the inverse solves that same closed form exactly. No numerical
// sampling / stepping is used anywhere.

var MIN_BPM = 1
var MAX_BPM = 1000

function isFiniteNumber(value) {
  return typeof value === 'number' && isFinite(value)
}

function validateBpm(bpm, what) {
  if (!isFiniteNumber(bpm) || bpm < MIN_BPM || bpm > MAX_BPM)
    throw new RangeError(what + ' must be a finite number in [' + MIN_BPM + ', ' + MAX_BPM + ']')
}

// `log(1 + x) / x`, numerically stable around zero.
function log1pRatio(x) {
  if (x === 0) return 1
  if (Math.abs(x) < 1e-4) {
    // log(1 + x) / x = 1 - x/2 + x^2/3 - x^3/4 + ...
    var x2 = x * x
    return 1 - x / 2 + x2 / 3 - x2 * x / 4 + x2 * x2 / 5 - x2 * x2 * x / 6
  }
  return Math.log1p(x) / x
}

// Time (seconds) needed to travel `u` beats inside a segment, where `u` is
// measured from the segment start.
function elapsedWithin(segment, u) {
  if (u <= 0) return 0
  if (segment.curve === 'step' || segment.bpmEnd === segment.bpmStart)
    return 60 * u / segment.bpmStart

  var length = segment.beatEnd - segment.beatStart
  var ratio = (segment.bpmEnd - segment.bpmStart) / segment.bpmStart
  return 60 * u / segment.bpmStart * log1pRatio(ratio * u / length)
}

// Inverse of `elapsedWithin`: how many beats `dt` seconds cover.
function beatsWithin(segment, dt) {
  if (dt <= 0) return 0
  if (segment.curve === 'step' || segment.bpmEnd === segment.bpmStart)
    return dt * segment.bpmStart / 60

  var length = segment.beatEnd - segment.beatStart
  var ratio = (segment.bpmEnd - segment.bpmStart) / segment.bpmStart

  // dt = 60 * u / bpm0 * log1p(ratio * u / length)
  // log1p(ratio * u / length) = dt * bpm0 * ratio / (60 * length)
  var beatsEquivalent = dt * segment.bpmStart / 60
  var q = beatsEquivalent * ratio / length
  var u = length * Math.expm1(q) / ratio
  if (u < 0) return 0
  if (u > length) return length
  return u
}

function copySegment(segment) {
  return {
    beatStart: segment.beatStart,
    beatEnd: segment.beatEnd,
    timeStart: segment.timeStart,
    bpmStart: segment.bpmStart,
    bpmEnd: segment.bpmEnd,
    curve: segment.curve
  }
}

// Returns the last segment whose `beatStart` is <= `beat`.
function findSegmentByBeat(segments, beat) {
  var low = 0
    , high = segments.length
    , mid
  while (low < high) {
    mid = Math.floor((low + high) / 2)
    if (segments[mid].beatStart <= beat) low = mid + 1
    else high = mid
  }
  return segments[low - 1]
}

// Returns the last segment whose `timeStart` is <= `time`.
function findSegmentByTime(segments, time) {
  var low = 0
    , high = segments.length
    , mid
  while (low < high) {
    mid = Math.floor((low + high) / 2)
    if (segments[mid].timeStart <= time) low = mid + 1
    else high = mid
  }
  return segments[low - 1]
}

// Validates and normalizes the points of a tempo graph.
function normalizePoints(points, anchorBeat) {
  if (!points || typeof points.length !== 'number' || points.length === 0)
    throw new TypeError('points must be a non-empty array')

  var normalized = []
  for (var i = 0; i < points.length; i++) {
    var point = points[i]
    if (!point || typeof point !== 'object')
      throw new TypeError('tempo point must be an object')

    var beat = point.beat
      , bpm = point.bpm
      , curve = point.curve === undefined ? 'step' : point.curve

    if (!isFiniteNumber(beat))
      throw new RangeError('tempo point beat must be a finite number')
    validateBpm(bpm, 'tempo point bpm')
    if (curve !== 'step' && curve !== 'linear')
      throw new RangeError("tempo point curve must be 'step' or 'linear'")

    if (i === 0) {
      if (beat !== anchorBeat)
        throw new RangeError('first tempo point must be at the anchor beat')
    } else {
      if (beat <= normalized[i - 1].beat)
        throw new RangeError('tempo point beats must be strictly increasing')
    }

    normalized.push({ beat: beat, bpm: bpm, curve: curve })
  }
  return normalized
}

// ==================== TempoMap ==================== //
var TempoMap = module.exports = function(originTime, originBeat, bpm) {
  if (!isFiniteNumber(originTime))
    throw new RangeError('originTime must be a finite number')
  if (!isFiniteNumber(originBeat))
    throw new RangeError('originBeat must be a finite number')
  validateBpm(bpm, 'bpm')

  this.originTime = originTime
  this.originBeat = originBeat
  this.segments = [{
    beatStart: originBeat,
    beatEnd: Infinity,
    timeStart: originTime,
    bpmStart: bpm,
    bpmEnd: bpm,
    curve: 'step'
  }]
}

// Builds a map from already-computed segments. Used internally.
TempoMap.fromSegments = function(originTime, originBeat, segments) {
  var map = Object.create(TempoMap.prototype)
  map.originTime = originTime
  map.originBeat = originBeat
  map.segments = segments
  return map
}

// Converts an absolute beat coordinate to absolute seconds.
TempoMap.prototype.timeAtBeat = function(beat) {
  if (!isFiniteNumber(beat))
    throw new RangeError('beat must be a finite number')
  if (beat < this.originBeat)
    throw new RangeError('beat lies before the tempo map origin')

  if (beat === this.originBeat) return this.originTime

  var segment = findSegmentByBeat(this.segments, beat)
  return segment.timeStart + elapsedWithin(segment, beat - segment.beatStart)
}

// Converts absolute seconds to an absolute beat coordinate.
TempoMap.prototype.beatAtTime = function(time) {
  if (!isFiniteNumber(time))
    throw new RangeError('time must be a finite number')
  if (time < this.originTime)
    throw new RangeError('time lies before the tempo map origin')

  if (time === this.originTime) return this.originBeat

  var segment = findSegmentByTime(this.segments, time)
  var u = beatsWithin(segment, time - segment.timeStart)
  var beat = segment.beatStart + u
  if (beat > segment.beatEnd) beat = segment.beatEnd
  return beat
}

// Returns a new TempoMap where the graph from `beat` onward is replaced by
// `points`. Never mutates this map : if anything is invalid, it throws and
// leaves the current map untouched.
TempoMap.prototype.replaced = function(beat, points) {
  if (!isFiniteNumber(beat))
    throw new RangeError('beat must be a finite number')
  if (beat < this.originBeat)
    throw new RangeError('beat lies before the tempo map origin')

  var normalized = normalizePoints(points, beat)
  var anchorTime = this.timeAtBeat(beat)

  // Keep the mapping strictly before the anchor. If the anchor falls inside a
  // segment, that segment is truncated. For a linear segment this preserves
  // the original slope on the left side, reaching the original left endpoint.
  var segments = []
  for (var i = 0; i < this.segments.length; i++) {
    var segment = this.segments[i]
    if (segment.beatStart >= beat) break

    if (segment.beatEnd <= beat) {
      segments.push(copySegment(segment))
      continue
    }

    // The anchor is strictly inside this segment.
    var bpmAtAnchor = segment.bpmStart
    if (segment.curve === 'linear' && segment.bpmEnd !== segment.bpmStart) {
      bpmAtAnchor = segment.bpmStart + (segment.bpmEnd - segment.bpmStart)
        * ((beat - segment.beatStart) / (segment.beatEnd - segment.beatStart))
    }
    segments.push({
      beatStart: segment.beatStart,
      beatEnd: beat,
      timeStart: segment.timeStart,
      bpmStart: segment.bpmStart,
      bpmEnd: bpmAtAnchor,
      curve: segment.curve
    })
    break
  }

  // Append the new suffix, chained from the anchor's absolute time.
  var time = anchorTime
  for (var i = 0; i < normalized.length; i++) {
    var point = normalized[i]
      , next = normalized[i + 1]
      , newSegment
    if (next) {
      newSegment = {
        beatStart: point.beat,
        beatEnd: next.beat,
        timeStart: time,
        bpmStart: point.bpm,
        bpmEnd: next.bpm,
        curve: point.curve
      }
    } else {
      newSegment = {
        beatStart: point.beat,
        beatEnd: Infinity,
        timeStart: time,
        bpmStart: point.bpm,
        bpmEnd: point.bpm,
        curve: 'step'
      }
    }
    segments.push(newSegment)
    if (next) time += elapsedWithin(newSegment, next.beat - point.beat)
  }

  return TempoMap.fromSegments(this.originTime, this.originBeat, segments)
}
