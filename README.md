# Go-MeltySynth

Go-MeltySynth is a SoundFont synthesizer written in Go, ported from [MeltySynth for C#](https://github.com/sinshu/meltysynth).



## Features

* Suitable for both real-time and offline synthesis.
* Support for standard MIDI files.
* No dependencies other than the standard library.



## SoundFont modulators (pmod / imod)

The `pmod` and `imod` chunks are now read instead of being discarded. A small,
explicit subset of the SF2 modulator model is supported:

* **Sources:** the constant source, note-on velocity and MIDI CC16 / CC17,
  linear only. The direction and polarity flags are honored:
  * unipolar: `v/127`, negative direction: `1 - v/127`
  * bipolar: `2*v/127 - 1`, negative direction: the negated value
  * the constant source evaluates to `1`
* **Amount source:** the same set of sources. The output is the amount
  multiplied by the two source values. Only the linear transform (0) is
  supported.
* **Destinations:** `initialAttenuation` (1 unit = 0.1 dB, applied as a
  linear gain), `pan` (1 unit = 0.1%, clamped to the endpoints on top of the
  existing pan) and `initialFilterFc` (1 unit = 1 cent, clamped between 1500
  and 13500 cents and fed through the existing low-pass/smoothing chain).
* Any other legal modulator (other sources, transforms or destinations) is
  silently ignored, exactly like before; it never prevents the SoundFont from
  loading or playing.

Modulators with the same source / destination / amount-source / transform
identity are replaced within a layer — a local zone entry replaces the global
zone entry, and an amount of zero masks it — while different identities
pointing at the same destination are added. The preset layer and the
instrument layer are merged independently and their contributions are then
added; replacement never crosses the layers.

The note-on velocity is the snapshot taken at the note-on, while CC16 / CC17
are the raw 7-bit channel values (initially zero). Changes take effect at the
next synthesis block that has not been rendered yet; PCM blocks already
rendered and waiting in the render cache are not rewritten. CC121 (Reset All
Controllers) and `Reset` clear CC16 / CC17, and only on the affected channel.
The contributions are recomputed from the raw generators and the current
sources on every block — no delta accumulation, and the oscillator and
envelopes are never restarted.

### Block boundary example

Controller changes are aligned to rendered blocks, not to individual samples.
With the default block size of 64 frames, the first 32 rendered frames belong
to a block computed before the CC17 event, so they keep the old value; the
event reaches the held voice on the following block:

```go
synth.NoteOn(0, 60, 100)

left := make([]float32, 32)
right := make([]float32, 32)
synth.Render(left, right) // half of block N; CC17 still at its old value

synth.ProcessMidiMessage(0, 0xB0, 0x11, 127) // CC17 = 127

synth.Render(left, right) // the cached second half of block N is unchanged
// the next Render call produces block N+1, where the new value takes effect
```

## Demo

https://www.youtube.com/watch?v=HLta6pASIFg

[![Youtube video](https://img.youtube.com/vi/HLta6pASIFg/0.jpg)](https://www.youtube.com/watch?v=HLta6pASIFg)



## Installation

```
go get github.com/sinshu/go-meltysynth
```



## Examples

An example code to synthesize a simple chord:

```go
// Load the SoundFont.
sf2, _ := os.Open("TimGM6mb.sf2")
soundFont, _ := meltysynth.NewSoundFont(sf2)
sf2.Close()

// Create the synthesizer.
settings := meltysynth.NewSynthesizerSettings(44100)
synthesizer, _ := meltysynth.NewSynthesizer(soundFont, settings)

// Play some notes (middle C, E, G).
synthesizer.NoteOn(0, 60, 100)
synthesizer.NoteOn(0, 64, 100)
synthesizer.NoteOn(0, 67, 100)

// The output buffer (3 seconds).
length := 3 * settings.SampleRate
left := make([]float32, length)
right := make([]float32, length)

// Render the waveform.
synthesizer.Render(left, right)
```

Another example code to synthesize a MIDI file:

```go
// Load the SoundFont.
sf2, _ := os.Open("TimGM6mb.sf2")
soundFont, _ := meltysynth.NewSoundFont(sf2)
sf2.Close()

// Create the synthesizer.
settings := meltysynth.NewSynthesizerSettings(44100)
synthesizer, _ := meltysynth.NewSynthesizer(soundFont, settings)

// Load the MIDI file.
mid, _ := os.Open("C:\\Windows\\Media\\flourish.mid")
midiFile, _ := meltysynth.NewMidiFile(mid)
mid.Close()

// Create the MIDI sequencer.
sequencer := meltysynth.NewMidiFileSequencer(synthesizer)
sequencer.Play(midiFile, true)

// The output buffer.
length := int(float64(settings.SampleRate) * float64(midiFile.GetLength()) / float64(time.Second))
left := make([]float32, length)
right := make([]float32, length)

// Render the waveform.
sequencer.Render(left, right)
```



## Todo

* __Wave synthesis__
    - [x] SoundFont reader
    - [x] Waveform generator
    - [x] Envelope generator
    - [x] Low-pass filter
    - [x] Vibrato LFO
    - [x] Modulation LFO
* __MIDI message processing__
    - [x] Note on/off
    - [x] Bank selection
    - [x] Modulation
    - [x] Volume control
    - [x] Pan
    - [x] Expression
    - [x] Hold pedal
    - [x] Program change
    - [x] Pitch bend
    - [x] Tuning
* __Effects__
    - [x] Reverb
    - [x] Chorus
* __Other things__
    - [x] Standard MIDI file support
    - [x] Performace optimization



## License

Go-MeltySynth is available under [the MIT license](LICENSE.txt).
