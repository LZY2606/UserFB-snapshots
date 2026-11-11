# Per-row transforms: `vsDoTransformRows()`

A rolling-shutter sensor exposes the top and bottom of a frame at different
instants. When the host already has the correction for both ends — for example
from two exposures it has already estimated — applying a single frame-wide
transform leaves the residual shear in place. `vsDoTransformRows()` takes those
two endpoints and warps every destination row with its interpolated transform.

## Calling it

The call sequence is the same as for `vsDoTransform()`, and one call processes
one whole frame:

```c
VSTransformConfig conf = vsTransformGetDefaultConfig("example");
conf.lensCorrection = VSLensCorrectOff;   /* required by this entry point */
conf.crop           = VSKeepBorder;       /* or VSCropBorder */

VSTransformData td;
vsTransformDataInit(&td, &conf, &fi, &fi);   /* fi: source == destination */

/* Transform at the exposure centre of the first and the last luma scanline. */
VSTransform top    = new_transform( 2.0, -1.0,  0.004, 0.0, 0, 0, 0);
VSTransform bottom = new_transform(-2.0,  1.0, -0.004, 0.0, 0, 0, 0);

vsTransformPrepare(&td, &src, &dest);
if (vsDoTransformRows(&td, top, bottom) != VS_OK) {
    /* rejected: unsupported configuration or pixel format, or a bad
       endpoint.  Nothing was written; the next legal frame is unaffected. */
    vsTransformDataCleanup(&td);
    return;
}
vsTransformFinish(&td);
```

## What each row gets

Row `y` of the destination luma plane uses the endpoints interpolated at
`u = y/(height-1)`; a single-row frame uses `u = 0.5`. `x`, `y` and `zoom` are
linear in `u`; `alpha` follows the shortest angular arc, and endpoints exactly
`pi` apart are bridged in the positive direction. The row is then resampled
with that transform — the parameters are interpolated, not two warped images
blended and not the average transform applied to the whole frame.

A chroma row `j` first maps back to its luma exposure position
`ly = (j+0.5)*2^hsub - 0.5`, clamped to `0 .. height-1`, and only then gets its
`u`. Rotation stays defined in the isotropic luma space (the plane's axis
scales are converted in the sine terms), so 4:2:2 and 4:4:0 do not shear the
chroma against the luma.

Units, transform direction, interpolation types, and both border modes
(`VSCropBorder`, `VSKeepBorder`, including its previous-frame cache) are those
of `vsDoTransform()`. Identical endpoints produce output byte-identical to
that entry point.

## Current restrictions

The entry point returns `VS_ERROR` — before writing the destination or the
keep-border cache — unless all of the following hold:

- `lensCorrection = VSLensCorrectOff` and `fov = 0`;
- source and destination have the same dimensions and pixel format;
- the format is GRAY8, YUV420P, YUV422P, YUV440P, YUV444P, RGB24 or RGBA;
- both endpoints are finite, have `zoom < 100`, and carry zero
  `barrel`, `rshutter` and `extra`.
