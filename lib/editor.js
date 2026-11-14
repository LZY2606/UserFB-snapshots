/**
 * Persistent, incrementally editable Surface Nets mesh.
 *
 * The extraction itself is unchanged from lib/surfacenets.js (same zero
 * level, negative-value classification, averaged 12-edge intersections,
 * 1e-6 interpolation guard, face winding and diagonals); only the bookkeeping
 * is incremental.  Vertices keep the stable id of their cell and triangles the
 * stable id derived in the task specification, so patches can be replayed
 * against a renderer's own buffers.
 */
"use strict";

var sn = require("./surfacenets.js")
  , edge_table = sn.edge_table
  , computeScaleShift = sn.computeScaleShift
  , evalCell = sn.evalCell;

var EPS = 1e-12;

function changedComponentwise(a, b) {
  return Math.abs(a[0] - b[0]) > EPS
      || Math.abs(a[1] - b[1]) > EPS
      || Math.abs(a[2] - b[2]) > EPS;
}

//Cell <-> linear sample index helpers.
//Cell id:   id  = x + (nx-1) * (y + (ny-1) * z)
//Sample id: s   = x + nx * (y + ny * z)
function cellToS(id, cnx, cny, nx, ny) {
  var x = id % cnx
    , t = (id - x) / cnx
    , y = t % cny
    , z = (t - y) / cny;
  return x + nx * (y + ny * z);
}
function sToCellId(s, cnx, cny, nx, ny) {
  var x = s % nx
    , t = (s - x) / nx
    , y = t % ny
    , z = (t - y) / ny;
  return x + cnx * (y + cny * z);
}

//Two triangles per grid edge, ids 2*(3*s+axis) and 2*(3*s+axis)+1, in the
//order the original extraction pushes them.
function triIdsForEdge(eid) {
  return [2 * eid, 2 * eid + 1];
}

function createSurfaceNetsEditor(dims, values, bounds) {
  dims = [dims[0], dims[1], dims[2]];
  var nx = dims[0]
    , ny = dims[1]
    , nz = dims[2]
    , cnx = nx - 1
    , cny = ny - 1
    , cnz = nz - 1
    , ncells = cnx * cny * cnz
    , n01 = nx * ny;

  if(!bounds) {
    bounds = [[0,0,0], dims];
  }
  var mapping = computeScaleShift(dims, bounds)
    , scale = mapping.scale
    , shift = mapping.shift;

  //Private mutable state.  Nothing reachable from the public API aliases it.
  var field = new Float64Array(values)            //editable sample copy
    , cellActive = new Uint8Array(ncells)          //sparse presence flags
    , cellMask = new Int32Array(ncells)            //last corner sign masks
    , cellPos = new Float64Array(3 * ncells)       //last world positions
    , triStore = new Map();                         //tri id -> [a,b,c]

  //Persistent triangle adjacency: the set of live triangle ids incident on
  //each vertex.  Local normal updates sum cross products over exactly this
  //set, so a vertex with no triangles gets an exact zero normal and no
  //full-mesh normal pass is ever needed.
  var incident = new Array(ncells);
  for(var i=0; i<ncells; ++i) {
    incident[i] = new Set();
  }
  var normals = new Float64Array(3 * ncells);

  function cellXYZ(id) {
    var x = id % cnx
      , t = (id - x) / cnx
      , y = t % cny
      , z = (t - y) / cny;
    return [x, y, z];
  }

  function crossOf(verts) {
    var a = verts[0], b = verts[1], c = verts[2]
      , oa = 3 * a, ob = 3 * b, oc = 3 * c;
    var ux = cellPos[ob]   - cellPos[oa]
      , uy = cellPos[ob+1] - cellPos[oa+1]
      , uz = cellPos[ob+2] - cellPos[oa+2]
      , wx = cellPos[oc]   - cellPos[oa]
      , wy = cellPos[oc+1] - cellPos[oa+1]
      , wz = cellPos[oc+2] - cellPos[oa+2];
    return [
      uy * wz - uz * wy,
      uz * wx - ux * wz,
      ux * wy - uy * wx
    ];
  }

  //Recompute one vertex normal from its persistent incident triangles,
  //using current world positions (anisotropic bounds included).  With no
  //incident triangle, or an exactly zero sum, the normal is [0,0,0].
  function recomputeNormal(id) {
    var o = 3 * id
      , ax = 0, ay = 0, az = 0;
    var tris = incident[id];
    var it = tris.values();
    for(var e = it.next(); !e.done; e = it.next()) {
      var cr = crossOf(triStore.get(e.value));
      ax += cr[0];
      ay += cr[1];
      az += cr[2];
    }
    var len = Math.sqrt(ax * ax + ay * ay + az * az);
    if(len > 0) {
      normals[o]   = ax / len;
      normals[o+1] = ay / len;
      normals[o+2] = az / len;
    } else {
      normals[o] = 0;
      normals[o+1] = 0;
      normals[o+2] = 0;
    }
  }

  function addTriangle(tid, verts) {
    triStore.set(tid, verts);
    for(var k=0; k<3; ++k) {
      incident[verts[k]].add(tid);
    }
  }

  function removeTriangle(tid) {
    var verts = triStore.get(tid);
    for(var k=0; k<3; ++k) {
      incident[verts[k]].delete(tid);
    }
    triStore.delete(tid);
  }

  //All twelve grid edges touching a cell, restricted to the interior
  //positions where the original extraction can own two triangles: the edge
  //axis coordinate is in [0, dims[axis]-2] and each perpendicular start
  //coordinate in [1, dims[a]-2].  Rebuilding every one of these is what lets
  //a moved vertex withdraw and re-add the area contribution of every
  //triangle it belongs to, including triangles owned by neighbouring cells.
  function cellEdgeIds(cid, out) {
    var xyz = cellXYZ(cid)
      , x = xyz[0], y = xyz[1], z = xyz[2]
      , s = cellToS(cid, cnx, cny, nx, ny);
    var dy, dz, dx;
    //Edges along x: start (x, y+dy, z+dz)
    for(dy=0; dy<=1; ++dy)
    for(dz=0; dz<=1; ++dz) {
      var yu = y + dy, zu = z + dz;
      if(yu >= 1 && yu <= ny - 2 && zu >= 1 && zu <= nz - 2) {
        out.push(3 * (s + dy * nx + dz * n01));
      }
    }
    //Edges along y: start (x+dx, y, z+dz)
    for(dx=0; dx<=1; ++dx)
    for(dz=0; dz<=1; ++dz) {
      var xu = x + dx;
      zu = z + dz;
      if(xu >= 1 && xu <= nx - 2 && zu >= 1 && zu <= nz - 2) {
        out.push(3 * (s + dx + dz * n01) + 1);
      }
    }
    //Edges along z: start (x+dx, y+dy, z)
    for(dx=0; dx<=1; ++dx)
    for(dy=0; dy<=1; ++dy) {
      xu = x + dx;
      yu = y + dy;
      if(xu >= 1 && xu <= nx - 2 && yu >= 1 && yu <= ny - 2) {
        out.push(3 * (s + dx + dy * nx) + 2);
      }
    }
    return out;
  }

  //Detach whatever triangles currently live on a grid edge.  Must run while
  //cellPos still holds the positions the triangles were built with, so the
  //later normal recomputation withdraws their old area contribution.
  function removeEdgeTris(eid) {
    var tids = triIdsForEdge(eid);
    for(var k=0; k<2; ++k) {
      if(triStore.has(tids[k])) {
        removeTriangle(tids[k]);
      }
    }
  }

  //Rebuild the connectivity of one grid edge from the four surrounding cell
  //states.  This is the direct, id-based equivalent of the original buffer
  //walk, including the edge-crossing test, the mask&1 winding flip and both
  //diagonals.
  function buildEdgeTris(eid) {
    var axis = eid % 3
      , ebase = (eid - axis) / 3
      , iu = (axis + 1) % 3
      , iv = (axis + 2) % 3
      , cellStrides = [1, cnx, cnx * cny]
      , cu = cellStrides[iu]
      , cv = cellStrides[iv];
    //Cell at the edge's lower-iu/lower-iv corner corresponds to sample s;
    //the other three sit one cell back along the perpendicular axes.
    var c0 = sToCellId(ebase, cnx, cny, nx, ny)
      , c1 = c0 - cu
      , c2 = c0 - cv
      , c3 = c0 - cu - cv;

    //The original emits only when the lower-corner edge actually crosses the
    //zero level and the cell is not on a perpendicular boundary.  A crossing
    //also implies all four surrounding cells are active.
    if(!cellActive[c0] || !(edge_table[cellMask[c0]] & (1 << axis))) {
      return;
    }
    if(!cellActive[c1] || !cellActive[c2] || !cellActive[c3]) {
      return;
    }

    var triIds = triIdsForEdge(eid)
      , verts0, verts1;
    if(cellMask[c0] & 1) {
      verts0 = [c0, c1, c2];
      verts1 = [c2, c1, c3];
    } else {
      verts0 = [c0, c2, c1];
      verts1 = [c1, c2, c3];
    }
    addTriangle(triIds[0], verts0);
    addTriangle(triIds[1], verts1);
  }

  //Initial full build.  This runs once at construction; updates never walk
  //the whole grid.
  (function buildAll() {
    for(var z=0; z<cnz; ++z)
    for(var y=0; y<cny; ++y)
    for(var x=0; x<cnx; ++x) {
      var id = x + cnx * (y + cny * z);
      var r = evalCell(field, dims, x, y, z, scale, shift);
      if(r) {
        cellActive[id] = 1;
        cellMask[id] = r.mask;
        cellPos[3*id]   = r.position[0];
        cellPos[3*id+1] = r.position[1];
        cellPos[3*id+2] = r.position[2];
      }
    }
    var initialEdges = new Set();
    for(var cid=0; cid<ncells; ++cid) {
      if(!cellActive[cid]) {
        continue;
      }
      cellEdgeIds(cid, []).forEach(function(eid) { initialEdges.add(eid); });
    }
    var initialEdgeIds = Array.from(initialEdges)
      .sort(function(a, b) { return a - b; });
    for(var k=0; k<initialEdgeIds.length; ++k) {
      buildEdgeTris(initialEdgeIds[k]);
    }
    for(var id2=0; id2<ncells; ++id2) {
      if(cellActive[id2]) {
        recomputeNormal(id2);
      }
    }
  })();

  function snapshot() {
    var vertices = []
      , triangles = [];
    for(var id=0; id<ncells; ++id) {
      if(cellActive[id]) {
        vertices.push({
          id: id,
          position: [cellPos[3*id], cellPos[3*id+1], cellPos[3*id+2]],
          normal:   [normals[3*id], normals[3*id+1], normals[3*id+2]]
        });
      }
    }
    //Map keys iterate in insertion order, not id order; collect and sort.
    var sorted = Array.from(triStore.keys())
      .sort(function(a, b) { return a - b; });
    for(var k=0; k<sorted.length; ++k) {
      var tid = sorted[k]
        , v = triStore.get(tid);
      triangles.push({ id: tid, vertices: [v[0], v[1], v[2]] });
    }
    return { vertices: vertices, triangles: triangles };
  }

  function emptyPatch() {
    return {
      verticesUpsert: [],
      verticesRemove: [],
      trianglesUpsert: [],
      trianglesRemove: [],
      work: { cellIds: [], edgeIds: [] }
    };
  }

  function update(changes) {
    if(!changes || changes.length === 0) {
      return emptyPatch();
    }

    //Dedup by sample index, last write in the batch wins.
    var lastValue = new Map();
    for(var k=0; k<changes.length; ++k) {
      lastValue.set(changes[k].index, changes[k].value);
    }

    //Keep only edits whose final value differs from the pre-edit field.
    var effective = [];
    var iter = lastValue.entries();
    for(var e = iter.next(); !e.done; e = iter.next()) {
      var idx = e.value[0]
        , val = e.value[1];
      if(val !== field[idx]) {
        effective.push([idx, val]);
      }
    }
    if(effective.length === 0) {
      return emptyPatch();
    }

    //Local cell dependency set: the up to eight cells touching each changed
    //sample.  No scan of the full volume.
    var cellSet = new Set();
    function addAdjacentCells(s) {
      var sx = s % nx
        , tt = (s - sx) / nx
        , sy = tt % ny
        , sz = (tt - sy) / ny;
      for(var dz=-1; dz<=0; ++dz) {
        var z = sz + dz;
        if(z < 0 || z >= cnz) continue;
        for(var dy=-1; dy<=0; ++dy) {
          var y = sy + dy;
          if(y < 0 || y >= cny) continue;
          for(var dx=-1; dx<=0; ++dx) {
            var x = sx + dx;
            if(x < 0 || x >= cnx) continue;
            cellSet.add(x + cnx * (y + cny * z));
          }
        }
      }
    }
    for(k=0; k<effective.length; ++k) {
      addAdjacentCells(effective[k][0]);
    }
    var cellIds = Array.from(cellSet).sort(function(a, b) { return a - b; });

    //Local edge dependency set: every grid edge touching one of those cells
    //(all twelve cube edges per cell), restricted to interior positions.
    var edgeSet = new Set();
    for(k=0; k<cellIds.length; ++k) {
      var edges = [];
      cellEdgeIds(cellIds[k], edges);
      for(var q=0; q<edges.length; ++q) {
        edgeSet.add(edges[q]);
      }
    }
    var edgeIds = Array.from(edgeSet).sort(function(a, b) { return a - b; });

    //Capture pre-edit geometry/connectivity for the diff.
    var oldPos = new Map()
      , oldActive = new Map();
    for(k=0; k<cellIds.length; ++k) {
      var cid = cellIds[k];
      oldActive.set(cid, cellActive[cid] === 1);
      if(cellActive[cid]) {
        oldPos.set(cid, [cellPos[3*cid], cellPos[3*cid+1], cellPos[3*cid+2]]);
      }
    }
    var oldTris = new Map();
    for(k=0; k<edgeIds.length; ++k) {
      var tids = triIdsForEdge(edgeIds[k]);
      for(q=0; q<2; ++q) {
        var tid = tids[q];
        if(triStore.has(tid)) {
          var tv = triStore.get(tid);
          oldTris.set(tid, [tv[0], tv[1], tv[2]]);
        }
      }
    }

    //Phase 1: detach old triangles (and their vertex adjacency) while cellPos
    //still holds the positions those triangles were built from.
    for(k=0; k<edgeIds.length; ++k) {
      removeEdgeTris(edgeIds[k]);
    }

    //Phase 2: apply the field edits and re-evaluate the affected cells.
    for(k=0; k<effective.length; ++k) {
      field[effective[k][0]] = effective[k][1];
    }
    for(k=0; k<cellIds.length; ++k) {
      cid = cellIds[k];
      var xyz = cellXYZ(cid)
        , r = evalCell(field, dims, xyz[0], xyz[1], xyz[2], scale, shift);
      if(r) {
        cellActive[cid] = 1;
        cellMask[cid] = r.mask;
        cellPos[3*cid]   = r.position[0];
        cellPos[3*cid+1] = r.position[1];
        cellPos[3*cid+2] = r.position[2];
      } else {
        cellActive[cid] = 0;
        cellMask[cid] = 0;
      }
    }

    //Phase 3: reconnect the affected edges (re-establishing adjacency).
    for(k=0; k<edgeIds.length; ++k) {
      buildEdgeTris(edgeIds[k]);
    }

    //Normals are recomputed locally only: vertices of removed/added
    //triangles plus every re-evaluated cell.  Each normal sums the current
    //cross products of its persistent incident triangles, so vertices that
    //did not move still pick up changed contributions from neighbours.
    var normalSet = new Set();
    var oldVertLists = Array.from(oldTris.values());
    for(k=0; k<oldVertLists.length; ++k) {
      for(q=0; q<3; ++q) normalSet.add(oldVertLists[k][q]);
    }
    for(k=0; k<edgeIds.length; ++k) {
      tids = triIdsForEdge(edgeIds[k]);
      for(q=0; q<2; ++q) {
        if(triStore.has(tids[q])) {
          var nv = triStore.get(tids[q]);
          for(var h=0; h<3; ++h) normalSet.add(nv[h]);
        }
      }
    }
    for(k=0; k<cellIds.length; ++k) {
      if(cellActive[cellIds[k]]) {
        normalSet.add(cellIds[k]);
      }
    }
    var normalIds = Array.from(normalSet).sort(function(a, b) { return a - b; });

    //Remember the pre-update normal of every vertex we are about to touch,
    //including neighbours outside cellIds whose geometry did not move.
    var oldNormalAll = new Map();
    for(k=0; k<normalIds.length; ++k) {
      var nid = normalIds[k];
      oldNormalAll.set(nid, [
        normals[3*nid], normals[3*nid+1], normals[3*nid+2]
      ]);
    }
    for(k=0; k<normalIds.length; ++k) {
      recomputeNormal(normalIds[k]);
    }

    //Diff vertices.  Presence/position changes only happen on re-evaluated
    //cells; normal changes can also reach neighbours outside that set, so the
    //normal comparison runs over every recomputed vertex.
    var verticesUpsert = []
      , verticesRemove = []
      , upsertSet = new Set();
    for(k=0; k<cellIds.length; ++k) {
      cid = cellIds[k];
      var wasActive = oldActive.get(cid)
        , isActive = cellActive[cid] === 1;
      if(wasActive && !isActive) {
        verticesRemove.push(cid);
      } else if(!wasActive && isActive) {
        verticesUpsert.push(cid);
        upsertSet.add(cid);
      } else if(isActive
                && changedComponentwise(
                     [cellPos[3*cid], cellPos[3*cid+1], cellPos[3*cid+2]],
                     oldPos.get(cid))) {
        verticesUpsert.push(cid);
        upsertSet.add(cid);
      }
    }
    for(k=0; k<normalIds.length; ++k) {
      nid = normalIds[k];
      if(!cellActive[nid] || upsertSet.has(nid)) {
        continue;
      }
      var newNormal = [normals[3*nid], normals[3*nid+1], normals[3*nid+2]];
      if(changedComponentwise(newNormal, oldNormalAll.get(nid))) {
        verticesUpsert.push(nid);
        upsertSet.add(nid);
      }
    }

    //Diff triangles.
    var trianglesUpsert = []
      , trianglesRemove = [];
    for(k=0; k<edgeIds.length; ++k) {
      tids = triIdsForEdge(edgeIds[k]);
      for(q=0; q<2; ++q) {
        tid = tids[q];
        var had = oldTris.has(tid)
          , has = triStore.has(tid);
        if(had && !has) {
          trianglesRemove.push(tid);
        } else if(has && !had) {
          trianglesUpsert.push(tid);
        } else if(has && had) {
          var before = oldTris.get(tid)
            , after = triStore.get(tid);
          if(before[0] !== after[0]
             || before[1] !== after[1]
             || before[2] !== after[2]) {
            trianglesUpsert.push(tid);
          }
        }
      }
    }
    verticesUpsert.sort(function(a, b) { return a - b; });
    verticesRemove.sort(function(a, b) { return a - b; });
    trianglesUpsert.sort(function(a, b) { return a - b; });
    trianglesRemove.sort(function(a, b) { return a - b; });

    function vertexRecord(id) {
      return {
        id: id,
        position: [cellPos[3*id], cellPos[3*id+1], cellPos[3*id+2]],
        normal:   [normals[3*id], normals[3*id+1], normals[3*id+2]]
      };
    }
    function triangleRecord(id) {
      var v = triStore.get(id);
      return { id: id, vertices: [v[0], v[1], v[2]] };
    }

    return {
      verticesUpsert: verticesUpsert.map(vertexRecord),
      verticesRemove: verticesRemove,
      trianglesUpsert: trianglesUpsert.map(triangleRecord),
      trianglesRemove: trianglesRemove,
      work: { cellIds: cellIds, edgeIds: edgeIds }
    };
  }

  return {
    snapshot: snapshot,
    update: update
  };
}

exports.createSurfaceNetsEditor = createSurfaceNetsEditor;
