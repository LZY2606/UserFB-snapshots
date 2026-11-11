var parseSVG = require('parse-svg-path')
var getContours = require('svg-path-contours')
var cdt2d = require('cdt2d')
var cleanPSLG = require('clean-pslg')
var getBounds = require('bound-points')
var normalize = require('normalize-path-scale')
var random = require('random-float')
var assign = require('object-assign')
var simplify = require('simplify-path')
var fillRule = require('./lib/fill-rule')

module.exports = svgMesh3d
function svgMesh3d (svgPath, opt) {
  if (typeof svgPath !== 'string') {
    throw new TypeError('must provide a string as first parameter')
  }

  opt = assign({
    delaunay: true,
    clean: true,
    exterior: false,
    randomization: 0,
    simplify: 0,
    scale: 1
  }, opt)

  // An explicit fill rule switches on SVG paint-rule classification.  It is
  // only supported with the default PSLG cleanup and face configuration.
  var useFillRule = opt.fillRule !== undefined
  if (useFillRule) {
    if (opt.fillRule !== 'nonzero' && opt.fillRule !== 'evenodd') {
      throw new TypeError('fillRule must be either "nonzero" or "evenodd"')
    }
    // Match the boolean semantics used below (clean !== false) and by cdt2d.
    if (opt.clean === false ||
        (opt.interior !== undefined && !opt.interior) ||
        !!opt.exterior ||
        !!opt.infinity) {
      throw new TypeError('fillRule is only supported with clean: true, interior: true, exterior: false and infinity: false')
    }
  }

  var i
  // parse string as a list of operations
  var svg = parseSVG(svgPath)

  // convert curves into discrete points
  var contours = getContours(svg, opt.scale)

  // optionally simplify the path for faster triangulation and/or aesthetics
  if (opt.simplify > 0 && typeof opt.simplify === 'number') {
    for (i = 0; i < contours.length; i++) {
      contours[i] = simplify(contours[i], opt.simplify)
    }
  }

  // Winding has to be captured before cleanPSLG: cleanup reorders edge
  // endpoints and merges coincident edges, dropping direction and coverage
  // multiplicity.  Contours with no area leave no fillable loops.
  var fillLoops = useFillRule ? fillRule.collectLoops(contours) : null
  if (useFillRule && fillLoops.length === 0) {
    return { positions: [], cells: [] }
  }

  // prepare for triangulation
  var polyline = denestPolyline(contours)
  var positions = polyline.positions
  var bounds = getBounds(positions)

  // optionally add random points for aesthetics
  var randomization = opt.randomization
  if (typeof randomization === 'number' && randomization > 0) {
    addRandomPoints(positions, bounds, randomization)
  }

  var loops = polyline.edges
  var edges = []
  for (i = 0; i < loops.length; ++i) {
    var loop = loops[i]
    for (var j = 0; j < loop.length; ++j) {
      edges.push([loop[j], loop[(j + 1) % loop.length]])
    }
  }

  // this updates points/edges so that they now form a valid PSLG
  if (opt.clean !== false) {
    cleanPSLG(positions, edges)
  }

  var cells
  if (useFillRule) {
    // Triangulate every finite face first, then keep exactly the faces
    // whose interior is painted according to the directed contours.
    var allCells = cdt2d(positions, edges, {
      delaunay: opt.delaunay,
      interior: true,
      exterior: true,
      infinity: false
    })
    cells = fillRule.selectFilledCells(allCells, positions, fillLoops, opt.fillRule)
  } else {
    // triangulate mesh
    cells = cdt2d(positions, edges, opt)
  }

  // rescale to [-1 ... 1]
  if (opt.normalize !== false) {
    normalize(positions, bounds)
  }

  // convert to 3D representation and flip on Y axis for convenience w/ OpenGL
  to3D(positions)

  return {
    positions: positions,
    cells: cells
  }
}

function to3D (positions) {
  for (var i = 0; i < positions.length; i++) {
    var xy = positions[i]
    xy[1] *= -1
    xy[2] = xy[2] || 0
  }
}

function addRandomPoints (positions, bounds, count) {
  var min = bounds[0]
  var max = bounds[1]

  for (var i = 0; i < count; i++) {
    positions.push([ // random [ x, y ]
      random(min[0], max[0]),
      random(min[1], max[1])
    ])
  }
}

function denestPolyline (nested) {
  var positions = []
  var edges = []

  for (var i = 0; i < nested.length; i++) {
    var path = nested[i]
    var loop = []
    for (var j = 0; j < path.length; j++) {
      var pos = path[j]
      var idx = positions.indexOf(pos)
      if (idx === -1) {
        positions.push(pos)
        idx = positions.length - 1
      }
      loop.push(idx)
    }
    edges.push(loop)
  }
  return {
    positions: positions,
    edges: edges
  }
}
