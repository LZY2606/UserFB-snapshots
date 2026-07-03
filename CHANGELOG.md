# Changelog

All notable changes to VideoForge are documented here.

---

## Unreleased

### Added

- **ChromaKeyEffect** — New `ChromaKeyEffect` class (`src/effects/ChromaKeyEffect.js`) that
  removes a solid background colour (green screen, blue screen, or any other flat backdrop)
  from a clip, with `color`, `similarity`, `blend`, and `yuv` parameters.
- **`clip.addChromaKey(options)`** — Convenience method on all `Clip` subclasses, equivalent
  to `clip.addEffect(new ChromaKeyEffect(options))`.
- **EFFECT_TYPES.CHROMA_KEY** — New `'chromaKey'` entry in the shared `EFFECT_TYPES` constant.
- **Chroma key export — MP4 (FFmpeg)**: `chromakey=color=…:similarity=…:blend=…:yuv=…` filter
  applied per clip; hex colours are translated to FFmpeg's `0xRRGGBB` syntax.
- **Chroma key export — Premiere XML / FCPXML / EDL**: Passed through via each exporter's
  existing generic effect/filter representation.
- **Chroma key export — JSON**: Fully serialised via the standard `effects` array; round-tripped
  with `ChromaKeyEffect.fromJSON()`.

### Fixed

- **FilterGraphBuilder respects `effect.enabled`** — Disabled effects (crop, fadeIn, fadeOut,
  chromaKey) are now correctly skipped when building the FFmpeg filter graph, instead of
  always being applied regardless of their enabled state.

---

## v0.9.1-alpha.1

### Added

- **CropEffect** — New `CropEffect` class (`src/effects/CropEffect.js`) that removes pixels
  from any combination of the four edges (`top`, `bottom`, `left`, `right`) while keeping
  the output frame size constant (black fill for removed regions).
- **CROP_ALIGNMENT constant** — Nine-position enum (`center`, `top`, `bottom`, `left`,
  `right`, `topLeft`, `topRight`, `bottomLeft`, `bottomRight`) controlling where the
  remaining content is anchored within the original canvas.
- **`clip.addCrop(options)`** — Convenience method on all `Clip` subclasses, equivalent
  to `clip.addEffect(new CropEffect(options))`.
- **EFFECT_TYPES.CROP** — New `'crop'` entry in the shared `EFFECT_TYPES` constant.
- **Crop export — MP4 (FFmpeg)**: `crop=iw-L-R:ih-T-B:L:T` + `pad` filters applied per
  clip, with padding offsets computed from the chosen `CROP_ALIGNMENT`.
- **Crop export — Premiere XML**: Emits Premiere's built-in `Crop` effect with
  left/right/top/bottom as canvas percentages; non-centre alignment shifts the clip via
  the `Basic Motion` centre parameter.
- **Crop export — FCPXML**: Emits `<adjust-crop mode="trim">` with normalised (0–1)
  values; non-centre alignment adds `<adjust-transform position="X Y">`.
- **Crop export — JSON**: Fully serialised via the standard `effects` array; round-tripped
  with `CropEffect.fromJSON()`.
- **Crop export — EDL**: Crop values and alignment noted as `* CROP: …` comment
  annotations (CMX3600 cannot represent crop natively).

---

## v0.9.0-alpha.1

### Added

- **MP4 Export V1** — FFmpeg-based render pipeline supporting VideoClip, AudioClip,
  trim, speed, reverse, fadeIn, fadeOut, volume, mute, and embedded audio
  auto-detected via ffprobe
- **Project Validation API** — `project.validate({ exporter? })` returns
  `{ valid, warnings, errors }` with checks for missing assets, invalid trim,
  and MP4-specific unsupported features
- **Integration tests** — Real FFmpeg integration test suite (auto-skipped when
  FFmpeg is absent), shared fixture generator in `tests/helpers/fixtures.js`
- **Examples** — `basic-edit.js`, `podcast-edit.js`, `premiere-export.js`,
  `fcpxml-export.js`, `caption-demo.js`

### Fixed

- **EDL export via `project.export()`** — `EXPORT_TYPES.EDL` was missing;
  `project.export({ type: 'edl', ... })` previously threw at runtime
- **Project.save() / Project.load() clip round-trip** — `Track.fromJSON()` was
  silently dropping all clips; all six clip types now deserialise correctly
- **onProgress callback value** — `Mp4Exporter` was passing a 0–1 fraction to
  `onProgress`; now correctly passes 0–100 percentage
- **Premiere XML audio** — Embedded audio from video clips correctly included
- **Premiere XML fade export** — Opacity keyframes generated for fadeIn/fadeOut
- **Caption animation export** — Caption keyframe and animation data preserved
  in Premiere XML via `vf:` namespace
- **TimeCode precision (29.97/59.94 fps)** — `fromFrames()` used exact rational
  division causing 0.1% drift; fixed to use integer nominal fps
- **FCPXML audio emission** — Audio clips extending past primary clip boundary
  were silently dropped; changed to "starts within" semantics

### Known Limitations

- Caption/text/shape rendering not supported in MP4 export (skipped with warning)
- Transitions not rendered in MP4 export (ignored with warning)
- Color correction, blur, and custom effects not rendered in MP4 export
- Gaps between clips are not padded in MP4 output (clips are back-to-back)
- `PreviewRenderer` and `PreviewPlayer` are interface skeletons — bring your own backend
- Effects (`FadeEffect`, `Transition`) are not rehydrated from JSON in `fromJSON()`
  round-trips (clip properties are preserved; only the effect chain is lost)
