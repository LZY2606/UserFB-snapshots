
#include "ShapeDistanceIndex.h"

#include <algorithm>
#include <cmath>
#include <cfloat>

#include "BitmapRef.hpp"
#include "SDFTransformation.h"
#include "generator-config.h"
#include "DistanceMapping.h"
#include "msdf-error-correction.h"

namespace msdfgen {

namespace {

const double HUGE_BOX = 1e240;

void initBox(double &l, double &b, double &r, double &t) {
    l = b = HUGE_BOX;
    r = t = -HUGE_BOX;
}

void expandBox(double &l, double &b, double &r, double &t, Point2 p) {
    if (p.x < l) l = p.x;
    if (p.y < b) b = p.y;
    if (p.x > r) r = p.x;
    if (p.y > t) t = p.y;
}

void mergeBox(double &l, double &b, double &r, double &t, double ol, double ob, double orr, double ot) {
    if (ol < l) l = ol;
    if (ob < b) b = ob;
    if (orr > r) r = orr;
    if (ot > t) t = ot;
}

} // namespace

ShapeDistanceIndex::ShapeDistanceIndex(const Shape &source) {
    shape.inverseYAxis = source.inverseYAxis;
    shape.contours = source.contours; // deep copy through EdgeHolder clones
    build();
}

ShapeDistanceIndex::ShapeDistanceIndex(const ShapeDistanceIndex &orig) {
    shape.inverseYAxis = orig.shape.inverseYAxis;
    shape.contours = orig.shape.contours;
    build();
}

#ifdef MSDFGEN_USE_CPP11
ShapeDistanceIndex::ShapeDistanceIndex(ShapeDistanceIndex &&orig)
    : shape(std::move(orig.shape)), edgeRecords(std::move(orig.edgeRecords)), leafOrder(std::move(orig.leafOrder)),
      nodes(std::move(orig.nodes)), rootNode(orig.rootNode) {
    // The moved snapshot reallocates its edges, so raw edge pointers and rays must be rebuilt.
    build();
}
#endif

ShapeDistanceIndex &ShapeDistanceIndex::operator=(const ShapeDistanceIndex &orig) {
    if (this != &orig) {
        shape.inverseYAxis = orig.shape.inverseYAxis;
        shape.contours = orig.shape.contours;
        build();
    }
    return *this;
}

#ifdef MSDFGEN_USE_CPP11
ShapeDistanceIndex &ShapeDistanceIndex::operator=(ShapeDistanceIndex &&orig) {
    if (this != &orig) {
        shape = std::move(orig.shape);
        build();
    }
    return *this;
}
#endif

ShapeDistanceIndex::~ShapeDistanceIndex() { }

const Shape &ShapeDistanceIndex::getShape() const {
    return shape;
}

int ShapeDistanceIndex::edgeCount() const {
    return (int) edgeRecords.size();
}

int ShapeDistanceIndex::nodeCount() const {
    return (int) nodes.size();
}

void ShapeDistanceIndex::build() {
    edgeRecords.clear();
    leafOrder.clear();
    nodes.clear();
    rootNode = -1;

    int totalEdges = shape.edgeCount();
    edgeRecords.reserve(totalEdges);
    for (int c = 0; c < (int) shape.contours.size(); ++c) {
        const Contour &contour = shape.contours[c];
        int count = (int) contour.edges.size();
        for (int e = 0; e < count; ++e) {
            const EdgeSegment *edge = contour.edges[e];
            const EdgeSegment *prevEdge = contour.edges[(e+count-1)%count];
            const EdgeSegment *nextEdge = contour.edges[(e+1)%count];

            EdgeRecord record;
            record.edge = edge;
            record.contourIndex = c;
            record.edgeIndex = e;
            double l = HUGE_BOX, b = HUGE_BOX, r = -HUGE_BOX, t = -HUGE_BOX;
            edge->bound(l, b, r, t);
            record.l = l, record.b = b, record.r = r, record.t = t;

            // Unit endpoint tangents and corner bisectors, computed exactly as in the edge selector.
            Point2 a = edge->point(0), bPoint = edge->point(1);
            Vector2 aDir = edge->direction(0).normalize(true);
            Vector2 bDir = edge->direction(1).normalize(true);
            Vector2 prevOut = prevEdge->direction(1).normalize(true);
            Vector2 nextIn = nextEdge->direction(0).normalize(true);
            record.rayAOrigin = a;
            record.aDir = aDir;
            record.mA = (prevOut+aDir).normalize(true);
            record.rayBOrigin = bPoint;
            record.bDir = bDir;
            record.mB = (bDir+nextIn).normalize(true);

            edgeRecords.push_back(record);
        }
    }

    if (edgeRecords.empty())
        return;

    // A binary tree over L leaves has at most 2L-1 nodes; reserve up front so node references
    // taken during recursive construction are never invalidated by reallocation.
    nodes.reserve(2*(int) edgeRecords.size()+1);
    leafOrder.reserve(edgeRecords.size());

    std::vector<int> order(edgeRecords.size());
    for (int i = 0; i < (int) order.size(); ++i)
        order[i] = i;

    struct Builder {
        ShapeDistanceIndex &self;

        Builder(ShapeDistanceIndex &self) : self(self) { }

        void unionBounds(Node &node) {
            initBox(node.l, node.b, node.r, node.t);
            for (int i = node.left; i < node.left+node.right; ++i) {
                const EdgeRecord &rec = self.edgeRecords[self.leafOrder[i]];
                mergeBox(node.l, node.b, node.r, node.t, rec.l, rec.b, rec.r, rec.t);
            }
        }

        int buildRecursive(int *begin, int *end, int depth) {
            int count = (int)(end-begin);
            int nodeIndex = (int) self.nodes.size();
            self.nodes.push_back(Node());
            Node &node = self.nodes[nodeIndex];

            const int LEAF_CAPACITY = 6;
            if (count <= LEAF_CAPACITY || depth > 64) {
                node.leaf = true;
                node.left = (int) self.leafOrder.size();
                node.right = count;
                for (const int *it = begin; it != end; ++it)
                    self.leafOrder.push_back(*it);
                unionBounds(node);
                return nodeIndex;
            }

            double cl = HUGE_BOX, cb = HUGE_BOX, cr = -HUGE_BOX, ct = -HUGE_BOX;
            for (const int *it = begin; it != end; ++it) {
                const EdgeRecord &rec = self.edgeRecords[*it];
                expandBox(cl, cb, cr, ct, Point2(.5*(rec.l+rec.r), .5*(rec.b+rec.t)));
            }
            int axis = (cr-cl) >= (ct-cb) ? 0 : 1;
            std::sort(begin, end, [this, axis](int x, int y) {
                const EdgeRecord &a = self.edgeRecords[x];
                const EdgeRecord &b = self.edgeRecords[y];
                double ca = axis ? .5*(a.b+a.t) : .5*(a.l+a.r);
                double cb2 = axis ? .5*(b.b+b.t) : .5*(b.l+b.r);
                return ca < cb2;
            });
            int *mid = begin+count/2;
            node.leaf = false;
            node.left = buildRecursive(begin, mid, depth+1);
            node.right = buildRecursive(mid, end, depth+1);
            const Node &leftNode = self.nodes[node.left];
            const Node &rightNode = self.nodes[node.right];
            node.l = std::min(leftNode.l, rightNode.l);
            node.b = std::min(leftNode.b, rightNode.b);
            node.r = std::max(leftNode.r, rightNode.r);
            node.t = std::max(leftNode.t, rightNode.t);
            return nodeIndex;
        }
    };

    Builder builder(*this);
    rootNode = builder.buildRecursive(order.data(), order.data()+order.size(), 0);
}

// ---- Queries -------------------------------------------------------------------------------

double ShapeDistanceIndex::sdfDistance(Point2 origin, bool overlapSupport, DistanceQueryStats *stats) const {
    if (edgeRecords.empty())
        return -DBL_MAX;
    if (overlapSupport) {
        IndexedSDFQueryRunner<OverlappingContourCombiner<TrueDistanceSelector> > runner(*this, shape);
        return runner.run(origin, stats);
    } else {
        IndexedSDFQueryRunner<SimpleContourCombiner<TrueDistanceSelector> > runner(*this, shape);
        return runner.run(origin, stats);
    }
}

MultiAndTrueDistance ShapeDistanceIndex::mtsdfDistance(Point2 origin, bool overlapSupport, DistanceQueryStats *stats) const {
    if (edgeRecords.empty()) {
        MultiAndTrueDistance empty;
        empty.r = empty.g = empty.b = empty.a = -DBL_MAX;
        return empty;
    }
    if (overlapSupport) {
        IndexedMTQueryRunner<OverlappingContourCombiner<MultiAndTrueDistanceSelector> > runner(*this, shape);
        return runner.run(origin, stats);
    } else {
        IndexedMTQueryRunner<SimpleContourCombiner<MultiAndTrueDistanceSelector> > runner(*this, shape);
        return runner.run(origin, stats);
    }
}

// ---- Indexed generation entry points -------------------------------------------------------

namespace {

template <typename DistanceType>
class IndexedPixelConversion;

template <>
class IndexedPixelConversion<double> {
    DistanceMapping mapping;
public:
    typedef BitmapSection<float, 1> BitmapSectionType;
    explicit IndexedPixelConversion(DistanceMapping mapping) : mapping(mapping) { }
    void operator()(float *pixels, double distance) const {
        *pixels = float(mapping(distance));
    }
};

template <>
class IndexedPixelConversion<MultiAndTrueDistance> {
    DistanceMapping mapping;
public:
    typedef BitmapSection<float, 4> BitmapSectionType;
    explicit IndexedPixelConversion(DistanceMapping mapping) : mapping(mapping) { }
    void operator()(float *pixels, const MultiAndTrueDistance &distance) const {
        pixels[0] = float(mapping(distance.r));
        pixels[1] = float(mapping(distance.g));
        pixels[2] = float(mapping(distance.b));
        pixels[3] = float(mapping(distance.a));
    }
};

} // namespace

void generateSDFIndexed(const BitmapSection<float, 1> &output, const ShapeDistanceIndex &index, const SDFTransformation &transformation,
                        const GeneratorConfig &config, DistanceQueryStats *stats) {
    IndexedPixelConversion<double> conversion(transformation.distanceMapping);
    BitmapSection<float, 1> target = output;
    target.reorient(index.getShape().getYAxisOrientation());
    // One per-generation working set; destroyed with the generation and never stored in the index.
    if (config.overlapSupport) {
        IndexedSDFQueryRunner<OverlappingContourCombiner<TrueDistanceSelector> > runner(index, index.getShape());
        DistanceQueryStats local;
        DistanceQueryStats *active = stats ? stats : &local;
        for (int y = 0; y < target.height; ++y)
            for (int x = 0; x < target.width; ++x) {
                Point2 p = transformation.unproject(Point2(x+.5, y+.5));
                conversion(target(x, y), runner.run(p, active));
            }
    } else {
        IndexedSDFQueryRunner<SimpleContourCombiner<TrueDistanceSelector> > runner(index, index.getShape());
        DistanceQueryStats local;
        DistanceQueryStats *active = stats ? stats : &local;
        for (int y = 0; y < target.height; ++y)
            for (int x = 0; x < target.width; ++x) {
                Point2 p = transformation.unproject(Point2(x+.5, y+.5));
                conversion(target(x, y), runner.run(p, active));
            }
    }
}

void generateMTSDFIndexed(const BitmapSection<float, 4> &output, const ShapeDistanceIndex &index, const SDFTransformation &transformation,
                          const MSDFGeneratorConfig &config, DistanceQueryStats *stats) {
    IndexedPixelConversion<MultiAndTrueDistance> conversion(transformation.distanceMapping);
    BitmapSection<float, 4> target = output;
    target.reorient(index.getShape().getYAxisOrientation());
    if (config.overlapSupport) {
        IndexedMTQueryRunner<OverlappingContourCombiner<MultiAndTrueDistanceSelector> > runner(index, index.getShape());
        DistanceQueryStats local;
        DistanceQueryStats *active = stats ? stats : &local;
        for (int y = 0; y < target.height; ++y)
            for (int x = 0; x < target.width; ++x) {
                Point2 p = transformation.unproject(Point2(x+.5, y+.5));
                conversion(target(x, y), runner.run(p, active));
            }
    } else {
        IndexedMTQueryRunner<SimpleContourCombiner<MultiAndTrueDistanceSelector> > runner(index, index.getShape());
        DistanceQueryStats local;
        DistanceQueryStats *active = stats ? stats : &local;
        for (int y = 0; y < target.height; ++y)
            for (int x = 0; x < target.width; ++x) {
                Point2 p = transformation.unproject(Point2(x+.5, y+.5));
                conversion(target(x, y), runner.run(p, active));
            }
    }
    // Error correction, projections, ranges and Y orientation keep using the original, unchanged algorithm.
    msdfErrorCorrection(target, index.getShape(), transformation, config);
}

void generateSDFIndexed(const BitmapSection<float, 1> &output, const ShapeDistanceIndex &index, const Projection &projection, Range range,
                        const GeneratorConfig &config, DistanceQueryStats *stats) {
    generateSDFIndexed(output, index, SDFTransformation(projection, range), config, stats);
}

void generateMTSDFIndexed(const BitmapSection<float, 4> &output, const ShapeDistanceIndex &index, const Projection &projection, Range range,
                          const MSDFGeneratorConfig &config, DistanceQueryStats *stats) {
    generateMTSDFIndexed(output, index, SDFTransformation(projection, range), config, stats);
}

}
