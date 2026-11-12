//
// Window-less bilinear patch rendering example.
//
// Renders one non-coplanar bilinear patch and one planar patch with a
// rectangular parametric hole, shaded by (u, v) and the oriented geometric
// normal, and writes a P6 PPM image.
//
// Usage:
//   bilinear_patch_example [output.ppm]
//
// Default output path is ./bilinear.ppm.
//
#include <cstdio>

#include "render.h"

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "bilinear.ppm";
  if (!bilinear_patch_example::RenderBilinearPatchExample(path)) {
    std::fprintf(stderr, "failed to render %s\n", path);
    return 1;
  }
  std::printf("wrote %s\n", path);
  return 0;
}
