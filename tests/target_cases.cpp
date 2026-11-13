//
// Target cases for light-linking masks (receive_mask / emission_mask).
//
// All scenes are generated procedurally and rendered through the public
// trace API in libs/yocto. The cases check that a surface-to-emitter
// segment is only lit by lights whose emission mask shares a bit with the
// receiving surface mask, that geometry occlusion stays shared, that the
// group is recomputed at every bounce (including ideal mirrors), and that
// multiple environments are filtered independently.
//

#include "libs/yocto/yocto_trace.h"
#include "test_api.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace yocto;

namespace {

// Light groups used across the cases.
constexpr uint64_t group_red  = 1ull << 0;
constexpr uint64_t group_blue = 1ull << 1;
constexpr uint64_t group_high = 1ull << 63;
constexpr uint64_t group_none = 0ull;

// A matte material with the given linear color.
material_data make_matte(const vec3f& color) {
  auto material     = material_data{};
  material.type     = material_type::matte;
  material.color    = color;
  return material;
}

// An emissive material.
material_data make_emissive(const vec3f& emission) {
  auto material     = material_data{};
  material.type     = material_type::matte;
  material.emission = emission;
  return material;
}

// An ideal mirror.
material_data make_mirror(const vec3f& color) {
  auto material      = material_data{};
  material.type      = material_type::reflective;
  material.roughness = 0;
  material.color     = color;
  return material;
}

// Adds an instance of an existing shape/material and returns its index.
int add_instance(scene_data& scene, int shape, int material,
    const frame3f& frame = identity3x4f, uint64_t receive_mask = ~uint64_t{0},
    uint64_t emission_mask = 1) {
  auto& instance         = scene.instances.emplace_back();
  instance.frame         = frame;
  instance.shape         = shape;
  instance.material      = material;
  instance.receive_mask  = receive_mask;
  instance.emission_mask = emission_mask;
  return (int)scene.instances.size() - 1;
}

// Adds a constant-color environment and returns its index.
int add_environment(scene_data& scene, const vec3f& emission,
    uint64_t emission_mask) {
  auto& environment         = scene.environments.emplace_back();
  environment.emission      = emission;
  environment.emission_mask = emission_mask;
  return (int)scene.environments.size() - 1;
}

// Frame placed at `position` with the given normal direction.
frame3f facing(const vec3f& position, const vec3f& normal) {
  return frame_fromz(position, normal);
}

// Renders a scene and returns the accumulated linear image.
vector<vec4f> render(const scene_data& scene, trace_sampler_type sampler,
    int resolution, int samples, int bounces = 4) {
  auto params       = trace_params{};
  params.resolution = resolution;
  params.samples    = samples;
  params.bounces    = bounces;
  params.sampler    = sampler;
  params.noparallel = true;
  params.seed       = 1234567ull;
  auto bvh          = make_trace_bvh(scene, params);
  auto lights       = make_trace_lights(scene, params);
  auto state        = make_trace_state(scene, params);
  for (auto n = 0; n < samples; n++)
    trace_samples(state, scene, bvh, lights, params);
  return state.image;
}

// Average RGB of an image.
vec3f average_rgb(const vector<vec4f>& image) {
  auto sum = vec3f{0, 0, 0};
  for (auto& pixel : image) sum += xyz(pixel);
  return sum / (float)image.size();
}

// Mean absolute error over RGB channels.
float mean_error(const vec3f& a, const vec3f& b) {
  return (fabsf(a.x - b.x) + fabsf(a.y - b.y) + fabsf(a.z - b.z)) / 3.0f;
}

// Average color of the pixels whose camera ray directly hits `instance`.
// Lets a test measure a receiver wall even when other geometry is directly
// visible in part of the frame.
vec3f average_on_instance(const scene_data& scene,
    const vector<vec4f>& image, int resolution, int instance) {
  auto params       = trace_params{};
  params.resolution = resolution;
  auto bvh          = make_trace_bvh(scene, params);
  auto sum          = vec3f{0, 0, 0};
  auto count        = 0;
  for (auto j = 0; j < resolution; j++) {
    for (auto i = 0; i < resolution; i++) {
      auto uv   = vec2f{(i + 0.5f) / resolution, (j + 0.5f) / resolution};
      auto ray  = eval_camera(scene.cameras[0], uv, {0, 0});
      auto hit  = intersect_scene_bvh(bvh.bvh, scene, ray);
      if (hit.hit && hit.instance == instance) {
        sum += xyz(image[j * resolution + i]);
        count++;
      }
    }
  }
  if (count == 0) throw std::runtime_error("no pixel hit the target instance");
  return sum / (float)count;
}

// Writes a tiny binary PPM (RGB, one byte per channel).
void save_ppm(const string& filename, const vector<vec4f>& image, int width,
    int height) {
  auto file = std::ofstream{filename, std::ios::binary};
  if (!file) throw std::runtime_error("cannot write " + filename);
  file << "P6\n" << width << " " << height << "\n255\n";
  for (auto& pixel : image) {
    auto bytes = std::array<unsigned char, 3>{};
    for (auto c = 0; c < 3; c++) {
      auto v   = pixel[c] <= 0 ? 0 : pixel[c] >= 1 ? 1 : pixel[c];
      bytes[c] = (unsigned char)roundf(v * 255.0f);
    }
    file.write((const char*)bytes.data(), 3);
  }
}

// Scene with an unoccluded white matte wall and two constant environments,
// one "red" group and one "blue" group. The wall receive mask selects the
// allowed environments.
scene_data make_env_scene(uint64_t receive_mask) {
  auto scene                      = scene_data{};
  scene.cameras.push_back({});
  scene.cameras[0].frame          = lookat_frame({0, 0, 2}, {0, 0, 0}, {0, 1, 0});
  scene.cameras[0].aspect         = 1;

  // vertical wall facing the camera (make_rect lies in the xy plane)
  scene.shapes.push_back(make_rect({1, 1}, {1, 1}));
  scene.materials.push_back(make_matte({0.8f, 0.8f, 0.8f}));
  add_instance(scene, 0, 0, identity3x4f, receive_mask);

  add_environment(scene, {0.6f, 0.1f, 0.1f}, group_red);
  add_environment(scene, {0.1f, 0.2f, 0.7f}, group_blue);
  return scene;
}

}  // namespace

void target_cases(Suite& suite) {
  // -------------------------------------------------------------------------
  // Case 1: constant environments, analytic matte solution.
  //
  // For an unoccluded matte surface under constant environments, the
  // linear exitant radiance equals albedo times the sum of the allowed
  // environment emissions (Lo = albedo/pi * integral Li cos dW, and the
  // hemisphere integral of a constant is pi). The same analytic value must
  // match both the path and naive samplers, not just each other.
  // -------------------------------------------------------------------------
  suite.run("analytic_environment_masks", [&] {
    const auto albedo = vec3f{0.8f, 0.8f, 0.8f};
    const auto red    = vec3f{0.6f, 0.1f, 0.1f};
    const auto blue   = vec3f{0.1f, 0.2f, 0.7f};

    // red group only
    {
      auto scene     = make_env_scene(group_red);
      auto expected  = albedo * red;
      auto path_img  = render(scene, trace_sampler_type::path, 16, 4096);
      auto naive_img = render(scene, trace_sampler_type::naive, 16, 4096);
      auto path_avg  = average_rgb(path_img);
      auto naive_avg = average_rgb(naive_img);
      auto err_path  = mean_error(path_avg, expected);
      auto err_naive = mean_error(naive_avg, expected);
      std::printf(
          "  red-only   analytic=(%.4f,%.4f,%.4f) path=(%.4f,%.4f,%.4f) "
          "err=%.5f naive=(%.4f,%.4f,%.4f) err=%.5f\n",
          expected.x, expected.y, expected.z, path_avg.x, path_avg.y,
          path_avg.z, err_path, naive_avg.x, naive_avg.y, naive_avg.z,
          err_naive);
      suite.require(err_path < 0.02f, "path red group does not match analytic");
      suite.require(
          err_naive < 0.02f, "naive red group does not match analytic");
    }

    // both groups: environments are filtered independently and summed
    {
      auto scene    = make_env_scene(group_red | group_blue);
      auto expected = albedo * (red + blue);
      auto path_img = render(scene, trace_sampler_type::path, 16, 4096);
      auto err      = mean_error(average_rgb(path_img), expected);
      std::printf("  red+blue   analytic=(%.4f,%.4f,%.4f) err=%.5f\n",
          expected.x, expected.y, expected.z, err);
      suite.require(err < 0.02f, "multi-environment sum does not match");
    }

    // no allowed group: the wall goes black; no light index is ever taken
    {
      auto scene    = make_env_scene(group_none);
      auto path_img = render(scene, trace_sampler_type::path, 16, 4096);
      auto avg      = average_rgb(path_img);
      std::printf("  empty-set  average=(%.4f,%.4f,%.4f)\n", avg.x, avg.y,
          avg.z);
      // center pixels fully cover the wall, so they must be black
      auto center = path_img[8 * 16 + 8];
      suite.require(center.x + center.y + center.z == 0.0f,
          "empty light set must not index a light and must render black");
    }
  });

  // -------------------------------------------------------------------------
  // Case 2: red/blue receiving groups with area lights, the highest bit
  // only, shared geometry occlusion, the coincident-lights pdf mixture,
  // and a PPM output.
  //
  // Two side-by-side matte walls share the same material but carry
  // different receive masks. Two emissive panels, one per group, light
  // them. Two extra scenes then check that an excluded emissive panel
  // stays in the BVH and blocks an allowed panel behind it, and that two
  // geometrically coincident lights both enter the direction-pdf mixture.
  // -------------------------------------------------------------------------
  suite.run("red_blue_groups_occlusion_and_ppm", [&] {
    auto scene                      = scene_data{};
    scene.cameras.push_back({});
    scene.cameras[0].frame          = lookat_frame({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    scene.cameras[0].aspect         = 2;

    // two receiver walls side by side, same material, different masks
    scene.shapes.push_back(make_rect({1, 1}, {0.95f, 0.95f}));
    scene.materials.push_back(make_matte({0.8f, 0.8f, 0.8f}));
    add_instance(scene, 0, 0, facing({-1.05f, 0, 0}, {0, 0, 1}), group_red);
    add_instance(scene, 0, 0, facing({1.05f, 0, 0}, {0, 0, 1}), group_blue);

    // emissive panels on the camera side, facing the receivers; per
    // instance emission masks assign each panel its group
    scene.shapes.push_back(make_rect({1, 1}, {0.9f, 0.9f}));
    scene.materials.push_back(make_emissive({4, 1, 1}));
    scene.materials.push_back(make_emissive({1, 1, 4}));
    add_instance(scene, 1, 1, facing({-1.05f, 0, 1.2f}, {0, 0, -1}),
        ~uint64_t{0}, group_red);
    add_instance(scene, 1, 2, facing({1.05f, 0, 1.2f}, {0, 0, -1}),
        ~uint64_t{0}, group_blue);

    auto img = render(scene, trace_sampler_type::path, 24, 2048, 4);
    // measure only the pixels whose camera ray reaches each receiver wall,
    // so the directly visible panels do not contaminate the averages
    auto left_avg  = average_on_instance(scene, img, 24, 0);
    auto right_avg = average_on_instance(scene, img, 24, 1);
    std::printf("  red group wall avg=(%.4f,%.4f,%.4f)\n", left_avg.x,
        left_avg.y, left_avg.z);
    std::printf("  blue group wall avg=(%.4f,%.4f,%.4f)\n", right_avg.x,
        right_avg.y, right_avg.z);
    suite.require(left_avg.x > left_avg.z * 2.0f,
        "red receive group must be lit by the red light only");
    suite.require(right_avg.z > right_avg.x * 2.0f,
        "blue receive group must be lit by the blue light only");

    // highest-bit-only group still works and is independent of low bits
    scene.instances[0].receive_mask  = group_high;
    scene.instances[2].emission_mask = group_high;  // red panel, high bit
    auto img_high                    = render(scene, trace_sampler_type::path,
            24, 1024, 4);
    auto high_avg                    = average_on_instance(scene, img_high, 24, 0);
    std::printf("  high-bit group wall avg=(%.4f,%.4f,%.4f)\n", high_avg.x,
        high_avg.y, high_avg.z);
    suite.require(high_avg.x > high_avg.z * 2.0f,
        "bit 63 group match must light the wall");

    // --- excluded light remains blocking geometry ----------------------
    // A wall that accepts only the blue group. A red panel, excluded for
    // the wall by its group, is placed just in front of the blue panel:
    // it must still occlude it, proving lights are never removed from
    // the BVH.
    auto block_scene = scene_data{};
    block_scene.cameras.push_back({});
    block_scene.cameras[0].frame =
        lookat_frame({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    block_scene.cameras[0].aspect = 1;
    block_scene.shapes.push_back(make_rect({1, 1}, {1, 1}));
    block_scene.materials.push_back(make_matte({0.8f, 0.8f, 0.8f}));
    add_instance(block_scene, 0, 0, identity3x4f, group_blue);
    block_scene.shapes.push_back(make_rect({1, 1}, {0.9f, 0.9f}));
    block_scene.materials.push_back(make_emissive({1, 1, 4}));  // blue
    // blue panel above the camera frustum, tilted toward the wall
    auto blue_pos = vec3f{0, 3.0f, 1.2f};
    add_instance(block_scene, 1, 1,
        facing(blue_pos, normalize(vec3f{0, 0, 0} - blue_pos)), ~uint64_t{0},
        group_blue);
    // reference: blue panel only, measured on the wall
    auto ref_img = render(block_scene, trace_sampler_type::path, 16, 2048, 4);
    auto ref_avg = average_on_instance(block_scene, ref_img, 16, 0);
    // probe ray from the wall center reaches the blue panel directly
    auto to_blue      = normalize(blue_pos - vec3f{0, 0, 0.001f});
    auto block_bvh    = make_trace_bvh(block_scene, trace_params{});
    auto probe_before = intersect_scene_bvh(
        block_bvh.bvh, block_scene, ray3f{{0, 0, 0.001f}, to_blue});
    suite.require(probe_before.hit && probe_before.instance == 1,
        "probe ray setup must hit the blue panel");
    // red panel just in front of the blue one along the same ray, excluded
    // for the wall by its group
    block_scene.materials.push_back(make_emissive({4, 1, 1}));
    auto red_pos    = blue_pos - to_blue * 0.2f;
    auto blocker_id = add_instance(block_scene, 1, 2,
        facing(red_pos, normalize(vec3f{0, 0, 0} - red_pos)), ~uint64_t{0},
        group_red);
    // the same probe ray now hits the excluded red panel as geometry
    block_bvh        = make_trace_bvh(block_scene, trace_params{});
    auto probe_after = intersect_scene_bvh(
        block_bvh.bvh, block_scene, ray3f{{0, 0, 0.001f}, to_blue});
    suite.require(probe_after.hit && probe_after.instance == blocker_id,
        "excluded emissive geometry must remain in the BVH and block rays");
    auto blocked_img = render(block_scene, trace_sampler_type::path, 16, 2048, 4);
    auto blocked_avg = average_on_instance(block_scene, blocked_img, 16, 0);
    std::printf("  blue-lit wall=(%.4f,%.4f,%.4f) blocked=(%.4f,%.4f,%.4f)\n",
        ref_avg.x, ref_avg.y, ref_avg.z, blocked_avg.x, blocked_avg.y,
        blocked_avg.z);
    suite.require(blocked_avg.z < ref_avg.z * 0.25f,
        "the allowed light must be shadowed by the excluded panel");
    suite.require(blocked_avg.x < 0.25f,
        "excluded red panel emission must not contribute");

    // --- coincident lights share the direction-pdf mixture -------------
    // Two panels at the same transform with identical emission, both
    // allowed. The mixture pdf must count both lights; otherwise the
    // light-sampling weight would double and the wall would render twice
    // as bright as the single-panel reference.
    auto mix_scene = scene_data{};
    mix_scene.cameras.push_back({});
    mix_scene.cameras[0].frame =
        lookat_frame({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    mix_scene.cameras[0].aspect = 1;
    mix_scene.shapes.push_back(make_rect({1, 1}, {1, 1}));
    mix_scene.materials.push_back(make_matte({0.8f, 0.8f, 0.8f}));
    add_instance(mix_scene, 0, 0, identity3x4f, group_red);
    mix_scene.shapes.push_back(make_rect({1, 1}, {0.9f, 0.9f}));
    mix_scene.materials.push_back(make_emissive({2, 2, 2}));
    // panels above the camera frustum so the wall stays fully visible
    auto light_pos = vec3f{0, 3.0f, 1.2f};
    auto light_frame =
        facing(light_pos, normalize(vec3f{0, 0, 0} - light_pos));
    add_instance(mix_scene, 1, 1, light_frame, ~uint64_t{0}, group_red);
    auto one_img = render(mix_scene, trace_sampler_type::path, 16, 2048, 4);
    auto one_avg = average_on_instance(mix_scene, one_img, 16, 0);
    add_instance(mix_scene, 1, 1, light_frame, ~uint64_t{0},
        group_red);  // second, coincident, same group
    auto two_img = render(mix_scene, trace_sampler_type::path, 16, 2048, 4);
    auto two_avg = average_on_instance(mix_scene, two_img, 16, 0);
    auto mix_err = mean_error(two_avg, one_avg);
    std::printf("  coincident lights one=(%.4f,%.4f,%.4f) two=(%.4f,%.4f,%.4f) "
                "err=%.5f\n",
        one_avg.x, one_avg.y, one_avg.z, two_avg.x, two_avg.y, two_avg.z,
        mix_err);
    suite.require(mix_err < 0.15f,
        "coincident lights must both enter the pdf mixture, not double it");

    // PPM written from a real trace_samples output
    const auto ppm_path = string{".build/light_link_groups.ppm"};
    save_ppm(ppm_path, img, 24, 24);
    std::printf("  wrote two-group lighting PPM: %s\n", ppm_path.c_str());
    auto check = std::ifstream{ppm_path, std::ios::binary};
    suite.require(check.good(), "ppm output must be written");
  });

  // -------------------------------------------------------------------------
  // Case 3: group ownership across two diffuse bounces.
  //
  // The camera sees only a small front wall (red group). A large back wall
  // sits behind the camera and is reached purely through a diffuse bounce;
  // it carries its own (blue) group. A light excluded from the front but
  // allowed for the back must still show up after the second bounce, while
  // a light allowed for the front but excluded from the back must never
  // illuminate it. Switching only the back mask must change the result.
  // -------------------------------------------------------------------------
  suite.run("two_bounce_group_ownership", [&] {
    auto scene              = scene_data{};
    scene.cameras.push_back({});
    scene.cameras[0].frame  = lookat_frame({0, 0, 2}, {0, 0, 0}, {0, 1, 0});
    scene.cameras[0].aspect = 1;

    // front wall: red group only
    scene.shapes.push_back(make_rect({1, 1}, {1, 1}));
    scene.materials.push_back(make_matte({0.7f, 0.7f, 0.7f}));
    add_instance(scene, 0, 0, identity3x4f, group_red);

    // back wall behind the camera, facing it, reached only after a bounce;
    // blue group only
    scene.shapes.push_back(make_rect({1, 1}, {4, 4}));
    scene.materials.push_back(make_matte({0.7f, 0.7f, 0.7f}));
    add_instance(scene, 1, 1, facing({0, 0, 2.6f}, {0, 0, -1}), group_blue);

    // red panel above, facing down onto the front wall
    scene.shapes.push_back(make_rect({1, 1}, {0.6f, 0.6f}));
    scene.materials.push_back(make_emissive({3, 0.2f, 0.2f}));
    add_instance(scene, 2, 2, facing({0, 1.8f, 1.2f}, {0, -1, 0}),
        ~uint64_t{0}, group_red);

    // blue panel below, facing up; geometrically visible to the back wall
    // but excluded from the front wall by its receive mask
    scene.shapes.push_back(make_rect({1, 1}, {0.6f, 0.6f}));
    scene.materials.push_back(make_emissive({0.2f, 0.2f, 3}));
    add_instance(scene, 3, 3, facing({0, -1.8f, 1.2f}, {0, 1, 0}),
        ~uint64_t{0}, group_blue);

    auto img    = render(scene, trace_sampler_type::path, 12, 4096, 5);
    auto avg    = average_rgb(img);
    std::printf("  two-bounce blue-back avg=(%.4f,%.4f,%.4f)\n", avg.x, avg.y,
        avg.z);
    suite.require(avg.x > avg.z,
        "front wall direct lighting follows its own red group");

    // switch only the back wall to the red group: the second bounce now
    // uses the red panel and loses the blue one
    scene.instances[1].receive_mask = group_red;
    auto img_red_back = render(scene, trace_sampler_type::path, 12, 4096, 5);
    auto avg_red_back = average_rgb(img_red_back);
    std::printf("  two-bounce red-back  avg=(%.4f,%.4f,%.4f)\n",
        avg_red_back.x, avg_red_back.y, avg_red_back.z);
    suite.require(avg_red_back.x > avg.x * 1.1f,
        "second bounce must be governed by the back wall group");
    suite.require(avg_red_back.z < avg.z,
        "blue light must stop contributing when the back group changes");
  });

  // -------------------------------------------------------------------------
  // Case 4: ideal mirror uses the mirror instance's group.
  //
  // The mirror's receive mask selects the environments seen through the
  // reflection. Changing only that mask changes the reflected colors while
  // geometry and materials stay identical. The naive sampler must apply
  // the same rule to the environment miss after the delta reflection.
  // -------------------------------------------------------------------------
  suite.run("mirror_uses_mirror_group", [&] {
    auto scene              = scene_data{};
    scene.cameras.push_back({});
    scene.cameras[0].frame  = lookat_frame({0, 0, 2}, {0, 0, 0}, {0, 1, 0});
    scene.cameras[0].aspect = 1;

    // mirror wall
    scene.shapes.push_back(make_rect({1, 1}, {1, 1}));
    scene.materials.push_back(make_mirror({0.95f, 0.95f, 0.95f}));
    add_instance(scene, 0, 0, identity3x4f, group_red);

    // two environments, one per group
    add_environment(scene, {0.5f, 0.05f, 0.05f}, group_red);
    add_environment(scene, {0.05f, 0.1f, 0.5f}, group_blue);

    auto img_red = render(scene, trace_sampler_type::path, 16, 2048, 3);
    auto avg_red = average_rgb(img_red);
    scene.instances[0].receive_mask = group_blue;
    auto img_blue = render(scene, trace_sampler_type::path, 16, 2048, 3);
    auto avg_blue = average_rgb(img_blue);
    std::printf("  mirror red-group  avg=(%.4f,%.4f,%.4f)\n", avg_red.x,
        avg_red.y, avg_red.z);
    std::printf("  mirror blue-group avg=(%.4f,%.4f,%.4f)\n", avg_blue.x,
        avg_blue.y, avg_blue.z);
    suite.require(avg_red.x > avg_red.z * 3.0f,
        "mirror reflection must use the mirror instance red group");
    suite.require(avg_blue.z > avg_blue.x * 3.0f,
        "mirror reflection must follow a changed mirror group");

    // naive sampler applies the same group rule to the miss after the
    // delta reflection
    scene.instances[0].receive_mask = group_red;
    auto img_naive  = render(scene, trace_sampler_type::naive, 16, 2048, 3);
    auto avg_naive  = average_rgb(img_naive);
    auto mirror_err = mean_error(avg_naive, avg_red);
    std::printf("  mirror naive avg=(%.4f,%.4f,%.4f) err vs path=%.5f\n",
        avg_naive.x, avg_naive.y, avg_naive.z, mirror_err);
    suite.require(avg_naive.x > avg_naive.z * 3.0f,
        "naive mirror must honor the same group");
  });
}
