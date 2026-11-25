#include "Csg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_set>

#include "Camera.hpp"

namespace Renderer {

const char* csgErrorName(CsgError error) {
    switch (error) {
        case CsgError::Ok: return "Ok";
        case CsgError::EmptyExpression: return "EmptyExpression";
        case CsgError::RootOutOfRange: return "RootOutOfRange";
        case CsgError::InvalidNode: return "InvalidNode";
        case CsgError::ChildOutOfRange: return "ChildOutOfRange";
        case CsgError::Cycle: return "Cycle";
        case CsgError::UnboundLeaf: return "UnboundLeaf";
        case CsgError::DuplicateLeafBinding: return "DuplicateLeafBinding";
        case CsgError::NullObject: return "NullObject";
        case CsgError::ObjectNotInScene: return "ObjectNotInScene";
        case CsgError::InstanceOutOfRange: return "InstanceOutOfRange";
        case CsgError::EmptyMesh: return "EmptyMesh";
        case CsgError::UnsupportedFeature: return "UnsupportedFeature";
        case CsgError::UnsupportedBuild: return "UnsupportedBuild";
    }
    return "Unknown";
}

CsgEngine::CsgEngine() = default;

uint32_t CsgEngine::leafForDraw(const Object* object, int32_t instance) const {
    auto it = drawToLeaf_.find({object, instance});
    return it == drawToLeaf_.end() ? UINT32_MAX : it->second;
}

bool CsgEngine::isBoundObject(const Object* object) const {
    if (!object) return false;
    for (const auto& entry : bindings_) {
        if (entry.second.object == object) return true;
    }
    return false;
}

CsgError CsgEngine::validateBinding(const CsgBinding& binding,
                                    const std::vector<Object*>* sceneObjects) const {
    if (!binding.object) return CsgError::NullObject;
    if (sceneObjects) {
        bool found = false;
        for (const Object* object : *sceneObjects) {
            if (object == binding.object) { found = true; break; }
        }
        if (!found) return CsgError::ObjectNotInScene;
    }

    const Object* mesh = binding.object;
    if (binding.instance < -1) return CsgError::InstanceOutOfRange;
    if (binding.instance >= 0) {
#if JET_MESH_INSTANCING
        if ((size_t)binding.instance >= binding.object->instances.size())
            return CsgError::InstanceOutOfRange;
        const auto& instance = binding.object->instances[binding.instance];
        if (!instance.mesh) return CsgError::EmptyMesh;
        mesh = instance.mesh.get();
#else
        return CsgError::UnsupportedFeature;
#endif
    }
    if (!mesh || mesh->triangles.empty() || mesh->vertices.empty())
        return CsgError::EmptyMesh;

    const Object* owner = binding.object;
    if (owner->isBillboard || owner->zBias != 0 ||
        owner->ignoreZBuffer || owner->noWriteZBuffer ||
        !owner->lodMeshes.empty() ||
        owner->fadeNear != 0 || owner->fadeFar != 0 ||
        owner->appearNear != 0 || owner->appearFar != 0 ||
        owner->blendMode != BlendMode::BLEND_REPLACE) {
        return CsgError::UnsupportedFeature;
    }

    for (size_t ti = 0; ti < mesh->triangles.size(); ++ti) {
        const auto& triangle = mesh->triangles[ti];
        Material* material = triangle.material;
#if JET_MESH_INSTANCING
        if (binding.instance >= 0) {
            const auto& inst = owner->instances[binding.instance];
            if (inst.materialOverride) material = inst.materialOverride;
            else if (inst.triangleMaterials &&
                     inst.triangleMaterials->size() == mesh->triangles.size() &&
                     (*inst.triangleMaterials)[ti]) {
                material = (*inst.triangleMaterials)[ti];
            }
        }
#endif
        if (!material || material->shader || material->alpha != 255)
            return CsgError::UnsupportedFeature;
        if (material->diffuseMap && material->diffuseMap->hasAlpha)
            return CsgError::UnsupportedFeature;
        switch (material->shadingMode) {
            case ShadingMode::UNLIT:
            case ShadingMode::FLAT:
            case ShadingMode::PHONG:
                break;
            default:
                return CsgError::UnsupportedFeature;
        }
    }

#if HALF_WIDTH_BUFFERS || FIELD_BUFFERS || !Z_BUFFERING || \
    SCREEN_DOOR_ALPHA || NOISE_ALPHA
    // The required target is full-resolution opaque rendering with a depth buffer.
    return CsgError::UnsupportedBuild;
#else
    return CsgError::Ok;
#endif
}

CsgError CsgEngine::set(const CsgDesc& desc,
                        const std::vector<Object*>* sceneObjects) {
    lastError_ = CsgError::Ok;
    if (desc.nodes.empty()) return lastError_ = CsgError::EmptyExpression;
    if (desc.root >= desc.nodes.size()) return lastError_ = CsgError::RootOutOfRange;

    for (const CsgNode& node : desc.nodes) {
        if (node.op < CsgOp::Leaf || node.op > CsgOp::Difference)
            return lastError_ = CsgError::InvalidNode;
        if (node.op != CsgOp::Leaf) {
            if (node.left >= desc.nodes.size() || node.right >= desc.nodes.size())
                return lastError_ = CsgError::ChildOutOfRange;
        }
    }

    // Complete cycle validation before touching the active configuration.
    std::function<bool(size_t, std::vector<int>&)> dfs =
        [&](size_t u, std::vector<int>& mark) -> bool {
            if (mark[u] == 1) return true;
            if (mark[u] == 2) return false;
            mark[u] = 1;
            const CsgNode& node = desc.nodes[u];
            if (node.op != CsgOp::Leaf) {
                if (dfs(node.left, mark) || dfs(node.right, mark)) return true;
            }
            mark[u] = 2;
            return false;
        };
    std::vector<int> mark(desc.nodes.size(), 0);
    for (size_t i = 0; i < desc.nodes.size(); ++i) {
        if (dfs(i, mark)) return lastError_ = CsgError::Cycle;
    }

    std::map<uint32_t, StoredBinding> newBindings;
    std::map<std::pair<const Object*, int32_t>, uint32_t> newDrawMap;
    for (const CsgBinding& binding : desc.bindings) {
        CsgError error = validateBinding(binding, sceneObjects);
        if (error != CsgError::Ok) return lastError_ = error;

        auto it = newBindings.find(binding.leaf);
        if (it != newBindings.end()) {
            if (it->second.object != binding.object ||
                it->second.instance != binding.instance) {
                return lastError_ = CsgError::DuplicateLeafBinding;
            }
            continue;
        }
        StoredBinding stored;
        stored.leaf = binding.leaf;
        stored.object = binding.object;
        stored.instance = binding.instance;
#if JET_MESH_INSTANCING
        if (binding.instance >= 0)
            stored.retainedMesh = binding.object->instances[binding.instance].mesh;
#endif
        newBindings.emplace(binding.leaf, std::move(stored));
        newDrawMap[{binding.object, binding.instance}] = binding.leaf;
    }

    for (const CsgNode& node : desc.nodes) {
        if (node.op == CsgOp::Leaf && newBindings.find(node.leaf) == newBindings.end())
            return lastError_ = CsgError::UnboundLeaf;
    }

    // Commit only after every check has succeeded.
    nodes_ = desc.nodes;
    root_ = desc.root;
    bindings_ = std::move(newBindings);
    drawToLeaf_ = std::move(newDrawMap);
    sourceToSlot_.clear();
    slots_.clear();
    coverTriangles_.clear();
    acceptMask_.clear();
    winners_.clear();
    usedTokens_.clear();
    active_ = true;
    return lastError_ = CsgError::Ok;
}

void CsgEngine::clear() {
    active_ = false;
    lastError_ = CsgError::Ok;
    nodes_.clear();
    bindings_.clear();
    drawToLeaf_.clear();
    sourceToSlot_.clear();
    slots_.clear();
    coverTriangles_.clear();
    acceptMask_.clear();
    winners_.clear();
    usedTokens_.clear();
    width_ = height_ = 0;
}

bool CsgEngine::expressionValue(size_t nodeIndex,
                                const std::map<uint32_t, int>& winding) const {
    const CsgNode& node = nodes_[nodeIndex];
    if (node.op == CsgOp::Leaf) {
        auto it = winding.find(node.leaf);
        return it != winding.end() && it->second != 0;
    }
    const bool a = expressionValue(node.left, winding);
    const bool b = expressionValue(node.right, winding);
    switch (node.op) {
        case CsgOp::Union: return a || b;
        case CsgOp::Intersect: return a && b;
        case CsgOp::Difference: return a && !b;
        default: return false;
    }
}

bool CsgEngine::rootValue(const std::map<uint32_t, int>& winding) const {
    if (!active_ || root_ >= nodes_.size()) return false;
    return expressionValue(root_, winding);
}

int CsgEngine::slotFor(uint32_t leaf, int32_t sourceTriangle) const {
    uint64_t key = (uint64_t(leaf) << 32) | uint32_t(sourceTriangle);
    auto it = sourceToSlot_.find(key);
    return it == sourceToSlot_.end() ? -1 : int(it->second);
}

const CsgWinner* CsgEngine::winnerAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || winners_.empty())
        return nullptr;
    const CsgWinner& winner = winners_[size_t(y) * width_ + x];
    return winner.valid ? &winner : nullptr;
}

namespace {

using Mat3 = std::array<float, 9>;

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c]
                           + a[r * 3 + 1] * b[1 * 3 + c]
                           + a[r * 3 + 2] * b[2 * 3 + c];
    return out;
}

std::array<float, 3> transform(const Mat3& m, const Vector3& p) {
    return {
        m[0] * float(p.x) + m[1] * float(p.y) + m[2] * float(p.z),
        m[3] * float(p.x) + m[4] * float(p.y) + m[5] * float(p.z),
        m[6] * float(p.x) + m[7] * float(p.y) + m[8] * float(p.z),
    };
}

struct DPoint {
    double x, y, z;
};

DPoint interpZ(const DPoint& a, const DPoint& b, double planeZ) {
    double t = (planeZ - a.z) / (b.z - a.z);
    return {a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            planeZ};
}

} // namespace

void CsgEngine::prepareFrame(const Camera* camera, int screenWidth, int screenHeight,
                             const float cameraMatrix[9]) {
    width_ = screenWidth;
    height_ = screenHeight;
    slots_.clear();
    coverTriangles_.clear();
    sourceToSlot_.clear();
    usedTokens_.clear();
    acceptMask_.clear();
    winners_.clear();
    if (!active_ || !camera || width_ <= 0 || height_ <= 0) return;

    acceptMask_.assign(size_t(width_) * height_, 0);
    winners_.assign(acceptMask_.size(), CsgWinner{});
    frameNear_ = camera->nearPlane;
    frameFar_ = camera->farPlane;

    const float fov = camera->fovFactor;
    const double halfW = width_ * 0.5;
    const double halfH = height_ * 0.5;
    const double clipZ = 1e-4;

    for (const auto& entry : bindings_) {
        const uint32_t leaf = entry.first;
        const StoredBinding& binding = entry.second;
        Object* owner = binding.object;
        if (!owner->enabled) continue;
        const Object* mesh = owner;

        Mat3 cameraMat{};
        for (int i = 0; i < 9; ++i) cameraMat[i] = cameraMatrix[i];
        Mat3 objectMat{};
        {
            const auto rotation = Object::InstanceTransform::rotated(owner->rotation);
            for (int i = 0; i < 9; ++i) objectMat[i] = rotation.basis[i];
        }
        Mat3 combined = multiply(cameraMat, objectMat);

        auto relative = Vector3(owner->position.x - camera->position.x,
                                owner->position.y - camera->position.y,
                                owner->position.z - camera->position.z);
        auto translated = transform(cameraMat, relative);
        std::array<float, 3> translation{translated[0], translated[1], translated[2]};

#if JET_MESH_INSTANCING
        if (binding.instance >= 0) {
            const auto& instance = owner->instances[binding.instance];
            Mat3 instanceMat{};
            for (int i = 0; i < 9; ++i) instanceMat[i] = instance.transform.basis[i];
            combined = multiply(combined, instanceMat);
            const Mat3 parentMat = multiply(cameraMat, objectMat);
            auto localT = transform(parentMat, instance.transform.position);
            translation[0] += localT[0];
            translation[1] += localT[1];
            translation[2] += localT[2];
            mesh = instance.mesh.get();
        }
#endif
        if (!mesh) continue;

        for (int32_t ti = 0; ti < (int32_t)mesh->triangles.size(); ++ti) {
            const auto& tri = mesh->triangles[ti];
            const Vector3* vp[3] = {
                &mesh->vertices[tri.v1].position,
                &mesh->vertices[tri.v2].position,
                &mesh->vertices[tri.v3].position,
            };

            struct P { int32_t x, y, z, sx, sy; };
            P p[3]{};
            DPoint dp[3]{};
            for (int k = 0; k < 3; ++k) {
                auto q = transform(combined, *vp[k]);
                int32_t cx = (int32_t)(q[0] + translation[0]);
                int32_t cy = (int32_t)(q[1] + translation[1]);
                int32_t cz = (int32_t)(q[2] + translation[2]);
                if (cz == 0) cz = 1;
                float invZ = fov / (float)cz;
                p[k] = {cx, cy, cz,
                        (int32_t)((float)cx * invZ) + width_ / 2,
                        height_ / 2 - (int32_t)((float)cy * invZ)};
                dp[k] = {(double)cx, (double)cy, (double)cz};
            }

            uint32_t slot = (uint32_t)slots_.size();
            sourceToSlot_[(uint64_t(leaf) << 32) | uint32_t(ti)] = slot;
            slots_.push_back({leaf, ti, owner, mesh, binding.instance});

            auto addCover = [&](long long x0, long long y0, double z0,
                                long long x1, long long y1, double z1,
                                long long x2, long long y2, double z2) {
                __int128 area = (__int128)(x1 - x0) * (y2 - y0)
                              - (__int128)(y1 - y0) * (x2 - x0);
                if (area == 0) return;
                CoverTriangle ct;
                ct.slot = slot;
                ct.leaf = leaf;
                ct.triangle = ti;
                ct.x[0] = x0; ct.y[0] = y0; ct.z[0] = z0;
                ct.x[1] = x1; ct.y[1] = y1; ct.z[1] = z1;
                ct.x[2] = x2; ct.y[2] = y2; ct.z[2] = z2;
                ct.direction = area > 0 ? 1 : -1;
                coverTriangles_.push_back(ct);
            };

            if (p[0].z >= 1 && p[1].z >= 1 && p[2].z >= 1) {
                addCover(p[0].sx, p[0].sy, p[0].z,
                         p[1].sx, p[1].sy, p[1].z,
                         p[2].sx, p[2].sy, p[2].z);
                continue;
            }

            // Clip only the camera plane singularity. Crossings below the
            // application near plane are retained for occupancy state.
            std::vector<DPoint> poly;
            for (int edge = 0; edge < 3; ++edge) {
                const DPoint& a = dp[edge];
                const DPoint& b = dp[(edge + 1) % 3];
                bool inA = a.z >= clipZ;
                bool inB = b.z >= clipZ;
                if (inA) poly.push_back(a);
                if (inA != inB) poly.push_back(interpZ(a, b, clipZ));
            }
            for (size_t k = 1; k + 1 < poly.size(); ++k) {
                const DPoint* q[3] = {&poly[0], &poly[k], &poly[k + 1]};
                long long sx[3], sy[3];
                for (int j = 0; j < 3; ++j) {
                    sx[j] = (long long)(halfW + (double)fov * q[j]->x / q[j]->z);
                    sy[j] = (long long)(halfH - (double)fov * q[j]->y / q[j]->z);
                }
                addCover(sx[0], sy[0], q[0]->z,
                         sx[1], sy[1], q[1]->z,
                         sx[2], sy[2], q[2]->z);
            }
        }
    }
    usedTokens_.assign(2 * slots_.size() + 1, 0);
}

namespace {

struct Event {
    uint32_t slot;
    uint32_t leaf;
    int32_t triangle;
    double z;
    int delta;
};

struct Boundary {
    double groupMinZ;
    double selectedZ;
    Event selected;
    int rootDirection;
};

} // namespace

void CsgEngine::evaluateBand(int yMin, int yMax) {
    if (!active_ || width_ <= 0 || height_ <= 0 || winners_.empty()) return;
    yMin = std::max(0, yMin);
    yMax = std::min(height_, yMax);
    for (int y = yMin; y < yMax; ++y) {
        for (int x = 0; x < width_; ++x) {
            size_t idx = size_t(y) * width_ + x;
            acceptMask_[idx] = 0;
            winners_[idx] = CsgWinner{};
        }
    }
    std::fill(usedTokens_.begin(), usedTokens_.end(), 0);

    // Camera near/far are supplied through the last prepared frame by Scene's
    // camera; keep broad inclusion here and let the visible-range test use the
    // values recorded below.
    const double nearZ = frameNear_;
    const double farZ = frameFar_;

    for (int y = yMin; y < yMax; ++y) {
        for (int x = 0; x < width_; ++x) {
            std::vector<Event> events;
            events.reserve(16);
            const double px = (double)x;
            const double py = (double)y;

            for (const CoverTriangle& ct : coverTriangles_) {
                const double ax = (double)ct.x[0], ay = (double)ct.y[0];
                const double bx = (double)ct.x[1], by = (double)ct.y[1];
                const double cx = (double)ct.x[2], cy = (double)ct.y[2];
                const double v0x = bx - ax, v0y = by - ay;
                const double v1x = cx - ax, v1y = cy - ay;
                const double v2x = px - ax, v2y = py - ay;
                const double d00 = v0x * v0x + v0y * v0y;
                const double d01 = v0x * v1x + v0y * v1y;
                const double d11 = v1x * v1x + v1y * v1y;
                const double d20 = v2x * v0x + v2y * v0y;
                const double d21 = v2x * v1x + v2y * v1y;
                const double denom = d00 * d11 - d01 * d01;
                if (std::abs(denom) < 1e-12) continue;
                const double wB = (d11 * d20 - d01 * d21) / denom;
                const double wC = (d00 * d21 - d01 * d20) / denom;
                const double wA = 1.0 - wB - wC;
                constexpr double kBias = -1e-7;
                if (wA < kBias || wB < kBias || wC < kBias) continue;

                double invZ = wA / ct.z[0] + wB / ct.z[1] + wC / ct.z[2];
                if (!(invZ > 0.0)) continue;
                double z = 1.0 / invZ;
                if (!std::isfinite(z) || z <= 0.0) continue;
                events.push_back({ct.slot, ct.leaf, ct.triangle, z, ct.direction});
            }
            if (events.empty()) continue;

            // A planar source triangle has exactly one ray crossing. Clip fans
            // and inclusive shared internal edges can emit it more than once.
            std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
                if (a.slot != b.slot) return a.slot < b.slot;
                return a.z < b.z;
            });
            std::vector<Event> unique;
            unique.reserve(events.size());
            for (size_t i = 0; i < events.size();) {
                size_t j = i + 1;
                double z = events[i].z;
                while (j < events.size() && events[j].slot == events[i].slot) {
                    z = std::min(z, events[j].z);
                    ++j;
                }
                Event merged = events[i];
                merged.z = z;
                unique.push_back(merged);
                i = j;
            }

            std::sort(unique.begin(), unique.end(), [](const Event& a, const Event& b) {
                return a.z < b.z;
            });

            // Depth groups use pre-quantisation perspective depth and the
            // mandated relative tolerance referenced to the group minimum.
            std::vector<std::vector<Event>> groups;
            for (const Event& event : unique) {
                if (groups.empty()) {
                    groups.push_back({event});
                    continue;
                }
                double minZ = groups.back().front().z;
                for (const Event& existing : groups.back())
                    minZ = std::min(minZ, existing.z);
                double tolerance = 1e-8 * std::max(1.0, std::abs(minZ));
                if (event.z - minZ <= tolerance)
                    groups.back().push_back(event);
                else
                    groups.push_back({event});
            }

            // A ray lies on a shared/clip edge only once geometrically. The
            // inclusive coverage test can otherwise report the same directed
            // crossing from two adjacent source triangles. Collapse coincident,
            // same-direction leaf events; prefer the larger original triangle
            // index, matching the deterministic ascending attribution tie.
            for (auto& group : groups) {
                std::sort(group.begin(), group.end(), [](const Event& a, const Event& b) {
                    if (a.leaf != b.leaf) return a.leaf < b.leaf;
                    if (a.delta != b.delta) return a.delta < b.delta;
                    return a.triangle < b.triangle;
                });
                std::vector<Event> collapsed;
                for (const Event& event : group) {
                    if (!collapsed.empty()) {
                        Event& last = collapsed.back();
                        double tol = 1e-8 * std::max(1.0, std::max(std::abs(last.z), std::abs(event.z)));
                        if (last.leaf == event.leaf && last.delta == event.delta &&
                            std::abs(last.z - event.z) <= tol) {
                            last = event; // larger triangle index by sort order
                            continue;
                        }
                    }
                    collapsed.push_back(event);
                }
                group.swap(collapsed);
            }

            // Reverse sweep from known-outside positive infinity.
            std::map<uint32_t, int> winding;
            std::vector<Boundary> boundaries;
            for (int gi = (int)groups.size() - 1; gi >= 0; --gi) {
                const auto& group = groups[gi];
                const bool farValue = rootValue(winding);
                auto nearWinding = winding;
                for (const Event& event : group)
                    nearWinding[event.leaf] -= event.delta;
                const bool nearValue = rootValue(nearWinding);

                if (farValue != nearValue) {
                    std::vector<Event> ordered = group;
                    std::sort(ordered.begin(), ordered.end(),
                              [](const Event& a, const Event& b) {
                                  if (a.leaf != b.leaf) return a.leaf < b.leaf;
                                  return a.triangle < b.triangle;
                              });
                    auto simulation = nearWinding;
                    Event last = ordered.front();
                    bool foundLast = false;
                    for (const Event& event : ordered) {
                        bool before = rootValue(simulation);
                        simulation[event.leaf] += event.delta;
                        bool after = rootValue(simulation);
                        if (before != after) {
                            last = event;
                            foundLast = true;
                        }
                    }
                    if (!foundLast) last = ordered.back();
                    int rootDirection = (!nearValue && farValue) ? 1 : -1;
                    double minZ = group.front().z;
                    for (const Event& e : group) minZ = std::min(minZ, e.z);
                    boundaries.push_back({minZ, last.z, last, rootDirection});
                }
                winding = std::move(nearWinding);
            }

            const Boundary* visible = nullptr;
            for (const Boundary& boundary : boundaries) {
                // Match the existing rasterizer's int32 perspective-depth test.
                int32_t integerZ = (int32_t)boundary.selectedZ;
                if (integerZ < nearZ || integerZ > farZ) continue;
                if (!visible || boundary.groupMinZ < visible->groupMinZ)
                    visible = &boundary;
            }
            if (!visible) continue;

            const Event& selected = visible->selected;
            bool flip = visible->rootDirection != selected.delta;
            uint32_t token = flip ? 2u * selected.slot + 2u : 2u * selected.slot + 1u;
            size_t idx2 = size_t(y) * width_ + x;
            acceptMask_[idx2] = token;
            usedTokens_[token] = 1;

            CsgWinner winner;
            winner.valid = true;
            winner.leaf = selected.leaf;
            winner.triangle = selected.triangle;
            winner.slot = selected.slot;
            winner.flipNormal = flip;
            winner.depth = selected.z;
            // Match Rasterizer's positive-depth int32 truncation and uint16 store.
            long depth16 = (long)selected.z;
            winner.depth16 = (uint16_t)std::max(0L, std::min(65535L, depth16));
            const Slot& slot = slots_[selected.slot];
            winner.object = slot.object;
            winner.mesh = slot.mesh;
            winner.instance = slot.instance;
            winners_[idx2] = winner;
        }
    }
}

} // namespace Renderer
