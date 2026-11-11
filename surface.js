"use strict"

var compileMesher = require("./greedy")

// One compiled 2D greedy mesher is reused for every face plane.  Mask cell 0
// means "no face here"; two non-zero cells merge only when their codes are
// identical, so material and (for uniform faces) the AO constant both form the
// merge key, while non-uniform faces carry unique ids and never merge.
var pendingRects = []
var maskMesher = compileMesher({
  order: [0, 1],
  skip: function(v) {
    return v === 0
  },
  merge: function(a, b) {
    return a === b
  },
  append: function(lo0, lo1, hi0, hi1, val) {
    pendingRects.push([lo0, lo1, hi0, hi1, val])
  }
})

var AXIS_VECTORS = [
  [1, 0, 0],
  [0, 1, 0],
  [0, 0, 1]
]

// Two candidate corner orders in (dv, dw) offsets along the in-plane axes.
var ORDER_A = [[0, 0], [1, 0], [1, 1], [0, 1]]
var ORDER_B = [[0, 0], [0, 1], [1, 1], [1, 0]]

function cross(a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  ]
}

// Pick the winding whose (p1-p0) x (p3-p0) points along the outward normal,
// i.e. counter-clockwise when the quad is viewed from outside.
function cornerOrder(u, d, v, w) {
  var ev = AXIS_VECTORS[v]
  var ew = AXIS_VECTORS[w]
  var normal = AXIS_VECTORS[u]
  var trial = cross(ev, ew)
  var aligned = trial[0] * normal[0] + trial[1] * normal[1] + trial[2] * normal[2]
  if (d * aligned > 0) {
    return ORDER_A
  }
  return ORDER_B
}

function meshChunk(array, options) {
  options = options || {}
  var origin = options.origin || [0, 0, 0]

  var data = array.data
  var baseOffset = array.offset | 0
  var stride = array.stride
  var shape = array.shape

  // Core dimensions: halo is one cell thick on every side.
  var dims = [
    shape[0] - 2,
    shape[1] - 2,
    shape[2] - 2
  ]

  function index(hx, hy, hz) {
    return baseOffset + hx * stride[0] + hy * stride[1] + hz * stride[2]
  }

  // Occupancy read in halo coordinates; out of range reads count as air.
  function occupied(hx, hy, hz) {
    if (hx < 0 || hy < 0 || hz < 0 ||
        hx >= shape[0] || hy >= shape[1] || hz >= shape[2]) {
      return 0
    }
    return data[index(hx, hy, hz)] !== 0 ? 1 : 0
  }

  // Material of a core cell given in core coordinates.
  function material(cx, cy, cz) {
    return data[index(cx + 1, cy + 1, cz + 1)]
  }

  // AO for one corner of the unit face of core cell p=(cx,cy,cz).
  // u is the normal axis, d its sign; v/w are the in-plane axes and
  // sv/sw in {-1,+1} point from p towards the corner along those axes.
  // The three samples live in the layer just outside the face (p+n+...).
  function cornerAO(cx, cy, cz, u, d, v, w, sv, sw) {
    // Halo coordinate of p, then step to the outside layer.
    var hx = cx + 1
    var hy = cy + 1
    var hz = cz + 1
    if (u === 0) {
      hx += d
    } else if (u === 1) {
      hy += d
    } else {
      hz += d
    }

    var ax = hx, ay = hy, az = hz    // p + n + s
    var bx = hx, by = hy, bz = hz    // p + n + t
    var cx2 = hx, cy2 = hy, cz2 = hz // p + n + s + t
    if (v === 0) {
      ax += sv
      cx2 += sv
    } else if (v === 1) {
      ay += sv
      cy2 += sv
    } else {
      az += sv
      cz2 += sv
    }
    if (w === 0) {
      bx += sw
      cx2 += sw
    } else if (w === 1) {
      by += sw
      cy2 += sw
    } else {
      bz += sw
      cz2 += sw
    }

    var A = occupied(ax, ay, az)
    var B = occupied(bx, by, bz)
    var C = occupied(cx2, cy2, cz2)
    if (A && B) {
      return 0
    }
    return 3 - A - B - C
  }

  var quads = []

  // Output ordering: normal axis X, Y, Z; then negative / positive face;
  // then the face plane coordinate ascending; rectangles within a plane
  // follow the compiler's own traversal order.
  for (var u = 0; u < 3; ++u) {
    // Remaining axes, in ascending order.
    var v = u === 0 ? 1 : 0
    var w = u === 2 ? 1 : 2
    var nv = dims[v]
    var nw = dims[w]
    var nu = dims[u]
    var corners = cornerOrder(u, 1, v, w)
    var negCorners = cornerOrder(u, -1, v, w)

    for (var d = -1; d <= 1; d += 2) {
      var faceCorners = d > 0 ? corners : negCorners
      for (var k = 0; k < nu; ++k) {
        var mask = new Int32Array(nv * nw)
        var idMaterial = [0]
        var idAO = [null]
        var nextId = 0

        for (var iv = 0; iv < nv; ++iv) {
          for (var iw = 0; iw < nw; ++iw) {
            var p = [0, 0, 0]
            p[u] = k
            p[v] = iv
            p[w] = iw

            var m = material(p[0], p[1], p[2])
            if (m === 0) {
              continue
            }

            // Neighbour cell across the face, in halo coordinates.
            var h = [p[0] + 1, p[1] + 1, p[2] + 1]
            h[u] += d
            if (occupied(h[0], h[1], h[2])) {
              continue
            }

            // AO grid indexed by geometric corner [dv][dw].
            var aoGrid = [
              [
                cornerAO(p[0], p[1], p[2], u, d, v, w, -1, -1),
                cornerAO(p[0], p[1], p[2], u, d, v, w, -1, 1)
              ],
              [
                cornerAO(p[0], p[1], p[2], u, d, v, w, 1, -1),
                cornerAO(p[0], p[1], p[2], u, d, v, w, 1, 1)
              ]
            ]

            if (aoGrid[0][0] === aoGrid[0][1] &&
                aoGrid[0][0] === aoGrid[1][0] &&
                aoGrid[0][0] === aoGrid[1][1]) {
              // Material and AO constant together form the merge key.
              mask[iv * nw + iw] = -(m * 4 + aoGrid[0][0] + 1)
            } else {
              // Unique id: this face can only come out as a unit rectangle.
              var id = ++nextId
              idMaterial[id] = m
              idAO[id] = aoGrid
              mask[iv * nw + iw] = id
            }
          }
        }

        var maskArray = {
          data: mask,
          shape: [nv, nw],
          stride: [nw, 1],
          offset: 0
        }

        pendingRects.length = 0
        maskMesher(maskArray)

        for (var r = 0; r < pendingRects.length; ++r) {
          var rect = pendingRects[r]
          var loV = rect[0]
          var loW = rect[1]
          var hiV = rect[2]
          var hiW = rect[3]
          var code = rect[4]

          var uniform = code < 0
          var mat
          var constAO = 0
          var aoGridOut = null
          if (uniform) {
            var packed = -code - 1
            mat = packed >> 2
            constAO = packed & 3
          } else {
            mat = idMaterial[code]
            aoGridOut = idAO[code]
          }

          // World coordinate of the face plane: positive faces sit at k+1,
          // negative faces at k.  Halo itself never contributes a coordinate.
          var planeCore = d > 0 ? k + 1 : k

          var positions = new Array(4)
          var ao = new Array(4)
          for (var c = 0; c < 4; ++c) {
            var dv = faceCorners[c][0]
            var dw = faceCorners[c][1]

            var coord = [0, 0, 0]
            coord[u] = planeCore
            coord[v] = dv ? hiV : loV
            coord[w] = dw ? hiW : loW
            positions[c] = [
              origin[0] + coord[0],
              origin[1] + coord[1],
              origin[2] + coord[2]
            ]

            ao[c] = uniform ? constAO : aoGridOut[dv][dw]
          }

          var normal = [0, 0, 0]
          normal[u] = d

          var diagonal = ao[0] + ao[2] > ao[1] + ao[3] ? 1 : 0

          quads.push({
            material: mat,
            normal: normal,
            positions: positions,
            ao: ao,
            diagonal: diagonal
          })
        }
      }
    }
  }

  return {quads: quads}
}

module.exports = meshChunk
module.exports.meshChunk = meshChunk
