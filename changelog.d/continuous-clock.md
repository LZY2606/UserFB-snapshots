### Added

- `FragmentDecoder.ContinuousClock` with `ResetClocks`, and the `WithContinuousClock` stream option, give
  fragments without a `tfdt` continuous per-track decode times instead of times that restart at zero. A
  `tfdt` still re-anchors the clock, and the fragment boxes and encoded bytes are unchanged.
