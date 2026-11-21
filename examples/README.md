# libdumb example programs

Three example programs are provided.


## dumbplay

dumplay will play a module file from command-line. It requires SDL2 for audio
output and argtable2 for argument parsing.


## dumbout

dumbout streams a module file to raw PCM. This can be used to pipe its output
to an encoder such as oggenc (with appropriate command-line options). Or it can
be written to a .pcm file which can be read by any respectable waveform editor.
It is also convenient for timing DUMB. Compare the time it takes to render a
module with the module's playing time! All options are set on the command line.
The argtable2-library is required for argument parsing.


## dumbseek

dumbseek seeks to an exact output PCM frame and writes raw 16-bit signed stereo
PCM. It builds a deterministic seek index, starts an independent renderer at the
requested frame, and writes a short block to stdout or a file. This demonstrates
sample-exact, reproducible seeking (see "Deterministic PCM-frame seeking" in the
top-level README). It has no external dependencies.

    dumbseek <module> <frame> [frames] [output.pcm]
