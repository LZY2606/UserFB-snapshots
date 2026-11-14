//
// Windowless motion blur example for NanoRT.
//
// Renders a small deforming scene (a triangle sweeping across a ground
// quad) by accumulating color over a fixed sequence of shutter times, then
// writes the result as a PPM image.
//
// Usage: motion_render [output.ppm]
//
#include <cstdio>
#include <cmath>
#include <vector>

#include "nanort_motion.h"

namespace {

const int kWidth = 240;
const int kHeight = 180;
const int kNumTimeSamples = 8;  // fixed time sequence over the shutter [0, 1]

struct Scene {
  // 3 key frames with non-uniform time spacing.
  float times[3];
  std::vector<float> frame[3];
  const float *frame_ptrs[3];
  unsigned int faces[9];

  Scene() {
    times[0] = 0.0f;
    times[1] = 0.4f;
    times[2] = 1.0f;

    // Vertices 0..3: static ground quad at y = -1.
    // Vertices 4..6: triangle sweeping from left to right.
    const float ground[12] = {-6.0f, -1.0f, -2.0f, 6.0f, -1.0f, -2.0f,
                              6.0f,  -1.0f, 12.0f, -6.0f, -1.0f, 12.0f};
    const float cx[3] = {-2.5f, 0.3f, 2.5f};  // triangle center x per key

    for (int k = 0; k < 3; ++k) {
      frame[k].assign(ground, ground + 12);
      const float tri[9] = {cx[k] - 0.8f, 0.2f, 5.0f, cx[k] + 0.8f, 0.2f,
                            5.0f,         cx[k], 1.4f, 5.0f};
      frame[k].insert(frame[k].end(), tri, tri + 9);
      frame_ptrs[k] = &frame[k][0];
    }

    const unsigned int f[9] = {0, 1, 2, 0, 2, 3, 4, 5, 6};
    for (int i = 0; i < 9; ++i) {
      faces[i] = f[i];
    }
  }
};

// Interpolated vertex position of vertex `v` at `time`.
void VertexAt(const Scene &scene, unsigned int v, float time, float p[3]) {
  size_t seg = 0;
  float alpha = 0.0f;
  if (!nanort::MotionTimeSegment(scene.times, 3, time, &seg, &alpha)) {
    seg = 0;
    alpha = 0.0f;
  }
  const float *a = &scene.frame[seg][3 * v];
  const float *b = &scene.frame[seg + 1][3 * v];
  for (int k = 0; k < 3; ++k) {
    p[k] = (1.0f - alpha) * a[k] + alpha * b[k];
  }
}

}  // namespace

int main(int argc, char **argv) {
  const char *out_path = (argc > 1) ? argv[1] : "motion.ppm";

  Scene scene;

  nanort::MotionTriangleMesh<float> mesh(scene.times, 3, scene.frame_ptrs,
                                         scene.faces, 3, 3 * sizeof(float));
  nanort::BVHAccel<float> accel;
  if (!accel.BuildMotion(mesh)) {
    std::fprintf(stderr, "Failed to build motion BVH\n");
    return 1;
  }
  nanort::MotionTriangleIntersector<float> isector(mesh);

  const float org[3] = {0.0f, 0.3f, -2.0f};
  const float light[3] = {0.2673f, 0.8909f, -0.3564f};  // normalized
  const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);

  std::vector<unsigned char> image(kWidth * kHeight * 3, 0);

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      float accum[3] = {0.0f, 0.0f, 0.0f};

      for (int s = 0; s < kNumTimeSamples; ++s) {
        const float time =
            static_cast<float>(s) / static_cast<float>(kNumTimeSamples - 1);

        nanort::Ray<float> ray;
        ray.org[0] = org[0];
        ray.org[1] = org[1];
        ray.org[2] = org[2];
        ray.dir[0] =
            (2.0f * (x + 0.5f) / kWidth - 1.0f) * aspect * 0.7f;
        ray.dir[1] = (1.0f - 2.0f * (y + 0.5f) / kHeight) * 0.7f;
        ray.dir[2] = 1.6f;
        ray.min_t = 0.0f;
        ray.max_t = 1.0e30f;
        ray.time = time;

        nanort::TriangleIntersection<float> hit;
        float r, g, b;
        if (accel.Traverse(ray, isector, &hit)) {
          // Flat shading with the normal of the time-interpolated triangle.
          float p0[3], p1[3], p2[3];
          VertexAt(scene, scene.faces[3 * hit.prim_id + 0], time, p0);
          VertexAt(scene, scene.faces[3 * hit.prim_id + 1], time, p1);
          VertexAt(scene, scene.faces[3 * hit.prim_id + 2], time, p2);

          float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
          float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
          float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                        e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0]};
          float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          if (len > 0.0f) {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
          }
          // Face the normal towards the ray.
          float nd = n[0] * ray.dir[0] + n[1] * ray.dir[1] + n[2] * ray.dir[2];
          if (nd > 0.0f) {
            n[0] = -n[0];
            n[1] = -n[1];
            n[2] = -n[2];
          }

          float diff = n[0] * light[0] + n[1] * light[1] + n[2] * light[2];
          if (diff < 0.0f) {
            diff = 0.0f;
          }
          const float shade = 0.25f + 0.75f * diff;

          if (hit.prim_id == 2) {  // sweeping triangle
            r = 0.9f * shade;
            g = 0.2f * shade;
            b = 0.15f * shade;
          } else {  // ground
            r = 0.55f * shade;
            g = 0.55f * shade;
            b = 0.6f * shade;
          }
        } else {
          // Background gradient.
          const float t = 0.5f * (ray.dir[1] / 0.7f + 1.0f);
          r = 0.08f + 0.10f * t;
          g = 0.10f + 0.14f * t;
          b = 0.16f + 0.24f * t;
        }

        accum[0] += r;
        accum[1] += g;
        accum[2] += b;
      }

      for (int c = 0; c < 3; ++c) {
        float v = accum[c] / static_cast<float>(kNumTimeSamples);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        v = std::pow(v, 1.0f / 2.2f);  // gamma
        image[3 * (y * kWidth + x) + c] =
            static_cast<unsigned char>(255.0f * v + 0.5f);
      }
    }
  }

  FILE *fp = std::fopen(out_path, "wb");
  if (!fp) {
    std::fprintf(stderr, "Cannot open output file: %s\n", out_path);
    return 1;
  }
  std::fprintf(fp, "P6\n%d %d\n255\n", kWidth, kHeight);
  const size_t written = std::fwrite(&image[0], 1, image.size(), fp);
  std::fclose(fp);

  if (written != image.size()) {
    std::fprintf(stderr, "Failed to write image: %s\n", out_path);
    return 1;
  }

  std::printf("Wrote %s (%d x %d, %d time samples)\n", out_path, kWidth,
              kHeight, kNumTimeSamples);
  return 0;
}
