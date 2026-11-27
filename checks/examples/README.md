# Texture-mapping perspective correction check

`perspective_ppm.c` renders a deterministic scene with two strongly
perspective textured triangles through a real `glFrustum` projection and
writes it as a binary PPM. It has no dependency beyond the C standard library
and the library under test.

```
cc -std=c99 -O2 -Iinclude checks/examples/perspective_ppm.c <libtinygl.a> -lm -o perspective_ppm
./perspective_ppm out.ppm
```

The checked-in pair was produced from the same source:

| file | tree |
|------|------|
| `before.ppm` | the unmodified tree (`git archive HEAD`) |
| `after.ppm`  | the fixed tree |

`before.ppm` and `after.ppm` differ in 1397 of the 4096 pixels (max channel
difference 197): with the old code the texture coordinates are interpolated
affinely, so the sampled texel drifts with the vertex depth. With the fix the
per-pixel coordinates follow the rational weights and the texture is stable.

The scene:

* 64x64 framebuffer, viewport 0,0,64,64, 256x256 procedural RGB texture
  (`texel(x,y) = (x,y,3)` plus a few high-blue bands), nearest/repeat.
* Left triangle: screen targets (2,56), (30,56), (2,8) with clip w 1.2/6.0/1.5.
* Right triangle: screen targets (34,8), (62,54), (34,56) with clip w 1.0/8.0/2.0.
* No culling, no depth test, white current colour.

To reproduce the "before" image on a modern checkout:

```sh
git archive HEAD | tar -x -C /tmp/tinygl-before
# build /tmp/tinygl-before/src/*.c into a static library, then compile this file
# against it and run with before.ppm as the output path.
```
