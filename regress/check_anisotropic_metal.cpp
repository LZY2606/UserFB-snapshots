// Checks for anisotropic GGX metal reflection in the CPU path tracer.
//
// This program verifies, with independent numerical references, the
// anisotropic metal BRDF evaluation, sampling and PDF, the tangent frame
// construction, JSON parameter persistence and validation, and the final
// rendered highlight orientation on a small metal plate.

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "yocto/yocto_image.h"
#include "yocto/yocto_math.h"
#include "yocto/yocto_scene.h"
#include "yocto/yocto_sceneio.h"
#include "yocto/yocto_shading.h"
#include "yocto/yocto_trace.h"

using namespace yocto;
using std::string;
using std::vector;

// -----------------------------------------------------------------------------
// CHECK UTILITIES
// -----------------------------------------------------------------------------

static int check_failures = 0;
static int check_count    = 0;

#define CHECK(condition, ...)                                     \
  do {                                                            \
    check_count++;                                                \
    if (!(condition)) {                                           \
      check_failures++;                                           \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                 \
      printf(__VA_ARGS__);                                        \
      printf("\n");                                               \
    }                                                             \
  } while (0)

static const auto pi64 = 3.14159265358979323846;

static double rel_error(double a, double b) {
  return std::abs(a - b) / std::max(1e-12, std::abs(b));
}

// -----------------------------------------------------------------------------
// INDEPENDENT NUMERICAL REFERENCES
// -----------------------------------------------------------------------------

// Reference anisotropic GGX distribution, written directly from the
// definition in spherical coordinates (Heitz 2014), in double precision.
static double reference_D(float alphax, float alphay, const vec3f& normal,
    const vec3f& tangent, const vec3f& halfway) {
  auto bitangent = cross(normal, tangent);
  auto cosine    = (double)dot(halfway, normal);
  if (cosine <= 0) return 0;
  auto cosphi = (double)dot(halfway, tangent);
  auto sinphi = (double)dot(halfway, bitangent);
  auto ax     = (double)alphax, ay = (double)alphay;
  auto denom  = cosphi * cosphi / (ax * ax) + sinphi * sinphi / (ay * ay) +
               cosine * cosine;
  return 1.0 / (pi64 * ax * ay * denom * denom);
}

// Reference Smith G1 computed by numerical hemisphere integration of the
// defining relation G1(w) = (n.w) / integral_{m.w > 0} D(m) (m.w) dm.
static double reference_G1(float alphax, float alphay, const vec3f& normal,
    const vec3f& tangent, const vec3f& direction, int ntheta = 360,
    int nphi = 1440) {
  auto mu = (double)dot(direction, normal);
  if (mu <= 0) return 0;
  auto bitangent = cross(normal, tangent);
  auto integral  = 0.0;
  for (auto i = 0; i < ntheta; i++) {
    auto theta = (i + 0.5) / ntheta * (pi64 / 2);
    auto sinth = std::sin(theta), costh = std::cos(theta);
    for (auto j = 0; j < nphi; j++) {
      auto phi = (j + 0.5) / nphi * (2 * pi64);
      auto m   = tangent * (float)(sinth * std::cos(phi)) +
               bitangent * (float)(sinth * std::sin(phi)) +
               normal * (float)costh;
      auto mw = (double)dot(m, direction);
      if (mw <= 0) continue;
      integral += reference_D(alphax, alphay, normal, tangent, m) * mw * sinth;
    }
  }
  integral *= (pi64 / 2 / ntheta) * (2 * pi64 / nphi);
  return mu / integral;
}

// Reference reflection PDF: D(h) max(n.h,0) with the half-vector Jacobian.
static double reference_pdf(float alphax, float alphay, const vec3f& normal,
    const vec3f& tangent, const vec3f& outgoing, const vec3f& incoming) {
  if (dot(normal, incoming) * dot(normal, outgoing) <= 0) return 0;
  auto halfway = normalize(outgoing + incoming);
  return reference_D(alphax, alphay, normal, tangent, halfway) *
         std::max(0.0, (double)dot(normal, halfway)) /
         (4 * std::abs((double)dot(outgoing, halfway)));
}

// Reference hemispherical integral of the BRDF times cosine by quadrature.
static double reference_integral(const vec3f& color, float alphax,
    float alphay, const vec3f& normal, const vec3f& tangent,
    const vec3f& outgoing, int ntheta = 360, int nphi = 1440) {
  auto bitangent = cross(normal, tangent);
  auto sum       = 0.0;
  for (auto i = 0; i < ntheta; i++) {
    auto theta = (i + 0.5) / ntheta * (pi64 / 2);
    auto sinth = std::sin(theta), costh = std::cos(theta);
    for (auto j = 0; j < nphi; j++) {
      auto phi      = (j + 0.5) / nphi * (2 * pi64);
      auto incoming = tangent * (float)(sinth * std::cos(phi)) +
                      bitangent * (float)(sinth * std::sin(phi)) +
                      normal * (float)costh;
      auto value = eval_reflective(
          color, alphax, alphay, normal, tangent, outgoing, incoming);
      sum += (value.x + value.y + value.z) / 3 * sinth;
    }
  }
  return sum * (pi64 / 2 / ntheta) * (2 * pi64 / nphi);
}

// Reference integral of the sampling PDF over the reflection hemisphere.
static double reference_pdf_integral(float alphax, float alphay,
    const vec3f& normal, const vec3f& tangent, const vec3f& outgoing,
    const vec3f& color, int ntheta = 360, int nphi = 1440) {
  auto bitangent = cross(normal, tangent);
  auto sum       = 0.0;
  for (auto i = 0; i < ntheta; i++) {
    auto theta = (i + 0.5) / ntheta * (pi64 / 2);
    auto sinth = std::sin(theta), costh = std::cos(theta);
    for (auto j = 0; j < nphi; j++) {
      auto phi      = (j + 0.5) / nphi * (2 * pi64);
      auto incoming = tangent * (float)(sinth * std::cos(phi)) +
                      bitangent * (float)(sinth * std::sin(phi)) +
                      normal * (float)costh;
      sum += (double)sample_reflective_pdf(
                 color, alphax, alphay, normal, tangent, outgoing, incoming) *
             sinth;
    }
  }
  return sum * (pi64 / 2 / ntheta) * (2 * pi64 / nphi);
}

// Stratified-sampling estimate of the BRDF-cosine hemispherical integral.
// Failed samples (reflections below the surface) count as zero contribution
// but remain in the denominator.
static double sampling_estimate(const vec3f& color, float alphax, float alphay,
    const vec3f& normal, const vec3f& tangent, const vec3f& outgoing,
    int samples, int* failed = nullptr) {
  auto estimate = 0.0;
  auto nfailed  = 0;
  for (auto i = 0; i < samples; i++) {
    for (auto j = 0; j < samples; j++) {
      auto rn = vec2f{(i + 0.5f) / samples, (j + 0.5f) / samples};
      auto incoming =
          sample_reflective(color, alphax, alphay, normal, tangent, outgoing, rn);
      if (incoming == vec3f{0, 0, 0}) {
        nfailed++;
        continue;
      }
      auto pdf = sample_reflective_pdf(
          color, alphax, alphay, normal, tangent, outgoing, incoming);
      auto value = eval_reflective(
          color, alphax, alphay, normal, tangent, outgoing, incoming);
      if (pdf <= 0 || !std::isfinite(pdf)) {
        CHECK(false, "invalid pdf %g for sampled direction", (double)pdf);
        continue;
      }
      estimate += (value.x + value.y + value.z) / 3 / pdf;
    }
  }
  if (failed) *failed = nfailed;
  return estimate / ((double)samples * samples);
}

// -----------------------------------------------------------------------------
// TEST SCENE
// -----------------------------------------------------------------------------

// Small scene with a square metal plate on the ground, four symmetric lights
// around it and a camera on the plate axis, so that the whole setup is
// invariant under quarter turns about the plate normal.
static scene_data make_plate_scene(
    float anisotropy, float rotation, bool rotate_instance) {
  auto scene = scene_data{};

  // camera
  auto& camera    = scene.cameras.emplace_back();
  camera.frame    = lookat_frame({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  camera.lens     = 0.050f;
  camera.film     = 0.036f;
  camera.aspect   = 1;
  camera.focus    = 3.0f;
  camera.aperture = 0;

  // plate shape with u aligned to world x
  auto& shape       = scene.shapes.emplace_back();
  shape.positions   = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  shape.quads       = {{0, 1, 2, 3}};
  shape.normals     = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  shape.texcoords   = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  auto& material    = scene.materials.emplace_back();
  material.type     = material_type::reflective;
  material.color    = {0.8f, 0.7f, 0.6f};
  material.roughness = 0.25f;
  material.anisotropy          = anisotropy;
  material.anisotropy_rotation = rotation;
  auto& instance    = scene.instances.emplace_back();
  instance.shape    = 0;
  instance.material = 0;
  if (rotate_instance) instance.frame = rotation_frame({0, 0, 1}, pif / 2);

  // four lights in a quarter-turn symmetric arrangement
  auto& lshape = scene.shapes.emplace_back();
  for (auto sx : {-1.2f, 1.2f}) {
    for (auto sy : {-1.2f, 1.2f}) {
      auto base = (int)lshape.positions.size();
      auto s    = 0.15f;
      lshape.positions.push_back({sx - s, sy - s, 1.5f});
      lshape.positions.push_back({sx + s, sy - s, 1.5f});
      lshape.positions.push_back({sx + s, sy + s, 1.5f});
      lshape.positions.push_back({sx - s, sy + s, 1.5f});
      lshape.quads.push_back({base, base + 1, base + 2, base + 3});
      for (auto k = 0; k < 4; k++) lshape.normals.push_back({0, 0, -1});
    }
  }
  auto& lmaterial  = scene.materials.emplace_back();
  lmaterial.emission = {40, 40, 40};
  auto& linstance    = scene.instances.emplace_back();
  linstance.shape    = 1;
  linstance.material = 1;

  return scene;
}

static image_data render_scene(
    const scene_data& scene, trace_sampler_type sampler, int samples) {
  auto params       = trace_params{};
  params.resolution = 128;
  params.sampler    = sampler;
  params.samples    = samples;
  params.bounces    = 4;
  params.clamp      = 100;
  return trace_image(scene, params);
}

// Second-moment analysis of the image luminance: principal axis as a
// double-angle vector and the axis ratio. Moments are accumulated within
// image quadrants around their own centroids, so that they measure the
// highlight streak shape rather than where highlights sit in the image.
struct image_axis {
  double angle = 0;
  double ratio = 1;
  double m20   = 0;
  double m02   = 0;
  double m11   = 0;
};

static image_axis compute_image_axis(const image_data& image) {
  auto W  = std::array<std::array<double, 2>, 2>{};
  auto cx = std::array<std::array<double, 2>, 2>{};
  auto cy = std::array<std::array<double, 2>, 2>{};
  for (auto j = 0; j < image.height; j++) {
    for (auto i = 0; i < image.width; i++) {
      auto& p = image.pixels[j * image.width + i];
      auto  w = std::max(0.0, (p.x + p.y + p.z) / 3.0);
      auto  qx = i >= image.width / 2 ? 1 : 0;
      auto  qy = j >= image.height / 2 ? 1 : 0;
      W[qx][qy] += w;
      cx[qx][qy] += i * w;
      cy[qx][qy] += j * w;
    }
  }
  for (auto qx = 0; qx < 2; qx++)
    for (auto qy = 0; qy < 2; qy++) {
      cx[qx][qy] /= W[qx][qy];
      cy[qx][qy] /= W[qx][qy];
    }
  auto m20 = 0.0, m02 = 0.0, m11 = 0.0;
  for (auto j = 0; j < image.height; j++) {
    for (auto i = 0; i < image.width; i++) {
      auto& p  = image.pixels[j * image.width + i];
      auto  w  = std::max(0.0, (p.x + p.y + p.z) / 3.0);
      auto  qx = i >= image.width / 2 ? 1 : 0;
      auto  qy = j >= image.height / 2 ? 1 : 0;
      m20 += (i - cx[qx][qy]) * (i - cx[qx][qy]) * w;
      m02 += (j - cy[qx][qy]) * (j - cy[qx][qy]) * w;
      m11 += (i - cx[qx][qy]) * (j - cy[qx][qy]) * w;
    }
  }
  auto root   = std::sqrt((m20 - m02) * (m20 - m02) + 4 * m11 * m11);
  auto result = image_axis{};
  result.angle = 0.5 * std::atan2(2 * m11, m20 - m02);
  result.ratio = std::sqrt(
      (m20 + m02 + root) / std::max(1e-12, m20 + m02 - root));
  result.m20 = m20;
  result.m02 = m02;
  result.m11 = m11;
  return result;
}

// Angle between two undirected axes, in [0, pi/2].
static double axis_difference(double angle1, double angle2) {
  auto delta = std::abs(angle1 - angle2);
  delta      = std::fmod(delta, pi64);
  if (delta > pi64 / 2) delta = pi64 - delta;
  return delta;
}

static double relative_l2(const image_data& image1, const image_data& image2) {
  auto num = 0.0, den = 0.0;
  for (auto idx = 0; idx < (int)image1.pixels.size(); idx++) {
    auto& p = image1.pixels[idx];
    auto& q = image2.pixels[idx];
    num += (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) +
           (p.z - q.z) * (p.z - q.z);
    den += p.x * p.x + p.y * p.y + p.z * p.z;
  }
  return std::sqrt(num / std::max(1e-12, den));
}

static void save_ppm(const string& filename, const image_data& image) {
  auto file = std::fopen(filename.c_str(), "wb");
  if (!file) return;
  std::fprintf(file, "P6\n%d %d\n255\n", image.width, image.height);
  for (auto& pixel : image.pixels) {
    auto rgb = std::array<unsigned char, 3>{};
    for (auto k = 0; k < 3; k++) {
      auto value = pixel[k] / (1 + pixel[k]);
      value      = std::pow(std::max(0.0f, value), 1 / 2.2f);
      rgb[k]     = (unsigned char)std::lround(
          std::min(255.0f, std::max(0.0f, value * 255)));
    }
    std::fwrite(rgb.data(), 1, 3, file);
  }
  std::fclose(file);
}

// -----------------------------------------------------------------------------
// TESTS
// -----------------------------------------------------------------------------

// Distribution, shadowing and pdf against independent references.
static void test_numerical_references() {
  printf("checking D/Smith/PDF against independent references ...\n");
  auto normal  = vec3f{0, 0, 1};
  auto tangent = vec3f{1, 0, 0};
  auto color   = vec3f{0.8f, 0.7f, 0.6f};
  struct combo {
    float anisotropy;
    float rotation;
  };
  for (auto combo : {combo{0.85f, 0.0f}, combo{-0.85f, 0.0f},
           combo{0.6f, 0.7f}, combo{-0.4f, -1.1f}}) {
    auto roughness2 = anisotropic_roughness(0.06f, combo.anisotropy);
    auto rotated    = tangent * std::cos(combo.rotation) +
                   cross(normal, tangent) * std::sin(combo.rotation);
    // check D and G1 on a direction grid
    for (auto i = 0; i < 12; i++) {
      for (auto j = 0; j < 24; j++) {
        auto theta = (i + 0.5) / 12 * (pi64 / 2) * 0.98;
        auto phi   = (j + 0.5) / 24 * 2 * pi64;
        auto h     = rotated * (float)(std::sin(theta) * std::cos(phi)) +
                 cross(normal, rotated) * (float)(std::sin(theta) * std::sin(phi)) +
                 normal * (float)std::cos(theta);
        auto D     = microfacet_distribution_anisotropic(
            roughness2.x, roughness2.y, normal, rotated, h);
        auto Dref = reference_D(roughness2.x, roughness2.y, normal, rotated, h);
        CHECK(rel_error(D, Dref) < 1e-4,
            "D mismatch: aniso=%g rot=%g h=(%g,%g,%g) D=%g ref=%g",
            (double)combo.anisotropy, (double)combo.rotation, (double)h.x,
            (double)h.y, (double)h.z, (double)D, Dref);
        auto G1 = microfacet_shadowing1_anisotropic(
            roughness2.x, roughness2.y, normal, rotated, h, h);
        auto G1ref = reference_G1(roughness2.x, roughness2.y, normal, rotated, h);
        CHECK(std::abs(G1 - G1ref) < 2e-3,
            "G1 mismatch: aniso=%g rot=%g w=(%g,%g,%g) G1=%g ref=%g",
            (double)combo.anisotropy, (double)combo.rotation, (double)h.x,
            (double)h.y, (double)h.z, (double)G1, G1ref);
      }
    }
    // check the reflection pdf on a direction grid
    for (auto elevation : {0.15, 0.5, 1.0}) {
      auto outgoing = normalize(
          vec3f{(float)std::cos(elevation), 0, (float)std::sin(elevation)});
      for (auto i = 2; i < 12; i++) {
        for (auto j = 0; j < 24; j += 3) {
          auto theta = (i + 0.5) / 12 * (pi64 / 2);
          auto phi   = (j + 0.5) / 24 * 2 * pi64;
          auto incoming =
              rotated * (float)(std::sin(theta) * std::cos(phi)) +
              cross(normal, rotated) * (float)(std::sin(theta) * std::sin(phi)) +
              normal * (float)std::cos(theta);
          auto pdf = sample_reflective_pdf(color, roughness2.x, roughness2.y,
              normal, rotated, outgoing, incoming);
          auto ref = reference_pdf(roughness2.x, roughness2.y, normal, rotated,
              outgoing, incoming);
          CHECK(rel_error(pdf, ref) < 1e-3,
              "pdf mismatch: aniso=%g rot=%g wi=(%g,%g,%g) pdf=%g ref=%g",
              (double)combo.anisotropy, (double)combo.rotation,
              (double)incoming.x, (double)incoming.y, (double)incoming.z,
              (double)pdf, ref);
        }
      }
      // pdf vanishes for directions on the other side
      auto below = sample_reflective_pdf(color, roughness2.x, roughness2.y,
          normal, rotated, outgoing, -outgoing);
      CHECK(below == 0, "pdf below surface not zero: %g", (double)below);
    }
  }
}

// Positive and negative anisotropy swap the two axes; a quarter-turn
// rotation maps one parametrization onto the other.
static void test_axis_swap() {
  printf("checking axis swap between +/- anisotropy and quarter turns ...\n");
  auto normal  = vec3f{0, 0, 1};
  auto tangent = vec3f{1, 0, 0};
  auto color   = vec3f{0.8f, 0.7f, 0.6f};
  auto pos     = anisotropic_roughness(0.06f, 0.85f);
  auto neg     = anisotropic_roughness(0.06f, -0.85f);
  CHECK(std::abs(pos.x - neg.y) < 1e-7 && std::abs(pos.y - neg.x) < 1e-7,
      "widths not swapped: pos=(%g,%g) neg=(%g,%g)", (double)pos.x,
      (double)pos.y, (double)neg.x, (double)neg.y);
  auto turned = cross(normal, tangent);  // quarter turn of the tangent
  for (auto i = 1; i < 10; i++) {
    for (auto j = 0; j < 16; j++) {
      auto theta_o = (i + 0.5) / 10 * (pi64 / 2);
      auto phi_o   = (j + 0.5) / 16 * 2 * pi64;
      auto outgoing = vec3f{(float)(std::sin(theta_o) * std::cos(phi_o)),
          (float)(std::sin(theta_o) * std::sin(phi_o)),
          (float)std::cos(theta_o)};
      for (auto k = 1; k < 10; k += 3) {
        auto theta_i = (k + 0.5) / 10 * (pi64 / 2);
        auto incoming = vec3f{(float)(std::sin(theta_i) * std::cos(phi_o)),
            (float)(std::sin(theta_i) * std::sin(phi_o)),
            (float)std::cos(theta_i)};
        auto value_pos = eval_reflective(
            color, pos.x, pos.y, normal, tangent, outgoing, incoming);
        auto value_neg = eval_reflective(
            color, neg.x, neg.y, normal, turned, outgoing, incoming);
        CHECK(rel_error(value_pos.x, value_neg.x) < 1e-4,
            "axis swap eval mismatch: (%g,%g) vs (%g,%g): %g vs %g",
            (double)pos.x, (double)pos.y, (double)neg.x, (double)neg.y,
            (double)value_pos.x, (double)value_neg.x);
        auto pdf_pos = sample_reflective_pdf(
            color, pos.x, pos.y, normal, tangent, outgoing, incoming);
        auto pdf_neg = sample_reflective_pdf(
            color, neg.x, neg.y, normal, turned, outgoing, incoming);
        CHECK(rel_error(pdf_pos, pdf_neg) < 1e-4,
            "axis swap pdf mismatch: %g vs %g", (double)pdf_pos,
            (double)pdf_neg);
      }
    }
  }
}

// Zero anisotropy must reproduce the isotropic evaluation and pdf.
static void test_zero_anisotropy() {
  printf("checking isotropic fallback for zero anisotropy ...\n");
  auto normal  = normalize(vec3f{0.2f, 0.1f, 1});
  auto tangent = normalize(orthogonal(normal));
  auto color   = vec3f{0.8f, 0.7f, 0.6f};
  auto widths  = anisotropic_roughness(0.25f, 0.0f);
  CHECK(widths.x == 0.25f && widths.y == 0.25f,
      "zero anisotropy widths: (%g,%g)", (double)widths.x, (double)widths.y);
  for (auto i = 1; i < 10; i++) {
    for (auto j = 0; j < 16; j++) {
      auto theta_o  = (i + 0.5) / 10 * (pi64 / 2);
      auto phi_o    = (j + 0.5) / 16 * 2 * pi64;
      auto bitan    = cross(normal, tangent);
      auto outgoing = tangent * (float)(std::sin(theta_o) * std::cos(phi_o)) +
                      bitan * (float)(std::sin(theta_o) * std::sin(phi_o)) +
                      normal * (float)std::cos(theta_o);
      for (auto k = 1; k < 10; k += 2) {
        auto theta_i  = (k + 0.5) / 10 * (pi64 / 2);
        auto incoming = tangent * (float)(std::sin(theta_i) * std::cos(phi_o)) +
                        bitan * (float)(std::sin(theta_i) * std::sin(phi_o)) +
                        normal * (float)std::cos(theta_i);
        auto iso = eval_reflective(color, 0.25f, normal, outgoing, incoming);
        auto ani = eval_reflective(
            color, widths.x, widths.y, normal, tangent, outgoing, incoming);
        CHECK(rel_error(ani.x, iso.x) < 1e-4,
            "zero-anisotropy eval mismatch: %g vs %g", (double)ani.x,
            (double)iso.x);
        auto pis = sample_reflective_pdf(color, 0.25f, normal, outgoing, incoming);
        auto pan = sample_reflective_pdf(
            color, widths.x, widths.y, normal, tangent, outgoing, incoming);
        CHECK(rel_error(pan, pis) < 1e-4,
            "zero-anisotropy pdf mismatch: %g vs %g", (double)pan,
            (double)pis);
      }
    }
  }
}

// Zero roughness still goes through the mirror delta path.
static void test_mirror_delta() {
  printf("checking mirror delta path ...\n");
  auto normal   = vec3f{0, 0, 1};
  auto color    = vec3f{0.8f, 0.7f, 0.6f};
  auto outgoing = normalize(vec3f{0.3f, 0.2f, 1});
  // a zero-roughness reflective material is a delta even with anisotropy set
  auto scene              = make_plate_scene(0.8f, 0.3f, false);
  scene.materials[0].roughness = 0;
  auto point = eval_material(scene, scene.instances[0], 0, {0.5f, 0.5f});
  CHECK(point.roughness == 0, "mirror roughness: %g", (double)point.roughness);
  CHECK(is_delta(point), "mirror material not delta");
  auto incoming = sample_reflective(color, normal, outgoing);
  auto expected = reflect(outgoing, normal);
  CHECK(length(incoming - expected) < 1e-6,
      "mirror sample mismatch: (%g,%g,%g)", (double)incoming.x,
      (double)incoming.y, (double)incoming.z);
  auto value = eval_reflective(color, normal, outgoing, incoming);
  CHECK(value.x > 0 && isfinite(value), "mirror eval invalid");
  CHECK(sample_reflective_pdf(color, normal, outgoing, incoming) == 1,
      "mirror pdf not 1");
}

// Energy conservation: stratified sampling estimates match independent
// hemispherical quadrature, with failed samples kept in the denominator.
static void test_energy_integration() {
  printf("checking energy integration with stratified samples ...\n");
  auto normal  = vec3f{0, 0, 1};
  auto tangent = vec3f{1, 0, 0};
  auto color   = vec3f{0.8f, 0.7f, 0.6f};
  struct combo {
    float anisotropy;
    float rotation;
    float elevation;
  };
  for (auto combo : {combo{0.0f, 0.0f, 1.2f}, combo{0.85f, 0.0f, 1.2f},
           combo{-0.85f, 0.0f, 0.8f}, combo{0.85f, 0.7f, 0.35f},
           combo{-0.6f, -1.1f, 0.1f}}) {
    auto roughness2 = anisotropic_roughness(0.06f, combo.anisotropy);
    auto rotated    = tangent * std::cos(combo.rotation) +
                   cross(normal, tangent) * std::sin(combo.rotation);
    auto outgoing = normalize(rotated * (float)std::cos(combo.elevation) +
                              normal * (float)std::sin(combo.elevation));
    auto nfailed  = 0;
    auto estimate = sampling_estimate(
        color, roughness2.x, roughness2.y, normal, rotated, outgoing, 64, &nfailed);
    auto reference = reference_integral(
        color, roughness2.x, roughness2.y, normal, rotated, outgoing);
    CHECK(rel_error(estimate, reference) < 0.02,
        "energy mismatch: aniso=%g rot=%g elev=%g estimate=%g reference=%g "
        "failed=%d/4096",
        (double)combo.anisotropy, (double)combo.rotation,
        (double)combo.elevation, estimate, reference, nfailed);
    printf("  aniso=%+.2f rot=%+.2f elev=%.2f estimate=%.5f reference=%.5f "
           "failed=%d\n",
        (double)combo.anisotropy, (double)combo.rotation,
        (double)combo.elevation, estimate, reference, nfailed);
    // the pdf integrates to the fraction of valid samples
    auto pdf_integral = reference_pdf_integral(roughness2.x, roughness2.y,
        normal, rotated, outgoing, color);
    auto valid_fraction = 1 - nfailed / 4096.0;
    CHECK(std::abs(pdf_integral - valid_fraction) < 0.02,
        "pdf integral mismatch: aniso=%g rot=%g elev=%g integral=%g "
        "valid=%g",
        (double)combo.anisotropy, (double)combo.rotation,
        (double)combo.elevation, pdf_integral, valid_fraction);
  }
}

// The sampled half-vectors follow the anisotropic distribution.
static void test_sample_moments() {
  printf("checking half-vector sample moments ...\n");
  auto normal  = vec3f{0, 0, 1};
  auto tangent = vec3f{1, 0, 0};
  auto widths  = anisotropic_roughness(0.06f, 0.85f);
  auto bitan   = cross(normal, tangent);
  // stratified sample moments (fine stratification since the moments are
  // slowly convergent near the hemisphere rim)
  auto mt = 0.0, mb = 0.0;
  auto S  = 512;
  for (auto i = 0; i < S; i++) {
    for (auto j = 0; j < S; j++) {
      auto rn = vec2f{(i + 0.5f) / S, (j + 0.5f) / S};
      auto h  = sample_microfacet_anisotropic(
          widths.x, widths.y, normal, tangent, rn);
      auto ht = (double)dot(h, tangent), hb = (double)dot(h, bitan);
      mt += ht * ht;
      mb += hb * hb;
    }
  }
  mt /= S * S;
  mb /= S * S;
  // quadrature reference under pdf D(h) (n.h), integrated over the (u,v)
  // disk with h = u t + v b + sqrt(1-u^2-v^2) n, where the pdf becomes D
  auto rt = 0.0, rb = 0.0, norm = 0.0;
  auto NU = 2048;
  for (auto i = 0; i < NU; i++) {
    auto u = (i + 0.5) / NU * 2 - 1;
    for (auto j = 0; j < NU; j++) {
      auto v  = (j + 0.5) / NU * 2 - 1;
      auto r2 = u * u + v * v;
      if (r2 >= 1) continue;
      auto h = tangent * (float)u + bitan * (float)v +
               normal * (float)std::sqrt(1 - r2);
      auto D = reference_D(widths.x, widths.y, normal, tangent, h);
      rt += u * u * D;
      rb += v * v * D;
      norm += D;
    }
  }
  auto cell = (2.0 / NU) * (2.0 / NU);
  rt *= cell;
  rb *= cell;
  norm *= cell;
  CHECK(std::abs(norm - 1) < 1e-3, "NDF not normalized: %g", norm);
  rt /= norm;
  rb /= norm;
  CHECK(rel_error(mt, rt) < 0.02, "tangent moment mismatch: %g vs %g", mt, rt);
  CHECK(rel_error(mb, rb) < 0.02, "bitangent moment mismatch: %g vs %g", mb,
      rb);
  CHECK(mt > 2 * mb, "samples not stretched along the tangent: %g vs %g", mt,
      mb);
  printf("  moments: sample=(%.5f,%.5f) reference=(%.5f,%.5f)\n", mt, mb, rt,
      rb);
}

// Tangent frame construction from the scene.
static void test_tangent_frame() {
  printf("checking shading tangent frames ...\n");
  auto scene = make_plate_scene(0.8f, 0, false);
  auto t0    = eval_shading_tangent(scene, scene.instances[0], 0, {0, 0, 1}, 0);
  CHECK(length(t0 - vec3f{1, 0, 0}) < 1e-5,
      "tangent not aligned to u: (%g,%g,%g)", (double)t0.x, (double)t0.y,
      (double)t0.z);
  auto t1 = eval_shading_tangent(scene, scene.instances[0], 0, {0, 0, 1}, pif / 2);
  CHECK(length(t1 - vec3f{0, 1, 0}) < 1e-5,
      "rotated tangent mismatch: (%g,%g,%g)", (double)t1.x, (double)t1.y,
      (double)t1.z);
  // the tangent follows the instance transform
  auto t2 = eval_shading_tangent(scene, scene.instances[0], 0, {0, 0, 1}, 0);
  auto rotated_scene = make_plate_scene(0.8f, 0, true);
  auto t3 = eval_shading_tangent(
      rotated_scene, rotated_scene.instances[0], 0, {0, 0, 1}, 0);
  CHECK(length(t3 - vec3f{0, 1, 0}) < 1e-5,
      "tangent not rotated with instance: (%g,%g,%g)", (double)t3.x,
      (double)t3.y, (double)t3.z);
  CHECK(length(t2 - vec3f{1, 0, 0}) < 1e-5, "base tangent changed");
  // degenerate frames fall back to a stable orthonormal basis
  auto& shape = scene.shapes[0];
  auto  saved = shape.texcoords;
  shape.texcoords.clear();
  auto t4 = eval_shading_tangent(scene, scene.instances[0], 0, {0, 0, 1}, 0);
  CHECK(std::abs(length(t4) - 1) < 1e-5 && std::abs(dot(t4, {0, 0, 1})) < 1e-5,
      "fallback tangent invalid: (%g,%g,%g)", (double)t4.x, (double)t4.y,
      (double)t4.z);
  auto t5 = eval_shading_tangent(
      scene, scene.instances[0], 0, normalize(vec3f{1, 2, 3}), 0.7f);
  CHECK(std::abs(length(t5) - 1) < 1e-4 &&
            std::abs(dot(t5, normalize(vec3f{1, 2, 3}))) < 1e-4,
      "fallback tangent invalid: (%g,%g,%g)", (double)t5.x, (double)t5.y,
      (double)t5.z);
  shape.texcoords = saved;
}

// Render the plate in two orientations with the CPU path tracer and the
// direct-lighting sampler, and check the highlight follows the brushing.
static void test_render(const string& outdir) {
  printf("rendering metal plate orientations ...\n");
  for (auto sampler : {trace_sampler_type::path, trace_sampler_type::pathdirect}) {
    auto sname = sampler == trace_sampler_type::path ? "path" : "pathdirect";
    auto base  = make_plate_scene(0.85f, 0, false);
    auto inst  = make_plate_scene(0.85f, 0, true);
    auto rot   = make_plate_scene(0.85f, pif / 2, false);
    auto iso   = make_plate_scene(0.0f, 0, false);
    auto image_base = render_scene(base, sampler, 256);
    auto image_inst = render_scene(inst, sampler, 256);
    auto image_rot  = render_scene(rot, sampler, 256);
    auto image_iso  = render_scene(iso, sampler, 256);
    save_ppm(outdir + "/plate_base_" + sname + ".ppm", image_base);
    save_ppm(outdir + "/plate_rotated_instance_" + sname + ".ppm", image_inst);
    save_ppm(outdir + "/plate_rotated_material_" + sname + ".ppm", image_rot);
    save_ppm(outdir + "/plate_isotropic_" + sname + ".ppm", image_iso);

    auto axis_base = compute_image_axis(image_base);
    auto axis_inst = compute_image_axis(image_inst);
    auto axis_rot  = compute_image_axis(image_rot);
    auto axis_iso  = compute_image_axis(image_iso);
    printf("  [%s] axis angle base=%.1f inst=%.1f rot=%.1f iso-ratio=%.3f "
           "aniso-ratio=%.3f\n",
        sname, axis_base.angle * 180 / pi64, axis_inst.angle * 180 / pi64,
        axis_rot.angle * 180 / pi64, axis_iso.ratio, axis_base.ratio);
    printf("  [%s] moments base m20=%.4g m02=%.4g m11=%.4g | inst m20=%.4g "
           "m02=%.4g\n",
        sname, axis_base.m20, axis_base.m02, axis_base.m11, axis_inst.m20,
        axis_inst.m02);

    // the highlight is stretched along one axis only for the anisotropic plate
    CHECK(axis_base.ratio > 1.3,
        "[%s] highlight not anisotropic: ratio=%g", sname, axis_base.ratio);
    CHECK(axis_iso.ratio < 1.25,
        "[%s] isotropic control not symmetric: ratio=%g", sname,
        axis_iso.ratio);
    // the highlight rotates with the instance and with the material rotation
    CHECK(axis_difference(axis_base.angle, axis_inst.angle) >
              75 * pi64 / 180,
        "[%s] highlight did not rotate with instance: base=%g inst=%g", sname,
        axis_base.angle, axis_inst.angle);
    CHECK(axis_difference(axis_base.angle, axis_rot.angle) > 75 * pi64 / 180,
        "[%s] highlight did not rotate with material rotation: base=%g "
        "rot=%g",
        sname, axis_base.angle, axis_rot.angle);
    CHECK(axis_difference(axis_inst.angle, axis_rot.angle) < 15 * pi64 / 180,
        "[%s] instance and material rotation disagree: inst=%g rot=%g", sname,
        axis_inst.angle, axis_rot.angle);
    // rotating the instance or the material gives the same image
    auto diff = relative_l2(image_inst, image_rot);
    CHECK(diff < 0.03, "[%s] instance/material rotation images differ: %g",
        sname, diff);
  }
}

// JSON round trip and validation.
static void test_json(const string& outdir) {
  printf("checking JSON persistence and validation ...\n");
  auto scene = make_plate_scene(0.85f, pif / 2, false);
  scene.material_names = {"brushed_metal", "light"};
  scene.camera_names   = {"camera"};
  scene.shape_names    = {"plate", "lights"};
  scene.instance_names = {"plate", "lights"};

  // round trip preserves the new parameters
  auto filename = outdir + "/scene.json";
  auto error    = string{};
  std::filesystem::create_directories(
      std::filesystem::path(filename).parent_path() / "shapes");
  CHECK(save_scene(filename, scene, error), "cannot save scene: %s",
      error.c_str());
  auto loaded = scene_data{};
  CHECK(load_scene(filename, loaded, error), "cannot reload scene: %s",
      error.c_str());
  if (loaded.materials.empty()) return;
  CHECK(loaded.materials[0].anisotropy == 0.85f,
      "reloaded anisotropy: %g", (double)loaded.materials[0].anisotropy);
  CHECK(loaded.materials[0].anisotropy_rotation == (float)(pif / 2),
      "reloaded anisotropy_rotation: %g",
      (double)loaded.materials[0].anisotropy_rotation);

  // the reloaded scene renders the same highlight direction
  auto image_orig   = render_scene(scene, trace_sampler_type::pathdirect, 256);
  auto image_loaded = render_scene(loaded, trace_sampler_type::pathdirect, 256);
  save_ppm(outdir + "/plate_reloaded_pathdirect.ppm", image_loaded);
  auto diff = relative_l2(image_orig, image_loaded);
  CHECK(diff < 0.01, "reloaded scene renders differently: %g", diff);

  // out-of-range values are rejected with the material name
  auto write_json = [](const string& path, const string& text) {
    auto stream = std::ofstream(path);
    stream << text;
  };
  auto header = string{
      "{\"asset\":{\"version\":\"4.2\"},\"materials\":["};
  auto bad1 = outdir + "/bad1.json";
  write_json(bad1, header +
                       "{\"name\":\"badmetal\",\"type\":\"reflective\","
                       "\"anisotropy\":2.0}]}");
  auto badscene = scene_data{};
  CHECK(!load_scene(bad1, badscene, error),
      "out-of-range anisotropy accepted");
  CHECK(error.find("badmetal") != string::npos,
      "error does not name the material: %s", error.c_str());
  auto bad2 = outdir + "/bad2.json";
  write_json(bad2, header +
                       "{\"name\":\"badmetal\",\"type\":\"reflective\","
                       "\"anisotropy\":-1.5}]}");
  badscene = scene_data{};
  CHECK(!load_scene(bad2, badscene, error),
      "out-of-range negative anisotropy accepted");
  CHECK(error.find("badmetal") != string::npos,
      "error does not name the material: %s", error.c_str());
  auto bad3 = outdir + "/bad3.json";
  write_json(bad3, header +
                       "{\"name\":\"badmetal\",\"type\":\"reflective\","
                       "\"anisotropy_rotation\":1e999}]}");
  badscene = scene_data{};
  CHECK(!load_scene(bad3, badscene, error),
      "non-finite anisotropy_rotation accepted");
  // boundary values are accepted
  auto good = outdir + "/good.json";
  write_json(good, header +
                       "{\"name\":\"edgemetal\",\"type\":\"reflective\","
                       "\"anisotropy\":1.0,\"anisotropy_rotation\":-3.14}]}");
  badscene = scene_data{};
  CHECK(load_scene(good, badscene, error), "boundary anisotropy rejected: %s",
      error.c_str());
  if (!badscene.materials.empty()) {
    CHECK(badscene.materials[0].anisotropy == 1.0f,
        "boundary anisotropy value: %g",
        (double)badscene.materials[0].anisotropy);
  }
  // non-finite values are caught by scene validation before shading
  auto nanscene              = scene_data{};
  auto& nanmaterial          = nanscene.materials.emplace_back();
  nanmaterial.anisotropy     = std::numeric_limits<float>::quiet_NaN();
  nanscene.material_names    = {"nanmetal"};
  auto validation            = scene_validation(nanscene);
  auto found                 = false;
  for (auto& err : validation)
    if (err.find("nanmetal") != string::npos) found = true;
  CHECK(found, "scene validation missed non-finite anisotropy");
}

int main(int argc, char** argv) {
  auto outdir = argc > 1 ? string{argv[1]} : string{"."};
  test_numerical_references();
  test_axis_swap();
  test_zero_anisotropy();
  test_mirror_delta();
  test_energy_integration();
  test_sample_moments();
  test_tangent_frame();
  test_render(outdir);
  test_json(outdir);
  printf("%d checks, %d failures\n", check_count, check_failures);
  if (check_failures != 0) {
    printf("FAILURES: %d\n", check_failures);
    return 1;
  }
  printf("all checks passed\n");
  return 0;
}
