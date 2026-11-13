//
// Alpha-tested layered quad example.
//
// Generates two perforated (alpha-textured) quad layers in front of a colored
// back wall, lit by one point light. The primary and the finite shadow rays
// share the same `OpacityIntersector`, so a hole in either layer lets light
// through while the cached micro regions avoid re-reading uniform texels.
//
// Usage: opacity_layers <output.ppm>
//
#include "nanort-opacity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

const int kWidth = 640;
const int kHeight = 480;

struct Vec3 {
  float v[3];
  Vec3() { v[0] = v[1] = v[2] = 0.0f; }
  Vec3(float x, float y, float z) {
    v[0] = x;
    v[1] = y;
    v[2] = z;
  }
  float operator[](int i) const { return v[i]; }
  float &operator[](int i) { return v[i]; }
};

Vec3 operator-(const Vec3 &a, const Vec3 &b) {
  return Vec3(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
Vec3 operator*(const Vec3 &a, float s) {
  return Vec3(a[0] * s, a[1] * s, a[2] * s);
}
float Dot(const Vec3 &a, const Vec3 &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 Normalize(const Vec3 &a) {
  const float len = std::sqrt(Dot(a, a));
  if (len > 1e-20f) {
    return a * (1.0f / len);
  }
  return Vec3();
}

// Procedurally punched alpha texture. `phase` shifts the hole grid so the two
// layers do not line up.
std::vector<unsigned char> MakePerforatedTexture(int size, float phase,
                                                 float radius) {
  std::vector<unsigned char> tex(static_cast<size_t>(size) * size, 0);
  const int cells = 4;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const float u = (x + 0.5f) / static_cast<float>(size);
      const float v = (y + 0.5f) / static_cast<float>(size);
      unsigned char alpha = 255;

      for (int cy = 0; cy < cells; cy++) {
        for (int cx = 0; cx < cells; cx++) {
          const float hx =
              (cx + 0.5f + phase) / static_cast<float>(cells);
          const float hy =
              (cy + 0.5f - phase) / static_cast<float>(cells);
          const float dx = (u - hx) * static_cast<float>(cells);
          const float dy = (v - hy) * static_cast<float>(cells);
          if (dx * dx + dy * dy < radius * radius) {
            alpha = 0;
          }
        }
      }
      tex[static_cast<size_t>(y) * size + x] = alpha;
    }
  }
  return tex;
}

void AppendQuad(std::vector<float> *positions, std::vector<unsigned int> *faces,
                std::vector<float> *uvs, float x0, float y0, float x1,
                float y1, float z) {
  const unsigned int base = static_cast<unsigned int>(positions->size() / 3);
  const float p[4][3] = {{x0, y0, z}, {x1, y0, z}, {x0, y1, z}, {x1, y1, z}};
  const float t[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};

  for (int i = 0; i < 4; i++) {
    for (int k = 0; k < 3; k++) {
      positions->push_back(p[i][k]);
    }
    uvs->push_back(t[i][0]);
    uvs->push_back(t[i][1]);
  }

  // Wind counter clockwise when seen from the camera at -z so the surface
  // normal points to -z.
  faces->push_back(base + 0);
  faces->push_back(base + 2);
  faces->push_back(base + 1);
  faces->push_back(base + 2);
  faces->push_back(base + 3);
  faces->push_back(base + 1);
}

float SmoothStep(float a, float b, float x) {
  const float t = std::max(0.0f, std::min(1.0f, (x - a) / (b - a)));
  return t * t * (3.0f - 2.0f * t);
}

Vec3 Shade(const Vec3 &albedo, const Vec3 &position, const Vec3 &normal,
           const Vec3 &light, bool visible) {
  const Vec3 to_light = Normalize(light - position);
  const float n_dot_l = std::max(0.0f, Dot(normal, to_light));
  const float ambient = 0.28f;
  const float direct = visible ? 0.72f * n_dot_l : 0.0f;
  const float gain = ambient + direct;
  return Vec3(
      std::min(1.0f, albedo[0] * gain),
      std::min(1.0f, albedo[1] * gain),
      std::min(1.0f, albedo[2] * gain));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <output.ppm>\n";
    return 1;
  }

  const std::string out_path = argv[1];

  //
  // Geometry: two perforated layers and one opaque colored back wall.
  //
  std::vector<float> positions;
  std::vector<unsigned int> faces;
  std::vector<float> uvs;

  // Front layer at z = 0.0 and back layer at z = 1.2. The wall is at z = 4.
  // The layers are smaller than the wall so the shadow pattern punched by
  // their holes is visible on the wall around them.
  AppendQuad(&positions, &faces, &uvs, -2.4f, -1.8f, 2.4f, 1.8f, 0.0f);
  const unsigned int front_base = 0;
  (void)front_base;
  const unsigned int front_faces = 2;

  AppendQuad(&positions, &faces, &uvs, -2.4f, -1.8f, 2.4f, 1.8f, 1.2f);
  const unsigned int back_base = front_faces;
  (void)back_base;
  const unsigned int back_faces = 2;

  AppendQuad(&positions, &faces, &uvs, -10.0f, -7.5f, 10.0f, 7.5f, 4.0f);
  const unsigned int wall_base = front_faces + back_faces;
  const unsigned int wall_faces = 2;

  const unsigned int num_faces = front_faces + back_faces + wall_faces;

  //
  // Alpha material for every primitive.
  //
  std::vector<unsigned char> front_tex = MakePerforatedTexture(128, 0.0f, 0.3f);
  std::vector<unsigned char> back_tex = MakePerforatedTexture(128, 0.3f, 0.26f);
  std::vector<unsigned char> wall_tex(1, 255);

  std::vector<nanort::OpacityMap<float>::PrimitiveDesc> descs(num_faces);

  for (unsigned int f = 0; f < num_faces; f++) {
    const std::vector<unsigned char> *tex =
        (f < front_faces) ? &front_tex
                          : ((f < front_faces + back_faces) ? &back_tex
                                                            : &wall_tex);
    descs[f].uv[0] = 0.0f;
    descs[f].uv[1] = 0.0f;
    descs[f].uv[2] = 1.0f;
    descs[f].uv[3] = 0.0f;
    descs[f].uv[4] = 0.0f;
    descs[f].uv[5] = 1.0f;
    // Match the per-vertex UV layout used by AppendQuad through the face
    // indices, so the material follows the triangle winding.
    static const float kQuadUv[4][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    for (int k = 0; k < 3; k++) {
      const unsigned int corner = faces[3 * f + k] % 4;
      descs[f].uv[2 * k + 0] = kQuadUv[corner][0];
      descs[f].uv[2 * k + 1] = kQuadUv[corner][1];
    }
    descs[f].width = 128;
    descs[f].height = 128;
    descs[f].texels = tex->data();
    descs[f].cutoff = 128;
    if (f >= front_faces + back_faces) {
      descs[f].width = 1;
      descs[f].height = 1;
    }
  }

  nanort::OpacityMap<float> opacity;
  if (!opacity.Build(descs.data(), num_faces, 3)) {
    std::cerr << "opacity map build failed\n";
    return 1;
  }

  // Primitive albedo, one color per layer.
  std::vector<Vec3> albedo(num_faces);
  for (unsigned int f = 0; f < num_faces; f++) {
    if (f < front_faces) {
      albedo[f] = Vec3(0.85f, 0.42f, 0.34f);
    } else if (f < front_faces + back_faces) {
      albedo[f] = Vec3(0.34f, 0.58f, 0.86f);
    } else {
      albedo[f] = Vec3(0.72f, 0.68f, 0.58f);
    }
  }

  nanort::OpacityMesh<float> mesh(positions.data(), faces.data(),
                                  sizeof(float) * 3, &opacity);
  nanort::TriangleSAHPred<float> pred(positions.data(), faces.data(),
                                      sizeof(float) * 3);
  nanort::BVHAccel<float> accel;
  nanort::BVHBuildOptions<float> build_options;
  if (!accel.Build(num_faces, mesh, pred, build_options)) {
    std::cerr << "BVH build failed\n";
    return 1;
  }

  const Vec3 camera(0.0f, 0.0f, -7.5f);
  const Vec3 light(3.2f, 3.8f, -2.4f);
  const float tan_half_fov = 0.52f;
  const float aspect =
      static_cast<float>(kWidth) / static_cast<float>(kHeight);

  std::vector<unsigned char> rgb(static_cast<size_t>(kWidth) * kHeight * 3);

  nanort::OpacityIntersector<float> intersector(mesh);
  nanort::TriangleIntersection<float> isect;

  unsigned long long primary_definite = 0;
  unsigned long long primary_exact = 0;
  unsigned long long shadow_definite = 0;
  unsigned long long shadow_exact = 0;

  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      const float px =
          (2.0f * (x + 0.5f) / static_cast<float>(kWidth) - 1.0f) *
          tan_half_fov * aspect;
      const float py =
          (1.0f - 2.0f * (y + 0.5f) / static_cast<float>(kHeight)) *
          tan_half_fov;

      nanort::Ray<float> ray;
      ray.org[0] = camera[0];
      ray.org[1] = camera[1];
      ray.org[2] = camera[2];
      ray.dir[0] = px;
      ray.dir[1] = py;
      ray.dir[2] = 1.0f;
      ray.min_t = 0.0f;
      ray.max_t = 1.0e+30f;

      Vec3 color(0.0f, 0.0f, 0.0f);
      nanort::BVHTraceOptions trace_options;

      const bool hit = accel.Traverse(ray, intersector, &isect, trace_options);
      primary_definite += intersector.GetDefiniteRegionHits();
      primary_exact += intersector.GetExactAlphaSamples();

      if (hit) {
        const unsigned int prim = isect.prim_id;
        const float t = isect.t;
        const Vec3 position(camera[0] + ray.dir[0] * t,
                            camera[1] + ray.dir[1] * t,
                            camera[2] + ray.dir[2] * t);
        const Vec3 normal(0.0f, 0.0f, -1.0f);

        // Finite shadow ray from the surface to the point light.
        const Vec3 to_light = light - position;
        const float light_distance = std::sqrt(Dot(to_light, to_light));
        const Vec3 shadow_dir = to_light * (1.0f / light_distance);

        nanort::Ray<float> shadow;
        shadow.org[0] = position[0] + normal[0] * 1.0e-3f;
        shadow.org[1] = position[1] + normal[1] * 1.0e-3f;
        shadow.org[2] = position[2] + normal[2] * 1.0e-3f;
        shadow.dir[0] = shadow_dir[0];
        shadow.dir[1] = shadow_dir[1];
        shadow.dir[2] = shadow_dir[2];
        shadow.min_t = 1.0e-4f;
        shadow.max_t = light_distance - 1.0e-3f;

        nanort::BVHTraceOptions shadow_options;
        shadow_options.skip_prim_id = prim;
        nanort::TriangleIntersection<float> shadow_isect;
        const bool occluded =
            accel.Traverse(shadow, intersector, &shadow_isect, shadow_options);
        shadow_definite += intersector.GetDefiniteRegionHits();
        shadow_exact += intersector.GetExactAlphaSamples();

        color = Shade(albedo[prim], position, normal, light, !occluded);

        // Keep the wall's broad falloff so the shadow holes read clearly.
        if (prim >= wall_base) {
          const float glow = 0.45f + 0.55f * SmoothStep(4.2f, 1.0f, t);
          color = Vec3(color[0] * glow, color[1] * glow, color[2] * glow);
        }
      }

      const size_t index =
          (static_cast<size_t>(y) * kWidth + x) * 3;
      rgb[index + 0] = static_cast<unsigned char>(
          255.0f * std::max(0.0f, std::min(1.0f, color[0])) + 0.5f);
      rgb[index + 1] = static_cast<unsigned char>(
          255.0f * std::max(0.0f, std::min(1.0f, color[1])) + 0.5f);
      rgb[index + 2] = static_cast<unsigned char>(
          255.0f * std::max(0.0f, std::min(1.0f, color[2])) + 0.5f);
    }
  }

  std::ofstream out(out_path.c_str(), std::ios::binary);
  if (!out) {
    std::cerr << "cannot open " << out_path << "\n";
    return 1;
  }
  out << "P6\n" << kWidth << " " << kHeight << "\n255\n";
  out.write(reinterpret_cast<const char *>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
  out.close();

  std::cout << "wrote " << out_path << "\n";
  std::cout << "primary: definite=" << primary_definite
            << " exact=" << primary_exact << "\n";
  std::cout << "shadow : definite=" << shadow_definite
            << " exact=" << shadow_exact << "\n";

  return 0;
}
