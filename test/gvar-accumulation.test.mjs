import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { VariationProcessor } from '../src/variationprocessor.mjs';
import Glyph from '../src/glyph.mjs';

// Root cause of the bug fixed together with these tests:
//
// 1. Order dependence: for simple (non-composite) gvar glyphs, each tuple's
//    IUP (interpolatePoints) used the already-transformed points - i.e. the
//    outline shifted by all previously applied tuples - as its interpolation
//    reference. The inferred deltas of later tuples therefore depended on
//    which tuples happened to be stored before them, and permuting the tuple
//    array changed the resulting outline. Per the OpenType spec, every tuple
//    must be interpolated against the original, unmodified outline, and the
//    per-tuple contributions summed afterwards.
//
// 2. Premature rounding: each tuple's contribution was rounded to integers
//    immediately after the tuple was applied. Small fractional contributions
//    (e.g. 0.25 + 0.25) were rounded away individually instead of summing to
//    a visible displacement. The contributions must be accumulated unrounded
//    and each final coordinate rounded exactly once with Math.round.
//
// All expected values below were computed by hand from the tuple data, not
// derived from any other code path of the library.

const AXES = [{ tag: 'wght', minValue: 0, defaultValue: 0, maxValue: 1 }];

function makeFont(glyphVariations) {
    return {
        tables: {
            fvar: { axes: AXES, instances: [] },
            gvar: { glyphVariations, sharedTuples: [] }
        }
    };
}

function makeProcessor(glyphVariations = []) {
    return new VariationProcessor(makeFont(glyphVariations));
}

// The triangle outline from the bug report: (0,0), (50,100), (100,0).
function trianglePoints() {
    return [
        { x: 0, y: 0, onCurve: true, lastPointOfContour: false },
        { x: 50, y: 100, onCurve: true, lastPointOfContour: false },
        { x: 100, y: 0, onCurve: true, lastPointOfContour: true }
    ];
}

// Tuple A: touches points 0 and 2 with x deltas 0 and 100.
function tupleA() {
    return { peakTuple: [1], privatePoints: [0, 2], deltas: [0, 100], deltasY: [0, 0] };
}

// Tuple B: touches points 0 and 1 with x deltas 0 and 50.
function tupleB() {
    return { peakTuple: [1], privatePoints: [0, 1], deltas: [0, 50], deltasY: [0, 0] };
}

function xy(points) {
    return points.map(p => [p.x, p.y]);
}

// Hand-computed per-tuple contributions for tuple A and B at wght=1 (factor 1):
//
// Tuple A anchors point 0 (x 0 -> 0) and point 2 (x 100 -> 200). Point 1 lies
// between the anchors in original x, so its inferred position is
// 0 + (50 - 0) * (200 - 0) / (100 - 0) = 100, i.e. deltas x = [0, 50, 100].
// Both anchors keep y = 0, so point 1 keeps y = 100.
//
// Tuple B anchors point 0 (x 0 -> 0) and point 1 (x 50 -> 100). Point 2 lies
// past the last anchor (wrap-around segment), so it is extrapolated:
// 100 + (100 - 50) = 150, i.e. deltas x = [0, 50, 50]. y is untouched.
//
// Summed on the original outline: x = [0, 150, 250], y = [0, 100, 0].
const EXPECTED_AB = [[0, 0], [150, 100], [250, 0]];

describe('gvar tuple accumulation', function() {
    it('produces the same outline regardless of tuple storage order', function() {
        const processor = makeProcessor();

        const ab = processor.applyTupleVariationStore(
            { headers: [tupleA(), tupleB()], sharedPoints: [] },
            trianglePoints(), { wght: 1 }, 'gvar');
        assert.deepEqual(xy(ab), EXPECTED_AB);

        const ba = processor.applyTupleVariationStore(
            { headers: [tupleB(), tupleA()], sharedPoints: [] },
            trianglePoints(), { wght: 1 }, 'gvar');
        assert.deepEqual(xy(ba), EXPECTED_AB);
    });

    it('feeds the same result through getTransform points and path', function() {
        const glyphVariations = [{ headers: [tupleB(), tupleA()], sharedPoints: [] }];
        const processor = makeProcessor(glyphVariations);
        const glyph = new Glyph({ index: 0, points: trianglePoints() });

        const transformed = processor.getTransform(glyph, { wght: 1 });
        assert.deepEqual(xy(transformed.points), EXPECTED_AB);

        // getPath starts the contour at its last on-curve point (250, 0).
        assert.deepEqual(transformed.path.commands, [
            { type: 'M', x: 250, y: 0 },
            { type: 'L', x: 0, y: 0 },
            { type: 'L', x: 150, y: 100 },
            { type: 'L', x: 250, y: 0 },
            { type: 'Z' }
        ]);
    });

    it('sums unrounded contributions and rounds each coordinate once', function() {
        const processor = makeProcessor();
        // Two tuples, each shifting every point by x +1 at peak 1.
        const makeTuple = () => ({
            peakTuple: [1], privatePoints: [], deltas: [1, 1, 1], deltasY: [0, 0, 0]
        });
        const store = { headers: [makeTuple(), makeTuple()], sharedPoints: [] };

        // At wght = 0.25 each tuple contributes 0.25; the sum 0.5 must be
        // rounded once to 1, not rounded away per tuple (0 + 0).
        const result = processor.applyTupleVariationStore(store, trianglePoints(), { wght: 0.25 }, 'gvar');
        assert.deepEqual(xy(result), [[1, 0], [51, 100], [101, 0]]);
    });

    it('sums positive and negative contributions before rounding', function() {
        const processor = makeProcessor();
        const plus = { peakTuple: [1], privatePoints: [], deltas: [3, 3, 3], deltasY: [0, 0, 0] };
        const minus = { peakTuple: [1], privatePoints: [], deltas: [-1, -1, -1], deltasY: [0, 0, 0] };
        const store = { headers: [plus, minus], sharedPoints: [] };

        // At wght = 0.5 the contributions are +1.5 and -0.5, summing to +1.
        // Rounding per tuple would yield 2 + 0 = 2 instead.
        const result = processor.applyTupleVariationStore(store, trianglePoints(), { wght: 0.5 }, 'gvar');
        assert.deepEqual(xy(result), [[1, 0], [51, 100], [101, 0]]);
    });

    it('keeps contours without touched points untouched', function() {
        const processor = makeProcessor();
        const points = [
            { x: 0, y: 0, onCurve: true, lastPointOfContour: false },
            { x: 50, y: 0, onCurve: true, lastPointOfContour: false },
            { x: 100, y: 0, onCurve: true, lastPointOfContour: true },
            { x: 0, y: 100, onCurve: true, lastPointOfContour: false },
            { x: 50, y: 100, onCurve: true, lastPointOfContour: false },
            { x: 100, y: 100, onCurve: true, lastPointOfContour: true }
        ];
        // Only points 0 and 2 (first contour) are touched, both by x +10.
        // Point 1 is inferred: 10 + (50 - 0) * (110 - 10) / (100 - 0) = 60.
        // The second contour has no touched points and must not move.
        const tuple = { peakTuple: [1], privatePoints: [0, 2], deltas: [10, 10], deltasY: [0, 0] };
        const result = processor.applyTupleVariationStore(
            { headers: [tuple], sharedPoints: [] }, points, { wght: 1 }, 'gvar');
        assert.deepEqual(xy(result), [
            [10, 0], [60, 0], [110, 0],
            [0, 100], [50, 100], [100, 100]
        ]);
    });

    it('shifts a whole contour when only one of its points is touched', function() {
        const processor = makeProcessor();
        const points = [
            { x: 0, y: 0, onCurve: true, lastPointOfContour: false },
            { x: 100, y: 0, onCurve: true, lastPointOfContour: true },
            { x: 0, y: 100, onCurve: true, lastPointOfContour: false },
            { x: 50, y: 100, onCurve: true, lastPointOfContour: false },
            { x: 100, y: 100, onCurve: true, lastPointOfContour: true }
        ];
        // Only point 3 (second contour) is touched, by (+7, +3). The whole
        // second contour shifts; the first contour stays put.
        const tuple = { peakTuple: [1], privatePoints: [3], deltas: [7], deltasY: [3] };
        const result = processor.applyTupleVariationStore(
            { headers: [tuple], sharedPoints: [] }, points, { wght: 1 }, 'gvar');
        assert.deepEqual(xy(result), [
            [0, 0], [100, 0],
            [7, 103], [57, 103], [107, 103]
        ]);
    });

    it('infers the wrap-around segment between last and first anchor', function() {
        const processor = makeProcessor();
        const points = [
            { x: 0, y: 0, onCurve: true, lastPointOfContour: false },
            { x: 25, y: 0, onCurve: true, lastPointOfContour: false },
            { x: 50, y: 0, onCurve: true, lastPointOfContour: false },
            { x: 100, y: 0, onCurve: true, lastPointOfContour: true }
        ];
        // Anchors are points 1 (x 25 -> 35) and 2 (x 50 -> 70). Points 3 and 0
        // form the wrap-around segment and are extrapolated from the anchors:
        // point 3: 100 + (70 - 50) = 120; point 0: 0 + (35 - 25) = 10.
        const tuple = { peakTuple: [1], privatePoints: [1, 2], deltas: [10, 20], deltasY: [0, 0] };
        const result = processor.applyTupleVariationStore(
            { headers: [tuple], sharedPoints: [] }, points, { wght: 1 }, 'gvar');
        assert.deepEqual(xy(result), [[10, 0], [35, 0], [70, 0], [120, 0]]);
    });

    it('infers zero delta between anchors with equal original coordinates', function() {
        const processor = makeProcessor();
        // Anchors 0 and 2 both have original y = 0 but different y deltas
        // (10 and 30), so the untouched point 1 gets an inferred y delta of 0
        // and keeps its original y = 100.
        const tuple = { peakTuple: [1], privatePoints: [0, 2], deltas: [0, 0], deltasY: [10, 30] };
        const result = processor.applyTupleVariationStore(
            { headers: [tuple], sharedPoints: [] }, trianglePoints(), { wght: 1 }, 'gvar');
        assert.deepEqual(xy(result), [[0, 10], [50, 100], [100, 30]]);
    });

    it('supports sharedPoints as the tuple point selection', function() {
        const processor = makeProcessor();
        // Empty privatePoints falls back to sharedPoints; a single shared
        // point shifts the whole contour by its delta.
        const tuple = { peakTuple: [1], privatePoints: [], deltas: [5], deltasY: [-5] };
        const result = processor.applyTupleVariationStore(
            { headers: [tuple], sharedPoints: [1] }, trianglePoints(), { wght: 1 }, 'gvar');
        assert.deepEqual(xy(result), [[5, -5], [55, 95], [105, -5]]);
    });

    it('does not borrow touched points across contours or tuples', function() {
        const processor = makeProcessor();
        const points = [
            { x: 0, y: 0, onCurve: true, lastPointOfContour: false },
            { x: 100, y: 0, onCurve: true, lastPointOfContour: true },
            { x: 0, y: 100, onCurve: true, lastPointOfContour: false },
            { x: 100, y: 100, onCurve: true, lastPointOfContour: true }
        ];
        // Tuple 1 touches only point 0 (contour 1) with x +10: point 1 is
        // extrapolated to 110, contour 2 untouched. Tuple 2 touches only
        // point 2 (contour 2) with x +20 and must be interpolated against the
        // original outline, not the one shifted by tuple 1: point 3 is
        // extrapolated from the original 0/100 to 120, contour 1 keeps only
        // its own +10.
        const tuple1 = { peakTuple: [1], privatePoints: [0], deltas: [10], deltasY: [0] };
        const tuple2 = { peakTuple: [1], privatePoints: [2], deltas: [20], deltasY: [0] };
        const result = processor.applyTupleVariationStore(
            { headers: [tuple1, tuple2], sharedPoints: [] }, points, { wght: 1 }, 'gvar');
        assert.deepEqual(xy(result), [
            [10, 0], [110, 0],
            [20, 100], [120, 100]
        ]);
    });

    it('returns the original outline when no tuple is active', function() {
        const processor = makeProcessor();
        const store = { headers: [tupleA(), tupleB()], sharedPoints: [] };
        // wght = 0 is the axis default, so every tuple factor is 0.
        const result = processor.applyTupleVariationStore(store, trianglePoints(), { wght: 0 }, 'gvar');
        assert.deepEqual(xy(result), [[0, 0], [50, 100], [100, 0]]);
    });

    it('does not modify the input points, tuples or glyph', function() {
        const glyphVariations = [{ headers: [tupleA(), tupleB()], sharedPoints: [] }];
        const processor = makeProcessor(glyphVariations);
        const points = trianglePoints();
        const glyph = new Glyph({ index: 0, points });

        const pointsSnapshot = JSON.stringify(points);
        const storeSnapshot = JSON.stringify(glyphVariations);

        processor.applyTupleVariationStore(glyphVariations[0], points, { wght: 1 }, 'gvar');
        processor.getTransform(glyph, { wght: 1 });

        assert.equal(JSON.stringify(points), pointsSnapshot);
        assert.equal(JSON.stringify(glyphVariations), storeSnapshot);
        assert.equal(glyph.points, points);
        assert.deepEqual(xy(glyph.points), [[0, 0], [50, 100], [100, 0]]);
    });

    it('is stable when alternating between axis coordinates', function() {
        const glyphVariations = [{ headers: [tupleA(), tupleB()], sharedPoints: [] }];
        const processor = makeProcessor(glyphVariations);
        const glyph = new Glyph({ index: 0, points: trianglePoints() });

        const first = xy(processor.getTransform(glyph, { wght: 1 }).points);
        assert.deepEqual(first, EXPECTED_AB);

        // Hand-computed at wght = 0.5 (factor 0.5 for both tuples):
        // A contributes x deltas [0, 25, 50], B contributes [0, 25, 25].
        const mid = xy(processor.getTransform(glyph, { wght: 0.5 }).points);
        assert.deepEqual(mid, [[0, 0], [100, 100], [175, 0]]);

        // Querying wght = 1 again must reproduce the first result exactly.
        const again = xy(processor.getTransform(glyph, { wght: 1 }).points);
        assert.deepEqual(again, first);

        // ... and match a fresh processor on a fresh glyph.
        const fresh = makeProcessor(glyphVariations)
            .getTransform(new Glyph({ index: 0, points: trianglePoints() }), { wght: 1 });
        assert.deepEqual(xy(fresh.points), first);
    });
});
