//
// Window-less bilinear patch rendering example (shared implementation).
//
// Renders a small scene containing one non-coplanar bilinear patch and one
// planar patch with a rectangular parametric hole, shades hits by their
// (u, v) parameter coordinates and the oriented geometric normal, and
// writes the result as a binary PPM (P6) image.
//
// Only nanort / nanort-patch and the C++ standard library are used.
//
#ifndef EXAMPLES_BILINEAR_PATCH_RENDER_H_
#define EXAMPLES_BILINEAR_PATCH_RENDER_H_

#include <cmath>
#include <cstdio>
#include <vector>

#include "../../nanort-patch.h"

namespace bilinear_patch_example {

// Renders the example scene and writes a P6 PPM to `path`.
// Returns true on success.
inline bool RenderBilinearPatchExample(const char *path, int width = 256,
                                       int height = 256) {
  typedef double T;

  // Patch 0: non-coplanar bilinear patch (p11 lifted towards the camera).
  // Patch 1: planar background quad with a rectangular hole in the middle
  // of its parameter domain.
  const T vertices[] = {
      // patch 0 (non-coplanar)
      -1.0, -1.0, 0.3,  // p00
      1.0, -1.0, 0.3,   // p10
      1.0, 1.0, 1.8,    // p11
      -1.0, 1.0, 0.3,   // p01
      // patch 1 (planar, behind)
      -1.5, -1.5, -0.4,  // p00
      1.5, -1.5, -0.4,   // p10
      1.5, 1.5, -0.4,    // p11
      -1.5, 1.5, -0.4,   // p01
  };
  const unsigned int faces[] = {
      0, 1, 2, 3,  // patch 0
      4, 5, 6, 7,  // patch 1
  };
  // Per-face parametric holes (u0, v0, u1, v1). Patch 0 has no hole
  // (u0 >= u1 disables it); patch 1 has a centered square hole whose
  // boundary still belongs to the surface.
  const T trims[] = {
      0.0, 0.0, 0.0, 0.0,  // patch 0: no hole
      0.3, 0.3, 0.7, 0.7,  // patch 1: strict interior is cut away
  };

  nanort::BilinearPatchMesh<T> mesh(vertices, faces, sizeof(T) * 3, trims);
  nanort::BilinearPatchSAHPred<T> pred(vertices, faces, sizeof(T) * 3);

  nanort::BVHAccel<T> accel;
  if (!accel.Build(2, mesh, pred)) {
    return false;
  }

  nanort::BilinearPatchIntersector<T> intersector(mesh);

  // Orthographic camera looking straight down -z. Direction is kept
  // unnormalized-in-spirit (unit here); t directly measures depth.
  const T scale = static_cast<T>(1.7);
  const T aspect = static_cast<T>(width) / static_cast<T>(height);

  // Simple distant light.
  const T ldir[3] = {0.2672612414124244, 0.5345224838248488,
                     0.8017836807372252};  // normalize(0.3, 0.6, 0.9)

  std::vector<unsigned char> image(size_t(width) * size_t(height) * 3, 0);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const T sx = (static_cast<T>(2.0) * (static_cast<T>(x) + T(0.5)) /
                        static_cast<T>(width) -
                    static_cast<T>(1.0)) *
                   scale * aspect;
      const T sy = (static_cast<T>(1.0) -
                    static_cast<T>(2.0) * (static_cast<T>(y) + T(0.5)) /
                        static_cast<T>(height)) *
                   scale;

      nanort::Ray<T> ray;
      ray.org[0] = sx;
      ray.org[1] = sy;
      ray.org[2] = static_cast<T>(5.0);
      ray.dir[0] = static_cast<T>(0.0);
      ray.dir[1] = static_cast<T>(0.0);
      ray.dir[2] = static_cast<T>(-1.0);
      ray.min_t = static_cast<T>(0.0);
      ray.max_t = static_cast<T>(100.0);

      nanort::BilinearPatchIntersection<T> isect;
      const bool hit = accel.Traverse(ray, intersector, &isect);

      unsigned char *px = &image[3 * (size_t(y) * size_t(width) + size_t(x))];
      if (hit) {
        // Shade by parameter coordinates and oriented normal.
        const T ndotl =
            isect.normal[0] * ldir[0] + isect.normal[1] * ldir[1] +
            isect.normal[2] * ldir[2];
        const T shade = static_cast<T>(0.25) +
                        static_cast<T>(0.75) * (ndotl > T(0.0) ? ndotl : T(0.0));
        T rgb[3];
        rgb[0] = shade * (static_cast<T>(0.15) + static_cast<T>(0.85) * isect.u);
        rgb[1] = shade * (static_cast<T>(0.15) + static_cast<T>(0.85) * isect.v);
        rgb[2] = shade * (static_cast<T>(0.35) +
                          static_cast<T>(0.65) *
                              (static_cast<T>(0.5) *
                               (isect.normal[2] + static_cast<T>(1.0))));
        for (int k = 0; k < 3; k++) {
          T c = rgb[k];
          if (c < static_cast<T>(0.0)) c = static_cast<T>(0.0);
          if (c > static_cast<T>(1.0)) c = static_cast<T>(1.0);
          px[k] = static_cast<unsigned char>(c * static_cast<T>(255.0) +
                                             static_cast<T>(0.5));
        }
      } else {
        // Background (visible through the hole of patch 1).
        px[0] = 20;
        px[1] = 22;
        px[2] = 30;
      }
    }
  }

  FILE *fp = std::fopen(path, "wb");
  if (!fp) {
    return false;
  }
  std::fprintf(fp, "P6\n%d %d\n255\n", width, height);
  const size_t written = std::fwrite(&image.at(0), 1, image.size(), fp);
  std::fclose(fp);
  return written == image.size();
}

}  // namespace bilinear_patch_example

#endif  // EXAMPLES_BILINEAR_PATCH_RENDER_H_
