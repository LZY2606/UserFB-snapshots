# emu2413

A YM2413 emulator written in C.

Audio demos are available at [MSXplay.com](https://msxplay.com)

# Regression tests

`tests/target_regression.c` holds regression tests for the rhythm section
(hi-hat, snare, cymbal and the noise/phase relations they share). It includes
`emu2413.c` directly to reach the static synthesis kernel; the public C
interface of the library is unchanged. Run them with:

```
bash checks/target.sh    # the three groups below, then checks/baseline.sh
bash checks/mutants.sh   # proves the groups reject three seeded bug classes
```

The tests carry their own independent references; they never read the
production `sin`/`exp` tables or call `to_linear`/`lookup_exp_table` to
compute an expected value. Covered by the independent references:

- **noise_clock**: the 23-bit noise register, tracked by a software model
  over 8192 consecutive internal samples, including the 14/2/2 shift-clock
  split between the drum voices of each internal update (replayed on a
  lockstep instance); the short noise, over all 32 combinations of the five
  contributing HH/CYM phase bits, and the fact that one internal sample uses
  the phase state from the start of that sample, not the advanced one.
- **percussion**: the table conventions (half-cell sampled quarter sine,
  integer attenuation steps, ones-complement negative half wave, final `<<1`
  shift) rebuilt from `sin`/`log2`/`exp2`, and integer-exact hi-hat/snare/
  cymbal outputs at controlled phases and attenuations (low, near-mute and
  the exact mute boundary). Failure diagnostics distinguish a wrong waveform
  phase selection from a wrong amplitude quantization.
- **render**: end-to-end rendering through `OPLL_writeReg`/`OPLL_calc`/
  `OPLL_calcStereo` — channel outputs and the mix at `rate == clock/72`
  (no rate converter), mono vs. center-panned stereo at 44100 Hz, rhythm
  off/on, and repeated resets restarting the noise and the audible stream.

The independent references do **not** cover the envelope generator state
machine: attack/decay/sustain/release transitions, envelope rates and the
damp behavior are not modeled — the percussion tests set `eg_out` directly
instead of predicting it. LFO (AM/PM), FM feedback and the rate converter's
filter response are likewise outside the independent references; the render
group only checks their wiring, not their values.

# Acknowledgements
emu2413 refers to the following documents. The author would like to thank all the authors who have contributed to the writing of them.

- [YM2413 notes](http://www.smspower.org/Development/YM2413) by andete
- ymf262.c by Jarek Burczynski
- [VRC7 presets](https://siliconpr0n.org/archive/doku.php?id=vendor:yamaha:opl2#opll_vrc7_patch_format) by Nuke.YKT
- YMF281B presets by Chabin
- [plgDavid's YMF281 ROM patches](https://github.com/plgDavid/misc/wiki/Copyright-free-OPLL(x)-ROM-patches) 
