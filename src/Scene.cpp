#include <scene/Scene.hpp>

SoftRasterizer::Scene::Scene(std::string, const glm::vec3 &eye,
                             const glm::vec3 &center, const glm::vec3 &up,
                             const glm::vec3 &backgroundColor)
    : m_backgroundColor(backgroundColor) {
  if (!finite(backgroundColor) ||
      glm::any(glm::lessThan(backgroundColor, glm::vec3(0)))) {
    throw std::invalid_argument("invalid background");
  }
  m_camera.lookAt(eye, center, up);
}

bool SoftRasterizer::Scene::addGraphicObj(std::shared_ptr<Object> object,
                                          const std::string &objectName) {
  if (!object || m_loadedObjs.count(objectName) ||
      m_objLoaders.count(objectName)) {
    return false;
  }
  m_loadedObjs.emplace(objectName, ObjData{std::move(object), 0, 0});
  m_geometryDirty = true;
  return true;
}

bool SoftRasterizer::Scene::addGraphicObj(const std::string &path,
                                          const std::string &meshName,
                                          const glm::vec3 &axis, float angle,
                                          const glm::vec3 &translation,
                                          const glm::vec3 &scale) {
  if (m_loadedObjs.count(meshName) || m_objLoaders.count(meshName)) {
    return false;
  }
  m_objLoaders.emplace(
      meshName, std::make_unique<ObjLoader>(
                    path, composeTransform(axis, angle, translation, scale)));
  return true;
}

bool SoftRasterizer::Scene::startLoadingMesh(const std::string &meshName) {
  if (m_loadedObjs.count(meshName)) {
    return true;
  }
  auto loaderIter = m_objLoaders.find(meshName);
  if (loaderIter == m_objLoaders.end()) {
    return false;
  }
  auto mesh = loaderIter->second->load();
  m_loadedObjs.emplace(meshName, ObjData{std::move(mesh), 0, 0});
  m_geometryDirty = true;
  return true;
}

std::optional<std::shared_ptr<SoftRasterizer::Object>>
SoftRasterizer::Scene::getMeshObj(const std::string &meshName) const {
  auto meshIter = m_loadedObjs.find(meshName);
  if (meshIter == m_loadedObjs.end()) {
    return std::nullopt;
  }
  return meshIter->second.mesh;
}

bool SoftRasterizer::Scene::setModelMatrix(const std::string &meshName,
                                           const glm::vec3 &axis, float angle,
                                           const glm::vec3 &translation,
                                           const glm::vec3 &scale) {
  startLoadingMesh(meshName);
  auto mesh = getMeshObj(meshName);
  if (!mesh) {
    return false;
  }
  (*mesh)->updateModelMatrix(axis, angle, translation, scale);
  return true;
}

bool SoftRasterizer::Scene::addShader(const std::string &shaderName,
                                      const std::string &texturePath,
                                      SHADERS_TYPE type) {
  if (m_shaders.count(shaderName)) {
    return false;
  }
  auto shader = std::make_shared<Shader>();
  shader->setFragmentShader(type);
  if (!texturePath.empty()) {
    shader->texture = std::make_shared<TextureLoader>(texturePath);
  }
  m_shaders.emplace(shaderName, std::move(shader));
  return true;
}

bool SoftRasterizer::Scene::bindShader2Mesh(const std::string &meshName,
                                            const std::string &shaderName) {
  startLoadingMesh(meshName);
  auto mesh = getMeshObj(meshName);
  auto shaderIter = m_shaders.find(shaderName);
  if (!mesh || shaderIter == m_shaders.end()) {
    return false;
  }
  (*mesh)->bindShader2Mesh(shaderIter->second);
  return true;
}

void SoftRasterizer::Scene::addLight(const std::string &name,
                                     std::shared_ptr<light_struct> light) {
  if (!light || !finite(light->position) || !finite(light->intensity) ||
      glm::any(glm::lessThan(light->intensity, glm::vec3(0)))) {
    throw std::invalid_argument("invalid point light");
  }
  m_lights[name] = std::move(light);
}

std::uint64_t
SoftRasterizer::Scene::primitiveStableId(const Object *primitive) const {
  for (const auto &object : m_primitives) {
    if (object.get() == primitive) {
      return object->stableId();
    }
  }
  return 0;
}

/*Load pending meshes, consume object deltas, then update acceleration
 *structures. A full rebuild happens on structural changes; otherwise only the
 *locally affected leaves are refit and their raster entries replaced.*/
void SoftRasterizer::Scene::prepare() {
  for (const auto &[meshName, loader] : m_objLoaders) {
    startLoadingMesh(meshName);
  }
  m_objLoaders.clear();

  bool needsFullRebuild = m_geometryDirty || m_bvh.empty();
  std::vector<ObjectDelta> deltas;
  for (auto &[name, objData] : m_loadedObjs) {
    /*Material emission can be edited in place without bumping the revision,
     *so the light signature is checked for every object on every prepare.*/
    const std::uint64_t signature = objData.mesh->lightSignature();
    if (objData.lightSignature != signature) {
      m_areaTreeDirty = true;
      objData.lightSignature = signature;
    }
    if (objData.preparedRevision == objData.mesh->revision()) {
      continue;
    }
    ObjectDelta delta = objData.mesh->consumeDelta();
    objData.preparedRevision = objData.mesh->revision();
    if (!delta.canUpdateIncrementally || !delta.structurePreserved ||
        delta.dirtyPrimitives.size() != delta.dirtyRasterTriangles.size()) {
      needsFullRebuild = true;
      continue;
    }
    deltas.push_back(std::move(delta));
  }

  if (needsFullRebuild) {
    fullRebuild();
  } else if (!deltas.empty()) {
    applyIncremental(deltas);
  }

  if (m_geometryDirty || needsFullRebuild) {
    m_areaTreeDirty = true;
  }
  if (m_areaTreeDirty) {
    m_areaTree.rebuild(m_primitives);
    m_areaTreeDirty = false;
  }
}

void SoftRasterizer::Scene::fullRebuild() {
  m_primitives.clear();
  m_rasterTriangles.clear();
  for (auto &[name, objData] : m_loadedObjs) {
    objData.mesh->validateMaterial();
    objData.mesh->appendPrimitives(m_primitives);
    objData.mesh->appendRasterTriangles(m_rasterTriangles);
    objData.preparedRevision = objData.mesh->revision();
    objData.lightSignature = objData.mesh->lightSignature();
  }
  m_rasterIndex.clear();
  for (std::size_t i = 0; i < m_rasterTriangles.size(); ++i) {
    const auto *source = m_rasterTriangles[i].source;
    if (source) {
      m_rasterIndex.emplace(source, i);
    }
  }
  m_bvh.rebuild(m_primitives);
  m_geometryDirty = false;
  m_areaTreeDirty = true;
}

void SoftRasterizer::Scene::applyIncremental(
    const std::vector<ObjectDelta> &deltas) {
  std::vector<std::shared_ptr<Object>> dirtyPrimitives;
  for (const auto &delta : deltas) {
    m_stats.contributionReads += delta.counters.contributionReads;
    m_stats.vertexWrites += delta.counters.vertexWrites;
    m_stats.triangleUpdates += delta.counters.triangleUpdates;
    for (std::size_t i = 0; i < delta.dirtyPrimitives.size(); ++i) {
      const auto &primitive = delta.dirtyPrimitives[i];
      dirtyPrimitives.push_back(primitive);
      /*Replace the cached raster entry at the primitive's stable slot.*/
      auto indexIter = m_rasterIndex.find(primitive.get());
      if (indexIter != m_rasterIndex.end() &&
          indexIter->second < m_rasterTriangles.size()) {
        m_rasterTriangles[indexIter->second] = delta.dirtyRasterTriangles[i];
      } else {
        /*Fall back to appending if the primitive was not previously visible
         *in the raster stream.*/
        if (!delta.dirtyRasterTriangles[i].vertices.empty()) {
          m_rasterTriangles.push_back(delta.dirtyRasterTriangles[i]);
          m_rasterIndex[primitive.get()] = m_rasterTriangles.size() - 1;
        }
      }
    }
  }
  const auto counters = m_bvh.refit(dirtyPrimitives);
  m_stats.bvhLeafRefits += counters.leafRefits;
  m_stats.bvhNodeRefits += counters.nodeRefits;
  /*A material signature change can add or remove emissive primitives, which a
   *pure area update cannot express; defer that case to the full tree rebuild
   *below prepare().*/
  if (!m_areaTreeDirty) {
    const std::uint64_t areaUpdates = m_areaTree.updateAreas(dirtyPrimitives);
    m_stats.areaTreeUpdates += areaUpdates;
  }
}

SoftRasterizer::SurfaceSample
SoftRasterizer::Scene::sampleLight(Sampler &sampler) const {
  return m_areaTree.sample(sampler);
}

void SoftRasterizer::Scene::updateLightDistribution() {
  m_areaTree.rebuild(m_primitives);
  m_areaTreeDirty = false;
}

void SoftRasterizer::Scene::setViewMatrix(const glm::vec3 &eye,
                                          const glm::vec3 &center,
                                          const glm::vec3 &up) {
  m_camera.lookAt(eye, center, up);
}

void SoftRasterizer::Scene::setProjectionMatrix(const float fovy,
                                                const float zNear,
                                                const float zFar) {
  m_camera.projection(fovy, zNear, zFar);
}

/*Generating BVH Structure*/
void SoftRasterizer::Scene::buildBVHAccel() {
  m_geometryDirty = true;
  prepare();
}
