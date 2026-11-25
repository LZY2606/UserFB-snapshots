#ifndef JET_CSG_HPP
#define JET_CSG_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Object.hpp"

namespace Renderer {

class Scene;

/// @brief Node kind in a caller-owned constructive-solid-geometry DAG.
enum class CsgOp : uint8_t {
    Leaf = 0,         ///< References one stable leaf number.
    Union = 1,        ///< left OR right.
    Intersection = 2, ///< left AND right.
    Difference = 3    ///< left AND NOT right.
};

/// @brief One expression DAG node.
///
/// Binary nodes refer to other entries in CsgDesc::nodes by index. Indices may
/// point forward and a node may be referenced more than once. A Leaf uses
/// CsgNode::leaf as the stable uint32 identifier supplied with a binding.
struct CsgNode {
    CsgOp op = CsgOp::Leaf;
    uint32_t leaf = 0;
    uint32_t left = UINT32_MAX;
    uint32_t right = UINT32_MAX;

    static CsgNode makeLeaf(uint32_t leafId) {
        CsgNode n;
        n.op = CsgOp::Leaf;
        n.leaf = leafId;
        return n;
    }
    static CsgNode make(CsgOp op, uint32_t leftIndex, uint32_t rightIndex) {
        CsgNode n;
        n.op = op;
        n.left = leftIndex;
        n.right = rightIndex;
        return n;
    }
    static CsgNode makeUnion(uint32_t a, uint32_t b) { return make(CsgOp::Union, a, b); }
    static CsgNode makeIntersection(uint32_t a, uint32_t b) { return make(CsgOp::Intersection, a, b); }
    static CsgNode makeDifference(uint32_t a, uint32_t b) { return make(CsgOp::Difference, a, b); }
};

/// @brief Numeric, structural description of a CSG expression.
///
/// Scene copies this vector and root; after setCsgExpression() returns, the
/// caller may modify or destroy the description storage.
struct CsgDesc {
    std::vector<CsgNode> nodes;
    uint32_t root = 0;
};

/// @brief Bind one stable leaf number to an ordinary mesh or shared instance.
///
/// instanceIndex == -1 binds object's own Object::triangles mesh. Values >= 0
/// bind object->instances[instanceIndex]. The Object pointer is borrowed: the
/// caller must keep the Object, its materials/textures and the instance table
/// entry alive until the binding is replaced or cleared. Moving Object
/// position/rotation or an existing instance's transform between frames is
/// supported; adding/removing instances can change indices and requires
/// resubmitting the expression.
struct CsgBinding {
    uint32_t leaf = 0;
    Object* object = nullptr;
    int32_t instanceIndex = -1;
};

/// @brief Detailed CSG configuration/validation error.
enum class CsgError {
    Ok = 0,
    UnsupportedBuild,
    EmptyExpression,
    RootOutOfRange,
    InvalidNode,
    ChildOutOfRange,
    Cycle,
    ExpressionLeafUnbound,
    NullObject,
    InstanceOutOfRange,
    MissingSharedMesh,
    EmptyMesh,
    DuplicateLeafBinding,
    DuplicatePlacement,
    UnsupportedMaterial,
    UnsupportedObjectFeature,
    SceneLodEnabled
};

/// @brief Return status from Scene::setCsgExpression().
struct CsgStatus {
    CsgError code = CsgError::Ok;
    std::string message;

    bool ok() const { return code == CsgError::Ok; }
    explicit operator bool() const { return ok(); }
};

/// @brief Copied, validated configuration retained privately by Scene.
struct CsgConfig {
    CsgDesc desc;
    std::vector<CsgBinding> bindings;
    std::vector<uint32_t> leafIds;
    std::unordered_map<uint32_t, int> leafIndex;
    bool active = false;

    int compactLeaf(uint32_t leafId) const {
        auto it = leafIndex.find(leafId);
        return it == leafIndex.end() ? -1 : it->second;
    }
    const CsgBinding* findBinding(const Object* object, int32_t instanceIndex) const {
        for (const CsgBinding& b : bindings) {
            if (b.object == object && b.instanceIndex == instanceIndex) return &b;
        }
        return nullptr;
    }
    bool hasObject(const Object* object) const {
        for (const CsgBinding& b : bindings) {
            if (b.object == object) return true;
        }
        return false;
    }
};

/// @brief One directed surface crossing at a pixel centre.
struct CsgCrossing {
    double z = 0.0;                 ///< Perspective camera-space depth before 16-bit storage.
    uint32_t leaf = 0;               ///< Stable leaf number.
    uint32_t originalTriangle = 0;  ///< Original triangle in source mesh/prototype.
    uint32_t queueIndex = 0;         ///< Scene RenderTri selected for shading.
    int8_t direction = 0;           ///< +1 front/projective entry, -1 exit.
};

/// @brief Resolved visible CSG boundary for one pixel.
struct CsgWinner {
    bool valid = false;
    double z = 0.0;
    uint32_t leaf = 0;
    uint32_t originalTriangle = 0;
    uint32_t queueIndex = 0;
    bool flipNormal = false;
};

/// @brief Per-frame CSG crossing store and expression sweep.
///
/// This class is internal to Scene's implementation but is exposed so the
/// translation unit can own all non-Scene CSG algorithms. One engine belongs
/// to exactly one Scene; engines are not shared between Scenes.
class CsgEngine {
public:
    int width = 0;
    int height = 0;
    std::vector<std::vector<CsgCrossing>> crossings;
    std::vector<CsgWinner> winners;
    /// Non-zero key accepted by Rasterizer for the boundary that won a pixel.
    std::vector<uint32_t> acceptKeys;
    /// Keys with at least one winner in the most recently resolved band.
    std::vector<uint32_t> usedKeys;

    CsgEngine() = default;
    CsgEngine(int w, int h) { resize(w, h); }

    void resize(int w, int h);
    void beginBand(int yMin, int yMax);

    /// Rasterizer fragment probe callback. user is a CsgEngine*.
    static void probeCallback(void* user, int x, int y, double z,
                              uint32_t leaf, uint32_t originalTriangle,
                              uint32_t queueIndex, int direction);

    void addCrossing(int x, int y, const CsgCrossing& crossing);

    /// Sweep all crossings in [yMin,yMax), evaluate the DAG and publish
    /// winners/accept keys. nearPlane/farPlane only filter the final boundary.
    void resolveBand(int yMin, int yMax, const CsgConfig& config,
                     int32_t nearPlane, int32_t farPlane);

    static uint32_t boundaryKey(uint32_t queueIndex, bool flipped) {
        // +1 keeps zero as the "no CSG boundary" mask value.
        return (queueIndex + 1u) * 2u + (flipped ? 1u : 0u);
    }
    static void decodeKey(uint32_t key, uint32_t& queueIndex, bool& flipped) {
        flipped = (key & 1u) != 0;
        queueIndex = (key / 2u) - 1u;
    }
};

/// @brief Validate and prepare a candidate configuration without touching Scene.
///
/// On success, @p prepared is fully replaced. On failure, @p prepared is left
/// untouched so callers can preserve their last-good configuration.
CsgStatus validateCsgConfiguration(const CsgDesc& desc,
                                   const CsgBinding* bindings,
                                   size_t bindingCount,
                                   int sceneLodScale,
                                   CsgConfig& prepared);

/// @brief Human-readable stable error description.
const char* csgErrorString(CsgError error);

} // namespace Renderer

#endif // JET_CSG_HPP
