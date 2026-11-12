
#pragma once

#include <vector>
#include "Vector2.hpp"
#include "Shape.h"
#include "BitmapRef.hpp"
#include "SDFTransformation.h"
#include "generator-config.h"
#include "edge-selectors.h"
#include "contour-combiners.h"

namespace msdfgen {

/// Counters of the work performed by a single ShapeDistanceIndex query (or a whole generation pass).
struct DistanceQueryStats {
    /// Number of BVH node prune decisions actually evaluated.
    long long nodeTests;
    /// Number of candidate edges handed over to an edge selector.
    long long leafEdgeVisits;
    /// Number of calls to an edge segment's signedDistance method. Estimates are never counted here.
    long long exactDistanceCalls;

    DistanceQueryStats() : nodeTests(0), leafEdgeVisits(0), exactDistanceCalls(0) { }
    void reset() {
        nodeTests = 0;
        leafEdgeVisits = 0;
        exactDistanceCalls = 0;
    }
    DistanceQueryStats &operator+=(const DistanceQueryStats &other) {
        nodeTests += other.nodeTests;
        leafEdgeVisits += other.leafEdgeVisits;
        exactDistanceCalls += other.exactDistanceCalls;
        return *this;
    }
};

/**
 * A reusable spatial index over an owned snapshot of a Shape.
 * The constructor deep-copies every edge (including its color), the contour ordering and the
 * Y-axis orientation, so the index remains valid after the source shape is edited or destroyed.
 *
 * Pruning is conservative and exact:
 *  - The true signed distance (alpha) is pruned against a tight bounding-volume hierarchy of the
 *    finite curves only.
 *  - The multi-channel perpendicular pseudo-distance is governed by unbounded endpoint tangent
 *    rays, so it can never be pruned by a finite curve box. Instead every node decision applies the
 *    same half-plane gated tangent-line test as the edge selector (using only dot/cross products,
 *    never an estimated distance), which keeps sharp-corner extension ridges and color boundaries
 *    exact at arbitrary distance.
 * All per-query working state lives outside the immutable index, so distant or interleaved queries
 * never borrow stale minima from previous queries.
 */
class ShapeDistanceIndex {

public:
    /// Builds the index and its owning snapshot from the given shape. The source shape does not need to outlive the index.
    explicit ShapeDistanceIndex(const Shape &shape);
    ShapeDistanceIndex(const ShapeDistanceIndex &orig);
#ifdef MSDFGEN_USE_CPP11
    ShapeDistanceIndex(ShapeDistanceIndex &&orig);
#endif
    ShapeDistanceIndex &operator=(const ShapeDistanceIndex &orig);
#ifdef MSDFGEN_USE_CPP11
    ShapeDistanceIndex &operator=(ShapeDistanceIndex &&orig);
#endif
    ~ShapeDistanceIndex();

    /// The owned snapshot of the shape, valid for the whole lifetime of the index.
    const Shape &getShape() const;
    /// Number of indexed edge segments.
    int edgeCount() const;
    /// Number of nodes in the hierarchy (grows linearly with the number of edges).
    int nodeCount() const;

    /// True (alpha) signed distance, identical to the per-edge Simple/Overlapping contour combiner result.
    double sdfDistance(Point2 origin, bool overlapSupport = true, DistanceQueryStats *stats = nullptr) const;
    /// Multi-channel perpendicular distance with the true signed distance in the alpha channel.
    MultiAndTrueDistance mtsdfDistance(Point2 origin, bool overlapSupport = true, DistanceQueryStats *stats = nullptr) const;

private:
    struct Node;
    struct EdgeRecord;

    Shape shape;
    std::vector<EdgeRecord> edgeRecords;
    std::vector<int> leafOrder;
    std::vector<Node> nodes;
    int rootNode;

    void build();

    template <class ContourCombiner>
    friend struct IndexedQueryRunnerBase;
    template <class ContourCombiner>
    friend struct IndexedSDFQueryRunner;
    template <class ContourCombiner>
    friend struct IndexedMTQueryRunner;

};

/// Indexed single-channel SDF generation. Mirrors generateSDF, taking an immutable index and optional per-generation statistics.
void generateSDFIndexed(const BitmapSection<float, 1> &output, const ShapeDistanceIndex &index, const SDFTransformation &transformation,
                        const GeneratorConfig &config = GeneratorConfig(), DistanceQueryStats *stats = nullptr);
void generateSDFIndexed(const BitmapSection<float, 1> &output, const ShapeDistanceIndex &index, const Projection &projection, Range range,
                        const GeneratorConfig &config = GeneratorConfig(), DistanceQueryStats *stats = nullptr);

/// Indexed multi-channel SDF with true distance in alpha. Mirrors generateMTSDF, including the unchanged error correction pass.
void generateMTSDFIndexed(const BitmapSection<float, 4> &output, const ShapeDistanceIndex &index, const SDFTransformation &transformation,
                          const MSDFGeneratorConfig &config = MSDFGeneratorConfig(), DistanceQueryStats *stats = nullptr);
void generateMTSDFIndexed(const BitmapSection<float, 4> &output, const ShapeDistanceIndex &index, const Projection &projection, Range range,
                          const MSDFGeneratorConfig &config = MSDFGeneratorConfig(), DistanceQueryStats *stats = nullptr);

}

#include "ShapeDistanceIndex.hpp"
