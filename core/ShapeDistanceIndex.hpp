
#include "ShapeDistanceIndex.h"

#include <cmath>
#include <cfloat>
#include <algorithm>

namespace msdfgen {

/// A leaf edge with its contour placement, tight curve box and the two unbounded endpoint tangent rays.
struct ShapeDistanceIndex::EdgeRecord {
    const EdgeSegment *edge;
    int contourIndex;
    int edgeIndex;
    // Tight bounding box of the finite curve.
    double l, b, r, t;
    // Endpoint tangent rays: aDir is the unit start tangent (into the edge), bDir the unit end tangent.
    // mA/mB are the unit corner bisectors used by the selector's outer domain gate.
    Point2 rayAOrigin, aDir, mA;
    Point2 rayBOrigin, bDir, mB;
};

/// A bounding-volume-hierarchy node over the finite curve boxes. Leaf nodes index a contiguous range of leafOrder.
struct ShapeDistanceIndex::Node {
    double l, b, r, t;
    int left, right; // children, or (first leafOrder entry, entry count) for a leaf
    bool leaf;
};

/**
 * Edge proxy that forwards every virtual method to the owned edge and counts each genuine
 * signedDistance invocation. The most recent result is stashed so the query runner can maintain
 * its own conservative distance radii without a second exact call.
 */
class CountedEdge : public EdgeSegment {

public:
    CountedEdge() : EdgeSegment(WHITE), inner(NULL), stats(NULL), lastValid(false) { }

    void attach(const EdgeSegment *edge, DistanceQueryStats *queryStats) {
        inner = edge;
        stats = queryStats;
        color = edge->color;
        lastValid = false;
    }

    void clearLast() { lastValid = false; }
    bool hasLast() const { return lastValid; }
    const SignedDistance &lastDistance() const { return last; }

    EdgeSegment *clone() const { return inner->clone(); }
    int type() const { return inner->type(); }
    const Point2 *controlPoints() const { return inner->controlPoints(); }
    Point2 point(double param) const { return inner->point(param); }
    Vector2 direction(double param) const { return inner->direction(param); }
    Vector2 directionChange(double param) const { return inner->directionChange(param); }
    SignedDistance signedDistance(Point2 origin, double &param) const {
        if (stats)
            ++stats->exactDistanceCalls;
        SignedDistance result = inner->signedDistance(origin, param);
        last = result;
        lastValid = true;
        return result;
    }
    void distanceToPerpendicularDistance(SignedDistance &distance, Point2 origin, double param) const {
        inner->distanceToPerpendicularDistance(distance, origin, param);
    }
    int scanlineIntersections(double x[3], int dy[3], double y) const { return inner->scanlineIntersections(x, dy, y); }
    void bound(double &xMin, double &yMin, double &xMax, double &yMax) const { inner->bound(xMin, yMin, xMax, yMax); }
    void reverse() { }
    void moveStartPoint(Point2 to) { (void) to; }
    void moveEndPoint(Point2 to) { (void) to; }
    void splitInThirds(EdgeSegment *&part0, EdgeSegment *&part1, EdgeSegment *&part2) const {
        inner->splitInThirds(part0, part1, part2);
    }

private:
    const EdgeSegment *inner;
    DistanceQueryStats *stats;
    mutable SignedDistance last;
    mutable bool lastValid;

};

namespace index_detail {

inline double pointBoxDistance(double l, double b, double r, double t, const Point2 &p) {
    double dx = 0, dy = 0;
    if (p.x < l) dx = l-p.x;
    else if (p.x > r) dx = p.x-r;
    if (p.y < b) dy = b-p.y;
    else if (p.y > t) dy = p.y-t;
    return sqrt(dx*dx+dy*dy);
}

} // namespace index_detail

/// Shared per-query scaffolding. Working state is owned here, never by the immutable index.
template <class ContourCombiner>
struct IndexedQueryRunnerBase {

    typedef typename ContourCombiner::EdgeSelectorType EdgeSelectorType;

    const ShapeDistanceIndex &index;
    ContourCombiner contourCombiner;
    std::vector<typename EdgeSelectorType::EdgeCache> edgeCaches;
    std::vector<CountedEdge> proxies;
    std::vector<int> contourStart;
    DistanceQueryStats *stats;
    Point2 origin;

    IndexedQueryRunnerBase(const ShapeDistanceIndex &owner, const Shape &snapshot)
        : index(owner), contourCombiner(snapshot), edgeCaches(owner.edgeRecords.size()), proxies(owner.edgeRecords.size()),
          contourStart(snapshot.contours.size()+1), stats(NULL) {
        contourStart[0] = 0;
        for (int i = 0; i < (int) snapshot.contours.size(); ++i)
            contourStart[i+1] = contourStart[i]+(int) snapshot.contours[i].edges.size();
        for (int i = 0; i < (int) owner.edgeRecords.size(); ++i)
            proxies[i].attach(owner.edgeRecords[i].edge, NULL);
    }

    void beginQuery(const Point2 &p, DistanceQueryStats *queryStats) {
        stats = queryStats;
        origin = p;
        contourCombiner.reset(p);
        for (int i = 0; i < (int) proxies.size(); ++i)
            proxies[i].attach(index.edgeRecords[i].edge, stats);
    }

    void submitEdge(int rid) {
        const ShapeDistanceIndex::EdgeRecord &record = index.edgeRecords[rid];
        if (stats)
            ++stats->leafEdgeVisits;
        int start = contourStart[record.contourIndex];
        int count = contourStart[record.contourIndex+1]-start;
        int prev = start+(record.edgeIndex+count-1)%count;
        int next = start+(record.edgeIndex+1)%count;
        EdgeSelectorType &edgeSelector = contourCombiner.edgeSelector(record.contourIndex);
        proxies[rid].clearLast();
        edgeSelector.addEdge(edgeCaches[rid], &proxies[prev], &proxies[rid], &proxies[next]);
    }

};

/// SDF runner. Prunes with the tight finite curve box against a per-contour true-distance radius.
template <class ContourCombiner>
struct IndexedSDFQueryRunner : IndexedQueryRunnerBase<ContourCombiner> {

    std::vector<double> contourBest; // nearest accepted true distance magnitude per contour

    IndexedSDFQueryRunner(const ShapeDistanceIndex &owner, const Shape &snapshot)
        : IndexedQueryRunnerBase<ContourCombiner>(owner, snapshot), contourBest(snapshot.contours.size(), DBL_MAX) { }

    double pruningRadius() const {
        double radius = 0;
        for (int i = 0; i < (int) contourBest.size(); ++i)
            if (contourBest[i] > radius)
                radius = contourBest[i];
        return radius;
    }

    void begin(const Point2 &p, DistanceQueryStats *stats) {
        std::fill(contourBest.begin(), contourBest.end(), DBL_MAX);
        this->beginQuery(p, stats);
    }

    void processLeaf(const ShapeDistanceIndex::Node &node) {
        for (int li = node.left; li < node.left+node.right; ++li) {
            int rid = this->index.leafOrder[li];
            const ShapeDistanceIndex::EdgeRecord &record = this->index.edgeRecords[rid];
            double boxDist = index_detail::pointBoxDistance(record.l, record.b, record.r, record.t, this->origin);
            if (boxDist > contourBest[record.contourIndex])
                continue; // cannot improve this contour's true distance
            this->submitEdge(rid);
            if (this->proxies[rid].hasLast())
                contourBest[record.contourIndex] = std::min(contourBest[record.contourIndex], fabs(this->proxies[rid].lastDistance().distance));
        }
    }

    void traverse(int nodeIndex) {
        const ShapeDistanceIndex::Node &node = this->index.nodes[nodeIndex];
        if (this->stats)
            ++this->stats->nodeTests;
        if (node.leaf) {
            processLeaf(node);
            return;
        }
        double radius = pruningRadius();
        double leftDist = index_detail::pointBoxDistance(this->index.nodes[node.left].l, this->index.nodes[node.left].b,
                                                          this->index.nodes[node.left].r, this->index.nodes[node.left].t, this->origin);
        double rightDist = index_detail::pointBoxDistance(this->index.nodes[node.right].l, this->index.nodes[node.right].b,
                                                           this->index.nodes[node.right].r, this->index.nodes[node.right].t, this->origin);
        // Nearer child first to tighten the per-contour radii quickly.
        if (leftDist <= rightDist) {
            if (leftDist <= radius) traverse(node.left);
            if (rightDist <= pruningRadius()) traverse(node.right);
        } else {
            if (rightDist <= radius) traverse(node.right);
            if (leftDist <= pruningRadius()) traverse(node.left);
        }
    }

    double run(const Point2 &p, DistanceQueryStats *stats) {
        begin(p, stats);
        traverse(this->index.rootNode);
        return this->contourCombiner.distance();
    }

};

/// MTSDF runner. The finite box handles alpha/true relevance; the unbounded endpoint tangent rays
/// are tested exactly (half-plane gate plus perpendicular magnitude with the selector's own sign
/// convention) against independently maintained per-channel extrema, so pseudo-distance ridges are
/// never pruned at any distance. State is per contour and per color channel.
template <class ContourCombiner>
struct IndexedMTQueryRunner : IndexedQueryRunnerBase<ContourCombiner> {

    struct ChannelState {
        double trueMag;     // nearest accepted true signed-distance magnitude (selector's minTrue distance)
        double negMag;      // magnitude of the closest-to-zero recorded negative perpendicular value
        double posMag;      // smallest recorded positive perpendicular value
    };

    // Three channels per contour, indexed [contour*3 + channel].
    std::vector<ChannelState> channels;

    IndexedMTQueryRunner(const ShapeDistanceIndex &owner, const Shape &snapshot)
        : IndexedQueryRunnerBase<ContourCombiner>(owner, snapshot),
          channels(3*snapshot.contours.size()) { }

    void begin(const Point2 &p, DistanceQueryStats *stats) {
        for (int i = 0; i < (int) channels.size(); ++i) {
            channels[i].trueMag = DBL_MAX;
            channels[i].negMag = DBL_MAX;
            channels[i].posMag = DBL_MAX;
        }
        this->beginQuery(p, stats);
    }

    // Endpoint A recorded perpendicular value: v = cross(ap, aDir).
    // Selector gates: outer dot(ap, mA) > 0 and inner dot(ap, -aDir) > 0.
    bool endpointA(const ShapeDistanceIndex::EdgeRecord &rec, double &v) const {
        Vector2 ap = this->origin-rec.rayAOrigin;
        if (dotProduct(ap, rec.mA) > 0 && dotProduct(ap, -rec.aDir) > 0) {
            v = crossProduct(ap, rec.aDir);
            return true;
        }
        return false;
    }

    // Endpoint B recorded perpendicular value: v = cross(bp, bDir).
    // Selector gates: outer -dot(bp, mB) > 0 and inner dot(bp, bDir) > 0.
    bool endpointB(const ShapeDistanceIndex::EdgeRecord &rec, double &v) const {
        Vector2 bp = this->origin-rec.rayBOrigin;
        if (-dotProduct(bp, rec.mB) > 0 && dotProduct(bp, rec.bDir) > 0) {
            v = crossProduct(bp, rec.bDir);
            return true;
        }
        return false;
    }

    static bool improvesExtremum(const ChannelState &s, double v) {
        if (v < 0)
            return fabs(v) < s.negMag;
        return v < s.posMag;
    }

    bool channelRelevant(const ChannelState &s, double boxDist, const ShapeDistanceIndex::EdgeRecord &rec) const {
        if (boxDist <= s.trueMag)
            return true; // the edge may become this channel's nearest true edge
        double v;
        // The perpendicular magnitude test against the edge's own true distance is deliberately
        // not used for exclusion: the box distance is only a lower bound on that true distance,
        // so whenever the geometry gates hold and the value improves the (looser) extrema, the
        // edge could still be accepted by the selector. Keeping it is therefore conservative.
        if (endpointA(rec, v) && improvesExtremum(s, v))
            return true;
        if (endpointB(rec, v) && improvesExtremum(s, v))
            return true;
        return false;
    }

    bool edgeRelevant(const ShapeDistanceIndex::EdgeRecord &record) {
        double boxDist = index_detail::pointBoxDistance(record.l, record.b, record.r, record.t, this->origin);
        int contour = record.contourIndex;
        EdgeColor color = record.edge->color;
        if ((color&RED) && channelRelevant(channels[contour*3+0], boxDist, record)) return true;
        if ((color&GREEN) && channelRelevant(channels[contour*3+1], boxDist, record)) return true;
        if ((color&BLUE) && channelRelevant(channels[contour*3+2], boxDist, record)) return true;
        return false;
    }

    // Update the tracked extrema with exactly the values the selector records.
    // MultiDistanceSelector records an endpoint perpendicular value v when both geometry gates
    // (computed in endpointA/endpointB) hold and fabs(v) < fabs(the edge's own true distance).
    // Using the edge's real true distance (not a box lower bound) mirrors the selector exactly,
    // so the tracked per-channel extrema are never tighter than the selector's.
    void tighten(const ShapeDistanceIndex::EdgeRecord &rec, double v, double edgeTrueDistance, EdgeColor color) {
        if (!(fabs(v) < fabs(edgeTrueDistance)))
            return;
        int contour = rec.contourIndex;
        for (int ch = 0; ch < 3; ++ch) {
            int mask = ch == 0 ? RED : ch == 1 ? GREEN : BLUE;
            if (!(color&mask))
                continue;
            ChannelState &s = channels[contour*3+ch];
            if (v < 0)
                s.negMag = std::min(s.negMag, fabs(v));
            else
                s.posMag = std::min(s.posMag, v);
        }
    }

    void processLeaf(const ShapeDistanceIndex::Node &node) {
        for (int li = node.left; li < node.left+node.right; ++li) {
            int rid = this->index.leafOrder[li];
            const ShapeDistanceIndex::EdgeRecord &record = this->index.edgeRecords[rid];
            if (!(record.edge->color&WHITE) || !edgeRelevant(record))
                continue;
            this->submitEdge(rid);
            if (!this->proxies[rid].hasLast())
                continue;
            double edgeTrueDistance = this->proxies[rid].lastDistance().distance;
            double trueMag = fabs(edgeTrueDistance);
            EdgeColor color = record.edge->color;
            int contour = record.contourIndex;
            for (int ch = 0; ch < 3; ++ch) {
                int mask = ch == 0 ? RED : ch == 1 ? GREEN : BLUE;
                if (color&mask)
                    channels[contour*3+ch].trueMag = std::min(channels[contour*3+ch].trueMag, trueMag);
            }
            double v;
            if (endpointA(record, v))
                tighten(record, v, edgeTrueDistance, color);
            if (endpointB(record, v))
                tighten(record, v, edgeTrueDistance, color);
        }
    }

    double nodeBoxDistance(int nodeIndex) const {
        const ShapeDistanceIndex::Node &node = this->index.nodes[nodeIndex];
        return index_detail::pointBoxDistance(node.l, node.b, node.r, node.t, this->origin);
    }

    void traverse(int nodeIndex) {
        const ShapeDistanceIndex::Node &node = this->index.nodes[nodeIndex];
        if (this->stats)
            ++this->stats->nodeTests;
        // Tangent pseudo-distance domains are unbounded, so internal nodes are never box-pruned;
        // the exact per-edge ray/extrema test runs at the leaves. Nearer leaves are visited first
        // so per-channel extrema tighten before distant leaves are judged.
        if (node.leaf) {
            processLeaf(node);
            return;
        }
        double leftDist = nodeBoxDistance(node.left);
        double rightDist = nodeBoxDistance(node.right);
        if (leftDist <= rightDist) {
            traverse(node.left);
            traverse(node.right);
        } else {
            traverse(node.right);
            traverse(node.left);
        }
    }

    MultiAndTrueDistance run(const Point2 &p, DistanceQueryStats *stats) {
        begin(p, stats);
        traverse(this->index.rootNode);
        return this->contourCombiner.distance();
    }

};

}
