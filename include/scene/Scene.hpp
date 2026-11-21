#pragma once
#include <bvh/BVHAcceleration.hpp>
#include <light/AreaLightTree.hpp>
#include <light/Light.hpp>
#include <loader/ObjLoader.hpp>
#include <map>
#include <optional>
#include <scene/Camera.hpp>

namespace SoftRasterizer {
/*Cumulative scene counters, advanced where the work really happens.
 *Contribution reads / vertex writes / triangle updates come from incremental
 *object edits; BVH and area-tree counts come from Scene.prepare.*/
struct SceneStats {
  std::uint64_t contributionReads = 0;
  std::uint64_t vertexWrites = 0;
  std::uint64_t triangleUpdates = 0;
  std::uint64_t bvhLeafRefits = 0;
  std::uint64_t bvhNodeRefits = 0;
  std::uint64_t areaTreeUpdates = 0;

  void reset() {
    *this = SceneStats{};
  }
};

class Scene {
public:
  explicit Scene(std::string sceneName = "Scene",
                 const glm::vec3 &eye = {0, 0, 3},
                 const glm::vec3 &center = {0, 0, 0},
                 const glm::vec3 &up = {0, 1, 0},
                 const glm::vec3 &backgroundColor = {0, 0, 0});

  /*Set camera view and projection; geometry remains in world space.*/
  void setViewMatrix(const glm::vec3 &eye, const glm::vec3 &center,
                     const glm::vec3 &up);
  void setProjectionMatrix(const float fovy, const float zNear,
                           const float zFar);

  const Camera &getCamera() const {
    return m_camera;
  }

  glm::vec3 backgroundColor() const {
    return m_backgroundColor;
  }

  template <class T>
  bool addGraphicObj(std::unique_ptr<T> object, const std::string &name) {
    return addGraphicObj(std::shared_ptr<Object>(std::move(object)), name);
  }

  bool addGraphicObj(std::shared_ptr<Object> object, const std::string &name);
  bool addGraphicObj(const std::string &path, const std::string &name,
                     const glm::vec3 &axis = {0, 1, 0}, float degrees = 0,
                     const glm::vec3 &translation = {0, 0, 0},
                     const glm::vec3 &scale = {1, 1, 1});
  /*Load once and register the mesh; prepare() clears the loaders.*/
  bool startLoadingMesh(const std::string &name);
  std::optional<std::shared_ptr<Object>>
  getMeshObj(const std::string &name) const;
  bool setModelMatrix(const std::string &name, const glm::vec3 &axis,
                      float degrees, const glm::vec3 &translation,
                      const glm::vec3 &scale);
  bool addShader(const std::string &name, const std::string &texture,
                 SHADERS_TYPE type);
  bool bindShader2Mesh(const std::string &object, const std::string &shader);
  void addLight(const std::string &name, std::shared_ptr<light_struct> light);
  /*Consume pending object edits. Height edits are applied locally; anything
   *else falls back to a full geometry rebuild. Do not mutate during draw().*/
  void prepare();

  void buildBVHAccel();

  Intersection intersect(const Ray &ray) const {
    return m_bvh.getIntersection(ray);
  }

  bool occluded(const Ray &ray) const {
    return m_bvh.occluded(ray);
  }

  SurfaceSample sampleLight(Sampler &rng) const;

  const std::vector<RasterTriangle> &triangles() const {
    return m_rasterTriangles;
  }

  const auto &pointLights() const {
    return m_lights;
  }

  const SceneStats &stats() const {
    return m_stats;
  }
  void resetStats() {
    m_stats.reset();
  }

  /*Stable identity of a primitive currently in the acceleration structure;
   *returns 0 when the primitive is unknown.*/
  std::uint64_t primitiveStableId(const Object *primitive) const;

  /*Identity of the BVH leaf that owns a primitive, or null if absent. The
   *pointer is stable until the next full rebuild.*/
  const void *primitiveLeafIdentity(const Object *primitive) const {
    return m_bvh.leafIdentity(primitive);
  }

  /*Immutable snapshots of node/leaf identities, used by tests to verify that
   *a local edit does not relink or reorder the tree.*/
  std::vector<const void *> bvhNodeIdentities() const {
    return m_bvh.nodeIdentities();
  }
  std::vector<const void *> bvhLeafIdentities() const {
    return m_bvh.leafIdentities();
  }

private:
  struct ObjData {
    std::shared_ptr<Object> mesh;
    std::uint64_t preparedRevision = 0;
    std::uint64_t lightSignature = 0;
  };

  void fullRebuild();
  void applyIncremental(const std::vector<ObjectDelta> &deltas);
  void updateLightDistribution();
  bool rebuildAreaTreeIfNeeded();

  std::map<std::string, ObjData> m_loadedObjs;
  std::map<std::string, std::unique_ptr<ObjLoader>> m_objLoaders;
  std::map<std::string, std::shared_ptr<Shader>> m_shaders;
  std::map<std::string, std::shared_ptr<light_struct>> m_lights;
  /*World-space acceleration, cached raster stream and area-light tree.*/
  BVHAcceleration m_bvh;
  EmissiveAreaTree m_areaTree;
  std::vector<std::shared_ptr<Object>> m_primitives;
  std::vector<RasterTriangle> m_rasterTriangles;
  std::map<const Object *, std::size_t> m_rasterIndex;
  bool m_geometryDirty = true;
  bool m_areaTreeDirty = true;
  SceneStats m_stats;
  Camera m_camera;
  glm::vec3 m_backgroundColor{0};
};
} // namespace SoftRasterizer
