# Go-MeltySynth

Go-MeltySynth is a SoundFont synthesizer written in Go, ported from [MeltySynth for C#](https://github.com/sinshu/meltysynth).



## Features

* Suitable for both real-time and offline synthesis.
* Support for standard MIDI files.
* No dependencies other than the standard library.



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



## Large SoundFonts: paged loading without reading the sample pool

`NewSoundFont` eagerly reads the whole `smpl` chunk into `WaveData`. For a
large SoundFont this means touching the entire sample pool even when only a
short instrument is ever played. `NewSoundFontReaderAt` instead reads only the
RIFF/LIST headers, the INFO list and the pdta list: not one byte of the
`smpl` payload is read at load time, and `WaveData` stays nil. Sample frames
are fetched on demand during synthesis through a SoundFont-wide LRU page
cache, which holds at most `maxPages` int16 pages of `pageFrames` frames each.

Only 16-bit PCM SoundFonts organized as INFO/sdta/pdta are accepted; fonts
using `sm24` (24-bit) or compressed samples are rejected. `pageFrames` must be
at least 2 and `maxPages` at least 1.

The `io.ReaderAt` is retained and used while synthesizers render. **Its
lifetime is managed by the caller**: keep the underlying source (for example
an `*os.File` or a memory-mapped region) open and readable for as long as any
`synthesizer.Synthesizer` built from this SoundFont is in use. Closing the
source early makes later renders fail.

```go
// Keep this file open for the whole lifetime of every synthesizer below.
sf2, err := os.Open("HugeFont.sf2")
if err != nil {
    log.Fatal(err)
}
defer sf2.Close()

stat, err := sf2.Stat()
if err != nil {
    log.Fatal(err)
}

// Each cache page holds 4096 frames; at most 4 pages are resident.
soundFont, err := meltysynth.NewSoundFontReaderAt(sf2, stat.Size(), 4096, 4)
if err != nil {
    log.Fatal(err)
}
// soundFont.WaveData is nil; the smpl payload is read page by page.

settings := meltysynth.NewSynthesizerSettings(44100)
synthesizer, err := meltysynth.NewSynthesizer(soundFont, settings)
if err != nil {
    log.Fatal(err)
}

synthesizer.NoteOn(0, 60, 100)

left := make([]float32, settings.SampleRate)
right := make([]float32, settings.SampleRate)

// RenderChecked surfaces ReaderAt errors instead of hiding them. When a page
// cannot be loaded, the current block is discarded: the returned count only
// covers frames delivered earlier in the call, and the untouched buffer tail
// keeps its previous contents. The error then becomes sticky: later calls
// return the same cause immediately and Reset does not clear it.
n, err := synthesizer.RenderChecked(left, right)
if err != nil {
    log.Fatalf("rendered %d frames before the sample source failed: %v", n, err)
}

// Cache observability: resident pages, capacity and successful page loads.
// Eagerly loaded SoundFonts report three zeroes.
resident, capacity, loads := soundFont.SampleCacheStats()
```

`Render` keeps its original signature and audio behavior; on a paged SoundFont
it panics with the underlying error if a sample read fails, rather than
rendering silence. Use `RenderChecked` when you want to handle source errors
as ordinary Go errors.



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
