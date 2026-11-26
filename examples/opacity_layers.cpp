// Procedural two-layer alpha-tested grids with hole shadows.
//
// Build:
//   c++ -std=c++17 -O2 -I.. opacity_layers.cpp -o opacity_layers
// Usage:
//   ./opacity_layers output.ppm

#include "nanort-opacity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

struct Vertex {
  float x, y, z;
  float pad;
};

struct Scene {
  std::vector<Vertex> vertices;
  std::vector<unsigned int> faces;
  std::vector<float> uvs;
  nanort::OpacityMap<float> opacity;
  nanort::BVHAccel<float> bvh;
};

void AddQuad(Scene *scene, float half_size, float z) {
  const unsigned int base = static_cast<unsigned int>(scene->vertices.size());
  const Vertex q[4] = {
      {-half_size, -half_size, z, 0.0f},
      { half_size, -half_size, z, 0.0f},
      { half_size,  half_size, z, 0.0f},
      {-half_size,  half_size, z, 0.0f},
  };
  for (const Vertex &v : q) scene->vertices.push_back(v);

  const unsigned int indices[6] = {
      base + 0, base + 1, base + 2,
      base + 0, base + 2, base + 3,
  };
  for (unsigned int id : indices) scene->faces.push_back(id);

  // Two independent face-corner UV sets.
  const float face_uv[12] = {
      0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,
      0.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
  };
  for (float uv : face_uv) scene->uvs.push_back(uv);
}

std::vector<unsigned char> MakeGridTexture(int size, int variant) {
  std::vector<unsigned char> tex(static_cast<size_t>(size) * size, 255);
  const int cell = (variant == 0) ? 16 : 12;

  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const int cx = x % cell;
      const int cy = y % cell;

      bool hole;
      if (variant == 0) {
        // Circular holes with substantial bars between them.
        const float dx = float(cx) - cell * 0.5f + 0.5f;
        const float dy = float(cy) - cell * 0.5f + 0.5f;
        hole = dx * dx + dy * dy < float(cell * cell) * 0.105f;
      } else {
        // Rectangular staggered holes in the rear layer.
        const bool horizontal_gap = cx > 3 && cx < cell - 3;
        const bool row_open = ((y / cell) & 1) ? cy < cell * 3 / 5
                                              : cy > cell * 2 / 5;
        hole = horizontal_gap && row_open && cy > 2 && cy < cell - 2;
      }

      tex[static_cast<size_t>(y) * size + x] = hole ? 0 : 255;
    }
  }
  return tex;
}

bool BuildScene(Scene *scene) {
  AddQuad(scene, 1.55f, 0.0f);     // primitives 0,1: front lattice
  AddQuad(scene, 1.95f, -0.42f);   // primitives 2,3: close rear lattice
  AddQuad(scene, 5.5f, -4.0f);     // primitives 4,5: colored back wall

  constexpr int kTexSize = 64;
  const std::vector<unsigned char> front_tex = MakeGridTexture(kTexSize, 0);
  const std::vector<unsigned char> rear_tex = MakeGridTexture(kTexSize, 1);
  const std::vector<unsigned char> wall_tex(1, 255);

  scene->opacity = nanort::OpacityMap<float>(6);
  for (unsigned int prim = 0; prim < 2; ++prim) {
    if (!scene->opacity.BuildPrimitive(
            prim, scene->uvs.data() + 6 * prim, front_tex.data(), kTexSize,
            kTexSize, 128, 5)) {
      return false;
    }
  }
  for (unsigned int prim = 2; prim < 4; ++prim) {
    if (!scene->opacity.BuildPrimitive(
            prim, scene->uvs.data() + 6 * prim, rear_tex.data(), kTexSize,
            kTexSize, 100, 5)) {
      return false;
    }
  }
  for (unsigned int prim = 4; prim < 6; ++prim) {
    if (!scene->opacity.BuildPrimitive(
            prim, scene->uvs.data() + 6 * prim, wall_tex.data(), 1, 1, 1, 0)) {
      return false;
    }
  }

  nanort::OpacityMesh<float> mesh(
      &scene->vertices[0].x, scene->faces.data(), sizeof(Vertex),
      &scene->opacity);
  nanort::TriangleSAHPred<float> pred(
      &scene->vertices[0].x, scene->faces.data(), sizeof(Vertex));
  nanort::BVHBuildOptions<float> options;
  options.min_leaf_primitives = 2;
  return scene->bvh.Build(static_cast<unsigned int>(scene->faces.size() / 3),
                          mesh, pred, options);
}

void WritePPM(const char *path, int width, int height,
              const std::vector<unsigned char> &rgb) {
  std::ofstream out(path, std::ios::binary);
  std::fprintf(stderr, "writing %s (%dx%d)\n", path, width, height);
  out << "P6\n" << width << ' ' << height << "\n255\n";
  out.write(reinterpret_cast<const char *>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
    return 2;
  }

  Scene scene;
  if (!BuildScene(&scene)) {
    std::fprintf(stderr, "failed to build opacity scene\n");
    return 1;
  }

  constexpr int kWidth = 360;
  constexpr int kHeight = 270;
  std::vector<unsigned char> image(static_cast<size_t>(kWidth) * kHeight * 3);

  // A slightly off-axis point light makes the two perforated layers cast
  // distinguishable, overlapping hole shadows onto the colored wall.
  const float light_pos[3] = {3.4f, 4.2f, 2.2f};
  const float layer_color[3][3] = {
      {0.20f, 0.72f, 0.38f},  // front green lattice
      {0.95f, 0.48f, 0.16f},  // rear orange lattice
      {0.20f, 0.42f, 0.95f},  // blue wall
  };

  unsigned long long total_deterministic = 0;
  unsigned long long total_alpha_samples = 0;

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const float aspect = float(kWidth) / float(kHeight);
      const float screen_x =
          (2.0f * (float(x) + 0.5f) / kWidth - 1.0f) * aspect * 0.62f;
      const float screen_y =
          (1.0f - 2.0f * (float(y) + 0.5f) / kHeight) * 0.62f;

      nanort::Ray<float> ray;
      ray.org[0] = 0.0f;
      ray.org[1] = 0.15f;
      ray.org[2] = 4.2f;
      const float inv_len =
          1.0f / std::sqrt(screen_x * screen_x + screen_y * screen_y + 1.0f);
      ray.dir[0] = screen_x * inv_len;
      ray.dir[1] = screen_y * inv_len;
      ray.dir[2] = -inv_len;
      ray.min_t = 0.0f;
      ray.max_t = 100.0f;

      nanort::OpacityIntersector<float> intersector(
          &scene.vertices[0].x, scene.faces.data(), sizeof(Vertex),
          &scene.opacity);
      nanort::TriangleIntersection<float> hit;
      const bool visible = scene.bvh.Traverse(ray, intersector, &hit);

      unsigned char *pixel =
          &image[static_cast<size_t>((y * kWidth + x) * 3)];
      if (!visible) {
        // Neutral studio background.
        pixel[0] = 24;
        pixel[1] = 27;
        pixel[2] = 34;
        continue;
      }

      total_deterministic += intersector.deterministic_hits;
      total_alpha_samples += intersector.alpha_samples;

      const float position[3] = {
          ray.org[0] + ray.dir[0] * hit.t,
          ray.org[1] + ray.dir[1] * hit.t,
          ray.org[2] + ray.dir[2] * hit.t,
      };

      float light_dir[3] = {
          light_pos[0] - position[0],
          light_pos[1] - position[1],
          light_pos[2] - position[2],
      };
      const float light_distance = std::sqrt(
          light_dir[0] * light_dir[0] + light_dir[1] * light_dir[1] +
          light_dir[2] * light_dir[2]);
      light_dir[0] /= light_distance;
      light_dir[1] /= light_distance;
      light_dir[2] /= light_distance;

      // All procedural quads face +Z.
      const float ndotl = std::max(0.0f, light_dir[2]);
      float lighting = 0.30f + 0.70f * ndotl;

      nanort::Ray<float> shadow_ray;
      shadow_ray.org[0] = position[0];
      shadow_ray.org[1] = position[1];
      shadow_ray.org[2] = position[2];
      shadow_ray.dir[0] = light_dir[0];
      shadow_ray.dir[1] = light_dir[1];
      shadow_ray.dir[2] = light_dir[2];
      shadow_ray.min_t = 0.0f;
      // Finite shadow line: geometry beyond the point light cannot shadow.
      shadow_ray.max_t = light_distance;

      nanort::BVHTraceOptions shadow_options;
      shadow_options.skip_prim_id = hit.prim_id;
      nanort::OpacityIntersector<float> shadow_intersector(
          &scene.vertices[0].x, scene.faces.data(), sizeof(Vertex),
          &scene.opacity);
      nanort::TriangleIntersection<float> shadow_hit;
      if (scene.bvh.Traverse(shadow_ray, shadow_intersector, &shadow_hit,
                             shadow_options)) {
        lighting *= 0.30f;
      }

      const int layer = std::min(2, static_cast<int>(hit.prim_id / 2));
      pixel[0] = static_cast<unsigned char>(std::min(
          255.0f, 255.0f * layer_color[layer][0] * lighting));
      pixel[1] = static_cast<unsigned char>(std::min(
          255.0f, 255.0f * layer_color[layer][1] * lighting));
      pixel[2] = static_cast<unsigned char>(std::min(
          255.0f, 255.0f * layer_color[layer][2] * lighting));
    }
  }

  WritePPM(argv[1], kWidth, kHeight, image);
  std::fprintf(stderr,
               "deterministic region tests=%llu exact alpha samples=%llu\n",
               total_deterministic, total_alpha_samples);
  return 0;
}
