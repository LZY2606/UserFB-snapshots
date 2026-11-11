// Regression tests for degenerate stroke inputs: duplicate points and
// exact 180-degree reversals used to produce non-finite vertices or
// drop geometry. Run with: node test/degenerate-stroke.cjs
'use strict'

var Stroke = require('../index.js')
var fs = require('fs')
var path = require('path')

var failures = []

function fail(msg) {
    failures.push(msg)
    console.error('FAIL: ' + msg)
}

function ok(cond, msg) {
    if (!cond) fail(msg)
}

function fmtPath(points) {
    return JSON.stringify(points)
}

// -- mesh sanity -------------------------------------------------------------

function checkMesh(complex, label) {
    ok(complex && Array.isArray(complex.positions) && Array.isArray(complex.cells),
        label + ': mesh has positions and cells arrays')
    var n = complex.positions.length
    complex.positions.forEach(function(p, i) {
        ok(Array.isArray(p) && p.length === 2 &&
            Number.isFinite(p[0]) && Number.isFinite(p[1]),
            label + ': position[' + i + '] is finite, got ' + JSON.stringify(p))
    })
    complex.cells.forEach(function(cell, i) {
        ok(Array.isArray(cell) && cell.length === 3 &&
            cell.every(function(idx) {
                return Number.isInteger(idx) && idx >= 0 && idx < n
            }),
            label + ': cell[' + i + '] ' + JSON.stringify(cell) +
                ' indexes within 0..' + (n - 1))
    })
}

// -- input immutability ------------------------------------------------------

function checkUnmutated(points, snapshot, label) {
    ok(JSON.stringify(points) === snapshot,
        label + ': input points were not mutated')
}

// -- mapThickness spying -----------------------------------------------------

// Builds a stroke whose mapThickness records each call's index, the
// identity of the point and of the points array.
function spiedStroke(opt, points, fn) {
    var stroke = Stroke(opt)
    var calls = []
    stroke.mapThickness = function(point, i, pts) {
        calls.push({
            i: i,
            pointIsOriginal: point === pts[i],
            arrayIsOriginal: pts === points
        })
        return fn ? fn(point, i, pts) : stroke.thickness
    }
    return { stroke: stroke, calls: calls }
}

function checkCalls(calls, expectedIdx, label) {
    var got = calls.map(function(c) { return c.i })
    ok(JSON.stringify(got) === JSON.stringify(expectedIdx),
        label + ': mapThickness called with indices ' + JSON.stringify(expectedIdx) +
            ', got ' + JSON.stringify(got))
    calls.forEach(function(c, k) {
        ok(c.pointIsOriginal && c.arrayIsOriginal,
            label + ': call ' + k + ' received original point object and points array')
    })
}

// -- 1. exact reversal, straight line ----------------------------------------

;(function() {
    var points = [[0, 0], [10, 0], [0, 0]]
    var snapshot = fmtPath(points)
    var spied = spiedStroke({ thickness: 2 }, points)
    var complex = spied.stroke.build(points)
    checkMesh(complex, 'straight reversal')
    ok(complex.positions.length === 8,
        'straight reversal: two capped quads (8 vertices), got ' +
            complex.positions.length)
    checkCalls(spied.calls, [1, 2], 'straight reversal')
    checkUnmutated(points, snapshot, 'straight reversal')
})()

// -- 2. left / right turn into a reversal ------------------------------------

;[[[0, 0], [10, 0], [10, 10], [10, 0]],   // left turn, fold back down
  [[0, 0], [10, 0], [10, -10], [10, 0]],  // right turn, fold back up
  [[0, 0], [10, 0], [0, 0], [10, 0], [20, 0]] // fold, then keep going
].forEach(function(points, k) {
    var label = 'turn-into-reversal[' + k + '] ' + fmtPath(points)
    var snapshot = fmtPath(points)
    var spied = spiedStroke({ thickness: 2 }, points)
    var complex = spied.stroke.build(points)
    checkMesh(complex, label)
    ok(complex.positions.length > 0, label + ': geometry after the fold is kept')
    checkCalls(spied.calls, points.map(function(_, i) { return i }).slice(1), label)
    checkUnmutated(points, snapshot, label)
})

// -- 3. duplicate points at start / middle / end -----------------------------

;[
    { points: [[0, 0], [0, 0], [10, 0], [10, 10]], idx: [2, 3], label: 'dup-start' },
    { points: [[0, 0], [5, 0], [5, 0], [5, 0], [10, 0]], idx: [3, 4], label: 'dup-middle' },
    { points: [[0, 0], [10, 0], [10, 0]], idx: [2], label: 'dup-end' }
].forEach(function(tc) {
    var points = tc.points
    var snapshot = fmtPath(points)
    var spied = spiedStroke({ thickness: 2 }, points)
    var complex = spied.stroke.build(points)
    checkMesh(complex, tc.label)
    checkCalls(spied.calls, tc.idx, tc.label)
    checkUnmutated(points, snapshot, tc.label)

    // geometry must match the manually de-duplicated path
    var deduped = []
    points.forEach(function(p) {
        var last = deduped[deduped.length - 1]
        if (!last || last[0] !== p[0] || last[1] !== p[1]) deduped.push(p)
    })
    var expected = Stroke({ thickness: 2 }).build(deduped)
    ok(JSON.stringify(complex) === JSON.stringify(expected),
        tc.label + ': mesh matches de-duplicated path ' + fmtPath(deduped))
})

// fewer than two distinct positions -> empty mesh
;(function() {
    var complex = Stroke({ thickness: 2 }).build([[1, 1], [1, 1], [1, 1]])
    ok(complex.positions.length === 0 && complex.cells.length === 0,
        'all-duplicate path: empty mesh, got ' + JSON.stringify(complex))
    var single = Stroke({ thickness: 2 }).build([[1, 1]])
    ok(single.positions.length === 0 && single.cells.length === 0,
        'single point: empty mesh')
})()

// -- 4. miterLimit boundary, both sides ---------------------------------------

// For a join with deflection angle theta, miterLen/halfThick = 1/cos(theta/2).
// With miterLimit 2 the critical angle is 120 degrees.
;[
    { deg: 119, expectVerts: 6, label: 'below-limit (miter)' },
    { deg: 120, expectVerts: 6, label: 'at-limit (miter)' },
    { deg: 121, expectVerts: 7, label: 'above-limit (bevel)' }
].forEach(function(tc) {
    var theta = tc.deg * Math.PI / 180
    var points = [
        [0, 0],
        [10, 0],
        [10 + 10 * Math.cos(theta), 10 * Math.sin(theta)]
    ]
    var complex = Stroke({ thickness: 2, miterLimit: 2 }).build(points)
    var label = 'miterLimit ' + tc.label + ' (' + tc.deg + ' deg)'
    checkMesh(complex, label)
    ok(complex.positions.length === tc.expectVerts,
        label + ': expected ' + tc.expectVerts + ' vertices, got ' +
            complex.positions.length)
})

// -- 5. mapThickness results drive both halves of a fold ----------------------

;(function() {
    var points = [[0, 0], [10, 0], [0, 0]]
    var spied = spiedStroke({}, points, function(point, i) {
        return i === 1 ? 2 : 6 // half-thickness 1 going out, 3 coming back
    })
    var complex = spied.stroke.build(points)
    checkMesh(complex, 'varying thickness fold')
    checkCalls(spied.calls, [1, 2], 'varying thickness fold')
    var ys = complex.positions.map(function(p) { return Math.abs(p[1]) })
    ok(ys.indexOf(1) !== -1 && ys.indexOf(3) !== -1,
        'varying thickness fold: both half-thicknesses 1 and 3 appear, got ' +
            JSON.stringify(complex.positions))
})()

// -- 6. same instance, consecutive builds -------------------------------------

;(function() {
    var fold = [[0, 0], [10, 0], [0, 0]]
    var plain = [[0, 0], [10, 0], [10, 10], [20, 10]]
    var stroke = Stroke({ thickness: 2 })

    var foldA = stroke.build(fold)
    var plainA = stroke.build(plain) // normals/flip state must not leak
    var foldB = stroke.build(fold)

    var freshPlain = Stroke({ thickness: 2 }).build(plain)
    ok(JSON.stringify(plainA) === JSON.stringify(freshPlain),
        'instance reuse: build after a reversal matches a fresh instance')
    ok(JSON.stringify(foldA) === JSON.stringify(foldB),
        'instance reuse: repeated reversal builds are identical')
})()

// -- 7. legacy inputs are bit-for-bit unchanged --------------------------------

;(function() {
    var legacy = [[0, 0], [10, 0], [10, 10], [20, 10], [20, 20]]
    var expectedDefault = {"positions":[[0,-1],[0,1],[11,-1],[9,1],[11,9],[9,11],[21,9],[19,11],[21,20],[19,20]],"cells":[[0,1,2],[2,1,3],[2,3,4],[4,3,5],[4,5,6],[6,5,7],[6,7,8],[8,7,9]]}
    var expectedBevelSquare = {"positions":[[-1.5,-1.5],[-1.5,1.5],[10,-1.5],[8.5,1.5],[11.5,0],[8.5,10],[11.5,8.5],[10,11.5],[20,8.5],[18.5,11.5],[21.5,10],[21.5,21.5],[18.5,21.5]],"cells":[[0,1,2],[2,1,3],[2,3,4],[3,4,5],[5,4,6],[5,6,7],[6,7,8],[8,7,9],[8,9,10],[9,10,11],[9,11,12]]}

    var a = Stroke({ thickness: 2 }).build(legacy)
    ok(JSON.stringify(a) === JSON.stringify(expectedDefault),
        'legacy regression: default join/cap output unchanged')
    var b = Stroke({ thickness: 3, join: 'bevel', cap: 'square' }).build(legacy)
    ok(JSON.stringify(b) === JSON.stringify(expectedBevelSquare),
        'legacy regression: bevel/square output unchanged')
})()

// -- 8. software triangle coverage ---------------------------------------------

function rasterize(complex, scale, pad) {
    var minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity
    complex.positions.forEach(function(p) {
        minX = Math.min(minX, p[0]); maxX = Math.max(maxX, p[0])
        minY = Math.min(minY, p[1]); maxY = Math.max(maxY, p[1])
    })
    var width = Math.max(1, Math.ceil((maxX - minX) * scale) + pad * 2)
    var height = Math.max(1, Math.ceil((maxY - minY) * scale) + pad * 2)
    var img = new Uint8Array(width * height)

    function cross(ax, ay, bx, by, px, py) {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax)
    }

    complex.cells.forEach(function(cell) {
        var a = complex.positions[cell[0]]
        var b = complex.positions[cell[1]]
        var c = complex.positions[cell[2]]
        var loX = Math.floor((Math.min(a[0], b[0], c[0]) - minX) * scale) + pad
        var hiX = Math.ceil((Math.max(a[0], b[0], c[0]) - minX) * scale) + pad
        var loY = Math.floor((Math.min(a[1], b[1], c[1]) - minY) * scale) + pad
        var hiY = Math.ceil((Math.max(a[1], b[1], c[1]) - minY) * scale) + pad
        for (var y = Math.max(0, loY); y <= Math.min(height - 1, hiY); y++) {
            for (var x = Math.max(0, loX); x <= Math.min(width - 1, hiX); x++) {
                var px = minX + (x + 0.5 - pad) / scale
                var py = minY + (y + 0.5 - pad) / scale
                var d0 = cross(a[0], a[1], b[0], b[1], px, py)
                var d1 = cross(b[0], b[1], c[0], c[1], px, py)
                var d2 = cross(c[0], c[1], a[0], a[1], px, py)
                var eps = 1e-9
                if ((d0 >= -eps && d1 >= -eps && d2 >= -eps) ||
                    (d0 <= eps && d1 <= eps && d2 <= eps)) {
                    img[y * width + x] = 1
                }
            }
        }
    })

    return {
        width: width,
        height: height,
        img: img,
        covered: function(wx, wy) {
            var x = Math.floor((wx - minX) * scale + pad)
            var y = Math.floor((wy - minY) * scale + pad)
            if (x < 0 || y < 0 || x >= width || y >= height) return false
            return img[y * width + x] === 1
        }
    }
}

function writePPM(file, raster) {
    var header = Buffer.from('P6\n' + raster.width + ' ' + raster.height + '\n255\n')
    var body = Buffer.alloc(raster.width * raster.height * 3)
    for (var i = 0; i < raster.img.length; i++) {
        var v = raster.img[i] ? 255 : 0
        body[i * 3] = v
        body[i * 3 + 1] = v
        body[i * 3 + 2] = v
    }
    fs.writeFileSync(file, Buffer.concat([header, body]))
}

function expectSamples(raster, samples, covered, label, points) {
    samples.forEach(function(s) {
        var got = raster.covered(s[0], s[1])
        ok(got === covered,
            label + ': sample (' + s[0] + ', ' + s[1] + ') expected ' +
                (covered ? 'stroke' : 'empty') + ', got ' +
                (got ? 'stroke' : 'empty') + ' for path ' + fmtPath(points))
    })
}

;(function() {
    var scale = 8
    var pad = 4

    // normal right-angle path, butt caps, thickness 4
    var corner = [[0, 0], [20, 0], [20, 20]]
    var cornerMesh = Stroke({ thickness: 4 }).build(corner)
    checkMesh(cornerMesh, 'coverage right-angle')
    var cornerRaster = rasterize(cornerMesh, scale, pad)
    writePPM(path.join(__dirname, 'degenerate-stroke-right-angle.ppm'), cornerRaster)
    expectSamples(cornerRaster, [
        [10, 0], [10, 1.9], [10, -1.9], // middle of first segment
        [0.2, 0], [0.2, 1.5],           // just inside the butt start cap
        [19.8, 0],                      // just before the corner
        [20, 10], [20, 19.8],           // second segment, near butt end cap
        [21.9, 1]                       // mitered outer corner region
    ], true, 'coverage right-angle', corner)
    expectSamples(cornerRaster, [
        [10, 3], [-0.5, 0], [20, 21], [18, -3]
    ], false, 'coverage right-angle', corner)

    // exact reversal, butt caps, thickness 4: the strip must be solid
    // along its whole length, including near both fold caps
    var fold = [[0, 0], [20, 0], [0, 0]]
    var foldMesh = Stroke({ thickness: 4 }).build(fold)
    checkMesh(foldMesh, 'coverage reversal')
    var foldRaster = rasterize(foldMesh, scale, pad)
    writePPM(path.join(__dirname, 'degenerate-stroke-reversal.ppm'), foldRaster)
    expectSamples(foldRaster, [
        [10, 0], [10, 1.5], [10, -1.5], // middle of the strip
        [0.2, 0], [0.2, 1.5],           // near the fold caps at x=0
        [19.8, 0], [19.8, -1.5],        // near the caps at the far end
        [5, 1], [15, -1]
    ], true, 'coverage reversal', fold)
    expectSamples(foldRaster, [
        [10, 3], [10, -3], [-0.5, 0], [20.5, 0]
    ], false, 'coverage reversal', fold)

    // square caps push the stroke out by half the thickness
    var square = [[0, 0], [20, 0]]
    var squareMesh = Stroke({ thickness: 4, cap: 'square' }).build(square)
    checkMesh(squareMesh, 'coverage square cap')
    var squareRaster = rasterize(squareMesh, scale, pad)
    expectSamples(squareRaster, [
        [-1.9, 0], [20 + 1.9, 0], [0, 0], [20, 0]
    ], true, 'coverage square cap', square)
    expectSamples(squareRaster, [
        [-2.1, 0], [22.1, 0]
    ], false, 'coverage square cap', square)
})()

// -- summary -------------------------------------------------------------------

if (failures.length) {
    console.error('\n' + failures.length + ' check(s) failed')
    process.exit(1)
}
console.log('all degenerate-stroke checks passed')
