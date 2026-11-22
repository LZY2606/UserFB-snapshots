# SFOTool for TinySoundFont
A tool to create heavily compressed .SFO files from .SF2 SoundFont files.

## Purpose
SFO files are just regular SoundFont v2 files with the entire block of raw PCM samples replaced
with a single Ogg Vorbis compressed stream. Unlike .sf3 files, which can have every separate font
sample compressed individually, this will compress the entire sound data as if it were a single
sample. This results in much higher compression than processing samples individually but also
higher loss of quality.

## Usage Help
```sh
sfotool <SF2/SFO>: Show type of sample stream contained (PCM or OGG)
sfotool <SF2> <WAV>: Dump PCM sample stream to .WAV file
sfotool <SFO> <OGG>: Dump OGG sample stream to .OGG file
sfotool <SF2/SFO> <WAV> <SF2>: Write new .SF2 soundfont file using PCM sample stream from .WAV file
sfotool <SF2/SFO> <OGG> <SFO>: Write new .SFO soundfont file using OGG sample stream from .OGG file
sfotool --subset <SF2> <SF2> <bank:preset[,bank:preset...]>: Write new .SF2 keeping only the selected presets
```

## Removing unused samples with --subset
If a game only uses a handful of presets from a large soundfont, the unused
sample data can be stripped for real:

```sh
sfotool --subset input.sf2 output.sf2 0:0,0:5,128:0
```

The list selects presets by their `bank:preset` identity (presets are not
renumbered, duplicates are merged, output keeps the original preset order).
The subset is computed from the original hydra records: every selected preset
keeps all of its global and local zones, the instrument/sample closure is
taken along instrument and sampleID references, and the linked partner of a
kept left/right sample is kept as well. Instruments and samples that become
unreachable are dropped.

The sample pool is repacked: every kept sample contributes its `[start,end)`
window plus the 46 guard sample points after it. Overlapping or touching
windows are merged so shared PCM is copied only once, and the sample addresses
(start/end/loop points) are translated accordingly, keeping the relative
meaning of loop and address offset generators intact. INFO and other unknown
chunks are preserved verbatim.

Only plain 16-bit PCM SF2 files are supported; `smpo` (Ogg) streams, `sm24`
chunks and ROM samples are rejected. Invalid references, out-of-order bag
indices, broken sample links or samples reaching past the `smpl` chunk are
rejected as well — in any error case the output file is left untouched.


## Making a .SFO file from a .SF2 file
1. Dump the PCM data of a .SF2 file to a .WAV file  
   `sfotool <SF2> <WAV>`
2. Compress the .WAV file to .OGG (i.e. with [Audacity](https://www.audacityteam.org/download/legacy-windows/))  
   Make sure to choose the desired compression quality level
3. Build the .SFO file from the .SF2 file and the new .OGG  
   `sfotool <SF2> <OGG> <SFO>`

# License
SFOTool is available under the [Unlicense](http://unlicense.org/) (public domain).
