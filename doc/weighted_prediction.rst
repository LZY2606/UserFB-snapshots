# Weighted prediction

Kvazaar supports explicit single-reference weighted prediction for P slices
(`weighted_pred_flag` in the PPS and `pred_weight_table()` in the slice
header). The feature is opt-in and deliberately limited to a small,
unambiguous configuration.

## Enabling the feature

Set the `weighted_pred` field of `kvz_config` to a non-zero value, or pass
`--weighted-pred` on the command line (the library option name is
`weighted-pred`). The default is 0 (disabled), and when disabled the encoder
is byte-for-byte identical to a build without the feature.

Weighted prediction is only accepted with the following combination; any
other configuration fails in `encoder_open`:

- 8-bit input (`input_bitdepth == 8`)
- YUV 4:2:0 chroma format
- `gop == 0` (no GOP reordering, low-delay P frames)
- `ref == 1` (a single reference picture)
- `bipred == 0` (no bi-prediction)

## Per-picture parameters (`kvz_picture.wp`)

The parameters travel with each input picture in the `wp` field of type
`kvz_wp_params_t`. They describe the weighting applied to the **current
(coded) picture's** P-slice prediction. The parameters of a reference
picture are never read.

| Field      | Meaning                                                        |
|------------|----------------------------------------------------------------|
| `enabled`  | When 0, all other fields are ignored and the identity transform is used. |
| `denom`    | `log2_weight_denom`, in the range 0..7.                        |
| `weight[3]`| Per-component weight, component order Y, U, V.                 |
| `offset[3]`| Final effective per-component offset (see chroma note below).  |

### Validity rules

For `enabled == 1` all of the following must hold, otherwise
`encoder_encode` returns 0 before the picture is consumed (no output is
produced and the picture can be corrected and resubmitted):

- `denom` in `0..7`
- `weight[c] - (1 << denom)` in `-128..127` for every component
- `offset[c]` in `-128..127` for every component
- for the chroma components (c = 1, 2):
  `offset[c] - 128 + floor(128 * weight[c] / 2^denom)` in `-512..511`

## Frame-level behaviour

- The first picture of a sequence is an I picture and never applies weights,
  even when `wp.enabled` is set.
- A P picture applies the weights only when the global switch is on **and**
  its own `wp.enabled` is set; otherwise it uses the identity transform.
  Consecutive pictures may freely switch between weighted and identity
  prediction. Parameters are snapshotted per frame, so delayed (OWF)
  pipelines, picture release and flushing never leak values between frames.

## Prediction math

The prediction is generated at the HEVC internal sample precision. Fractional
positions use the standard luma 8-tap and chroma 4-tap interpolation filters;
integer positions at the frame border use edge extension by repetition.
Only the final weighted sample is clipped to the 8-bit range:

```
internal = interpolated_or_integer_reference_sample * 64
pred     = clip(0, 255,
                ((internal * weight + 32) >> denom) + offset)
```

Weighting is therefore applied *before* clipping, not to a pre-clipped
8-bit sample.

## Bitstream mapping

- `luma_log2_weight_denom` is written as `denom`; the chroma denominator is
  identical (`delta_chroma_log2_weight_denom == 0`).
- `delta_luma_weight_l0` and `delta_chroma_weight_l0` are written as
  `weight[c] - (1 << denom)`.
- `luma_offset_l0` is written directly as the luma `offset[0]`.
- The API chroma offset is the final effective offset; the syntax element
  `delta_chroma_offset_l0` is written as `offset[c] - 128`.
- A component flag is omitted (written as 0) exactly when that component is
  the identity transform. The shared chroma flag is 0 only when both chroma
  components are identity.
