// Beat-driven scheduling demo.
//
// The transport keeps every musical event on the beat grid, while the tempo
// graph can change at any time. The callbacks still receive an absolute
// second deadline, which is exactly what Web Audio primitives such as
// `AudioBufferSourceNode.start(deadline)` need.

var context, clock, transport, metronome, bass

// A short click made with an oscillator. In a real app this would be a buffer
// loaded from disk; here it is synthesized so the example needs no assets.
var createClick = function(freq, duration) {
  var osc = context.createOscillator()
    , gain = context.createGain()
  osc.type = 'square'
  osc.frequency.value = freq
  gain.gain.value = 0.15
  osc.connect(gain)
  gain.connect(context.destination)
  return { osc: osc, gain: gain, duration: duration }
}

var scheduleClick = function(freq, deadline) {
  var click = createClick(freq)
  click.osc.start(deadline)
  click.osc.stop(deadline + 0.05)
}

var start = function() {
  context = new AudioContext()
  clock = new WAAClock(context)
  clock.start()

  // 120 BPM, beat 0 at the audio clock's "now".
  transport = clock.createTransport({ bpm: 120, originTime: context.currentTime, originBeat: 0 })

  // A click on every beat. The callback gets `occurrenceBeat`, and `deadline`
  // is the absolute second the sound source must be started at.
  metronome = transport.callbackAtBeat(function(event) {
    var accent = (event.occurrenceBeat % 4 === 0) ? 1500 : 1000
    scheduleClick(accent, event.deadline)
  }, 0).repeat(1).tolerance({ early: 0.05, late: 0.2 })

  // A bass note every two beats, only on bar starts.
  transport.callbackAtBeat(function(event) {
    scheduleClick(400, event.deadline)
  }, 0).repeat(4).tolerance({ early: 0.05, late: 0.2 })

  // Change the tempo from beat 16 onward. The right side ramps linearly from
  // 120 BPM (still in effect at the anchor) down to 60 BPM over 8 beats, then
  // holds 60 BPM. Pending events are re-timed by the transport; nothing has
  // to reload or re-create them.
  transport.replaceTempoFrom(16, [
    { beat: 16, bpm: 120, curve: 'linear' },
    { beat: 24, bpm: 60, curve: 'step' }
  ])
}

var accelerate = function() {
  // Change the tempo from the current audio beat. The new point may jump to
  // any tempo; the transport only retimes occurrences that are still pending.
  transport.replaceTempoFrom(transport.beatAtTime(context.currentTime), [
    { beat: transport.beatAtTime(context.currentTime), bpm: 200, curve: 'step' }
  ])
}

document.getElementById('startButton').addEventListener('click', function() {
  document.getElementById('startButton').disabled = true
  start()
})
document.getElementById('speedButton').addEventListener('click', function() {
  if (transport) accelerate()
})

// Handy for poking around from the console.
window.transport = function() { return transport }
