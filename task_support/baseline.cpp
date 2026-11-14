#include "minipbrt.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace minipbrt;

static void identity(Transform& transform) {
  for (int i = 0; i != 4; ++i)
    for (int j = 0; j != 4; ++j)
      transform.start[i][j] = transform.end[i][j] = i == j ? 1.0f : 0.0f;
}

static LoopSubdiv* control_cage() {
  LoopSubdiv* cage = new LoopSubdiv;
  const float points[] = {0,0,0, 2,0,0, 0,3,0, 0,0,4};
  const int faces[] = {0,2,1, 0,1,3, 0,3,2, 1,2,3};
  cage->levels = 2;
  cage->num_points = 4;
  cage->num_indices = 12;
  cage->P = new float[12];
  cage->indices = new int[12];
  std::memcpy(cage->P, points, sizeof points);
  std::memcpy(cage->indices, faces, sizeof faces);
  identity(cage->shapeToWorld);
  cage->shapeToWorld.start[0][3] = 3;
  cage->shapeToWorld.end[1][3] = 7;
  cage->material = 7;
  cage->areaLight = 11;
  cage->insideMedium = 2;
  cage->outsideMedium = 3;
  cage->object = 5;
  cage->reverseOrientation = true;
  return cage;
}

int main() {
  Loader loader;
  if (!loader.load("task_support/scenes/loop.pbrt")) {
    const Error* error = loader.error();
    std::fprintf(stderr, "Loader failed: %s\n", error ? error->message() : "no diagnostic");
    return 1;
  }
  std::unique_ptr<Scene> loaded(loader.take_scene());
  assert(loaded && loaded->shapes.size() == 1);
  assert(loaded->objects.size() == 1 && loaded->instances.size() == 1);
  assert(loaded->shapes[0]->type() == ShapeType::LoopSubdiv);
  const LoopSubdiv* parsed = static_cast<const LoopSubdiv*>(loaded->shapes[0]);
  assert(parsed->levels == 2 && parsed->num_indices == 12 && parsed->P);
  assert(parsed->P[3] == 2 && parsed->P[7] == 3 && parsed->P[11] == 4);
  assert(parsed->object == 0 && loaded->objects[0]->numShapes == 1);
  assert(loaded->instances[0]->object == 0 && parsed->reverseOrientation);
  const unsigned parsed_count = parsed->num_points;
  // Observe the parser count; do not convert this cage while its count is wrong.
  std::unique_ptr<LoopSubdiv> cage(control_cage());
  float original[12];
  std::memcpy(original, cage->P, sizeof original);
  std::unique_ptr<TriangleMesh> mesh(cage->triangle_mesh());
  assert(mesh && mesh->num_indices && mesh->num_vertices && mesh->P && mesh->indices);
  assert(mesh->P != cage->P && mesh->indices != cage->indices);
  assert(!std::memcmp(original, cage->P, sizeof original));
  assert(mesh->material == 7 && mesh->areaLight == 11 && mesh->object == 5);
  assert(mesh->insideMedium == 2 && mesh->outsideMedium == 3 && mesh->reverseOrientation);
  assert(!std::memcmp(&mesh->shapeToWorld, &cage->shapeToWorld, sizeof(Transform)));
  for (unsigned i = 0; i < mesh->num_indices; ++i)
    assert(mesh->indices[i] >= 0 && unsigned(mesh->indices[i]) < mesh->num_vertices);
  std::printf("LOOP_OBSERVED levels=2 vertices=%u faces=%u normals=%d parsed_num_points=%u\n",
              mesh->num_vertices, mesh->num_indices / 3, mesh->N != nullptr, parsed_count);

  Scene scene;
  scene.shapes.push_back(control_cage());
  assert(scene.shapes_to_triangle_mesh(ShapeType::LoopSubdiv));
  assert(scene.shapes.size() == 1 && scene.shapes[0]->type() == ShapeType::TriangleMesh);
  assert(scene.shapes[0]->material == 7 && scene.shapes[0]->object == 5);

  Loader basic;
  assert(basic.load("task_support/scenes/basic.pbrt"));
  const Scene* basic_scene = basic.borrow_scene();
  assert(basic_scene && basic_scene->shapes.size() == 1);
  assert(basic_scene->shapes[0]->type() == ShapeType::TriangleMesh);
  const TriangleMesh* triangle = static_cast<const TriangleMesh*>(basic_scene->shapes[0]);
  assert(triangle->num_vertices == 3 && triangle->num_indices == 3);
  assert(triangle->P[3] == 1 && triangle->P[7] == 1);
  std::puts("BASELINE_OK loader=parsed conversion=owned scene=replaced triangle=3");
}
