### Added

- Optional continuous decode-time clock for fragmented MP4: `FragmentDecoder.ContinuousClock` plus
  `FragmentDecoder.ResetContinuousClocks`, and the `WithContinuousClock` `StreamOption` for `InitDecodeStream`,
  keep each track's sample DTS running across fragments whose `traf` has no `tfdt`, re-anchoring on any `tfdt`
  without adding or changing boxes.
