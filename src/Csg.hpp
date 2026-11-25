#ifndef JET_CSG_HPP
#define JET_CSG_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "JetConfig.hpp"
#include "Object.hpp"

namespace Renderer {

class Camera;

/// @brief Node operation in a read-only CSG expression DAG.
enum class CsgOp : uint8_t {
    Leaf = 0,
    Union = 1,
    Intersect = 2,
    Difference = 3, // left - right
};

/// @brief One expression node.
///
/// Leaf nodes use @ref leaf. Binary nodes are indices into CsgDesc::nodes;
/// Difference computes left minus right. Indices may be reused, so the
/// submitted graph is a DAG rather than necessarily a tree.
struct CsgNode {
    CsgOp op = CsgOp::Leaf;
    uint32_t leaf = 0;
    uint32_t left = UINT32_MAX;
    uint32_t right = UINT32_MAX;
};

/// @brief Binds a stable leaf number to a borrowed Object entity.
///
/// instance == -1 binds object's own mesh. An instance >= 0 binds
/// object->instances[instance], including its frozen shared mesh prototype.
/// Different placements of one shared prototype must use different leaves.
struct CsgBinding {
    uint32_t leaf = 0;
    Object* object = nullptr;
    int32_t instance = -1;
};

/// @brief Complete immutable-at-submission CSG description.
struct CsgDesc {
    std::vector<CsgNode> nodes;
    uint32_t root = 0;
    std::vector<CsgBinding> bindings;
};

/// @brief Validation/setup error. A failed set never replaces the active config.
enum class CsgError {
    Ok = 0,
    EmptyExpression,
    RootOutOfRange,
    InvalidNode,
    ChildOutOfRange,
    Cycle,
    UnboundLeaf,
    DuplicateLeafBinding,
    NullObject,
    ObjectNotInScene,
    InstanceOutOfRange,
    EmptyMesh,
    UnsupportedFeature,
    UnsupportedBuild,
};

const char* csgErrorName(CsgError error);

/// @brief Per-pixel selected CSG boundary and picking attribution.
struct CsgWinner {
    bool valid = false;
    uint32_t leaf = 0;
    int32_t triangle = -1;
    uint32_t slot = 0;
    bool flipNormal = false;
    double depth = 0.0;
    uint16_t depth16 = 65535;
    Object* object = nullptr;
    const Object* mesh = nullptr;
    int32_t instance = -1;
};

/// @brief Validated expression plus per-frame screen-space CSG evaluation.
///
/// The expression nodes and leaf numbering are copied and owned here. Object
/// pointers are borrowed: callers must keep the bound Object alive until the
/// expression is cleared/replaced and no Scene rendering is using it. For an
/// instance binding, the owner Object and the indexed entry in its instances
/// vector must remain valid; changing the instance transform between frames is
/// supported, while removing/reordering instances requires resubmission.
class CsgEngine {
public:
    CsgEngine();

    /// Validate @p desc completely, then atomically replace the active config.
    /// @p sceneObjects optionally verifies that borrowed owners are in Scene.
    CsgError set(const CsgDesc& desc, const std::vector<Object*>* sceneObjects = nullptr);

    /// Remove the expression. Rendering immediately returns to the old path.
    void clear();

    bool active() const { return active_; }
    CsgError lastError() const { return lastError_; }

    /// Stable leaf for an Object's own mesh (-1) or one of its instances.
    uint32_t leafForDraw(const Object* object, int32_t instance) const;
    bool isBoundObject(const Object* object) const;

    // Scene builds all source triangles once in prepareFrame().
    void prepareFrame(const Camera* camera, int screenWidth, int screenHeight,
                      const float cameraMatrix[9]);

    /// Evaluate rows [yMin,yMax) and build the per-pixel draw-token mask.
    void evaluateBand(int yMin, int yMax);

    const uint32_t* maskData() const { return acceptMask_.data(); }
    int maskWidth() const { return width_; }
    int maskHeight() const { return height_; }

    /// Slot used to correlate a Scene RenderTri with a CSG source triangle.
    int slotFor(uint32_t leaf, int32_t sourceTriangle) const;

    /// Tokens are 2*slot+1 (normal) and 2*slot+2 (inward/flipped normal).
    bool tokenUsed(uint32_t token) const {
        return token < usedTokens_.size() && usedTokens_[token] != 0;
    }

    const CsgWinner* winnerAt(int x, int y) const;

private:
    struct StoredBinding {
        uint32_t leaf = 0;
        Object* object = nullptr;
        int32_t instance = -1;
#if JET_MESH_INSTANCING
        Object::SharedMesh retainedMesh;
#endif
    };

    struct CoverTriangle {
        uint32_t slot = 0;
        uint32_t leaf = 0;
        int32_t triangle = -1;
        long long x[3] = {};
        long long y[3] = {};
        double z[3] = {};
        int direction = 1;
    };

    struct Slot {
        uint32_t leaf = 0;
        int32_t triangle = -1;
        Object* object = nullptr;
        const Object* mesh = nullptr;
        int32_t instance = -1;
    };

    CsgError validateBinding(const CsgBinding& binding,
                             const std::vector<Object*>* sceneObjects) const;
    bool expressionValue(size_t node, const std::map<uint32_t, int>& winding) const;
    bool rootValue(const std::map<uint32_t, int>& winding) const;

    bool active_ = false;
    CsgError lastError_ = CsgError::Ok;
    std::vector<CsgNode> nodes_;
    uint32_t root_ = 0;
    std::map<uint32_t, StoredBinding> bindings_;
    std::map<std::pair<const Object*, int32_t>, uint32_t> drawToLeaf_;
    std::map<uint64_t, uint32_t> sourceToSlot_;
    std::vector<Slot> slots_;
    std::vector<CoverTriangle> coverTriangles_;

    int width_ = 0;
    int height_ = 0;
    double frameNear_ = 0.0;
    double frameFar_ = 1.0e30;
    std::vector<uint32_t> acceptMask_;
    std::vector<CsgWinner> winners_;
    std::vector<unsigned char> usedTokens_;
};

} // namespace Renderer

#endif // JET_CSG_HPP
