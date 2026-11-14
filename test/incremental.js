"use strict";
/*
 * Incremental Surface Nets editor tests.
 *
 * Every geometric claim is checked against an independent reconstruction:
 * cell identity is derived directly from the flat sample array with the id
 * formulas from the specification, the reference mesh comes from the
 * original surfaceNets extractor, and normals are re-accumulated from the
 * triangles the editor itself emits.
 */

var api = require("../index.js");
var surfaceNets = api.surfaceNets;
var createSurfaceNetsEditor = api.createSurfaceNetsEditor;

var EPS = 1e-12;

//The twelve cube edges as (low corner, high corner) pairs, corner index
//g = i + 2*j + 4*k, listed in the extractor's own edge numbering: edges
//0, 1 and 2 are the lower-corner edges along x, y and z respectively.
//Hand-listed here so the tests do not borrow the extractor's own tables.
var CUBE_EDGES = [
  [0, 1], [0, 2], [0, 4], [1, 3],
  [1, 5], [2, 3], [2, 6], [3, 7],
  [4, 5], [4, 6], [5, 7], [6, 7]
];

function mulberry32(seed) {
  return function() {
    seed |= 0;
    seed = (seed + 0x6D2B79F5) | 0;
    var t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function cellIdOf(x, y, z, cnx, cny) {
  return x + cnx * (y + cny * z);
}
function sampleIdOf(x, y, z, nx, ny) {
  return x + nx * (y + ny * z);
}
function cellXYZ(id, cnx, cny) {
  var x = id % cnx;
  var t = (id - x) / cnx;
  var y = t % cny;
  var z = (t - y) / cny;
  return [x, y, z];
}

function cornerValues(values, dims, cx, cy, cz) {
  var nx = dims[0], ny = dims[1], n01 = nx * ny;
  var o = sampleIdOf(cx, cy, cz, nx, ny);
  return [
    values[o],            values[o + 1],
    values[o + nx],       values[o + nx + 1],
    values[o + n01],      values[o + n01 + 1],
    values[o + n01 + nx], values[o + n01 + nx + 1]
  ];
}

function signMask(grid) {
  var mask = 0;
  for (var g = 0; g < 8; ++g) {
    if (grid[g] < 0) mask |= 1 << g;
  }
  return mask;
}

function edgeMaskOf(mask) {
  var em = 0;
  for (var e = 0; e < 12; ++e) {
    var a = mask & (1 << CUBE_EDGES[e][0]);
    var b = mask & (1 << CUBE_EDGES[e][1]);
    if ((a === 0) !== (b === 0)) em |= 1 << e;
  }
  return em;
}

function approx(a, b, tol) {
  return Math.abs(a - b) <= (tol || EPS);
}
function vecApprox(a, b, tol) {
  return approx(a[0], b[0], tol) && approx(a[1], b[1], tol) && approx(a[2], b[2], tol);
}

function isSortedUnique(nums) {
  for (var i = 1; i < nums.length; ++i) {
    if (!(nums[i] > nums[i - 1])) return false;
  }
  return true;
}

//Independent enumeration of the active cells in the original z/y/x traversal
//order, returning { ids, masks } keyed by the stable cell id formula.
function enumerateActiveCells(dims, values) {
  var nx = dims[0], ny = dims[1], nz = dims[2];
  var cnx = nx - 1, cny = ny - 1;
  var ids = [];
  var masks = new Map();
  for (var z = 0; z < nz - 1; ++z) {
    for (var y = 0; y < ny - 1; ++y) {
      for (var x = 0; x < nx - 1; ++x) {
        var mask = signMask(cornerValues(values, dims, x, y, z));
        if (mask !== 0 && mask !== 255) {
          var id = cellIdOf(x, y, z, cnx, cny);
          ids.push(id);
          masks.set(id, mask);
        }
      }
    }
  }
  return { ids: ids, masks: masks };
}

//The twelve cube edges of a cell, filtered to the interior grid-edge range
//where the original extraction can own two triangles.  Mirrors the exact
//dependency rule given in the specification.
function cellInteriorEdges(dims, cid) {
  var nx = dims[0], ny = dims[1], nz = dims[2];
  var cnx = nx - 1, cny = ny - 1;
  var xyz = cellXYZ(cid, cnx, cny);
  var x = xyz[0], y = xyz[1], z = xyz[2];
  var s = sampleIdOf(x, y, z, nx, ny);
  var n01 = nx * ny;
  var out = [];
  var dy, dz, dx, yu, zu, xu;
  for (dy = 0; dy <= 1; ++dy) {
    for (dz = 0; dz <= 1; ++dz) {
      yu = y + dy; zu = z + dz;
      if (yu >= 1 && yu <= ny - 2 && zu >= 1 && zu <= nz - 2) {
        out.push(3 * (s + dy * nx + dz * n01));
      }
    }
  }
  for (dx = 0; dx <= 1; ++dx) {
    for (dz = 0; dz <= 1; ++dz) {
      xu = x + dx; zu = z + dz;
      if (xu >= 1 && xu <= nx - 2 && zu >= 1 && zu <= nz - 2) {
        out.push(3 * (s + dx + dz * n01) + 1);
      }
    }
  }
  for (dx = 0; dx <= 1; ++dx) {
    for (dy = 0; dy <= 1; ++dy) {
      xu = x + dx; yu = y + dy;
      if (xu >= 1 && xu <= nx - 2 && yu >= 1 && yu <= ny - 2) {
        out.push(3 * (s + dx + dy * nx) + 2);
      }
    }
  }
  return out;
}

//Expected dependency sets for a batch whose effective edits are
//[[sampleIndex, value], ...].
function expectedWork(dims, effective) {
  var nx = dims[0], ny = dims[1], nz = dims[2];
  var cnx = nx - 1, cny = ny - 1, cnz = nz - 1;
  var cells = new Set();
  effective.forEach(function(pair) {
    var s = pair[0];
    var sx = s % nx;
    var t = (s - sx) / nx;
    var sy = t % ny;
    var sz = (t - sy) / ny;
    for (var dz = -1; dz <= 0; ++dz) {
      var z = sz + dz;
      if (z < 0 || z >= cnz) continue;
      for (var dy = -1; dy <= 0; ++dy) {
        var y = sy + dy;
        if (y < 0 || y >= cny) continue;
        for (var dx = -1; dx <= 0; ++dx) {
          var x = sx + dx;
          if (x < 0 || x >= cnx) continue;
          cells.add(cellIdOf(x, y, z, cnx, cny));
        }
      }
    }
  });
  var edges = new Set();
  cells.forEach(function(cid) {
    cellInteriorEdges(dims, cid).forEach(function(eid) { edges.add(eid); });
  });
  return {
    cellIds: Array.from(cells).sort(function(a, b) { return a - b; }),
    edgeIds: Array.from(edges).sort(function(a, b) { return a - b; })
  };
}

//Independent normal accumulation straight from a snapshot's triangles,
//using world coordinates and the mandated (b-a) x (c-a) orientation.
function independentNormals(snapshot) {
  var pos = new Map();
  snapshot.vertices.forEach(function(v) { pos.set(v.id, v.position); });
  var acc = new Map();
  function add(id, cr) {
    var cur = acc.get(id);
    if (!cur) { cur = [0, 0, 0]; acc.set(id, cur); }
    cur[0] += cr[0]; cur[1] += cr[1]; cur[2] += cr[2];
  }
  snapshot.triangles.forEach(function(t) {
    var a = pos.get(t.vertices[0]);
    var b = pos.get(t.vertices[1]);
    var c = pos.get(t.vertices[2]);
    var u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    var w = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    var cr = [
      u[1] * w[2] - u[2] * w[1],
      u[2] * w[0] - u[0] * w[2],
      u[0] * w[1] - u[1] * w[0]
    ];
    t.vertices.forEach(function(id) { add(id, cr); });
  });
  var out = new Map();
  snapshot.vertices.forEach(function(v) {
    var a = acc.get(v.id) || [0, 0, 0];
    var len = Math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    out.set(v.id, len > 0 ? [a[0] / len, a[1] / len, a[2] / len] : [0, 0, 0]);
  });
  return out;
}

// ---------------------------------------------------------------------------
// geometry_identity: analytic planes on anisotropic bounds must reproduce the
// original extractor cell for cell, with stable ids and the original winding.
// ---------------------------------------------------------------------------
function geometry_identity(c) {
  var dims = [8, 6, 7];
  var bounds = [[-3, -2, -1], [5, 10, 4]];
  var nx = dims[0], ny = dims[1], nz = dims[2];
  var cnx = nx - 1, cny = ny - 1;
  var scale = dims.map(function(d, k) {
    return (bounds[1][k] - bounds[0][k]) / d;
  });
  function world(ix, iy, iz) {
    return [
      bounds[0][0] + scale[0] * ix,
      bounds[0][1] + scale[1] * iy,
      bounds[0][2] + scale[2] * iz
    ];
  }
  //A tilted plane whose normal has a component on every anisotropic axis.
  function potential(x, y, z) {
    return 0.7 * x - 0.4 * y + 1.3 * z - 2.1;
  }
  var values = new Float64Array(nx * ny * nz);
  for (var iz = 0; iz < nz; ++iz) {
    for (var iy = 0; iy < ny; ++iy) {
      for (var ix = 0; ix < nx; ++ix) {
        var w = world(ix, iy, iz);
        values[sampleIdOf(ix, iy, iz, nx, ny)] = potential(w[0], w[1], w[2]);
      }
    }
  }

  var ref = surfaceNets(dims, potential, bounds);
  var editor = createSurfaceNetsEditor(dims, values, bounds);
  var snap = editor.snapshot();

  //The editor must keep its own copy: mutating the caller's array is a no-op.
  var sourceBackup = values.slice();
  values[0] = 12345;
  values[values.length - 1] = -12345;
  var snap2 = editor.snapshot();
  c.check(JSON.stringify(snap2) === JSON.stringify(snap),
    "editor must own an independent copy of the input values");
  values.set(sourceBackup);

  //Independent cell identity straight from the samples.
  var active = enumerateActiveCells(dims, sourceBackup);
  c.check(active.ids.length === ref.positions.length,
    "active cell count matches original vertex count");
  c.check(snap.vertices.length === active.ids.length,
    "snapshot vertex count matches independent active cell count");
  c.check(isSortedUnique(snap.vertices.map(function(v) { return v.id; })),
    "snapshot vertices sorted by id");
  c.check(isSortedUnique(snap.triangles.map(function(t) { return t.id; })),
    "snapshot triangles sorted by id");

  //Original vertex ordinal -> stable cell id, following the traversal order.
  var ordinalToId = active.ids;
  var snapVertex = new Map();
  snap.vertices.forEach(function(v) { snapVertex.set(v.id, v); });
  for (var j = 0; j < ordinalToId.length; ++j) {
    var id = ordinalToId[j];
    var rec = snapVertex.get(id);
    c.check(!!rec, "every active cell id appears in the snapshot");
    c.check(vecApprox(rec.position, ref.positions[j], 1e-12),
      "stable-id vertex position matches original extractor");
  }

  //Independently rebuild the expected triangles (id and winding) from the
  //cell sign masks, including the mask&1 orientation flip and both diagonals.
  var cellStrides = [1, cnx, cnx * cny];
  var expected = [];
  active.ids.forEach(function(cid) {
    var xyz = cellXYZ(cid, cnx, cny);
    var x = xyz[0], y = xyz[1], z = xyz[2];
    var mask = active.masks.get(cid);
    var em = edgeMaskOf(mask);
    var s = sampleIdOf(x, y, z, nx, ny);
    for (var axis = 0; axis < 3; ++axis) {
      if (!(em & (1 << axis))) continue;
      var iu = (axis + 1) % 3;
      var iv = (axis + 2) % 3;
      if (xyz[iu] === 0 || xyz[iv] === 0) continue;
      var c1 = cid - cellStrides[iu];
      var c2 = cid - cellStrides[iv];
      var c3 = cid - cellStrides[iu] - cellStrides[iv];
      var v0, v1;
      if (mask & 1) {
        v0 = [cid, c1, c2];
        v1 = [c2, c1, c3];
      } else {
        v0 = [cid, c2, c1];
        v1 = [c1, c2, c3];
      }
      expected.push([2 * (3 * s + axis), v0]);
      expected.push([2 * (3 * s + axis) + 1, v1]);
    }
  });
  c.check(expected.length === ref.cells.length,
    "expected triangle count matches original face count");
  c.check(snap.triangles.length === expected.length,
    "snapshot triangle count matches independent expectation");

  var snapTri = new Map();
  snap.triangles.forEach(function(t) { snapTri.set(t.id, t.vertices); });
  for (var e = 0; e < expected.length; ++e) {
    var wantId = expected[e][0];
    var got = snapTri.get(wantId);
    c.check(!!got, "triangle id " + wantId + " exists with the stable formula");
    c.check(got.join(",") === expected[e][1].join(","),
      "triangle " + wantId + " uses the original winding and diagonal");
    //Cross-check against the original extractor's own face in push order.
    var refIds = ref.cells[e].map(function(ord) { return ordinalToId[ord]; });
    c.check(refIds.join(",") === got.join(","),
      "triangle " + wantId + " agrees with the original face in push order");
  }

  //Boundary vertices with no incident triangles must still be reported.
  //A dedicated fixture: the zero level only crosses the y = 0 cell layer,
  //where the original extractor can never emit a face, so every active cell
  //is a triangle-free boundary vertex.
  var bd = [5, 4, 6];
  var bnx = bd[0], bny = bd[1], bnz = bd[2];
  var bValues = new Float64Array(bnx * bny * bnz).fill(1);
  for (var bz = 0; bz < bnz; ++bz) {
    //Only the (ix=0, iy=0) sample column is negative, so the sole active
    //cells are the (cx=0, cy=0, *) column.  Every face they could own is
    //blocked by a zero perpendicular cell coordinate, leaving them all as
    //triangle-free boundary vertices.
    bValues[sampleIdOf(0, 0, bz, bnx, bny)] = -1;
  }
  var bEditor = createSurfaceNetsEditor(bd, bValues);
  var bSnap = bEditor.snapshot();
  var bRef = surfaceNets(bd, function(x, y, z) {
    return Math.round(x) === 0 && Math.round(y) === 0 ? -1 : 1;
  });
  c.check(bSnap.triangles.length === 0 && bRef.cells.length === 0,
    "boundary-column fixture produces no faces");
  c.check(bSnap.vertices.length === bnz - 1 &&
    bSnap.vertices.length === bRef.positions.length,
    "every triangle-free active cell still yields a vertex");
  var bNormals = independentNormals(bSnap);
  bSnap.vertices.forEach(function(v, k) {
    c.check(vecApprox(v.position, bRef.positions[k], 1e-12),
      "boundary vertex position matches the original extractor");
    c.check(vecApprox(bNormals.get(v.id), [0, 0, 0], 0),
      "a vertex without triangles has a zero normal");
  });

  //Same property on the main fixture when it has any loose vertices.
  var referenced = new Set();
  snap.triangles.forEach(function(t) {
    t.vertices.forEach(function(v) { referenced.add(v); });
  });
  var boundary = snap.vertices.filter(function(v) {
    return !referenced.has(v.id);
  });
  if (boundary.length > 0) {
    var normals = independentNormals(snap);
    boundary.forEach(function(v) {
      c.check(vecApprox(normals.get(v.id), [0, 0, 0], 0),
        "a vertex without triangles has a zero normal");
    });
  }
}

// ---------------------------------------------------------------------------
// edit_topology: growing/shrinking a connected face, stable ids across
// disappearance, and sign-preserving value edits that leave topology alone.
// ---------------------------------------------------------------------------
function edit_topology(c) {
  var dims = [7, 7, 7];
  var nx = dims[0], ny = dims[1];
  var cnx = nx - 1, cny = ny - 1;
  var values = new Float64Array(nx * ny * dims[2]).fill(1);

  //A solid slab: every sample with x <= 2 is negative, giving one connected
  //sheet of surface cells between grid columns 2 and 3.
  for (var iy = 0; iy < ny; ++iy) {
    for (var iz = 0; iz < dims[2]; ++iz) {
      for (var ix = 0; ix <= 2; ++ix) {
        values[sampleIdOf(ix, iy, iz, nx, ny)] = -1;
      }
    }
  }
  var editor = createSurfaceNetsEditor(dims, values);
  var before = editor.snapshot();
  var triIdsBefore = new Set(before.triangles.map(function(t) { return t.id; }));
  var vtxIdsBefore = new Set(before.vertices.map(function(v) { return v.id; }));

  //Grow a connected bump: flip the x=3 column negative over a small patch.
  var growEdits = [];
  for (var gy = 2; gy <= 4; ++gy) {
    for (var gz = 2; gz <= 4; ++gz) {
      growEdits.push({ index: sampleIdOf(3, gy, gz, nx, ny), value: -1 });
    }
  }
  var growPatch = editor.update(growEdits);
  var grown = editor.snapshot();
  c.check(grown.triangles.length > before.triangles.length,
    "growing a connected bump increases the triangle count");
  c.check(grown.vertices.length > before.vertices.length,
    "growing a connected bump increases the vertex count");
  c.check(growPatch.trianglesUpsert.length > 0,
    "growing a face emits new triangles");
  c.check(isSortedUnique(growPatch.trianglesUpsert.map(function(t) { return t.id; })),
    "added triangle ids are sorted and unique");
  growPatch.trianglesUpsert.forEach(function(t) {
    c.check(!triIdsBefore.has(t.id),
      "grown triangles carry ids that did not exist before");
  });
  growPatch.trianglesRemove.forEach(function(id) {
    c.check(triIdsBefore.has(id),
      "any triangle removed while growing previously existed");
  });
  var grownIds = new Set(grown.triangles.map(function(t) { return t.id; }));
  triIdsBefore.forEach(function(id) {
    if (!grownIds.has(id)) {
      c.check(growPatch.trianglesRemove.indexOf(id) !== -1,
        "a missing pre-existing id is always announced in trianglesRemove");
    }
  });
  //Every newly added triangle uses a stable id derived from its cell/edge:
  //id = 2*(3*s+axis)(+1), and its vertices must include the cell at the
  //edge's lower corner sample s.
  growPatch.trianglesUpsert.forEach(function(t) {
    var eid = Math.floor(t.id / 2);
    var axis = eid % 3;
    var s = (eid - axis) / 3;
    c.check(axis >= 0 && axis <= 2 && s >= 0 && s < nx * ny * dims[2] &&
      (t.id === 2 * eid || t.id === 2 * eid + 1),
      "triangle id follows the 2*(3*s+axis)(+1) scheme");
    var lowerCell = (function() {
      var sx = s % nx;
      var tt = (s - sx) / nx;
      var sy = tt % ny;
      var sz = (tt - sy) / ny;
      return cellIdOf(sx, sy, sz, cnx, cny);
    })();
    if (t.id === 2 * eid) {
      //The first of the two faces always contains the lower-corner cell.
      c.check(t.vertices.indexOf(lowerCell) !== -1,
        "first triangle id's edge belongs to the cell of its lower corner");
    } else {
      //The second face is the diagonal across the three neighbour cells, so
      //each of its vertices must be a distinct neighbour of that cell.
      c.check(t.vertices.length === 3 &&
        new Set(t.vertices).size === 3 &&
        t.vertices.every(function(v) { return v !== lowerCell; }),
        "second triangle is the neighbour-cell diagonal face");
    }
    t.vertices.forEach(function(v) {
      c.check(v >= 0 && v < cnx * cny * (dims[2] - 1),
        "triangle references a valid stable vertex id");
    });
  });

  //Shrink back: the mesh must return to exactly the original id sets.
  var shrinkEdits = growEdits.map(function(ch) {
    return { index: ch.index, value: 1 };
  });
  var shrinkPatch = editor.update(shrinkEdits);
  c.check(shrinkPatch.verticesRemove.length > 0,
    "shrinking announces the cells that disappear");
  var shrunk = editor.snapshot();
  c.check(shrunk.triangles.map(function(t) { return t.id; }).join(",") ===
    before.triangles.map(function(t) { return t.id; }).join(","),
    "shrinking restores the exact original triangle id set");
  c.check(shrunk.vertices.map(function(v) { return v.id; }).join(",") ===
    before.vertices.map(function(v) { return v.id; }).join(","),
    "shrinking restores the exact original vertex id set");
  shrunk.vertices.forEach(function(v, k) {
    c.check(vecApprox(v.position, before.vertices[k].position, EPS) &&
      vecApprox(v.normal, before.vertices[k].normal, EPS),
      "shrinking restores the original vertex records");
  });

  //Re-grow: cells and triangles that disappeared must reclaim their old ids
  //and reproduce the grown mesh.
  var regrowPatch = editor.update(growEdits);
  c.check(regrowPatch.trianglesUpsert.length > 0, "re-growth emits triangles");
  var regrown = editor.snapshot();
  c.check(regrown.triangles.map(function(t) { return t.id; }).join(",") ===
    grown.triangles.map(function(t) { return t.id; }).join(","),
    "reappearing triangles keep their original stable ids");
  c.check(regrown.vertices.map(function(v) { return v.id; }).join(",") ===
    grown.vertices.map(function(v) { return v.id; }).join(","),
    "reappearing cells keep their original stable vertex ids");
  regrown.vertices.forEach(function(v, k) {
    c.check(vecApprox(v.position, grown.vertices[k].position, EPS),
      "reappearing vertices reproduce their earlier positions");
  });

  //Sign-preserving magnitude edits near the surface must not change topology.
  //Pick samples outside the bump (y=1) so their current signs are still -1/+1.
  var negSample = sampleIdOf(2, 1, 3, nx, ny);
  var posSample = sampleIdOf(3, 1, 3, nx, ny);
  var signPatch = editor.update([
    { index: negSample, value: -0.25 },
    { index: posSample, value: 0.25 }
  ]);
  c.check(signPatch.trianglesUpsert.length === 0 &&
    signPatch.trianglesRemove.length === 0,
    "edits that preserve every corner sign leave topology untouched");
  c.check(signPatch.work.cellIds.length > 0 &&
    signPatch.work.edgeIds.length > 0,
    "sign-preserving edits still recompute the local dependency sets");
  var afterSign = editor.snapshot();
  c.check(afterSign.triangles.length === grown.triangles.length,
    "no triangle is created or destroyed by a sign-preserving edit");
  afterSign.triangles.forEach(function(t) {
    c.check(grownIds.has(t.id), "triangle identity set is preserved");
  });

  //Empty batches and final values equal to the current field do no work at all.
  var emptyPatch = editor.update([]);
  [["verticesUpsert", []], ["verticesRemove", []],
   ["trianglesUpsert", []], ["trianglesRemove", []]].forEach(function(p) {
    c.check(Array.isArray(emptyPatch[p[0]]) && emptyPatch[p[0]].length === 0,
      "empty batch returns an empty " + p[0]);
  });
  c.check(emptyPatch.work.cellIds.length === 0 &&
    emptyPatch.work.edgeIds.length === 0,
    "empty batch reports no work");
  var untouchedSample = sampleIdOf(5, 5, 5, nx, ny);
  var noopPatch = editor.update([
    { index: untouchedSample, value: 1 }
  ]);
  c.check(noopPatch.verticesUpsert.length === 0 &&
    noopPatch.verticesRemove.length === 0 &&
    noopPatch.trianglesUpsert.length === 0 &&
    noopPatch.trianglesRemove.length === 0 &&
    noopPatch.work.cellIds.length === 0 &&
    noopPatch.work.edgeIds.length === 0,
    "an edit whose final value matches the field does no work");

  //Last write in a duplicated batch wins, compared against the pre-edit value.
  //(5,5,5) was never touched by the bump edits, so its field value is still +1.
  var target = sampleIdOf(5, 5, 5, nx, ny);
  var dupPatch = editor.update([
    { index: target, value: 1 },
    { index: target, value: -0.9 },
    { index: target, value: 1 }
  ]);
  c.check(dupPatch.work.cellIds.length === 0,
    "duplicated indices collapse to the last value, which here is a no-op");
  c.check(vtxIdsBefore.size > 0, "fixture keeps a non-empty mesh throughout");
}

// ---------------------------------------------------------------------------
// normal_closure: snapshot normals equal an independent accumulation of
// oriented cross products, including zero normals and propagation to vertices
// that never moved.
// ---------------------------------------------------------------------------
function normal_closure(c) {
  var dims = [9, 8, 10];
  var bounds = [[-4, -8, -1], [4, 4, 9]];
  var nx = dims[0], ny = dims[1], nz = dims[2];
  var scale = dims.map(function(d, k) {
    return (bounds[1][k] - bounds[0][k]) / d;
  });
  function fieldAt(ix, iy, iz) {
    var x = bounds[0][0] + scale[0] * ix;
    var y = bounds[0][1] + scale[1] * iy;
    var z = bounds[0][2] + scale[2] * iz;
    //An off-centre sphere on strongly anisotropic world axes.
    return (x + 0.7) * (x + 0.7)
      + (y - 1.1) * (y - 1.1)
      + 0.25 * (z - 2.0) * (z - 2.0) - 4.0;
  }
  var values = new Float64Array(nx * ny * nz);
  for (var iz = 0; iz < nz; ++iz) {
    for (var iy = 0; iy < ny; ++iy) {
      for (var ix = 0; ix < nx; ++ix) {
        values[sampleIdOf(ix, iy, iz, nx, ny)] = fieldAt(ix, iy, iz);
      }
    }
  }
  var editor = createSurfaceNetsEditor(dims, values, bounds);
  var snap = editor.snapshot();
  c.check(snap.triangles.length > 0, "normal fixture contains a surface");

  var expectedNormals = independentNormals(snap);
  snap.vertices.forEach(function(v) {
    c.check(vecApprox(v.normal, expectedNormals.get(v.id), 1e-12),
      "snapshot normal matches independent cross-product accumulation");
  });

  //At least one active boundary cell has no triangles and a zero normal.
  var referenced = new Set();
  snap.triangles.forEach(function(t) {
    t.vertices.forEach(function(id) { referenced.add(id); });
  });
  var loose = snap.vertices.filter(function(v) { return !referenced.has(v.id); });
  c.check(loose.length > 0, "fixture exposes triangle-free boundary vertices");
  loose.forEach(function(v) {
    c.check(v.normal[0] === 0 && v.normal[1] === 0 && v.normal[2] === 0,
      "triangle-free vertices report the exact zero normal");
  });

  //Propagation: magnitude-only edits move some vertices while leaving the
  //triangle index structure intact; neighbouring vertices that did not move
  //must still receive updated normals through the persistent adjacency.
  var foundPropagated = false;
  var candidates = [];
  for (var k = 0; k < 12; ++k) {
    candidates.push(sampleIdOf(2 + (k % 5), 1 + ((k * 3) % 6), 2 + ((k * 2) % 7), nx, ny));
  }
  for (var ci = 0; ci < candidates.length && !foundPropagated; ++ci) {
    var idx = candidates[ci];
    var oldValue = values[idx];
    if (oldValue === 0) continue;
    var preSnap = editor.snapshot();
    var prePos = new Map();
    preSnap.vertices.forEach(function(v) { prePos.set(v.id, v.position); });
    var patch = editor.update([{ index: idx, value: oldValue * 0.15 }]);
    values[idx] = oldValue * 0.15;
    if (patch.trianglesUpsert.length !== 0 || patch.trianglesRemove.length !== 0) {
      continue; //need a topology-stable round to isolate normal propagation
    }
    var postSnap = editor.snapshot();
    var postNormals = independentNormals(postSnap);
    var inCellSet = new Set(patch.work.cellIds);
    for (var ui = 0; ui < patch.verticesUpsert.length; ++ui) {
      var rec = patch.verticesUpsert[ui];
      if (inCellSet.has(rec.id)) continue;
      //Vertex outside the recomputed-cell set: its position cannot have moved.
      var beforePos = prePos.get(rec.id);
      c.check(!!beforePos && vecApprox(beforePos, rec.position, 0),
        "propagated-update vertex did not move");
      c.check(vecApprox(rec.normal, postNormals.get(rec.id), 1e-12),
        "a non-moved neighbour receives the recomputed incident-area normal");
      foundPropagated = true;
      break;
    }
  }
  c.check(foundPropagated,
    "a normal change propagates to a neighbour vertex outside the cell set");

  //The final full snapshot is still internally normal-consistent.
  var finalNormals = independentNormals(editor.snapshot());
  editor.snapshot().vertices.forEach(function(v) {
    c.check(vecApprox(v.normal, finalNormals.get(v.id), 1e-12),
      "normals stay consistent after propagation edits");
  });
}

// ---------------------------------------------------------------------------
// sparse_replay: many random edits replayed through a renderer-side mirror,
// exact local work sets, diff minimality and encapsulation of internal state.
// ---------------------------------------------------------------------------
function sparse_replay(c) {
  var rnd = mulberry32(20261114);
  var dims = [7, 6, 5];
  var bounds = [[-2, -5, 1], [12, 7, 11]];
  var n = dims[0] * dims[1] * dims[2];
  var values = new Float64Array(n);
  for (var i = 0; i < n; ++i) values[i] = rnd() * 2 - 1;
  var field = values.slice();

  var editor = createSurfaceNetsEditor(dims, values, bounds);
  var mirrorV = new Map();
  var mirrorT = new Map();
  function consume(patch) {
    patch.trianglesRemove.forEach(function(id) { mirrorT.delete(id); });
    patch.verticesRemove.forEach(function(id) { mirrorV.delete(id); });
    patch.verticesUpsert.forEach(function(rec) {
      mirrorV.set(rec.id, {
        id: rec.id,
        position: rec.position.slice(),
        normal: rec.normal.slice()
      });
    });
    patch.trianglesUpsert.forEach(function(rec) {
      mirrorT.set(rec.id, rec.vertices.slice());
    });
  }
  editor.snapshot().vertices.forEach(function(v) {
    mirrorV.set(v.id, { id: v.id, position: v.position.slice(), normal: v.normal.slice() });
  });
  editor.snapshot().triangles.forEach(function(t) {
    mirrorT.set(t.id, t.vertices.slice());
  });

  function checkMirrorAgainst(snap) {
    c.check(mirrorV.size === snap.vertices.length,
      "renderer mirror vertex count matches snapshot");
    c.check(mirrorT.size === snap.triangles.length,
      "renderer mirror triangle count matches snapshot");
    snap.vertices.forEach(function(v) {
      var m = mirrorV.get(v.id);
      c.check(!!m && vecApprox(m.position, v.position, EPS) &&
        vecApprox(m.normal, v.normal, EPS),
        "mirror vertex matches snapshot after applying the patch");
    });
    snap.triangles.forEach(function(t) {
      var m = mirrorT.get(t.id);
      c.check(!!m && m.join(",") === t.vertices.join(","),
        "mirror triangle matches snapshot after applying the patch");
    });
  }

  var rounds = 60;
  for (var round = 0; round < rounds; ++round) {
    var changes = [];
    var count = 1 + Math.floor(rnd() * 5);
    for (var q = 0; q < count; ++q) {
      changes.push({ index: Math.floor(rnd() * n), value: rnd() * 2 - 1 });
    }
    if (round % 4 === 1) {
      //Duplicated indices: last write must win.
      changes.push({ index: changes[0].index, value: rnd() * 2 - 1 });
      changes.push({ index: changes[0].index, value: rnd() * 2 - 1 });
    }
    if (round % 7 === 3) {
      //A guaranteed no-op entry mixed into the batch.
      changes.push({ index: 0, value: field[0] });
    }

    var last = new Map();
    changes.forEach(function(ch) { last.set(ch.index, ch.value); });
    var effective = [];
    last.forEach(function(value, index) {
      if (value !== field[index]) effective.push([index, value]);
    });

    var patch = editor.update(changes);

    //Every output collection is ascending and duplicate-free.
    c.check(isSortedUnique(patch.verticesRemove), "verticesRemove sorted/unique");
    c.check(isSortedUnique(patch.trianglesRemove), "trianglesRemove sorted/unique");
    c.check(isSortedUnique(patch.verticesUpsert.map(function(r) { return r.id; })),
      "verticesUpsert sorted/unique");
    c.check(isSortedUnique(patch.trianglesUpsert.map(function(r) { return r.id; })),
      "trianglesUpsert sorted/unique");
    c.check(isSortedUnique(patch.work.cellIds), "work.cellIds sorted/unique");
    c.check(isSortedUnique(patch.work.edgeIds), "work.edgeIds sorted/unique");

    //The work sets must be exactly the local dependency sets.
    var want = expectedWork(dims, effective);
    c.check(patch.work.cellIds.join(",") === want.cellIds.join(","),
      "cell work set is exactly the changed-sample neighbourhood");
    c.check(patch.work.edgeIds.join(",") === want.edgeIds.join(","),
      "edge work set is exactly the twelve-edge interior closure");

    //Upserts are complete records, never bare ids.
    patch.verticesUpsert.forEach(function(rec) {
      c.check(typeof rec.id === "number" &&
        Array.isArray(rec.position) && rec.position.length === 3 &&
        Array.isArray(rec.normal) && rec.normal.length === 3 &&
        rec.position.every(Number.isFinite) &&
        rec.normal.every(Number.isFinite),
        "vertex upsert carries a complete finite record");
    });
    patch.trianglesUpsert.forEach(function(rec) {
      c.check(typeof rec.id === "number" &&
        Array.isArray(rec.vertices) && rec.vertices.length === 3,
        "triangle upsert carries a complete record");
    });

    //Apply to the renderer mirror and compare against the live snapshot.
    consume(patch);
    var snap = editor.snapshot();
    checkMirrorAgainst(snap);

    //A fresh full editor on the accumulated field must agree exactly.
    effective.forEach(function(pair) { field[pair[0]] = pair[1]; });
    var freshSnap = createSurfaceNetsEditor(dims, field, bounds).snapshot();
    c.check(freshSnap.vertices.length === snap.vertices.length &&
      freshSnap.triangles.length === snap.triangles.length,
      "incremental mesh matches a fresh full rebuild (counts)");
    for (var vi = 0; vi < snap.vertices.length; ++vi) {
      c.check(freshSnap.vertices[vi].id === snap.vertices[vi].id,
        "incremental and fresh vertex ids agree");
      c.check(vecApprox(freshSnap.vertices[vi].position,
        snap.vertices[vi].position, EPS),
        "incremental and fresh vertex positions agree");
      c.check(vecApprox(freshSnap.vertices[vi].normal,
        snap.vertices[vi].normal, EPS),
        "incremental and fresh vertex normals agree");
    }
    for (var ti = 0; ti < snap.triangles.length; ++ti) {
      c.check(freshSnap.triangles[ti].id === snap.triangles[ti].id &&
        freshSnap.triangles[ti].vertices.join(",") ===
        snap.triangles[ti].vertices.join(","),
        "incremental and fresh triangles agree");
    }

    //Replaying the same batch must now be a complete no-op.
    var replay = editor.update(changes.map(function(ch) {
      return { index: ch.index, value: field[ch.index] };
    }));
    c.check(replay.verticesUpsert.length === 0 &&
      replay.verticesRemove.length === 0 &&
      replay.trianglesUpsert.length === 0 &&
      replay.trianglesRemove.length === 0 &&
      replay.work.cellIds.length === 0 &&
      replay.work.edgeIds.length === 0,
      "replaying settled values produces an empty patch");
  }

  //Encapsulation: mutating returned records must not corrupt editor state,
  //and snapshot must hand out fresh arrays on every call.
  var s1 = editor.snapshot();
  if (s1.vertices.length > 0) {
    s1.vertices[0].position[0] = 9999;
    s1.vertices[0].normal[2] = 9999;
  }
  if (s1.triangles.length > 0) {
    s1.triangles[0].vertices[0] = 9999;
  }
  var s2 = editor.snapshot();
  var tampered = false;
  if (s1.vertices.length > 0) {
    tampered = tampered || s2.vertices[0].position[0] === 9999;
    tampered = tampered || s2.vertices[0].normal[2] === 9999;
  }
  if (s1.triangles.length > 0) {
    tampered = tampered || s2.triangles[0].vertices[0] === 9999;
  }
  c.check(!tampered, "snapshot records are detached from internal arrays");
  c.check(s1.vertices !== s2.vertices && s1.triangles !== s2.triangles,
    "each snapshot allocates fresh output arrays");
}

module.exports = {
  geometry_identity: geometry_identity,
  edit_topology: edit_topology,
  normal_closure: normal_closure,
  sparse_replay: sparse_replay
};
