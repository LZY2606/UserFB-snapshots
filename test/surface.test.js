"use strict"

var assert = require("node:assert")
var meshChunk = require("../surface.js")

// ---------------------------------------------------------------------------
// Helpers for building ndarray-style chunks.  Coordinates passed to fill are
// core coordinates; -1 and dim are the halo layer.
// ---------------------------------------------------------------------------

function makeChunk(dims, fill, layout) {
  var shape = [dims[0] + 2, dims[1] + 2, dims[2] + 2]
  var stride
  var offset
  var data

  layout = layout || "contiguous"

  if (layout === "contiguous") {
    stride = [shape[1] * shape[2], shape[2], 1]
    offset = 0
    data = new Uint8Array(shape[0] * shape[1] * shape[2])
  } else if (layout === "padded") {
    // Rows along x are padded with two extra elements of slack.
    var rowStride = shape[1] * shape[2] + 2
    stride = [rowStride, shape[2], 1]
    offset = 0
    data = new Uint8Array(shape[0] * rowStride)
  } else if (layout === "negative") {
    var base = [shape[1] * shape[2], shape[2], 1]
    stride = [-base[0], -base[1], -base[2]]
    offset = (shape[0] - 1) * base[0] +
             (shape[1] - 1) * base[1] +
             (shape[2] - 1) * base[2]
    data = new Uint8Array(shape[0] * shape[1] * shape[2])
  } else {
    throw new Error("unknown layout " + layout)
  }

  var arr = {data: data, shape: shape, stride: stride, offset: offset}
  for (var hx = 0; hx < shape[0]; ++hx) {
    for (var hy = 0; hy < shape[1]; ++hy) {
      for (var hz = 0; hz < shape[2]; ++hz) {
        data[offset + hx * stride[0] + hy * stride[1] + hz * stride[2]] =
          fill(hx - 1, hy - 1, hz - 1) | 0
      }
    }
  }
  return arr
}

function emptyFill() {
  return 0
}

function sub(a, b) {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

function cross(a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  ]
}

var DIRS = [
  [1, 0, 0], [-1, 0, 0],
  [0, 1, 0], [0, -1, 0],
  [0, 0, 1], [0, 0, -1]
]

function getOccupied(arr, hx, hy, hz) {
  if (hx < 0 || hy < 0 || hz < 0 ||
      hx >= arr.shape[0] || hy >= arr.shape[1] || hz >= arr.shape[2]) {
    return 0
  }
  return arr.data[arr.offset + hx * arr.stride[0] +
                   hy * arr.stride[1] + hz * arr.stride[2]] !== 0 ? 1 : 0
}

function faceKey(u, d, level, lo) {
  return [u, d, level, lo[0], lo[1], lo[2]].join(":")
}

// Independently compute every expected unit face straight from the voxel
// grid, in model coordinates (origin stripped on the actual side).
function expectedUnitFaces(arr, dims) {
  var expected = {}
  for (var x = 0; x < dims[0]; ++x) {
    for (var y = 0; y < dims[1]; ++y) {
      for (var z = 0; z < dims[2]; ++z) {
        var m = arr.data[arr.offset + (x + 1) * arr.stride[0] +
                         (y + 1) * arr.stride[1] + (z + 1) * arr.stride[2]]
        if (m === 0) {
          continue
        }
        for (var di = 0; di < 6; ++di) {
          var n = DIRS[di]
          // Neighbour in halo coordinates; halo occupancy occludes.
          if (getOccupied(arr, x + 1 + n[0], y + 1 + n[1], z + 1 + n[2])) {
            continue
          }
          var u = n[0] !== 0 ? 0 : (n[1] !== 0 ? 1 : 2)
          var d = n[u]
          var level = d > 0 ? 1 : 0
          var lo = [x, y, z]
          lo[u] += d > 0 ? 1 : 0
          expected[faceKey(u, d, lo[u], lo)] = m
        }
      }
    }
  }
  return expected
}

// Subdivide every output quad back into unit faces and index them the same
// way.  Also returns the total quad area.
function actualUnitFaces(result, origin) {
  var actual = {}
  var area = 0
  result.quads.forEach(function(q) {
    var u = 0
    while (q.normal[u] === 0) {
      ++u
    }
    var d = q.normal[u]

    var lo = [Infinity, Infinity, Infinity]
    var hi = [-Infinity, -Infinity, -Infinity]
    q.positions.forEach(function(p) {
      for (var a = 0; a < 3; ++a) {
        lo[a] = Math.min(lo[a], p[a] - origin[a])
        hi[a] = Math.max(hi[a], p[a] - origin[a])
      }
    })

    var qArea = 1
    var inPlane = [0, 1, 2].filter(function(a) {
      return a !== u
    })
    inPlane.forEach(function(a) {
      qArea *= hi[a] - lo[a]
    })
    area += qArea

    var a0 = inPlane[0]
    var a1 = inPlane[1]
    for (var c0 = lo[a0]; c0 < hi[a0]; ++c0) {
      for (var c1 = lo[a1]; c1 < hi[a1]; ++c1) {
        var cell = [0, 0, 0]
        cell[u] = lo[u]
        cell[a0] = c0
        cell[a1] = c1
        actual[faceKey(u, d, lo[u], cell)] = q.material
      }
    }
  })
  return {faces: actual, area: area}
}

// Full geometric check: exact unit-face coverage, winding, AO/diagonal shape.
function assertCoverage(arr, dims, result, origin) {
  origin = origin || [0, 0, 0]
  var expected = expectedUnitFaces(arr, dims)
  var got = actualUnitFaces(result, origin)

  var expectedKeys = Object.keys(expected).sort()
  var actualKeys = Object.keys(got.faces).sort()
  assert.deepStrictEqual(actualKeys, expectedKeys, "unit face coverage mismatch")
  expectedKeys.forEach(function(k) {
    assert.strictEqual(got.faces[k], expected[k], "material mismatch on face " + k)
  })
  assert.strictEqual(got.area, expectedKeys.length, "total area mismatch")

  result.quads.forEach(function(q) {
    assert.strictEqual(q.ao.length, 4)
    q.ao.forEach(function(v) {
      assert.ok(Number.isInteger(v) && v >= 0 && v <= 3)
    })
    assert.ok(q.diagonal === 0 || q.diagonal === 1)
    assert.strictEqual(q.positions.length, 4)
    assert.strictEqual(q.normal.length, 3)
    assert.strictEqual(q.normal.reduce(function(s, x) {
      return s + Math.abs(x)
    }, 0), 1)

    // Counter-clockwise seen from outside: (p1-p0) x (p3-p0) == normal.
    // Normalize the edge directions so rectangle size does not affect it.
    var e1 = sub(q.positions[1], q.positions[0]).map(function(x) {
      return x === 0 ? 0 : x / Math.abs(x)
    })
    var e3 = sub(q.positions[3], q.positions[0]).map(function(x) {
      return x === 0 ? 0 : x / Math.abs(x)
    })
    assert.deepStrictEqual(cross(e1, e3), q.normal, "winding is not CCW from outside")

    var wantDiagonal = q.ao[0] + q.ao[2] > q.ao[1] + q.ao[3] ? 1 : 0
    assert.strictEqual(q.diagonal, wantDiagonal)
  })
}

function findQuad(result, normal) {
  var matches = result.quads.filter(function(q) {
    return q.normal[0] === normal[0] &&
           q.normal[1] === normal[1] &&
           q.normal[2] === normal[2]
  })
  assert.strictEqual(matches.length, 1)
  return matches[0]
}

// ---------------------------------------------------------------------------
// 1. Single voxel: six unit faces, AO 3 everywhere, bounds 0/1.
// ---------------------------------------------------------------------------

;(function singleVoxel() {
  var arr = makeChunk([1, 1, 1], function(x, y, z) {
    return x === 0 && y === 0 && z === 0 ? 1 : 0
  })
  var result = meshChunk(arr, {origin: [0, 0, 0]})

  assert.strictEqual(result.quads.length, 6)
  result.quads.forEach(function(q) {
    assert.strictEqual(q.material, 1)
    assert.deepStrictEqual(q.ao, [3, 3, 3, 3])
    assert.strictEqual(q.diagonal, 0)
    q.positions.forEach(function(p) {
      p.forEach(function(c) {
        assert.ok(c === 0 || c === 1)
      })
    })
  })
  assertCoverage(arr, [1, 1, 1], result)

  // Exact ordering: axis X/Y/Z, negative then positive.
  assert.deepStrictEqual(result.quads.map(function(q) {
    return q.normal
  }), [
    [-1, 0, 0], [1, 0, 0],
    [0, -1, 0], [0, 1, 0],
    [0, 0, -1], [0, 0, 1]
  ])
  console.log("ok - single voxel")
})()

// ---------------------------------------------------------------------------
// 2. +Z face ambient occlusion against halo blockers.
// ---------------------------------------------------------------------------

;(function plusZAO() {
  // Blockers at core coords (1,0,1) and (0,1,1): both are halo cells.
  var arr = makeChunk([1, 1, 1], function(x, y, z) {
    if (x === 0 && y === 0 && z === 0) {
      return 1
    }
    if ((x === 1 && y === 0 && z === 1) ||
        (x === 0 && y === 1 && z === 1)) {
      return 1
    }
    return 0
  })
  var result = meshChunk(arr, {origin: [0, 0, 0]})
  var plusZ = findQuad(result, [0, 0, 1])
  assert.deepStrictEqual(plusZ.positions, [
    [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]
  ])
  assert.deepStrictEqual(plusZ.ao, [3, 2, 0, 2])
  assert.strictEqual(plusZ.diagonal, 0)
  assertCoverage(arr, [1, 1, 1], result)

  // Only the (1,-1,1) halo blocker: non-uniform AO, diagonal flips to 1.
  var arr2 = makeChunk([1, 1, 1], function(x, y, z) {
    if (x === 0 && y === 0 && z === 0) {
      return 1
    }
    if (x === 1 && y === -1 && z === 1) {
      return 1
    }
    return 0
  })
  var result2 = meshChunk(arr2, {origin: [0, 0, 0]})
  var plusZ2 = findQuad(result2, [0, 0, 1])
  assert.deepStrictEqual(plusZ2.ao, [3, 2, 3, 3])
  assert.strictEqual(plusZ2.diagonal, 1)
  assertCoverage(arr2, [1, 1, 1], result2)
  console.log("ok - +Z face AO and diagonal")
})()

// ---------------------------------------------------------------------------
// 3. Solid 2x2x2 core merges into six large faces.
// ---------------------------------------------------------------------------

;(function solidCube() {
  var arr = makeChunk([2, 2, 2], function(x, y, z) {
    return (x >= 0 && x < 2 && y >= 0 && y < 2 && z >= 0 && z < 2) ? 1 : 0
  })
  var result = meshChunk(arr, {origin: [0, 0, 0]})

  assert.strictEqual(result.quads.length, 6)
  result.quads.forEach(function(q) {
    assert.strictEqual(q.material, 1)
    assert.deepStrictEqual(q.ao, [3, 3, 3, 3])
    var mins = q.positions.reduce(function(acc, p) {
      return [Math.min(acc[0], p[0]), Math.min(acc[1], p[1]), Math.min(acc[2], p[2])]
    }, [Infinity, Infinity, Infinity])
    var maxs = q.positions.reduce(function(acc, p) {
      return [Math.max(acc[0], p[0]), Math.max(acc[1], p[1]), Math.max(acc[2], p[2])]
    }, [-Infinity, -Infinity, -Infinity])
    var u = q.normal[0] !== 0 ? 0 : (q.normal[1] !== 0 ? 1 : 2)
    for (var a = 0; a < 3; ++a) {
      if (a === u) {
        assert.ok(mins[a] === 0 || mins[a] === 2)
        assert.strictEqual(mins[a], maxs[a])
      } else {
        assert.strictEqual(mins[a], 0)
        assert.strictEqual(maxs[a], 2)
      }
    }
  })
  assertCoverage(arr, [2, 2, 2], result)
  console.log("ok - solid 2x2x2 merges to six faces")
})()

// ---------------------------------------------------------------------------
// 4. Halo occupancy from a neighbouring chunk suppresses internal faces.
// ---------------------------------------------------------------------------

;(function haloOcclusion() {
  // Two core cells along x, plus a halo wall past x=1 standing in for the
  // adjacent chunk.  No faces may be generated between occupied cells.
  var arr = makeChunk([2, 1, 1], function(x, y, z) {
    if ((x === 0 || x === 1) && y === 0 && z === 0) {
      return 1
    }
    if (x === 2 && y === 0 && z === 0) {
      return 1 // halo cell owned by the neighbour chunk
    }
    return 0
  })
  var result = meshChunk(arr, {origin: [0, 0, 0]})

  var normals = result.quads.map(function(q) {
    return q.normal.join(",")
  })
  assert.ok(normals.indexOf("1,0,0") === -1, "+X face must be occluded by halo")
  assert.ok(normals.indexOf("-1,0,0") !== -1)
  assertCoverage(arr, [2, 1, 1], result)
  console.log("ok - halo occupancy suppresses internal faces")
})()

// ---------------------------------------------------------------------------
// 5. Different materials occlude each other but never merge.
// ---------------------------------------------------------------------------

;(function materialBoundary() {
  // 2x2x1 core: x=0 is material 1, x=1 is material 2.  The +Z / -Z faces
  // must stay as two 1x2 rectangles, not one 2x2 quad.
  var arr = makeChunk([2, 2, 1], function(x, y, z) {
    if (z !== 0 || y < 0 || y > 1) {
      return 0
    }
    return x === 0 ? 1 : 2
  })
  var result = meshChunk(arr, {origin: [0, 0, 0]})

  ;[1, -1].forEach(function(sign) {
    var zFaces = result.quads.filter(function(q) {
      return q.normal[2] === sign
    })
    assert.strictEqual(zFaces.length, 2)
    zFaces.forEach(function(q) {
      var inPlane = q.positions.map(function(p) {
        return [p[0], p[1]]
      })
      var xs = inPlane.map(function(p) {
        return p[0]
      })
      var ys = inPlane.map(function(p) {
        return p[1]
      })
      assert.strictEqual(Math.max.apply(null, xs) - Math.min.apply(null, xs), 1)
      assert.strictEqual(Math.max.apply(null, ys) - Math.min.apply(null, ys), 2)
    })
    var materials = zFaces.map(function(q) {
      return q.material
    }).sort()
    assert.deepStrictEqual(materials, [1, 2])
  })

  // No faces on the shared x=1 boundary: coverage check handles this because
  // every non-zero material occludes every other.
  assertCoverage(arr, [2, 2, 1], result)
  console.log("ok - material boundaries occlude without merging")
})()

// ---------------------------------------------------------------------------
// 6. Negative strides, non-zero offset and a non-zero origin.
// ---------------------------------------------------------------------------

;(function negativeStrideAndOrigin() {
  var dims = [2, 2, 2]
  function fill(x, y, z) {
    if (x < 0 || y < 0 || z < 0 || x >= 2 || y >= 2 || z >= 2) {
      return 0
    }
    return ((x + 2 * y + 3 * z) % 2) + 1
  }
  var plain = makeChunk(dims, fill, "contiguous")
  var reversed = makeChunk(dims, fill, "negative")
  var origin = [10, -3, 7]

  var r1 = meshChunk(plain, {origin: origin})
  var r2 = meshChunk(reversed, {origin: origin})

  var k1 = r1.quads.map(function(q) {
    return JSON.stringify(q)
  }).sort()
  var k2 = r2.quads.map(function(q) {
    return JSON.stringify(q)
  }).sort()
  assert.deepStrictEqual(k2, k1, "negative stride must give identical quads")

  r2.quads.forEach(function(q) {
    q.positions.forEach(function(p) {
      // Every emitted coordinate carries the origin and stays within model bounds.
      assert.ok(p[0] >= origin[0] && p[0] <= origin[0] + 2)
      assert.ok(p[1] >= origin[1] && p[1] <= origin[1] + 2)
      assert.ok(p[2] >= origin[2] && p[2] <= origin[2] + 2)
    })
  })
  assertCoverage(reversed, dims, r2, origin)

  // Padded underlying storage as well.
  var padded = makeChunk(dims, fill, "padded")
  var r3 = meshChunk(padded, {origin: origin})
  var k3 = r3.quads.map(function(q) {
    return JSON.stringify(q)
  }).sort()
  assert.deepStrictEqual(k3, k1, "padded storage must give identical quads")
  assertCoverage(padded, dims, r3, origin)
  console.log("ok - negative stride, padding and origin offset")
})()

// ---------------------------------------------------------------------------
// 7. All air produces no quads.
// ---------------------------------------------------------------------------

;(function allAir() {
  var arr = makeChunk([3, 2, 4], emptyFill)
  var result = meshChunk(arr, {origin: [0, 0, 0]})
  assert.deepStrictEqual(result.quads, [])
  assertCoverage(arr, [3, 2, 4], result)
  console.log("ok - all air")
})()

// ---------------------------------------------------------------------------
// 8. Repeated calls are independent and deterministic.
// ---------------------------------------------------------------------------

;(function repeatedCalls() {
  var arr = makeChunk([2, 2, 2], function(x, y, z) {
    if (x < 0 || y < 0 || z < 0 || x >= 2 || y >= 2 || z >= 2) {
      return 0
    }
    return ((x + y + z) % 2) + 1
  })
  var first = meshChunk(arr, {origin: [5, 6, 7]})
  var second = meshChunk(arr, {origin: [5, 6, 7]})
  assert.deepStrictEqual(second, first)

  // Interleaving with a different-shaped chunk must not leak state.
  var other = makeChunk([1, 1, 1], function(x, y, z) {
    return x === 0 && y === 0 && z === 0 ? 3 : 0
  })
  var otherMesh = meshChunk(other, {origin: [0, 0, 0]})
  assert.strictEqual(otherMesh.quads.length, 6)
  var third = meshChunk(arr, {origin: [5, 6, 7]})
  assert.deepStrictEqual(third, first)
  assertCoverage(arr, [2, 2, 2], first, [5, 6, 7])
  console.log("ok - repeated calls")
})()

console.log("all surface tests passed")
