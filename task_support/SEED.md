This seed contains the fixed upstream source and a headless production baseline.
Run `bash task_support/run.sh baseline` with a C11 compiler, ar and libm.
The baseline exercises real sphere intersection, BVH shadow traversal, material
transmission and cast_ray. TIFF, OpenMP, meshes and scene/image loading are not
needed for this headless path. The bundled SimplexNoise source retains its license.
`headless_image.c` supplies the Image global without loading TIFF. Production
camera/material/geometry/render code is compiled unchanged. `production.h` declares
existing external functions not exposed by upstream render.h. The target runner
compiles tests/lighttree_test.c and all raytracer C files except main.c, scene.c
and image.c, then requires bounds/visibility/transport groups with real TASK_CHECK
calls. LightTree, new renderer bindings, target tests, demo and README remain to
be implemented. New production files belong in src/raytracer.
