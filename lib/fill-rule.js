'use strict'

// Explicit SVG fill rule support.
//
// clean-pslg reorders edge endpoints and merges coincident edges, which
// destroys contour direction and coverage multiplicity.  Winding information
// therefore has to be captured from the discrete, simplified contours
// *before* the PSLG cleanup, and then used to classify the triangulated
// faces by the SVG nonzero / evenodd paint rules.

function samePoint (a, b) {
  return a[0] === b[0] && a[1] === b[1]
}

// Convert each contour into a closed directed loop of distinct points.
// Contours are already closed by the caller's edge wrap-around, with or
// without an explicit Z command.  Adjacent duplicate points and zero-length
// runs are dropped so that they never contribute any winding.
function collectLoops (contours) {
  var loops = []
  for (var i = 0; i < contours.length; i++) {
    var source = contours[i]
    var points = []
    for (var j = 0; j < source.length; j++) {
      var p = source[j]
      if (points.length === 0 || !samePoint(points[points.length - 1], p)) {
        points.push(p)
      }
    }
    if (points.length > 1 && samePoint(points[0], points[points.length - 1])) {
      points.pop()
    }
    if (points.length >= 3) {
      loops.push(points)
    }
  }
  return loops
}

// Signed winding number (nonzero rule) and unsigned ray crossing count
// (evenodd rule) of point (x, y) relative to one directed loop.
//
// The ray runs towards +x and each edge uses a half-open vertical interval,
// so a ray passing through a vertex counts exactly one crossing instead of
// two.  Horizontal edges, including any remaining zero-length edge, are
// skipped.
function loopWinding (loop, x, y) {
  var winding = 0
  var crossings = 0
  for (var i = 0; i < loop.length; i++) {
    var a = loop[i]
    var b = loop[(i + 1) % loop.length]
    if (a[1] === b[1]) {
      continue
    }
    var side = (b[0] - a[0]) * (y - a[1]) - (x - a[0]) * (b[1] - a[1])
    if (a[1] <= y) {
      if (b[1] > y && side > 0) {
        winding++
        crossings++
      }
    } else if (b[1] <= y && side < 0) {
      winding--
      crossings++
    }
  }
  return { winding: winding, crossings: crossings }
}

// Whether a point lies inside the painted region for the given fill rule.
// nonzero: directed winding number is not zero.
// evenodd: an odd number of contour crossings.
function isFilled (loops, x, y, fillRule) {
  var winding = 0
  var crossings = 0
  for (var i = 0; i < loops.length; i++) {
    var result = loopWinding(loops[i], x, y)
    winding += result.winding
    crossings += result.crossings
  }
  if (fillRule === 'nonzero') {
    return winding !== 0
  }
  return (crossings & 1) === 1
}

// Keep the finite triangulation faces whose interior is painted.  A face
// centroid is always strictly inside the face, so it can never sit on a
// cleaned contour edge and its classification is unambiguous.  Triangles
// share only edges afterwards and none crosses a cleaned boundary.
function selectFilledCells (cells, positions, loops, fillRule) {
  var selected = []
  for (var i = 0; i < cells.length; i++) {
    var cell = cells[i]
    var a = positions[cell[0]]
    var b = positions[cell[1]]
    var c = positions[cell[2]]
    var cx = (a[0] + b[0] + c[0]) / 3
    var cy = (a[1] + b[1] + c[1]) / 3
    if (isFilled(loops, cx, cy, fillRule)) {
      selected.push(cell)
    }
  }
  return selected
}

module.exports = {
  collectLoops: collectLoops,
  isFilled: isFilled,
  selectFilledCells: selectFilledCells
}
