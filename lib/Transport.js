'use strict'

var TempoMap = require('./TempoMap')

function isFiniteNumber(value) {
  return typeof value === 'number' && isFinite(value)
}

// ==================== BeatEvent ==================== //
// A beat event is a scheduled occurrence living in the clock's event queue.
// It looks enough like a regular WAAClock `Event` for `clock.tick()` to drive
// it, but its position in time is backed by the transport's TempoMap.
//
// Every delivered occurrence is forgotten. While the event is pending, it
// represents exactly one occurrence (`occurrenceBeat`), and tempo changes may
// only move that pending occurrence around.

var BeatEvent = function(transport, func, beat) {
  if (typeof func !== 'function')
    throw new TypeError('callback must be a function')
  if (!isFiniteNumber(beat))
    throw new RangeError('beat must be a finite number')

  this.transport = transport
  this.clock = transport.clock
  this.func = func
  this.baseBeat = beat
  this.occurrenceBeat = beat
  this.occurrenceIndex = 0
  this.repeatInterval = null
  this._cleared = false
  this.onexpired = null

  this.toleranceLate = this.clock.toleranceLate
  this.toleranceEarly = this.clock.toleranceEarly
  this.deadline = null
  this._latestTime = null
  this._earliestTime = null

  this._refreshFromMap()
  this.clock._insertEvent(this)
  this._runImmediatelyIfDue()
}

// Cancels the event.
BeatEvent.prototype.clear = function() {
  this.clock._removeEvent(this)
  this._cleared = true
  return this
}

// Sets the event to repeat every `interval` beats. `interval` must be a finite
// strictly positive number. Passing `null` removes the repeat.
BeatEvent.prototype.repeat = function(interval) {
  if (interval === null || interval === undefined) {
    this.repeatInterval = null
    return this
  }
  if (!isFiniteNumber(interval) || interval <= 0)
    throw new RangeError('repeat interval must be a finite strictly positive number')

  this.repeatInterval = interval
  // Happens when `repeat` is called from inside the callback, after the
  // occurrence has already been unlinked from the clock queue.
  if (!this._cleared && !this.clock._hasEvent(this))
    this._scheduleNextOccurrence()
  return this
}

// Sets the event's tolerance (in seconds).
BeatEvent.prototype.tolerance = function(values) {
  if (typeof values.late === 'number')
    this.toleranceLate = values.late
  if (typeof values.early === 'number')
    this.toleranceEarly = values.early
  this._refreshEarlyLateDates()
  if (this.clock._hasEvent(this)) {
    this.clock._removeEvent(this)
    this.clock._insertEvent(this)
  }
  return this
}

BeatEvent.prototype.isRepeated = function() {
  return this.repeatInterval !== null
}

// Called by `clock.tick()` when the pending occurrence enters its early
// tolerance window, or is found expired.
BeatEvent.prototype._execute = function() {
  var clock = this.clock
    , now = clock.context.currentTime

  if (clock._started === false) return
  clock._removeEvent(this)

  if (now >= this._latestTime) {
    // Reached `deadline + toleranceLate` : expired, exactly like a regular
    // WAAClock event.
    if (this.onexpired) this.onexpired(this)
  } else {
    // The callback gets this event with `occurrenceBeat` and the deadline
    // that has been decided for this occurrence.
    this.func(this)
  }

  // If the callback cleared the event, or if it already re-scheduled it (by
  // calling `repeat` from inside the callback), there is nothing else to do.
  if (!this._cleared && this.repeatInterval !== null && !clock._hasEvent(this))
    this._scheduleNextOccurrence()
}

BeatEvent.prototype._refreshEarlyLateDates = function() {
  this._latestTime = this.deadline + this.toleranceLate
  this._earliestTime = this.deadline - this.toleranceEarly
}

BeatEvent.prototype._refreshFromMap = function() {
  this.deadline = this.transport._map.timeAtBeat(this.occurrenceBeat)
  this._refreshEarlyLateDates()
}

// Picks the next occurrence, skipping over occurrences that are already
// expired (`deadline + toleranceLate <= currentTime`). The skip is O(1): it
// uses the tempo map's inverse lookup instead of walking each repetition.
BeatEvent.prototype._scheduleNextOccurrence = function() {
  var interval = this.repeatInterval
  if (interval === null) return

  var nextIndex = this.occurrenceIndex + 1
  var firstLiveIndex = this._firstLiveIndex(nextIndex)
  if (firstLiveIndex > nextIndex) nextIndex = firstLiveIndex

  this.occurrenceIndex = nextIndex
  this.occurrenceBeat = this.baseBeat + nextIndex * interval
  this._refreshFromMap()
  this.clock._insertEvent(this)
}

// Smallest index >= `minIndex` whose occurrence is not expired yet.
BeatEvent.prototype._firstLiveIndex = function(minIndex) {
  var map = this.transport._map
    , cutoff = this.clock.context.currentTime - this.toleranceLate
    , thresholdBeat
    , shifted
    , index

  if (cutoff <= map.originTime) return minIndex

  thresholdBeat = map.beatAtTime(cutoff)
  shifted = (thresholdBeat - this.baseBeat) / this.repeatInterval
  index = Math.floor(shifted) + 1
  if (!isFinite(index) || index < minIndex) return minIndex

  // Guard against floating point landing exactly on the boundary : the
  // occurrence at `index - 1` must not be live, and the one at `index` must.
  while (map.timeAtBeat(this.baseBeat + index * this.repeatInterval) <= cutoff)
    index += 1
  while (index > minIndex
      && map.timeAtBeat(this.baseBeat + (index - 1) * this.repeatInterval) > cutoff)
    index -= 1
  return index
}

// Mirrors `Event.schedule`'s "run straight away if the deadline is already
// within the early tolerance window" behaviour. During a tick we defer, so
// that newly due occurrences are handled by the tick that is already running
// rather than nested inside a callback.
BeatEvent.prototype._runImmediatelyIfDue = function() {
  var clock = this.clock
  if (clock._started === true && !clock._ticking
      && clock.context.currentTime >= this._earliestTime) {
    this._execute()
  }
}

// ==================== Transport ==================== //
var Transport = module.exports = function(clock, opts) {
  opts = opts || {}
  var originTime = opts.originTime === undefined ? clock.context.currentTime : opts.originTime
  var originBeat = opts.originBeat === undefined ? 0 : opts.originBeat

  if (!isFiniteNumber(originTime))
    throw new RangeError('originTime must be a finite number')
  if (!isFiniteNumber(originBeat))
    throw new RangeError('originBeat must be a finite number')

  this.clock = clock
  this.originTime = originTime
  this.originBeat = originBeat

  // Throws if `bpm` is not a finite number in [1, 1000].
  this._map = new TempoMap(originTime, originBeat, opts.bpm)
}

// Absolute seconds for a beat coordinate.
Transport.prototype.timeAtBeat = function(beat) {
  return this._map.timeAtBeat(beat)
}

// Beat coordinate for an absolute time in seconds.
Transport.prototype.beatAtTime = function(time) {
  return this._map.beatAtTime(time)
}

// Schedules `func` at `beat`. The callback receives the event, with
// `occurrenceBeat` and the deadline decided for this occurrence.
Transport.prototype.callbackAtBeat = function(func, beat) {
  return new BeatEvent(this, func, beat)
}

// Replaces the tempo graph from `beat` onwards. Everything before the anchor
// beat keeps its old mapping. All points are validated before anything is
// committed; on error neither the map nor the queue is touched.
Transport.prototype.replaceTempoFrom = function(beat, points) {
  if (!isFiniteNumber(beat))
    throw new RangeError('beat must be a finite number')

  var now = this.clock.context.currentTime
    , currentBeat

  if (now < this._map.originTime) {
    currentBeat = this._map.originBeat
  } else {
    currentBeat = this._map.beatAtTime(now)
  }
  if (beat < currentBeat)
    throw new RangeError('cannot replace tempo from a beat before the current audio beat')

  var newMap = this._map.replaced(beat, points)
  this._map = newMap
  this._reschedulePendingEvents()
}

// Recomputes deadlines / tolerance bounds of every pending (not yet delivered)
// beat event of this transport, and re-sorts the clock queue.
Transport.prototype._reschedulePendingEvents = function() {
  var clock = this.clock
    , pending = []
    , i
    , event

  for (i = 0; i < clock._events.length; i++) {
    event = clock._events[i]
    if (event && event.transport === this) pending.push(event)
  }

  for (i = 0; i < pending.length; i++)
    clock._removeEvent(pending[i])

  for (i = 0; i < pending.length; i++)
    pending[i]._refreshFromMap()

  for (i = 0; i < pending.length; i++)
    clock._insertEvent(pending[i])
}
