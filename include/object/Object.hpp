#pragma once
#include <array>
#include <bvh/Bounds3.hpp>
#include <ray/Intersection.hpp>
#include <shader/Shader.hpp>
#include <vector>

namespace SoftRasterizer {
struct Vertex {
  glm::vec3 position{0}, normal{0};
  glm::vec2 texCoord{0};
  glm::vec3 color{1};
  Vertex() = default;
  Vertex(const glm::vec3 &position, const glm::vec3 &normal = glm::vec3(0.f),
         const glm::vec2 &texCoord = glm::vec2(0.f));
};

struct RasterTriangle {
  std::array<Vertex, 3> vertices;
  std::shared_ptr<Material> material;
  std::shared_ptr<Shader> shader;
  /*Object that produced this entry. Scene.prepare uses it to replace one
   *cached raster entry after an incremental edit without rebuilding the whole
   *stream. Null for entries built outside an object.*/
  const Object *source = nullptr;
};

/*Work charged for one incremental edit. Counters advance only where the
 *corresponding operation really happens; construction and model-matrix
 *updates are bulk operations and are not charged.*/
struct EditCounters {
  std::uint64_t contributionReads = 0;
  std::uint64_t vertexWrites = 0;
  std::uint64_t triangleUpdates = 0;

  void reset() {
    *this = EditCounters{};
  }
};

/*Incremental mutation collected by Scene::prepare. Objects that cannot
 *describe a local edit leave canUpdateIncrementally false, which selects the
 *full rebuild path.*/
struct ObjectDelta {
  bool canUpdateIncrementally = false;
  /*The object's primitive set, ordering and addresses are unchanged; only the
   *listed leaves moved. False forces a full scene primitive rebuild.*/
  bool structurePreserved = false;
  /*Leaves whose world-space geometry changed, in stable address order.*/
  std::vector<std::shared_ptr<Object>> dirtyPrimitives;
  /*Raster entries aligned one-to-one with dirtyPrimitives.*/
  std::vector<RasterTriangle> dirtyRasterTriangles;
  EditCounters counters;
};

class Object : public std::enable_shared_from_this<Object> {
public:
  virtual ~Object() = default;
  void updateModelMatrix(const glm::vec3 &axis, float degrees,
                         const glm::vec3 &translation, const glm::vec3 &scale);
  void setModelMatrix(const glm::mat4 &modelMatrix);

  const glm::mat4 &getModelMatrix() const {
    return m_model;
  }

  std::uint64_t revision() const {
    return m_revision;
  }

  virtual void setMaterial(std::shared_ptr<Material> material);

  const std::shared_ptr<Material> &getMaterial() const {
    return m_material;
  }

  virtual void bindShader2Mesh(std::shared_ptr<Shader> shader);
  virtual Bounds3 getBounds() const = 0;
  virtual Intersection getIntersect(const Ray &ray) const = 0;
  virtual float getArea() const = 0;
  virtual SurfaceSample sample(Sampler &rng) const = 0;
  virtual void
  appendRasterTriangles(std::vector<RasterTriangle> &out) const = 0;

  virtual void appendPrimitives(std::vector<std::shared_ptr<Object>> &out) {
    out.push_back(shared_from_this());
  }

  /*Collect and clear mutations accumulated since the previous call. Objects
   *without local edits leave canUpdateIncrementally false.*/
  virtual ObjectDelta consumeDelta() {
    return {};
  }

  /*Object that groups this primitive in the two-level area-light tree. Micro
   *triangles report their owning mesh; ordinary objects group as
   *themselves.*/
  virtual const Object *samplingOwner() const {
    return this;
  }

  /*Stable identity for primitives that refit in place (0 otherwise).*/
  virtual std::uint64_t stableId() const {
    return 0;
  }

  /*Reject an invalid material before it reaches geometry. The default
   *performs the same checks Scene.prepare already applies to every
   *emissive-capable object.*/
  virtual void validateMaterial() const;

  /*Changes whenever the material fields Scene uses for light selection
   *change. Objects that can recompute their own area return a per-object
   *signature; the default hashes emission and albedo.*/
  virtual std::uint64_t lightSignature() const;

protected:
  virtual void transform() = 0;

  /*Model-space source data is preserved by derived objects.*/
  glm::mat4 m_model{1};
  std::shared_ptr<Material> m_material = std::make_shared<Material>();
  std::shared_ptr<Shader> m_shader;
  std::uint64_t m_revision = 1;
};

/*Validate the fields Scene.prepare requires from every object material.*/
void validateMaterialFields(const Material &material);
} // namespace SoftRasterizer
