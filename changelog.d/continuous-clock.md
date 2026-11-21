### Added

- Continuous-clock mode for sequential decoding of fragments without `tfdt`:
  `FragmentDecoder.ContinuousClock` with `FragmentDecoder.ClearTrackClocks`, and the stream option
  `WithContinuousClock`, give the samples decode times that continue from the previous fragment of the track
