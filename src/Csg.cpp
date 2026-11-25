#include "Csg.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "JetConfig.hpp"

namespace Renderer {

const char* csgErrorString(CsgError error) {
    switch (error) {
        case CsgError::Ok: return "ok";
        case CsgError::UnsupportedBuild: return "CSG requires full-resolution z-buffer perspective depth";
        case CsgError::EmptyExpression: return "CSG expression has no nodes";
        case CsgError::RootOutOfRange: return "CSG root index is out of range";
        case CsgError::InvalidNode: return "CSG node has an invalid operation";
        case CsgError::ChildOutOfRange: return "CSG binary node refers outside the node vector";
        case CsgError::Cycle: return "CSG expression contains a cycle";
        case CsgError::ExpressionLeafUnbound: return "CSG expression leaf has no valid binding";
        case CsgError::NullObject: return "CSG binding has a null Object";
        case CsgError::InstanceOutOfRange: return "CSG instance index is out of range";
        case CsgError::MissingSharedMesh: return "CSG instance has no shared mesh";
        case CsgError::EmptyMesh: return "CSG mesh has no triangles";
        case CsgError::DuplicateLeafBinding: return "same CSG leaf number is bound to different placements";
        case CsgError::DuplicatePlacement: return "one placement cannot be bound to two different CSG leaves";
        case CsgError::UnsupportedMaterial: return "CSG leaf uses an unsupported shader/material feature";
        case CsgError::UnsupportedObjectFeature: return "CSG leaf uses billboard, LOD, zBias, fade or special depth flags";
        case CsgError::SceneLodEnabled: return "Scene LOD is enabled while a CSG expression is active";
    }
    return "unknown CSG error";
}

namespace {

CsgStatus fail(CsgError code) {
    return CsgStatus{code, csgErrorString(code)};
}

bool materialSupported(const Material* material) {
    if (!material) return false;
    if (material->shader != nullptr) return false;
    if (material->alpha != 255) return false;
    switch (material->shadingMode) {
        case ShadingMode::UNLIT:
        case ShadingMode::FLAT:
        case ShadingMode::PHONG:
            break;
        default:
            return false;
    }
    if (const Texture* texture = material->diffuseMap) {
        // Ordinary UV textures are supported. Colour-key holes, screen-space
        // mapping, reflection mapping and tiled span caches are outside the
        // promised first scope.
        if (texture->hasAlpha || texture->screenSpace || texture->reflectionMap)
            return false;
        if (texture->tiled != nullptr)
            return false;
    }
    return true;
}

CsgStatus validateBindingAppearance(const CsgBinding& binding) {
    const Object* object = binding.object;
    if (object->isBillboard || object->zBias != 0 ||
        object->ignoreZBuffer || object->noWriteZBuffer ||
        object->fadeNear != 0 || object->fadeFar != 0 ||
        object->appearNear != 0 || object->appearFar != 0 ||
        !object->lodMeshes.empty() ||
        object->blendMode != BlendMode::BLEND_REPLACE) {
        return fail(CsgError::UnsupportedObjectFeature);
    }

    const Object* mesh = object;
    Material* uniformOverride = nullptr;
#if JET_MESH_INSTANCING
    Object::TriangleMaterials triangleMaterials;
    if (binding.instanceIndex >= 0) {
        const auto& instance = object->instances[static_cast<size_t>(binding.instanceIndex)];
        mesh = instance.mesh.get();
        uniformOverride = instance.materialOverride;
        triangleMaterials = instance.triangleMaterials;
    }
#else
    if (binding.instanceIndex >= 0) return fail(CsgError::UnsupportedBuild);
#endif
    if (!mesh || mesh->triangles.empty()) return fail(CsgError::EmptyMesh);

    for (size_t i = 0; i < mesh->triangles.size(); ++i) {
        const Object::Triangle& tri = mesh->triangles[i];
#if JET_MESH_INSTANCING
        Material* faceOverride = triangleMaterials && i < triangleMaterials->size()
            ? (*triangleMaterials)[i] : nullptr;
#else
        Material* faceOverride = nullptr;
#endif
        Material* replacement = uniformOverride ? uniformOverride : faceOverride;
        if (tri.colorBaked && !replacement) {
            // Baked colours use Scene's internal opaque FLAT material.
            continue;
        }
        if (!materialSupported(replacement ? replacement : tri.material))
            return fail(CsgError::UnsupportedMaterial);
    }
    return CsgStatus{};
}

CsgStatus validateDag(const CsgDesc& desc) {
    if (desc.nodes.empty()) return fail(CsgError::EmptyExpression);
    if (desc.root >= desc.nodes.size()) return fail(CsgError::RootOutOfRange);

    for (const CsgNode& node : desc.nodes) {
        if (node.op < CsgOp::Leaf || node.op > CsgOp::Difference)
            return fail(CsgError::InvalidNode);
        if (node.op != CsgOp::Leaf) {
            if (node.left >= desc.nodes.size() || node.right >= desc.nodes.size())
                return fail(CsgError::ChildOutOfRange);
        }
    }

    // Explicit three-colour DFS cycle check over every node, including nodes
    // that are not reachable from root.
    std::vector<uint8_t> color(desc.nodes.size(), 0);
    std::fill(color.begin(), color.end(), 0);
    struct Frame { size_t id; uint8_t nextChild; };
    std::vector<Frame> dfs;
    for (size_t start = 0; start < desc.nodes.size(); ++start) {
        if (color[start] == 2) continue;
        dfs.push_back({start, 0});
        while (!dfs.empty()) {
            Frame& f = dfs.back();
            const CsgNode& n = desc.nodes[f.id];
            if (f.nextChild == 0 && color[f.id] == 0) color[f.id] = 1;

            if (n.op == CsgOp::Leaf || f.nextChild >= 2) {
                color[f.id] = 2;
                dfs.pop_back();
                continue;
            }
            const uint32_t child = (f.nextChild == 0) ? n.left : n.right;
            ++f.nextChild;
            if (color[child] == 1) return fail(CsgError::Cycle);
            if (color[child] == 0) dfs.push_back({child, 0});
        }
    }
    return CsgStatus{};
}

} // namespace

CsgStatus validateCsgConfiguration(const CsgDesc& desc,
                                   const CsgBinding* bindings,
                                   size_t bindingCount,
                                   int sceneLodScale,
                                   CsgConfig& prepared) {
#if HALF_WIDTH_BUFFERS || !Z_BUFFERING || FAST_Z || !JET_PERSPECTIVE_DEPTH || MAX_PICK_QUERIES <= 0
    (void)desc; (void)bindings; (void)bindingCount; (void)sceneLodScale; (void)prepared;
    return fail(CsgError::UnsupportedBuild);
#else
    if (sceneLodScale != 0) return fail(CsgError::SceneLodEnabled);
    if (bindingCount > 0 && bindings == nullptr) return fail(CsgError::NullObject);

    CsgStatus status = validateDag(desc);
    if (!status) return status;

    CsgConfig candidate;
    candidate.desc = desc;
    if (bindingCount > 0 && bindings != nullptr)
        candidate.bindings.assign(bindings, bindings + bindingCount);

    // Validate binding targets and collapse exact duplicate binding records.
    std::vector<CsgBinding> unique;
    for (const CsgBinding& b : candidate.bindings) {
        if (!b.object) return fail(CsgError::NullObject);
        if (b.instanceIndex < -1) return fail(CsgError::InstanceOutOfRange);
        if (b.instanceIndex >= 0) {
#if JET_MESH_INSTANCING
            if (static_cast<size_t>(b.instanceIndex) >= b.object->instances.size())
                return fail(CsgError::InstanceOutOfRange);
#else
            return fail(CsgError::UnsupportedBuild);
#endif
        }

        for (const CsgBinding& u : unique) {
            const bool sameId = u.leaf == b.leaf;
            const bool samePlacement = u.object == b.object &&
                                       u.instanceIndex == b.instanceIndex;
            if (sameId && !samePlacement)
                return fail(CsgError::DuplicateLeafBinding);
            if (!sameId && samePlacement)
                return fail(CsgError::DuplicatePlacement);
        }

        bool exact = false;
        for (CsgBinding& u : unique) {
            if (u.leaf == b.leaf && u.object == b.object &&
                u.instanceIndex == b.instanceIndex) {
                exact = true;
                break;
            }
        }
        if (!exact) unique.push_back(b);

        status = validateBindingAppearance(b);
        if (!status) return status;
    }
    candidate.bindings = std::move(unique);

    for (const CsgNode& node : candidate.desc.nodes) {
        if (node.op != CsgOp::Leaf) continue;
        bool bound = false;
        for (const CsgBinding& b : candidate.bindings) {
            if (b.leaf == node.leaf) { bound = true; break; }
        }
        if (!bound) return fail(CsgError::ExpressionLeafUnbound);
    }

    candidate.leafIds.reserve(candidate.bindings.size());
    for (const CsgBinding& b : candidate.bindings) candidate.leafIds.push_back(b.leaf);
    std::sort(candidate.leafIds.begin(), candidate.leafIds.end());
    candidate.leafIds.erase(std::unique(candidate.leafIds.begin(), candidate.leafIds.end()),
                            candidate.leafIds.end());
    for (int i = 0; i < static_cast<int>(candidate.leafIds.size()); ++i)
        candidate.leafIndex[candidate.leafIds[i]] = i;
    candidate.active = true;

    prepared = std::move(candidate);
    return CsgStatus{};
#endif
}

void CsgEngine::resize(int w, int h) {
    width = w;
    height = h;
    const size_t pixels = static_cast<size_t>(std::max(0, w)) * std::max(0, h);
    crossings.assign(pixels, {});
    winners.assign(pixels, CsgWinner{});
    acceptKeys.assign(pixels, 0);
    usedKeys.clear();
}

void CsgEngine::beginBand(int yMin, int yMax) {
    if (width <= 0 || height <= 0) return;
    yMin = std::max(0, yMin);
    yMax = std::min(height, yMax);
    for (int y = yMin; y < yMax; ++y) {
        const size_t row = static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            crossings[row + x].clear();
            winners[row + x] = CsgWinner{};
            acceptKeys[row + x] = 0;
        }
    }
    usedKeys.clear();
}

void CsgEngine::probeCallback(void* user, int x, int y, double z,
                              uint32_t leaf, uint32_t originalTriangle,
                              uint32_t queueIndex, int direction) {
    if (!user) return;
    auto* engine = static_cast<CsgEngine*>(user);
    if (x < 0 || y < 0 || x >= engine->width || y >= engine->height) return;
    CsgCrossing c;
    c.z = z;
    c.leaf = leaf;
    c.originalTriangle = originalTriangle;
    c.queueIndex = queueIndex;
    c.direction = static_cast<int8_t>(direction >= 0 ? 1 : -1);
    engine->addCrossing(x, y, c);
}

void CsgEngine::addCrossing(int x, int y, const CsgCrossing& crossing) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    auto& pixel = crossings[static_cast<size_t>(y) * width + x];
    for (CsgCrossing& existing : pixel) {
        if (existing.leaf == crossing.leaf &&
            existing.originalTriangle == crossing.originalTriangle) {
            // Near-plane clipping fans can cover one fan diagonal twice. A
            // ray intersects the original triangle at most once, so retain a
            // single order-independent representative.
            if (crossing.z < existing.z ||
                (crossing.z == existing.z && crossing.queueIndex < existing.queueIndex)) {
                existing = crossing;
            }
            return;
        }
    }
    pixel.push_back(crossing);
}

void CsgEngine::resolveBand(int yMin, int yMax, const CsgConfig& config,
                            int32_t nearPlane, int32_t farPlane) {
    if (width <= 0 || height <= 0 || !config.active) return;
    yMin = std::max(0, yMin);
    yMax = std::min(height, yMax);
    const int leafCount = static_cast<int>(config.leafIds.size());
    std::vector<int> winding(leafCount, 0);
    std::vector<uint8_t> inside(leafCount, 0);
    std::vector<int8_t> memo(config.desc.nodes.size(), 0);

    auto eval = [&](uint32_t root) -> bool {
        std::fill(memo.begin(), memo.end(), 0);
        // Iterative postorder evaluation for the validated DAG.
        std::vector<uint32_t> order;
        std::vector<uint8_t> seen(config.desc.nodes.size(), 0);
        std::vector<uint32_t> stack{root};
        while (!stack.empty()) {
            uint32_t id = stack.back();
            stack.pop_back();
            if (seen[id]) continue;
            seen[id] = 1;
            order.push_back(id);
            const CsgNode& n = config.desc.nodes[id];
            if (n.op != CsgOp::Leaf) {
                stack.push_back(n.left);
                stack.push_back(n.right);
            }
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            uint32_t id = *it;
            const CsgNode& n = config.desc.nodes[id];
            bool value = false;
            if (n.op == CsgOp::Leaf) {
                int li = config.compactLeaf(n.leaf);
                value = li >= 0 && inside[li];
            } else {
                bool a = memo[n.left] > 0;
                bool b = memo[n.right] > 0;
                if (n.op == CsgOp::Union) value = a || b;
                else if (n.op == CsgOp::Intersection) value = a && b;
                else value = a && !b;
            }
            memo[id] = value ? 1 : -1;
        }
        return memo[root] > 0;
    };

    usedKeys.clear();
    for (int y = yMin; y < yMax; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t pixelIndex = static_cast<size_t>(y) * width + x;
            auto& events = crossings[pixelIndex];
            winners[pixelIndex] = CsgWinner{};
            acceptKeys[pixelIndex] = 0;
            if (events.empty()) continue;

            std::sort(events.begin(), events.end(), [](const CsgCrossing& a, const CsgCrossing& b) {
                if (a.z != b.z) return a.z < b.z;
                if (a.leaf != b.leaf) return a.leaf < b.leaf;
                if (a.originalTriangle != b.originalTriangle)
                    return a.originalTriangle < b.originalTriangle;
                return a.queueIndex < b.queueIndex;
            });

            struct Group { size_t begin, end; double minZ; };
            std::vector<Group> groups;
            for (size_t i = 0; i < events.size();) {
                size_t j = i + 1;
                const double minZ = events[i].z;
                const double eps = 1e-8 * std::max(1.0, std::fabs(minZ));
                while (j < events.size() &&
                       events[j].z <= minZ + eps) {
                    ++j;
                }
                groups.push_back({i, j, minZ});
                i = j;
            }

            std::fill(winding.begin(), winding.end(), 0);
            std::fill(inside.begin(), inside.end(), 0);

            CsgWinner pixelWinner;
            for (int gi = static_cast<int>(groups.size()) - 1; gi >= 0; --gi) {
                const Group& group = groups[gi];
                const bool farRoot = eval(config.desc.root);

                // Deterministic attribution simulation for one same-depth
                // group: stable leaf number, then original triangle number.
                std::vector<CsgCrossing> ordered(events.begin() + group.begin,
                                                 events.begin() + group.end);
                std::sort(ordered.begin(), ordered.end(),
                          [](const CsgCrossing& a, const CsgCrossing& b) {
                              if (a.leaf != b.leaf) return a.leaf < b.leaf;
                              if (a.originalTriangle != b.originalTriangle)
                                  return a.originalTriangle < b.originalTriangle;
                              if (a.direction != b.direction) return a.direction < b.direction;
                              return a.queueIndex < b.queueIndex;
                          });

                bool previousRoot = farRoot;
                bool hasAttribution = false;
                CsgCrossing attributed{};
                for (const CsgCrossing& c : ordered) {
                    const int li = config.compactLeaf(c.leaf);
                    if (li >= 0) {
                        // Groups are swept from positive infinity toward the
                        // camera. The projective/front sign is the forward
                        // delta, so reverse traversal subtracts it.
                        winding[li] -= static_cast<int>(c.direction);
                        inside[li] = winding[li] != 0;
                    }
                    const bool nowRoot = eval(config.desc.root);
                    if (nowRoot != previousRoot) {
                        attributed = c;
                        hasAttribution = true;
                        previousRoot = nowRoot;
                    }
                }
                const bool nearRoot = previousRoot;

                // Whole-group before/after equality means coplanar/shared
                // edges cancelled or only changed winding multiplicity.
                if (nearRoot == farRoot || !hasAttribution)
                    continue;

                // Near side -> far side transition direction.
                const bool rootEntersForward = !nearRoot && farRoot;
                const bool sourceEntersForward = attributed.direction > 0;
                const bool flip = rootEntersForward != sourceEntersForward;

                // Near/far planes restrict only the final visible boundary.
                if (attributed.z < static_cast<double>(nearPlane) ||
                    attributed.z > static_cast<double>(farPlane))
                    continue;

                CsgWinner candidate;
                candidate.valid = true;
                candidate.z = attributed.z;
                candidate.leaf = attributed.leaf;
                candidate.originalTriangle = attributed.originalTriangle;
                candidate.queueIndex = attributed.queueIndex;
                candidate.flipNormal = flip;
                // Groups are visited far-to-near; later assignments are nearer.
                pixelWinner = candidate;
            }

            if (pixelWinner.valid) {
                winners[pixelIndex] = pixelWinner;
                const uint32_t key = boundaryKey(pixelWinner.queueIndex,
                                                 pixelWinner.flipNormal);
                acceptKeys[pixelIndex] = key;
                usedKeys.push_back(key);
            }
        }
    }
    std::sort(usedKeys.begin(), usedKeys.end());
    usedKeys.erase(std::unique(usedKeys.begin(), usedKeys.end()), usedKeys.end());
}

} // namespace Renderer
