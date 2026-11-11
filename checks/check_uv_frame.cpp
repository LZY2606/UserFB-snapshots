// Numeric checks for Mesh::computeSurfaceFrame() and its use by
// PBRShaderCPU's tangent-space normal mapping.
//
// Driven by checks/check_uv_frame.sh, which also generates a small glTF
// asset and passes its path as argv[1]; the glTF test goes through
// example::LoadGLTF -> Mesh -> BVH ray hit -> material normal texture and
// writes a normal-debug PPM next to the glTF file.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nanort.h"

#include "examples/pbr_surface/gltf-loader.h"
#include "examples/pbr_surface/mesh.h"
#include "examples/pbr_surface/pbr_maths.hh"
#include "examples/pbr_surface/texture.h"

namespace {

int g_failures = 0;

void fail(const char* test, unsigned face, float u, float v,
          const std::string& detail) {
  std::cerr << "FAIL [" << test << "] face=" << face << " u=" << u
            << " v=" << v << " : " << detail << "\n";
  ++g_failures;
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

bool vecNear(const glm::vec3& a, const glm::vec3& b, float tol) {
  return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.z, b.z, tol);
}

std::string vecStr(const glm::vec3& v) {
  std::ostringstream ss;
  ss << "(" << v.x << ", " << v.y << ", " << v.z << ")";
  return ss.str();
}

// Shade the constant normal-map texel (255,128,255) through PBRShaderCPU
// using the surface frame of the given hit, mirroring the main.cc hit
// shading path.
glm::vec3 shadeNormalMap(const Mesh<float>& mesh, unsigned face, float u,
                         float v, SurfaceFrameStatus* statusOut) {
  const auto frame = mesh.computeSurfaceFrame(face, u, v);
  *statusOut = frame.status;

  pbr_maths::PBRShaderCPU<float> shader;
  pbr_maths::sampler2D<float>::pixel texel{255, 128, 255, 255};
  shader.u_NormalSampler.width = 1;
  shader.u_NormalSampler.height = 1;
  shader.u_NormalSampler.pixels = &texel;
  shader.u_NormalSampler.boundsOperation =
      pbr_maths::sampler2D<float>::outOfBounds::clamp;
  shader.v_Normal = frame.normal;
  shader.v_TBN = pbr_maths::PBRShaderCPU<float>::t_mat3(
      frame.tangent, frame.bitangent, frame.normal);
  shader.hasTangentFrame = true;
  shader.v_UV = mesh.getTextureCoord(face, u, v);
  shader.useNormalMap = frame.useNormalMap();
  return shader.getNormal();
}

Mesh<float> makeXYTriangle(const float uvs[6]) {
  Mesh<float> m;
  m.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  m.faces = {0, 1, 2};
  m.facevarying_uvs.assign(uvs, uvs + 6);
  m.num_vertices = 3;
  m.num_faces = 1;
  return m;
}

// Plain UVs: the constant normal-map texel must tilt towards +X.
void testIdentityUV() {
  const char* test = "identity-uv";
  const float uvs[6] = {0, 0, 1, 0, 0, 1};
  auto m = makeXYTriangle(uvs);

  SurfaceFrameStatus st;
  const glm::vec3 n = shadeNormalMap(m, 0, 0.2f, 0.3f, &st);
  if (st != SurfaceFrameStatus::kValid)
    fail(test, 0, 0.2f, 0.3f, "expected a valid frame");
  const glm::vec3 expected(0.707104f, 0.002773f, 0.707104f);
  if (!vecNear(n, expected, 1e-3f))
    fail(test, 0, 0.2f, 0.3f,
         "normal " + vecStr(n) + " expected ~" + vecStr(expected));
}

// Rotated UVs (90 degrees): the tilt must follow the texture and point
// towards -Y, not stay on the world X axis.
void testRotatedUV() {
  const char* test = "rotated-uv";
  const float uvs[6] = {0, 0, 0, 1, -1, 0};
  auto m = makeXYTriangle(uvs);

  SurfaceFrameStatus st;
  const glm::vec3 n = shadeNormalMap(m, 0, 0.2f, 0.3f, &st);
  if (st != SurfaceFrameStatus::kValid)
    fail(test, 0, 0.2f, 0.3f, "expected a valid frame");
  const glm::vec3 expected(0.002773f, -0.707104f, 0.707104f);
  if (!vecNear(n, expected, 1e-3f))
    fail(test, 0, 0.2f, 0.3f,
         "normal " + vecStr(n) + " expected ~" + vecStr(expected));
}

// Mirrored UVs (corners 2 and 3 swapped): the big tilt component must
// move to +Y, and the handedness must be preserved.
void testMirroredUV() {
  const char* test = "mirrored-uv";
  const float uvs[6] = {0, 0, 0, 1, 1, 0};
  auto m = makeXYTriangle(uvs);

  SurfaceFrameStatus st;
  const glm::vec3 n = shadeNormalMap(m, 0, 0.2f, 0.3f, &st);
  if (st != SurfaceFrameStatus::kValid)
    fail(test, 0, 0.2f, 0.3f, "expected a valid frame");
  const glm::vec3 expected(0.002773f, 0.707104f, 0.707104f);
  if (!vecNear(n, expected, 1e-3f))
    fail(test, 0, 0.2f, 0.3f,
         "normal " + vecStr(n) + " expected ~" + vecStr(expected));
}

// Slanted triangle without vertex normals: the frame must be orthonormal
// and follow the geometric face normal.
void testSlantedTriangle() {
  const char* test = "slanted-triangle";
  Mesh<float> m;
  m.vertices = {0, 0, 0, 1, 0, 1, 0, 1, 0};
  m.faces = {0, 1, 2};
  const float uvs[6] = {0, 0, 1, 0, 0, 1};
  m.facevarying_uvs.assign(uvs, uvs + 6);
  m.num_vertices = 3;
  m.num_faces = 1;

  const auto f = m.computeSurfaceFrame(0, 0.25f, 0.4f);
  if (f.status != SurfaceFrameStatus::kValid) {
    fail(test, 0, 0.25f, 0.4f, "expected a valid frame");
    return;
  }
  const glm::vec3 expectedN =
      glm::normalize(glm::cross(glm::vec3(1, 0, 1), glm::vec3(0, 1, 0)));
  if (!vecNear(f.normal, expectedN, 1e-5f))
    fail(test, 0, 0.25f, 0.4f,
         "normal " + vecStr(f.normal) + " expected ~" + vecStr(expectedN));
  if (!near(glm::length(f.tangent), 1.0f, 1e-5f) ||
      !near(glm::length(f.bitangent), 1.0f, 1e-5f))
    fail(test, 0, 0.25f, 0.4f, "frame vectors are not unit length");
  if (!near(glm::dot(f.tangent, f.normal), 0.0f, 1e-5f) ||
      !near(glm::dot(f.bitangent, f.normal), 0.0f, 1e-5f) ||
      !near(glm::dot(f.tangent, f.bitangent), 0.0f, 1e-5f))
    fail(test, 0, 0.25f, 0.4f, "frame is not orthonormal");
  // u increases along +e1 = (1,0,1) here.
  if (glm::dot(f.tangent, glm::vec3(1, 0, 1)) <= 0.0f)
    fail(test, 0, 0.25f, 0.4f,
         "tangent does not point towards increasing u");
}

// Three different vertex normals: the frame must be re-orthogonalized
// against the interpolated normal at the hit point.
void testVaryingVertexNormals() {
  const char* test = "varying-normals";
  const float uvs[6] = {0, 0, 1, 0, 0, 1};
  auto m = makeXYTriangle(uvs);
  const glm::vec3 n0(0, 0, 1);
  const glm::vec3 n1 = glm::normalize(glm::vec3(0.3f, 0, 1));
  const glm::vec3 n2 = glm::normalize(glm::vec3(0, 0.3f, 1));
  m.facevarying_normals = {n0.x, n0.y, n0.z, n1.x, n1.y, n1.z,
                           n2.x, n2.y, n2.z};

  const float u = 0.2f, v = 0.3f;
  const auto f = m.computeSurfaceFrame(0, u, v);
  if (f.status != SurfaceFrameStatus::kValid) {
    fail(test, 0, u, v, "expected a valid frame");
    return;
  }
  const glm::vec3 expectedN = glm::normalize(
      (1.0f - u - v) * n0 + u * n1 + v * n2);
  if (!vecNear(f.normal, expectedN, 1e-5f))
    fail(test, 0, u, v,
         "normal " + vecStr(f.normal) + " expected ~" + vecStr(expectedN));
  // The tangent is the u gradient (+X here) re-orthogonalized against the
  // interpolated normal at the hit point.
  const glm::vec3 expectedT = glm::normalize(
      glm::vec3(1, 0, 0) - expectedN * glm::dot(expectedN, glm::vec3(1, 0, 0)));
  if (!vecNear(f.tangent, expectedT, 1e-5f))
    fail(test, 0, u, v,
         "tangent " + vecStr(f.tangent) + " expected ~" + vecStr(expectedT));
  if (!near(glm::dot(f.tangent, f.normal), 0.0f, 1e-5f))
    fail(test, 0, u, v, "tangent is not orthogonal to the shading normal");
  // Positive handedness for this non-mirrored UV island.
  if (!vecNear(glm::cross(f.tangent, f.bitangent), f.normal, 1e-5f))
    fail(test, 0, u, v, "frame handedness is inconsistent");
}

// No UVs at all: the normal map must be ignored, geometric normal used.
void testNoUVs() {
  const char* test = "no-uvs";
  Mesh<float> m;
  m.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  m.faces = {0, 1, 2};
  m.num_vertices = 3;
  m.num_faces = 1;

  const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
  if (f.status != SurfaceFrameStatus::kNoNormalMap)
    fail(test, 0, 0.2f, 0.3f, "expected kNoNormalMap status");
  if (!vecNear(f.normal, glm::vec3(0, 0, 1), 1e-5f))
    fail(test, 0, 0.2f, 0.3f,
         "geometric normal " + vecStr(f.normal) + " expected ~(0, 0, 1)");
}

// Degenerate inputs: degenerate UVs, degenerate area, degenerate
// projected tangent, and fully invalid geometry.
void testDegenerate() {
  // Identical UVs: zero UV determinant.
  {
    const char* test = "degenerate-uv-det";
    const float uvs[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    auto m = makeXYTriangle(uvs);
    const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
    if (f.status != SurfaceFrameStatus::kNoNormalMap)
      fail(test, 0, 0.2f, 0.3f, "expected kNoNormalMap status");
    if (!vecNear(f.normal, glm::vec3(0, 0, 1), 1e-5f))
      fail(test, 0, 0.2f, 0.3f, "geometric normal should still be valid");
  }
  // Nearly collinear UVs: determinant near zero relative to the UV edge
  // lengths.
  {
    const char* test = "degenerate-uv-det-relative";
    const float uvs[6] = {0, 0, 1, 0, 2, 1e-12f};
    auto m = makeXYTriangle(uvs);
    const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
    if (f.status != SurfaceFrameStatus::kNoNormalMap)
      fail(test, 0, 0.2f, 0.3f, "expected kNoNormalMap status");
  }
  // Collinear positions (zero area) but valid vertex normals: no tangent
  // frame, but the interpolated normal is still usable.
  {
    const char* test = "degenerate-area";
    Mesh<float> m;
    m.vertices = {0, 0, 0, 1, 0, 0, 2, 0, 0};
    m.faces = {0, 1, 2};
    const float uvs[6] = {0, 0, 1, 0, 0, 1};
    m.facevarying_uvs.assign(uvs, uvs + 6);
    m.facevarying_normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    m.num_vertices = 3;
    m.num_faces = 1;
    const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
    if (f.status != SurfaceFrameStatus::kNoNormalMap)
      fail(test, 0, 0.2f, 0.3f, "expected kNoNormalMap status");
    if (!vecNear(f.normal, glm::vec3(0, 0, 1), 1e-5f))
      fail(test, 0, 0.2f, 0.3f, "vertex normal should still be usable");
  }
  // Collinear positions and no vertex normals: nothing usable left.
  {
    const char* test = "degenerate-invalid";
    Mesh<float> m;
    m.vertices = {0, 0, 0, 1, 0, 0, 2, 0, 0};
    m.faces = {0, 1, 2};
    const float uvs[6] = {0, 0, 1, 0, 0, 1};
    m.facevarying_uvs.assign(uvs, uvs + 6);
    m.num_vertices = 3;
    m.num_faces = 1;
    const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
    if (f.status != SurfaceFrameStatus::kInvalid)
      fail(test, 0, 0.2f, 0.3f, "expected kInvalid status");
  }
  // Vertex normals parallel to the u gradient: the projected tangent
  // degenerates.
  {
    const char* test = "degenerate-projected-tangent";
    const float uvs[6] = {0, 0, 1, 0, 0, 1};
    auto m = makeXYTriangle(uvs);
    m.facevarying_normals = {1, 0, 0, 1, 0, 0, 1, 0, 0};
    const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
    if (f.status != SurfaceFrameStatus::kNoNormalMap)
      fail(test, 0, 0.2f, 0.3f, "expected kNoNormalMap status");
    if (!vecNear(f.normal, glm::vec3(1, 0, 0), 1e-5f))
      fail(test, 0, 0.2f, 0.3f, "geometric normal should still be valid");
  }
}

// Consecutive hits on different faces: each call must be independent, a
// degenerate hit must not reuse the previous hit's frame.
void testConsecutiveFaces() {
  const char* test = "consecutive-faces";
  Mesh<float> m;
  // Face 0: XY plane, plain UVs. Face 1: XZ plane, mirrored UVs.
  // Face 2: collinear positions without normals (invalid).
  m.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0,  //
                0, 0, 0, 1, 0, 0, 0, 0, 1,  //
                0, 0, 0, 1, 0, 0, 2, 0, 0};
  m.faces = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  m.facevarying_uvs = {0, 0, 1, 0, 0, 1,  //
                       0, 0, 0, 1, 1, 0,  //
                       0, 0, 1, 0, 0, 1};
  m.num_vertices = 9;
  m.num_faces = 3;

  SurfaceFrameStatus st;
  glm::vec3 n = shadeNormalMap(m, 0, 0.2f, 0.3f, &st);
  if (st != SurfaceFrameStatus::kValid ||
      !vecNear(n, glm::vec3(0.707104f, 0.002773f, 0.707104f), 1e-3f))
    fail(test, 0, 0.2f, 0.3f, "face 0 normal " + vecStr(n));

  n = shadeNormalMap(m, 1, 0.2f, 0.3f, &st);
  if (st != SurfaceFrameStatus::kValid ||
      !vecNear(n, glm::vec3(0.002773f, -0.707104f, 0.707104f), 1e-3f))
    fail(test, 1, 0.2f, 0.3f, "face 1 normal " + vecStr(n));

  // A degenerate hit between two valid ones must report its own status,
  // not reuse the previous frame.
  const auto bad = m.computeSurfaceFrame(2, 0.2f, 0.3f);
  if (bad.status != SurfaceFrameStatus::kInvalid)
    fail(test, 2, 0.2f, 0.3f, "expected kInvalid status on degenerate face");

  n = shadeNormalMap(m, 0, 0.2f, 0.3f, &st);
  if (st != SurfaceFrameStatus::kValid ||
      !vecNear(n, glm::vec3(0.707104f, 0.002773f, 0.707104f), 1e-3f))
    fail(test, 0, 0.2f, 0.3f,
         "face 0 normal changed after a degenerate hit: " + vecStr(n));
}

// Degeneracy thresholds are scale-relative: the same model in metres,
// millimetres or kilometres must give the same frames and statuses.
void testScaleInvariance() {
  const char* test = "scale-invariance";
  const float uvs[6] = {0, 0, 1, 0, 0, 1};
  const float scales[3] = {1e-3f, 1.0f, 1e3f};
  glm::vec3 refT, refB, refN;
  for (int i = 0; i < 3; ++i) {
    const float s = scales[i];
    Mesh<float> m;
    m.vertices = {0, 0, 0, s, 0, 0, 0, s, 0};
    m.faces = {0, 1, 2};
    m.facevarying_uvs.assign(uvs, uvs + 6);
    m.num_vertices = 3;
    m.num_faces = 1;
    const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
    if (f.status != SurfaceFrameStatus::kValid) {
      fail(test, 0, 0.2f, 0.3f,
           "expected a valid frame at scale " + std::to_string(s));
      continue;
    }
    if (i == 0) {
      refT = f.tangent;
      refB = f.bitangent;
      refN = f.normal;
    } else if (!vecNear(f.tangent, refT, 1e-4f) ||
               !vecNear(f.bitangent, refB, 1e-4f) ||
               !vecNear(f.normal, refN, 1e-4f)) {
      fail(test, 0, 0.2f, 0.3f,
           "frame changed at scale " + std::to_string(s));
    }
  }
  // A near-degenerate UV determinant must stay near-degenerate at any
  // scale.
  const float scales2[2] = {1e-3f, 1e3f};
  for (float s : scales2) {
    Mesh<float> m;
    m.vertices = {0, 0, 0, s, 0, 0, 0, s, 0};
    m.faces = {0, 1, 2};
    const float uvs2[6] = {0, 0, 1, 0, 2, 1e-12f};
    m.facevarying_uvs.assign(uvs2, uvs2 + 6);
    m.num_vertices = 3;
    m.num_faces = 1;
    const auto f = m.computeSurfaceFrame(0, 0.2f, 0.3f);
    if (f.status != SurfaceFrameStatus::kNoNormalMap)
      fail(test, 0, 0.2f, 0.3f,
           "degenerate UV judgement changed at scale " + std::to_string(s));
  }
}

// The Mesh template must keep working with double precision.
void testDoublePrecision() {
  const char* test = "double-precision";
  Mesh<double> m;
  m.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  m.faces = {0, 1, 2};
  m.facevarying_uvs = {0, 0, 1, 0, 0, 1};
  m.num_vertices = 3;
  m.num_faces = 1;
  const auto f = m.computeSurfaceFrame(0, 0.2, 0.3);
  if (f.status != SurfaceFrameStatus::kValid) {
    fail(test, 0, 0.2f, 0.3f, "expected a valid frame");
    return;
  }
  const glm::tvec3<double> eT(1, 0, 0), eB(0, 1, 0), eN(0, 0, 1);
  if (glm::length(f.tangent - eT) > 1e-9 ||
      glm::length(f.bitangent - eB) > 1e-9 ||
      glm::length(f.normal - eN) > 1e-9)
    fail(test, 0, 0.2f, 0.3f, "double precision frame mismatch");
}

// Full path: glTF file -> LoadGLTF -> Mesh -> BVH ray hit -> material
// normal texture -> normal-debug PPM. The asset holds two triangles in
// the XY plane with opposite UV orientations and a constant normal map.
void testGltfPath(const std::string& gltfPath, const std::string& ppmPath) {
  const char* test = "gltf-path";
  Mesh<float> mesh;
  std::vector<Texture> textures;
  if (!example::LoadGLTF(gltfPath, 1.0f, mesh, textures)) {
    fail(test, 0, 0, 0, "LoadGLTF failed for " + gltfPath);
    return;
  }
  mesh.num_vertices = mesh.vertices.size();
  mesh.num_faces = (unsigned)mesh.faces.size() / 3;
  if (mesh.num_faces != 2) {
    fail(test, 0, 0, 0, "expected 2 faces in the test glTF");
    return;
  }

  const auto normalIt = std::find_if(
      textures.begin(), textures.end(),
      [](const Texture& t) { return t.use == Texture::usage::normal; });
  if (normalIt == textures.end()) {
    fail(test, 0, 0, 0, "no normal texture found in the test glTF");
    return;
  }
  pbr_maths::sampler2D<float> normalMap;
  normalMap.width = normalIt->width;
  normalMap.height = normalIt->height;
  normalMap.pixels =
      new pbr_maths::sampler2D<float>::pixel[normalIt->width * normalIt->height];
  for (size_t i = 0; i < size_t(normalIt->width) * normalIt->height; ++i) {
    normalMap.pixels[i].r = normalIt->image[i * normalIt->components + 0];
    normalMap.pixels[i].g = normalIt->image[i * normalIt->components + 1];
    normalMap.pixels[i].b = normalIt->image[i * normalIt->components + 2];
    normalMap.pixels[i].a = normalIt->components == 4
                                ? normalIt->image[i * normalIt->components + 3]
                                : 255;
  }
  normalMap.boundsOperation = pbr_maths::sampler2D<float>::outOfBounds::clamp;
  for (auto texture : textures) delete[] texture.image;
  textures.clear();

  nanort::BVHBuildOptions<float> build_options;
  nanort::TriangleMesh<float> object(mesh.vertices.data(), mesh.faces.data(),
                                     3 * sizeof(float));
  nanort::TriangleSAHPred<float> object_pred(mesh.vertices.data(),
                                             mesh.faces.data(),
                                             3 * sizeof(float));
  nanort::BVHAccel<float> accel;
  accel.Build(mesh.num_faces, object, object_pred, build_options);

  // Shoot parallel rays towards -Z over the two triangles and record the
  // shading normal as a debug color.
  const int width = 128, height = 64;
  std::vector<unsigned char> img(3 * width * height, 0);
  int hitsPerFace[2] = {0, 0};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float px = -1.7f + 3.4f * (x + 0.5f) / width;
      const float py = -0.7f + 1.4f * (y + 0.5f) / height;

      nanort::Ray<float> ray;
      ray.org[0] = px;
      ray.org[1] = py;
      ray.org[2] = 2.0f;
      ray.dir[0] = 0.0f;
      ray.dir[1] = 0.0f;
      ray.dir[2] = -1.0f;
      ray.min_t = 0.0f;
      ray.max_t = 1.0e+30f;

      nanort::TriangleIntersector<float, nanort::TriangleIntersection<float> >
          intersector(mesh.vertices.data(), mesh.faces.data(),
                      sizeof(float) * 3);
      nanort::TriangleIntersection<float> isect;
      if (!accel.Traverse(ray, intersector, &isect)) continue;

      const unsigned face = isect.prim_id;
      const float hu = isect.u, hv = isect.v;
      const auto frame = mesh.computeSurfaceFrame(face, hu, hv);

      glm::vec3 n(1, 0, 1);  // stable debug color for invalid frames
      if (frame.status == SurfaceFrameStatus::kInvalid) {
        fail(test, face, hu, hv, "invalid frame on the test glTF");
      } else {
        pbr_maths::PBRShaderCPU<float> shader;
        shader.u_NormalSampler = normalMap;
        shader.v_Normal = frame.normal;
        shader.v_TBN = pbr_maths::PBRShaderCPU<float>::t_mat3(
            frame.tangent, frame.bitangent, frame.normal);
        shader.hasTangentFrame = true;
        shader.v_UV = mesh.getTextureCoord(face, hu, hv);
        shader.useNormalMap = frame.useNormalMap();
        n = shader.getNormal();
      }

      if (face > 1) {
        fail(test, face, hu, hv, "unexpected face id");
        continue;
      }
      ++hitsPerFace[face];

      // Face attribution: face 0 is the left triangle (x < 0 after the
      // loader recenters the mesh), face 1 the right one.
      const float hitX = px;
      if ((face == 0 && hitX > 0.0f) || (face == 1 && hitX < 0.0f))
        fail(test, face, hu, hv, "hit belongs to the wrong face");

      // The constant normal-map texel tilts towards +u: +X on face 0
      // (plain UVs), +Y on face 1 (mirrored UVs).
      if (face == 0 && !(n.x > 0.6f && std::fabs(n.y) < 0.1f && n.z > 0.6f))
        fail(test, face, hu, hv, "normal " + vecStr(n) + " should tilt +X");
      if (face == 1 && !(n.y > 0.6f && std::fabs(n.x) < 0.1f && n.z > 0.6f))
        fail(test, face, hu, hv, "normal " + vecStr(n) + " should tilt +Y");

      const size_t o = 3 * (size_t(y) * width + x);
      img[o + 0] = (unsigned char)(255 * (0.5f * n.x + 0.5f));
      img[o + 1] = (unsigned char)(255 * (0.5f * n.y + 0.5f));
      img[o + 2] = (unsigned char)(255 * (0.5f * n.z + 0.5f));
    }
  }

  if (hitsPerFace[0] < 100 || hitsPerFace[1] < 100)
    fail(test, 0, 0, 0,
         "too few ray hits: face0=" + std::to_string(hitsPerFace[0]) +
             " face1=" + std::to_string(hitsPerFace[1]));

  FILE* fp = fopen(ppmPath.c_str(), "wb");
  if (fp) {
    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    fwrite(img.data(), 1, img.size(), fp);
    fclose(fp);
    std::cout << "wrote normal-debug PPM: " << ppmPath << "\n";
  } else {
    fail(test, 0, 0, 0, "cannot write " + ppmPath);
  }

  normalMap.releasePixels();
}

}  // namespace

int main(int argc, char** argv) {
  testIdentityUV();
  testRotatedUV();
  testMirroredUV();
  testSlantedTriangle();
  testVaryingVertexNormals();
  testNoUVs();
  testDegenerate();
  testConsecutiveFaces();
  testScaleInvariance();
  testDoublePrecision();

  if (argc > 2) testGltfPath(argv[1], argv[2]);

  if (g_failures == 0) {
    std::cout << "check_uv_frame: all tests passed\n";
    return 0;
  }
  std::cerr << "check_uv_frame: " << g_failures << " failure(s)\n";
  return 1;
}
