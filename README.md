# AudioFile

<!-- Version and License Badges -->

![Version](https://img.shields.io/badge/version-1.1.4-green.svg?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square)
![Language](https://img.shields.io/badge/language-C++-yellow.svg?style=flat-square)

A simple header-only C++ library for reading and writing audio files.

Current supported formats:

- WAV
- AIFF

## Author

AudioFile is written and maintained by Adam Stark.

[http://www.adamstark.co.uk](http://www.adamstark.co.uk)

## Usage

### Create an AudioFile object:

    #include "AudioFile.h"

    AudioFile<double> audioFile;

### Load an audio file:

    audioFile.load ("/path/to/my/audiofile.wav");

### Get some information about the loaded audio:

    int sampleRate = audioFile.getSampleRate();
    int bitDepth = audioFile.getBitDepth();

    int numSamples = audioFile.getNumSamplesPerChannel();
    double lengthInSeconds = audioFile.getLengthInSeconds();

    int numChannels = audioFile.getNumChannels();
    bool isMono = audioFile.isMono();
    bool isStereo = audioFile.isStereo();

    // or, just use this quick shortcut to print a summary to the console
    audioFile.printSummary();

### Access the samples directly:

    int channel = 0;
    int numSamples = audioFile.getNumSamplesPerChannel();

    for (int i = 0; i < numSamples; i++)
    {
        double currentSample = audioFile.samples[channel][i];
    }

### Replace the AudioFile audio buffer with another

    // 1. Create an AudioBuffer
    // (BTW, AudioBuffer is just a vector of vectors)

    AudioFile<double>::AudioBuffer buffer;

    // 2. Set to (e.g.) two channels
    buffer.resize (2);

    // 3. Set number of samples per channel
    buffer[0].resize (100000);
    buffer[1].resize (100000);

    // 4. do something here to fill the buffer with samples, e.g.

    #include <math.h> // somewhere earler (for M_PI and sinf())

    // then...

    int numChannels = 2;
    int numSamplesPerChannel = 100000;
    float sampleRate = 44100.f;
    float frequency = 440.f;

    for (int i = 0; i < numSamplesPerChannel; i++)
    {
        float sample = sinf (2. * M_PI * ((float) i / sampleRate) * frequency) ;

        for (int channel = 0; channel < numChannels; channel++)
             buffer[channel][i] = sample * 0.5;
    }

    // 5. Put into the AudioFile object
    bool ok = audioFile.setAudioBuffer (buffer);

### Resize the audio buffer

    // Set both the number of channels and number of samples per channel
    audioFile.setAudioBufferSize (numChannels, numSamples);

    // Set the number of samples per channel
    audioFile.setNumSamplesPerChannel (numSamples);

    // Set the number of channels
    audioFile.setNumChannels (numChannels);

### Set bit depth and sample rate

    audioFile.setBitDepth (24);
    audioFile.setSampleRate (44100);

### Save the audio file to disk

    // Wave file (implicit)
    audioFile.save ("path/to/desired/audioFile.wav");

    // Wave file (explicit)
    audioFile.save ("path/to/desired/audioFile.wav", AudioFileFormat::Wave);

    // Aiff file
    audioFile.save ("path/to/desired/audioFile.aif", AudioFileFormat::Aiff);

### Save the audio file to memory

Write the audio file data directly to a vector of bytes (without writing to a file on disk):

    std::vector<uint8_t> fileData;
    audioFile.saveToMemory (fileData, AudioFileFormat::Wave);

    or

    audioFile.saveToMemory (fileData, AudioFileFormat::Aiff);

### Broadcast Wave Format (BWF)

`AudioFileFormat::Bwf` writes a RIFF/WAVE file that also carries a `bext`
chunk with loudness measurements recomputed from the current audio on every
save. It is intended for the case where the samples have been shortened,
re-gained or re-quantised, so that any loudness values stored in an earlier
file no longer apply.

Scope and rules:

- Only `AudioFile<float>` and `AudioFile<double>` can save BWF, and only at
  48 kHz with one or two channels.
- The sample encoding is unchanged from ordinary WAVE saving: 8/16/24-bit
  integer PCM or 32-bit IEEE float. The volume and the existing quantisation
  rules are not redefined; saving never modifies the in-memory samples.
- The loudness is measured from the **final data bytes that are written**,
  not from the pre-quantisation samples. Integer PCM is normalised to the
  standard full scale of `2^(bits-1)` (with the 8-bit 128 offset removed
  first), and 32-bit output uses the actual `float` values written to disk.
  This deliberately differs from the reading side, which normalises integers
  by the maximum positive value.
- Measurements are ITU-R BS.1770 K-weighting with dual gating
  (Integrated, Max Momentary, Max Short-Term) plus EBU Tech 3342 Loudness
  Range (LRA). Each channel is filtered independently from zero history and
  the per-channel energies are summed; there is no down-mix, no channel
  weighting for mono, and no true-peak interpolation. Windows are 400 ms and
  3 s, frame-0 aligned, advanced every 100 ms, and only complete windows are
  used.
- `bext` is written with Version 2. The Integrated, LRA, Max Momentary and
  Max Short-Term fields (offsets 412/414/418/420) are set to the measured
  loudness in centi-LUFS (halves rounded away from zero). The true-peak field
  (offset 416) is always the unknown marker `0x7fff`, because true peak is not
  measured. Any value that cannot be determined (no complete window, no gated
  set, non-finite result) or that does not fit a signed 16-bit field is also
  written as `0x7fff`.

How BWF saving differs from ordinary `Wave` saving:

- `Wave` always rebuilds a minimal `RIFF/WAVE/fmt/data` file (plus iXML) and
  writes no `bext`; its behaviour is unchanged.
- `Bwf` preserves the loaded container skeleton. The `fmt` and `data` chunks
  are replaced in place, `bext` is updated in place or inserted immediately
  before `fmt`, iXML is updated in place or appended, and a `fact` chunk has
  its frame count refreshed (a 32-bit float file without one gains a minimal
  `fact` before `data`). Every other chunk - including unknown or repeated
  FourCCs, chunk ordering and the original odd-byte padding - is copied
  byte-for-byte; unknown payloads are never scanned for chunk names. A new
  object with no loaded source is saved with a zero-initialised 602-byte
  `bext`.
- WAVE loading is strictly validated: one `fmt`, one whole-frame `data`, at
  most one each of `bext` (>= 602 bytes, Version 0/1/2), `fact` (>= 4 bytes)
  and iXML, with every RIFF boundary and chunk length checked. RF64, RIFX,
  compressed formats, Extensible `fmt` chunks and multiple `data` chunks are
  outside the BWF scope; a BWF save of an unsupported configuration,
  non-finite samples, unequal channel lengths or a corrupt container returns
  `false`, leaves a `saveToMemory` output vector untouched, and never creates
  or truncates the target file of a `save`. A failed load never disturbs a
  previously loaded file.

## Examples

Please see the `examples` folder for some examples on library usage.

## A Note On Types

AudioFile is a template class and so it can be instantiated using different types to represent the audio samples.

For example, we can use floating point precision...

    AudioFile<float> audioFile;

...or double precision...

    AudioFile<double> audioFile;

...or an integer type:

    AudioFile<int> audioFile;

This simply reflects the data type you would like to use to store the underlying audio samples.

When you use an integer type to store the samples (e.g. `int` or `int8_t` or `int16_t` or `uint32_t`), the library will read in the integer sample values directly from the audio file.

A couple of notes on integer types:

- The range of samples is designed to be symmetric. This means that for (e.g.) an signed 8-bit integer (`int8_t`) we will use the range `[-127, 127]` for storing samples representing the `[-1., 1.]` range. The value `-128` is possible here given the `int8_t` type, but this is interpreted as a value slightly lower than `-1` (specifically `-1.007874015748`).

- In the case of unsigned types, we obviously can't store samples as negative values. Therefore, we used the equivalent range of the unsigned type in use. E.g. if with a 8-bit signed integer (`int8_t`) the range would be `[-127, 127]`, for an 8-bit unsigned integer we would use the range `[1, 255]`. Note that we don't use `-128` for `int8_t` or `0` in `uint8_t`.

- If you try to read an audio file with a larger bit-depth than the type you are using to store samples, the attempt to read the file will fail. Put more simply, you can't read a 16-bit audio sample into an 8-bit integer.

- If you are writing audio samples in integer formats, you should use the correct sample range for both a) the type you are using to store samples; and b) the bit depth of the audio you want to write.

The following table details the sample range for each bit-depth:

| Type       | 8-bit Audio   | 16-bit Audio         | 24-bit Audio          | 32-bit Audio                |
| ---------- | ------------- | -------------------- | --------------------- | --------------------------- |
| `float`    | `[-1.0, 1.0]` | `[-1.0, 1.0]`        | `[-1.0, 1.0]`         | `[-1.0, 1.0]`               |
| `double`   | `[-1.0, 1.0]` | `[-1.0, 1.0]`        | `[-1.0, 1.0]`         | `[-1.0, 1.0]`               |
| `int8_t`   | `[-127, 127]` | :x: (type too small) | :x: (type too small)  | :x: (type too small)        |
| `uint8_t`  | `[1, 255]`    | :x: (type too small) | :x: (type too small)  | :x: (type too small)        |
| `int16_t`  | `[-127, 127]` | `[-32767, 32767]`    | :x: (type too small)  | :x: (type too small)        |
| `uint16_t` | `[1, 255]`    | `[1, 65535]`         | :x: (type too small)  | :x: (type too small)        |
| `int32_t`  | `[-127, 127]` | `[-32767, 32767]`    | [`-8388607, 8388607]` | `[-2147483647, 2147483647]` |
| `uint32_t` | `[1, 255]`    | `[1, 65535]`         | `[1, 16777215]`       | `[1, 4294967295]`           |
| `int64_t`  | `[-127, 127]` | `[-32767, 32767]`    | [`-8388607, 8388607]` | `[-2147483647, 2147483647]` |
| `uint64_t` | `[1, 255]`    | `[1, 65535]`         | `[1, 16777215]`       | `[1, 4294967295]`           |

## Error Messages

By default, the library logs error messages to the console to provide information on what has gone wrong (e.g. a file we tried to load didn't exist).

If you prefer not to see these messages, you can disable this error logging behaviour using:

    audioFile.shouldLogErrorsToConsole (false);

## Versions

##### 1.1.4 - 15th Oct 2025

- Bug fixes and code improvements

##### 1.1.3 - 31st May 2025

- Added saveToMemory() function to save audio data to bytes
- Bug fixes and extra testing

##### 1.1.2 - 18th November 2024

- Improved AIFF sample rate calculations
- Improved CMake support
- Code improvements
- Bug and warning fixes

##### 1.1.1 - 4th April 2023

- Support for integer formats
- Improved unit testing
- Many bug fixes

##### 1.1.0 - 15th January 2022

- Moved project to MIT licence
- Added option to load an audio file already in memory
- CI Workflow improvements and bug fixes

##### 1.0.9 - 23rd January 2021

- Faster loading of audio files
- Bug fixes

##### 1.0.8 - 18th October 2020

- CMake support
- Construct instances with a file path
- Bug fixes

##### 1.0.7 - 3rd July 2020

- Support for 32-bit audio files
- Support for multi-channel audio files
- Reading/writing of [iXML data chunks](http://www.ixml.info/)

##### 1.0.6 - 29th February 2020

- Made error logging to the console optional
- Fixed lots of compiler warnings

##### 1.0.5 - 14th October 2019

- Added include of <algorithm> to better support Visual Studio

##### 1.0.4 - 13th October 2019

- Changed to a header-only library. Now you can just include AudioFile.h
- Bug fixes

##### 1.0.3 - 28th October 2018

- Bug fixes
- Documentation updates

##### 1.0.2 - 6th June 2017

- Bug fixes

## Contributions

Many thanks to the following people for their contributions to this library:

- [Abhinav1997](https://github.com/Abhinav1997)
- [alxarsenault](https://github.com/alxarsenault)
- [ascii255](https://github.com/ascii255)
- [BenjaminHinchliff](https://github.com/BenjaminHinchliff)
- [BesselJ](https://github.com/BesselJ)
- [cgraf78](https://github.com/cgraf78)
- [danielzgtg](https://github.com/danielzgtg)
- [emiro85](https://github.com/emiro85)
- [encoded](https://github.com/encoded)
- [gitelope](https://github.com/gitelope)
- [heartofrain](https://github.com/heartofrain)
- [helloimmatt](https://github.com/helloimmatt/)
- [leocstone](https://github.com/leocstone)
- [MatthieuHernandez](https://github.com/MatthieuHernandez)
- [Metalsofa](https://github.com/Metalsofa)
- [mrpossoms](https://github.com/mrpossoms)
- [mynameisjohn](https://github.com/mynameisjohn)
- [nicmell](https://github.com/nicmell)
- [Sidelobe](https://github.com/Sidelobe)
- [sschaetz](https://github.com/sschaetz)
- [Yhcrown](https://github.com/Yhcrown)
- [zhouchanglin-rr](https://github.com/zhouchanglin-rr)

## Want to Contribute?

If you would like to submit a pull request for this library, please do! But kindly follow the following simple guidelines...

- Make the changes as concise as is possible for the change you are proposing
- Avoid unnecessarily changing a large number of lines - e.g. commits changing the number of spaces in indentations on all lines (and so on)
- Keep to the code style of this library which is the [JUCE Coding Standards](https://juce.com/discover/stories/coding-standards)
- Make the changes relative to the develop branch of the library (as this may have advanced beyond the master branch)

## License

MIT License

Copyright (c) 2017 Adam Stark

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
