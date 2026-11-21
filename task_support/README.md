# Starting environment

Use `bash task_support/run.sh baseline` to build the unchanged production library, decode a tiny stereo IMA WAVE, compare its known samples and perform a real seek. It also reports what the current library does with LIST/wavl; that observation is not a permanent failure assertion.

`bash task_support/run.sh target` requires the future tests, src/wavl.h, src/wavl.c and examples/wavl_ima.c. It compiles the production library with the required header included, explicitly links the new production object, runs the required example, runs all registered test groups, and keeps the normal IMA path. Missing/empty files, no tests, skipped groups, an early exit or a failing example cannot count as success.

The seek/segment implementation is not supplied. Build tools are Bash, CMake, and C/C++ compilers. Optional codecs, MPEG, shared-library symbol generation and Python discovery are disabled. Existing upstream Python maintenance scripts are retained unchanged as provenance; this build never runs them. Fixtures, the baseline, and all requested new implementation/tests/examples use C. No device, external media, package cache or network is needed.

## Read-only LIST/wavl IMA-ADPCM support

Some WAVE files alternate real recordings (`data`) with hold intervals
(`slnt`) inside a single RIFF `LIST` chunk of form type `wavl`, instead of
carrying one top level `data` chunk. The segment manager in `src/wavl.c`
and `src/wavl.h` presents those files as one continuous, expanded frame
axis, so they preview through the ordinary public API.

Supported scope:

* seekable, little-endian RIFF/WAVE, mono or stereo IMA-ADPCM;
* the `fmt ` chunk precedes the audio `LIST`, with the original complete
  block layout (`samplesPerBlock`, `blockAlign`);
* exactly one audio `LIST/wavl` and no top level `data` chunk;
* `data` and `slnt` subchunks are played in order; unknown subchunks are
  skipped by length including their odd padding byte;
* every non-empty `data` is a whole number of complete IMA blocks, each
  block header restating the per-channel predictor;
* an `slnt` chunk is exactly one little-endian uint32 frame count and
  repeats the last fully decoded frame of the nearest preceding non-empty
  `data` (zero before any audio); empty `data`, zero-length and consecutive
  `slnt` entries never reset the held value;
* with a `fact` chunk its value must equal the decodable `data` frames plus
  the `slnt` frames; without one that sum is reported directly.

Not supported here: short trailing blocks, nested `wavl`, other compression
formats, RF64, non-seekable streams, and writing. Truncation, block
misalignment, chunks crossing the LIST/RIFF boundary, a duplicated audio
root, bad block headers and count overflows fail before any audio is
exposed; metadata is never fed to the ADPCM decoder.

How to call it: open the file exactly like a normal WAVE with
`sf_open()` or `sf_open_virtual()` in `SFM_READ`. `SF_INFO.frames`,
`sf_seek()` with `SEEK_SET`/`SEEK_CUR`/`SEEK_END`, and all four
`sf_readf_short/int/float/double` readers operate on the expanded logical
frame axis, including points inside a block, across a `data`/`slnt`
boundary or in the middle of a hold region. Seeking into an `slnt` region
re-decodes only the last compressed block of the nearest preceding `data`
(the real final frame, never the block-header predictor), so the I/O for a
seek does not grow with the recording length or the hold count. Seeking to
the total length is the legal tail and reads zero frames; out-of-range
seeks fail and leave the next frame unchanged. See `examples/wavl_ima.c`
for a self-contained build/open/seek/check example that prints
`WAVL_IMA_DEMO_OK`, and `tests/wavl_ima_contract.c` for the contract
coverage (decoding, seeking, bounds).

