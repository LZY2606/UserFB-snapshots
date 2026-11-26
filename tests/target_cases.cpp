//
// Target cases for light linking (receive_mask / emission_mask).
//
// All scenes are procedurally generated. Rendering goes through the real
// trace_samples() API of the in-memory Yocto/GL CPU scene and is checked
// against analytic values where possible (not merely implementation vs
// implementation).
//

#include "test_api.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "libs/yocto/yocto_trace.h"

using namespace yocto;

namespace {

// receiver / emitter groups
constexpr auto GROUP_A   = uint64_t{1} << 0;
constexpr auto GROUP_B   = uint64_t{1} << 1;
constexpr auto GROUP_C   = uint64_t{1} << 2;
constexpr auto GROUP_TOP = uint64_t{1} << 63;
constexpr auto MASK_ALL  = ~uint64_t{0};

// ---------------------------------------------------------------------------
// procedural scene helpers
// ---------------------------------------------------------------------------

// A flat horizontal quad made of two triangles. Normal points +Y when
// facing_up, -Y otherwise.
shape_data horizontal_quad(float size, float y, bool facing_up) {
  auto s        = size;
  auto shape    = shape_data{};
  shape.positions = facing_up
                        ? vector<vec3f>{{-s, y, -s}, {-s, y, s}, {s, y, s},
                               {s, y, -s}}
                        : vector<vec3f>{{-s, y, -s}, {s, y, -s}, {s, y, s},
                               {-s, y, s}};
  shape.triangles = {{0, 1, 2}, {0, 2, 3}};
  return shape;
}

// A vertical quad in the plane z = z0, normal pointing +z.
shape_data vertical_quad(float width, float height, float z0) {
  auto shape       = shape_data{};
  shape.positions  = {{-width, 0, z0}, {width, 0, z0},
      {width, height, z0}, {-width, height, z0}};
  shape.triangles  = {{0, 1, 2}, {0, 2, 3}};
  return shape;
}

int add_matte(
    scene_data& scene, vec3f color, float roughness = 1.0f) {
  auto material            = material_data{};
  material.type            = material_type::matte;
  material.color           = color;
  material.roughness       = roughness;
  scene.materials.push_back(material);
  return (int)scene.materials.size() - 1;
}

int add_mirror(scene_data& scene, vec3f color = {1, 1, 1}) {
  auto material      = material_data{};
  material.type      = material_type::reflective;
  material.color     = color;
  material.roughness = 0;
  scene.materials.push_back(material);
  return (int)scene.materials.size() - 1;
}

int add_emission_material(scene_data& scene, vec3f emission) {
  auto material       = material_data{};
  material.type       = material_type::matte;
  material.emission   = emission;
  material.roughness  = 1;
  scene.materials.push_back(material);
  return (int)scene.materials.size() - 1;
}

int add_shape(scene_data& scene, shape_data shape) {
  scene.shapes.push_back(std::move(shape));
  return (int)scene.shapes.size() - 1;
}

int add_instance(scene_data& scene, int shape, int material,
    frame3f frame = identity3x4f, uint64_t receive_mask = MASK_ALL,
    uint64_t emission_mask = 1) {
  auto instance         = instance_data{};
  instance.shape        = shape;
  instance.material     = material;
  instance.frame        = frame;
  instance.receive_mask = receive_mask;
  instance.emission_mask = emission_mask;
  scene.instances.push_back(instance);
  return (int)scene.instances.size() - 1;
}

int add_environment(
    scene_data& scene, vec3f emission, uint64_t emission_mask = 1) {
  auto environment         = environment_data{};
  environment.emission     = emission;
  environment.emission_mask = emission_mask;
  scene.environments.push_back(environment);
  return (int)scene.environments.size() - 1;
}

void add_default_camera(scene_data& scene, vec3f eye = {0, 2.4f, 2.8f},
    vec3f center = {0, 0, 0}) {
  auto camera         = camera_data{};
  camera.frame        = lookat_frame(eye, center, {0, 1, 0});
  camera.aspect       = 1;
  scene.cameras.push_back(camera);
}

// ---------------------------------------------------------------------------
// rendering / measurement helpers
// ---------------------------------------------------------------------------

trace_state render(scene_data& scene, trace_sampler_type sampler, int resolution,
    int samples, int bounces = 3) {
  auto params             = trace_params{};
  params.resolution       = resolution;
  params.samples          = samples;
  params.bounces          = bounces;
  params.sampler          = sampler;
  params.noparallel       = true;
  params.clamp            = 1000;
  auto bvh                = make_trace_bvh(scene, params);
  auto lights             = make_trace_lights(scene, params);
  auto state              = make_trace_state(scene, params);
  for (auto n = 0; n < samples; n++)
    trace_samples(state, scene, bvh, lights, params);
  return state;
}

vec3f region_mean(const trace_state& state, int x0, int y0, int x1, int y1) {
  auto sum = vec3f{0, 0, 0};
  auto num = 0;
  for (auto j = y0; j < y1; j++) {
    for (auto i = x0; i < x1; i++) {
      auto& p = state.image[j * state.width + i];
      sum += vec3f{p.x, p.y, p.z};
      num++;
    }
  }
  return sum / max(1, num);
}

vec3f image_mean(const trace_state& state) {
  return region_mean(state, 0, 0, state.width, state.height);
}

// Binary PPM (P6) written from a real trace_samples() linear HDR image.
void write_ppm(const string& path, const trace_state& state, float gain) {
  auto* f = std::fopen(path.c_str(), "wb");
  if (!f) throw std::runtime_error("cannot write " + path);
  std::fprintf(f, "P6\n%d %d\n255\n", state.width, state.height);
  for (auto idx = 0; idx < state.width * state.height; idx++) {
    auto& p = state.image[idx];
    auto  bytes = array<unsigned char, 3>{};
    for (auto c = 0; c < 3; c++) {
      auto v = p[c] * gain;
      v      = clamp(v, 0.0f, 1.0f);
      bytes[c] = (unsigned char)round(v * 255);
    }
    std::fwrite(bytes.data(), 1, 3, f);
  }
  std::fclose(f);
  std::printf("PPM %s (%dx%d, gain=%.2f)\n", path.c_str(), state.width,
      state.height, gain);
}

void require_close(Suite& suite, vec3f got, vec3f expected, float tol,
    const char* what) {
  auto err = abs(got - expected);
  std::printf("  %-28s got=(%.4f %.4f %.4f) want=(%.4f %.4f %.4f) "
              "err=(%.4f %.4f %.4f)\n",
      what, got.x, got.y, got.z, expected.x, expected.y, expected.z, err.x,
      err.y, err.z);
  suite.require(max(err) <= tol, what);
}

}  // namespace

// ---------------------------------------------------------------------------
// target cases
// ---------------------------------------------------------------------------

void target_cases(Suite& suite) {
  // -------------------------------------------------------------------------
  // 1. red/blue receiver groups against the analytic constant-environment
  //    value (albedo * sum of allowed environments), for path and naive.
  //    A default-mask floor must receive the sum of every environment, and
  //    a small PPM of the two-group lighting is emitted from trace_samples.
  // -------------------------------------------------------------------------
  suite.run("analytic_env_groups", [&] {
    auto red  = vec3f{0.6f, 0, 0};
    auto blue = vec3f{0, 0.4f, 0};

    // one centered floor per scene: the mean is free of neighbor geometry
    auto floor_scene = [&](uint64_t receive_mask) {
      auto scene = scene_data{};
      add_default_camera(scene);
      auto floor_shape = add_shape(scene, horizontal_quad(2, 0, true));
      auto matte       = add_matte(scene, {0.5f, 0.5f, 0.5f}, 1);
      add_instance(scene, floor_shape, matte, identity3x4f, receive_mask);
      add_environment(scene, red, GROUP_A);
      add_environment(scene, blue, GROUP_B);
      return scene;
    };

    struct check {
      uint64_t mask;
      vec3f    expected;
      const char* name;
    };
    auto checks = vector<check>{
        {GROUP_A, 0.5f * red, "red group floor"},
        {GROUP_B, 0.5f * blue, "blue group floor"},
        {MASK_ALL, 0.5f * (red + blue), "default mask floor gets all envs"},
    };

    auto res = 24, spp = 256;
    for (auto sampler : {trace_sampler_type::path, trace_sampler_type::naive}) {
      auto tag = sampler == trace_sampler_type::path ? "path" : "naive";
      std::printf("[%s] mean errors vs analytic albedo*sum(env):\n", tag);
      for (auto& c : checks) {
        auto scene = floor_scene(c.mask);
        auto state = render(scene, sampler, res, spp, 2);
        require_close(suite, region_mean(state, 7, 7, 17, 17), c.expected,
            0.025f, (string(tag) + ": " + c.name).c_str());
      }
    }

    // two adjacent floors with distinct groups, rendered through the real
    // trace_samples API and written as a small PPM showing the two groups
    {
      auto scene       = scene_data{};
      add_default_camera(scene);
      auto floor_shape = add_shape(scene, horizontal_quad(1, 0, true));
      auto matte       = add_matte(scene, {0.5f, 0.5f, 0.5f}, 1);
      auto frame_a     = identity3x4f;
      frame_a.o        = {-1.05f, 0, 0};
      auto frame_b     = identity3x4f;
      frame_b.o        = {1.05f, 0, 0};
      add_instance(scene, floor_shape, matte, frame_a, GROUP_A);
      add_instance(scene, floor_shape, matte, frame_b, GROUP_B);
      add_environment(scene, red, GROUP_A);
      add_environment(scene, blue, GROUP_B);
      auto state = render(scene, trace_sampler_type::path, 32, 256, 2);
      write_ppm("tests/_renderings/lightlink_groups.ppm", state, 2.2f);
    }
  });

  // -------------------------------------------------------------------------
  // 2. highest-bit-only matching, empty eligible set, and exemption of
  //    directly viewed emitters / environments.
  // -------------------------------------------------------------------------
  suite.run("topbit_empty_and_direct_view", [&] {
    // 2a. only the top bit matches
    {
      auto scene = scene_data{};
      add_default_camera(scene);
      auto floor = add_shape(scene, horizontal_quad(2, 0, true));
      auto matte = add_matte(scene, {0.5f, 0.5f, 0.5f}, 1);
      add_instance(scene, floor, matte, identity3x4f, GROUP_TOP);
      add_environment(scene, {0.5f, 0.5f, 0.5f}, GROUP_TOP);
      add_environment(scene, {0.3f, 0, 0}, GROUP_A);  // excluded
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto state = render(scene, sampler, 24, 256, 2);
        auto mean  = region_mean(state, 6, 6, 18, 18);
        require_close(suite, mean, vec3f{0.25f, 0.25f, 0.25f}, 0.03f,
            sampler == trace_sampler_type::path
                ? "path: top-bit-only env lights floor"
                : "naive: top-bit-only env lights floor");
      }
    }

    // 2b. no eligible light: must not index an empty set and must stay dark
    {
      auto scene = scene_data{};
      add_default_camera(scene);
      auto floor = add_shape(scene, horizontal_quad(2, 0, true));
      auto matte = add_matte(scene, {0.5f, 0.5f, 0.5f}, 1);
      add_instance(scene, floor, matte, identity3x4f, GROUP_C);
      add_environment(scene, {0.6f, 0, 0}, GROUP_A);
      add_environment(scene, {0, 0.4f, 0}, GROUP_B);
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto state = render(scene, sampler, 24, 128, 2);
        auto mean  = region_mean(state, 6, 6, 18, 18);
        require_close(suite, mean, vec3f{0, 0, 0}, 0.02f,
            sampler == trace_sampler_type::path
                ? "path: empty light set stays dark"
                : "naive: empty light set stays dark");
      }
    }

    // 2c. directly seen emissive geometry ignores the masks
    {
      auto scene = scene_data{};
      auto cam          = camera_data{};
      cam.frame         = lookat_frame({0, 1, 2.5f}, {0, 1, 0}, {0, 1, 0});
      cam.aspect        = 1;
      scene.cameras.push_back(cam);
      auto quad = add_shape(scene, vertical_quad(4, 4, 0));
      auto emit = add_emission_material(scene, {0.7f, 0.13f, 0.29f});
      add_instance(scene, quad, emit, identity3x4f, MASK_ALL, GROUP_A);
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto state = render(scene, sampler, 16, 64, 1);
        auto mean  = image_mean(state);
        require_close(suite, mean, vec3f{0.7f, 0.13f, 0.29f}, 0.02f,
            sampler == trace_sampler_type::path
                ? "path: directly seen emitter unfiltered"
                : "naive: directly seen emitter unfiltered");
      }
    }

    // 2d. directly seen environment (camera miss) ignores the masks
    {
      auto scene = scene_data{};
      add_default_camera(scene);
      add_environment(scene, {0.2f, 0.25f, 0.3f}, GROUP_B);
      auto state = render(scene, trace_sampler_type::path, 16, 32, 1);
      require_close(suite, image_mean(state), vec3f{0.2f, 0.25f, 0.3f}, 0.01f,
          "directly seen environment unfiltered");
    }
  });

  // -------------------------------------------------------------------------
  // 3. an excluded emitter stays visible geometry and still occludes the
  //    included emitter behind it.
  // -------------------------------------------------------------------------
  suite.run("excluded_emitter_occludes", [&] {
    // Floor at y=0 seen from +z. An overhead blocker quad on the camera side
    // of the floor (y=1.5, z=1) covers part of the floor's upper hemisphere
    // but lies above the camera rays, so the camera never sees it directly.
    // The included emitter is a group-A constant environment.
    auto make_scene = [&](uint64_t floor_mask) {
      auto scene  = scene_data{};
      add_default_camera(scene, {0, 1.2f, 2.5f}, {0, 0, 0});
      auto floor  = add_shape(scene, horizontal_quad(1.5f, 0, true));
      auto matte  = add_matte(scene, {0.5f, 0.5f, 0.5f}, 1);
      add_instance(scene, floor, matte, identity3x4f, floor_mask);
      // excluded emissive quad: red, group B, still ordinary BVH geometry
      auto blocker_frame = identity3x4f;
      blocker_frame.o    = {0, 1.5f, 1};
      auto blocker_shape = add_shape(scene, horizontal_quad(2, 0, false));
      auto blocker_mat   = add_emission_material(scene, {6, 0, 0});
      add_instance(scene, blocker_shape, blocker_mat, blocker_frame,
          MASK_ALL, GROUP_B);
      // included environment
      add_environment(scene, {0.5f, 0.5f, 0.5f}, GROUP_A);
      return scene;
    };

    // group A floor: the blocker is excluded; the environment behind it is
    // physically hidden by the quad, so the floor must be darker than the
    // analytic unblocked value (albedo * env = 0.25) and must not pick up
    // the blocker's red emission.
    {
      auto scene_a = make_scene(GROUP_A);
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto state = render(scene_a, sampler, 24, 384, 2);
        auto mean  = region_mean(state, 8, 8, 16, 16);
        std::printf("  occluded group-A floor (%s) mean=(%.4f %.4f %.4f)\n",
            sampler == trace_sampler_type::path ? "path" : "naive", mean.x,
            mean.y, mean.z);
        suite.require(max(mean) < 0.225f,
            sampler == trace_sampler_type::path
                ? "path: excluded emitter still occludes included env"
                : "naive: excluded emitter still occludes included env");
        suite.require(mean.x - mean.y < 0.01f,
            sampler == trace_sampler_type::path
                ? "path: excluded red emitter does not leak"
                : "naive: excluded red emitter does not leak");
      }
    }

    // group B floor: the same quad is now an included red emitter and the
    // environment is excluded, proving the quad is ordinary visible
    // geometry that can illuminate its own receiver group.
    {
      auto scene_b = make_scene(GROUP_B);
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto state = render(scene_b, sampler, 24, 384, 2);
        auto mean  = region_mean(state, 8, 8, 16, 16);
        std::printf("  lit group-B floor (%s) mean=(%.4f %.4f %.4f)\n",
            sampler == trace_sampler_type::path ? "path" : "naive", mean.x,
            mean.y, mean.z);
        suite.require(mean.x > 0.3f && mean.y < 0.03f,
            sampler == trace_sampler_type::path
                ? "path: blocker lights its own receiver group"
                : "naive: blocker lights its own receiver group");
      }
    }
  });

  // -------------------------------------------------------------------------
  // 4. two diffuse bounces: the second surface's receive group decides the
  //    environment seen after the bounce; the first floor does not inherit
  //    its own group along the path.
  // -------------------------------------------------------------------------
  suite.run("two_bounce_group_ownership", [&] {
    auto make_scene = [&](uint64_t ceiling_mask) {
      auto scene   = scene_data{};
      add_default_camera(scene, {0, 1.8f, 1.6f}, {0, 0, 0});
      auto floor_s = add_shape(scene, horizontal_quad(1.5f, 0, true));
      auto ceil_s  = add_shape(scene, horizontal_quad(2.5f, 2, false));
      auto floor_m = add_matte(scene, {0.5f, 0.5f, 0.5f}, 1);
      auto ceil_m  = add_matte(scene, {0.8f, 0.8f, 0.8f}, 1);
      add_instance(scene, floor_s, floor_m, identity3x4f, GROUP_A);
      add_instance(scene, ceil_s, ceil_m, identity3x4f, ceiling_mask);
      add_environment(scene, {0.6f, 0, 0}, GROUP_A);
      add_environment(scene, {0, 0.4f, 0}, GROUP_B);
      return scene;
    };

    auto scene_b = make_scene(GROUP_B);  // ceiling receives blue only
    auto scene_a = make_scene(GROUP_A);  // control: ceiling receives red
    for (auto sampler :
        {trace_sampler_type::path, trace_sampler_type::naive}) {
      auto st_b = render(scene_b, sampler, 24, 384, 3);
      auto st_a = render(scene_a, sampler, 24, 384, 3);
      auto m_b  = region_mean(st_b, 8, 8, 16, 16);
      auto m_a  = region_mean(st_a, 8, 8, 16, 16);
      auto tag  = sampler == trace_sampler_type::path ? "path" : "naive";
      std::printf("  [%s] floor with blue ceiling=(%.4f %.4f %.4f) "
                  "red ceiling=(%.4f %.4f %.4f)\n",
          tag, m_b.x, m_b.y, m_b.z, m_a.x, m_a.y, m_a.z);
      suite.require(m_b.y > m_a.y + 0.02f,
          (string(tag) + ": indirect follows ceiling group (green)").c_str());
      suite.require(m_a.x > m_b.x + 0.02f,
          (string(tag) + ": control ceiling keeps red indirect").c_str());
    }
  });

  // -------------------------------------------------------------------------
  // 5. ideal specular: the mirror instance's receive group selects the
  //    reflected radiance, for both path and naive.
  // -------------------------------------------------------------------------
  suite.run("mirror_group", [&] {
    // 5a. mirror reflects only the matching constant environment
    {
      auto scene = scene_data{};
      auto cam   = camera_data{};
      cam.frame  = lookat_frame({0, 1, 2.5f}, {0, 1, 0}, {0, 1, 0});
      cam.aspect = 1;
      scene.cameras.push_back(cam);
      auto mirror_s = add_shape(scene, vertical_quad(2, 2, 0));
      auto mirror_m = add_mirror(scene, {1, 1, 1});
      add_instance(scene, mirror_s, mirror_m, identity3x4f, GROUP_B);
      add_environment(scene, {0.6f, 0, 0}, GROUP_A);
      add_environment(scene, {0, 0.4f, 0}, GROUP_B);
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto state = render(scene, sampler, 24, 128, 2);
        auto mean  = region_mean(state, 6, 6, 18, 18);
        auto tag   = sampler == trace_sampler_type::path ? "path" : "naive";
        std::printf("  [%s] mirror mean=(%.4f %.4f %.4f)\n", tag, mean.x,
            mean.y, mean.z);
        suite.require(mean.y > 0.15f && mean.x < 0.08f,
            (string(tag) + ": mirror reflects only blue group").c_str());
      }
    }

    // 5b. an excluded emissive quad seen in the mirror contributes nothing;
    //     with a matching mask the same quad lights the mirror.
    {
      auto make_scene = [&](uint64_t mirror_mask, uint64_t light_mask) {
        auto scene    = scene_data{};
        auto cam      = camera_data{};
        cam.frame     = lookat_frame({0, 1, 2.5f}, {0, 1, 0}, {0, 1, 0});
        cam.aspect    = 1;
        scene.cameras.push_back(cam);
        auto mirror_s = add_shape(scene, vertical_quad(2, 2, 0));
        auto mirror_m = add_mirror(scene, {1, 1, 1});
        add_instance(
            scene, mirror_s, mirror_m, identity3x4f, mirror_mask);
        auto quad_s = add_shape(scene, vertical_quad(2, 2, 4));
        auto emit_m = add_emission_material(scene, {4, 4, 4});
        add_instance(
            scene, quad_s, emit_m, identity3x4f, MASK_ALL, light_mask);
        return scene;
      };
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto scene_excluded = make_scene(GROUP_B, GROUP_A);
        auto scene_included = make_scene(GROUP_A, GROUP_A);
        auto excluded = render(
            scene_excluded, sampler, 24, 128, 2);
        auto included = render(
            scene_included, sampler, 24, 128, 2);
        auto m_e = region_mean(excluded, 6, 6, 18, 18);
        auto m_i = region_mean(included, 6, 6, 18, 18);
        auto tag = sampler == trace_sampler_type::path ? "path" : "naive";
        std::printf("  [%s] mirror excluded=(%.4f) included=(%.4f)\n", tag,
            max(m_e), max(m_i));
        suite.require(max(m_e) < 0.05f,
            (string(tag) + ": mirror hides excluded emitter").c_str());
        suite.require(max(m_i) > 1.0f,
            (string(tag) + ": mirror shows included emitter").c_str());
      }
    }
  });

  // -------------------------------------------------------------------------
  // 6. multiple environments filtered separately, plus geometrically
  //    coincident lights whose mixture pdf must count every eligible light.
  // -------------------------------------------------------------------------
  suite.run("multi_env_and_coincident_lights", [&] {
    // 6a. two matching constant environments: analytic albedo * (e1 + e2)
    {
      auto scene = scene_data{};
      add_default_camera(scene);
      auto floor = add_shape(scene, horizontal_quad(2, 0, true));
      auto matte = add_matte(scene, {0.5f, 0.5f, 0.5f}, 1);
      add_instance(scene, floor, matte, identity3x4f, GROUP_A);
      auto e1 = vec3f{0.2f, 0, 0};
      auto e2 = vec3f{0, 0.3f, 0};
      add_environment(scene, e1, GROUP_A);
      add_environment(scene, e2, GROUP_A);
      add_environment(scene, {0.9f, 0.9f, 0}, GROUP_B);  // excluded
      for (auto sampler :
          {trace_sampler_type::path, trace_sampler_type::naive}) {
        auto state = render(scene, sampler, 24, 256, 2);
        require_close(suite, region_mean(state, 6, 6, 18, 18),
            0.5f * (e1 + e2), 0.03f,
            sampler == trace_sampler_type::path
                ? "path: sum of two matching envs"
                : "naive: sum of two matching envs");
      }
    }

    // 6b. coincident emissive quads: the light mixture must count both,
    //     checked against the naive ground truth. A separate, slightly
    //     closer excluded quad occludes the included light behind it.
    {
      auto make_coincident = [&] {
        auto scene   = scene_data{};
        add_default_camera(scene, {0, 1.8f, 1.6f}, {0, 0.8f, 0});
        auto floor_s = add_shape(scene, horizontal_quad(1.5f, 0, true));
        auto floor_m = add_matte(scene, {0.8f, 0.8f, 0.8f}, 1);
        add_instance(scene, floor_s, floor_m, identity3x4f, GROUP_A);
        auto light_s = add_shape(scene, horizontal_quad(1.5f, 2, false));
        auto emit    = add_emission_material(scene, {8, 8, 8});
        // same frame and shape: geometrically coincident second light
        add_instance(
            scene, light_s, emit, identity3x4f, MASK_ALL, GROUP_A);
        add_instance(
            scene, light_s, emit, identity3x4f, MASK_ALL, GROUP_A);
        return scene;
      };

      // both lights eligible: two lights must be present and the path
      // estimator must stay unbiased relative to the naive result
      {
        auto scene  = make_coincident();
        auto lights = make_trace_lights(scene, trace_params{});
        suite.require(lights.lights.size() == 2,
            "coincident emitters create two lights");
        auto st_path  = render(scene, trace_sampler_type::path, 24, 512, 2);
        auto scene_n  = make_coincident();
        auto st_naive = render(
            scene_n, trace_sampler_type::naive, 24, 256, 2);
        auto mp = region_mean(st_path, 8, 8, 16, 16);
        auto mn = region_mean(st_naive, 8, 8, 16, 16);
        auto ratio = max(mn) > 0 ? max(mp) / max(mn) : 0;
        std::printf("  coincident lights path=(%.4f) naive=(%.4f) ratio=%.3f\n",
            max(mp), max(mn), ratio);
        suite.require(ratio > 0.75f && ratio < 1.25f,
            "path mixture pdf counts both coincident lights");
      }

      // excluded front light (zero-bounce color) still blocks the included
      // back light: separated slightly so hit ordering is deterministic
      {
        auto scene   = scene_data{};
        add_default_camera(scene, {0, 1.8f, 1.6f}, {0, 0.8f, 0});
        auto floor_s = add_shape(scene, horizontal_quad(1.5f, 0, true));
        auto floor_m = add_matte(scene, {0.8f, 0.8f, 0.8f}, 1);
        add_instance(scene, floor_s, floor_m, identity3x4f, GROUP_B);
        auto light_s = add_shape(scene, horizontal_quad(1.5f, 0, false));
        auto emit    = add_emission_material(scene, {8, 8, 8});
        auto front_f = identity3x4f;
        front_f.o    = {0, 1.9f, 0};
        auto back_f  = identity3x4f;
        back_f.o     = {0, 2.1f, 0};
        add_instance(scene, light_s, emit, front_f, MASK_ALL, GROUP_A);
        add_instance(scene, light_s, emit, back_f, MASK_ALL, GROUP_B);
        for (auto sampler :
            {trace_sampler_type::path, trace_sampler_type::naive}) {
          auto state = render(scene, sampler, 24, 192, 2);
          auto mean  = region_mean(state, 8, 8, 16, 16);
          suite.require(max(mean) < 0.05f,
              sampler == trace_sampler_type::path
                  ? "path: excluded front light occludes included back"
                  : "naive: excluded front light occludes included back");
        }
      }
    }
  });
}
