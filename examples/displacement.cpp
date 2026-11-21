// Render one local displacement edit with the production rasterizer. Usage:
//   displacement <output-directory>
// Writes before.ppm and after.ppm. Both frames contain the displaced mesh and
// differ in the pixels covering the edited region.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <object/DisplacedMesh.hpp>
#include <render/Rasterizer.hpp>
#include <scene/Scene.hpp>

using namespace SoftRasterizer;

namespace {

std::shared_ptr<Material> diffuse(glm::vec3 kd) {
  auto m = std::make_shared<Material>();
  m->Kd = kd;
  return m;
}

// A gently curved grid whose corners are pinned and whose centre is displaced.
// The parameterisation is a two-face quad so shared-edge welding and per-face
// UVs are both exercised.
std::shared_ptr<DisplacedMesh> makeSurface(std::uint32_t n,
                                           std::shared_ptr<Material> material) {
  std::vector<Vertex> vertices{
      Vertex({-1.2f, -1.2f, 0}, {0, 0, 1}),
      Vertex({1.2f, -1.2f, 0}, {0, 0, 1}),
      Vertex({-1.2f, 1.2f, 0}, {0, 0, 1}),
      Vertex({1.2f, 1.2f, 0}, {0, 0, 1})};
  std::vector<glm::uvec3> faces{{0, 1, 2}, {1, 3, 2}};
  std::vector<std::array<glm::vec2, 3>> uvs{
      {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1)},
      {glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1)}};
  std::vector<std::shared_ptr<Material>> materials{2, material};
  const std::size_t points = std::size_t(n + 1) * (n + 2) / 2;
  std::vector<std::vector<float>> heights(2, std::vector<float>(points, 0.f));
  return std::make_shared<DisplacedMesh>(vertices, faces, uvs, materials,
                                         heights, n);
}

void render(const std::shared_ptr<Scene> &scene, std::size_t width,
            std::size_t height, const std::string &path) {
  TraditionalRasterizer rasterizer(width, height);
  rasterizer.addScene(scene);
  rasterizer.draw();
  rasterizer.save(path);
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: displacement <output-directory>\n";
      return 1;
    }
    const std::string directory = argv[1];
    std::filesystem::create_directories(directory);

    constexpr std::uint32_t n = 24;
    auto material = diffuse({0.62f, 0.36f, 0.18f});
    auto mesh = makeSurface(n, material);

    auto scene = std::make_shared<Scene>("displacement", glm::vec3(0, 0, 4.2f),
                                        glm::vec3(0, 0, 0), glm::vec3(0, 1, 0),
                                        glm::vec3(0.02f, 0.03f, 0.05f));
    scene->setProjectionMatrix(45.f, 0.05f, 100.f);
    scene->addGraphicObj(mesh, "surface");
    scene->addLight("key", std::make_shared<light_struct>(
                               glm::vec3(1.5f, 1.5f, 3.f), glm::vec3(18.f)));
    scene->prepare();

    const std::string beforePath = directory + "/before.ppm";
    render(scene, 320, 240, beforePath);

    // One local height edit near the centre of face 0. Everything else stays
    // untouched; Scene.prepare consumes only the affected closure.
    const std::uint32_t i = n / 4, j = n / 4;
    mesh->replaceHeights(std::vector<HeightSampleRef>{{0, i, j, 0.9f},
                                                      {0, i + 1, j, 0.55f}});
    scene->prepare();

    const std::string afterPath = directory + "/after.ppm";
    render(scene, 320, 240, afterPath);

    std::cout << "wrote " << beforePath << " and " << afterPath << "\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
