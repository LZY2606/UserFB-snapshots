# DRCS grayscale depth tests

Covers DRCS bitmap definitions whose depth is not a power of two, from the
data-unit parser through `Renderer` output.

Run from the repository root:

```sh
bash verification/check_drcs_depth.sh
```

The script configures and builds `test_drcs_depth` in a temporary directory
with tests enabled, then runs it. It does not download fonts or a test
framework.

What it checks:

- the `verification/baseline.cc` input (3x1, depth 3, samples 0,1,2 in
  `0x18`) produces alphas 0, 125, 255;
- depths 2, 3, 4, 5, 17 and 256, including samples that span bytes, rows that
  share a byte with the next row, nearest-neighbor scaling to non-uniform
  target sizes, non-zero target positions, stroke, and a semi-transparent
  background;
- invalid definitions (zero size, `depth_bits` mismatch, short pixel buffer,
  out-of-range sample, depth 257) are rejected by `DrawDRCS` without touching
  the target bitmap, even when a stroke was requested;
- legal definitions decode through both the C++ `Decoder` and the C
  `aribcc_decoder_decode()` entry points, and the retained pixels/depth match
  the bit-packed input;
- a failed redefinition returns a decode error but leaves the previous legal
  glyph usable;
- rendering through `Renderer::AppendCaption` / `Renderer::Render` keeps
  region order, the merge switch, and the caption cache intact, with the
  grayscale glyph pixels asserted against computed values;
- the existing depth 2/4 pixel values and the `sample_data_drcs_1` DRCS
  replacement summary (MD5 and alternative text) are unchanged.
