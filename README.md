isosurface
==========

Isosurface polygonizer algorithms in JavaScript.  For more details, see the following blog posts:

* [What is a solid?](http://0fps.wordpress.com/2012/08/29/what-is-a-solid/)
* Smooth voxel terrain [Part 1](http://0fps.wordpress.com/2012/07/10/smooth-voxel-terrain-part-1/) [Part 2](http://0fps.wordpress.com/2012/07/12/smooth-voxel-terrain-part-2/)

[Or try out a live demo](http://mikolalysenko.github.com/Isosurface/)

# Example

```javascript
var isosurface = require("isosurface")

var mesh = isosurface.surfaceNets([64,64,64], function(x,y,z) {
  return x*x + y*y + z*z - 100
}, [[-11,-11,-11], [11,11,11]])

console.log(mesh)
```


# Install

```
npm install isosurface
```
    
# API

```javascript
var isosurface = require("isosurface")
```

#### `isosurface.surfaceNets(dims, potential[, bounds])`
Extracts an isosurface from `potential` using surface nets with resolution given by `dims`.

Params:
* `dims`: A 3D vector of integers representing the resolution of the isosurface
* `potential(x,y,z)`: A scalar valued potential function taking 3 coordinates as arguments returning a scalar.
* `bounds`: A pair of 3D vectors `[lo, hi]` giving bounds on the potential to sample.  If not specified, default is `[[0,0,0], dims]`.

Returns: A mesh object with the following members:
* `positions`: The coordinates of the vertices of the mesh
* `cells`: The faces of the mesh.

#### `isosurface..marchingCubes(dims, potential[, bounds])`

Same as above, except uses marching cubes instead of surface nets to extract the isosurface.

#### `isosurface.marchingTetrahedra(dims, potential[, bounds])`

Same as above, except uses marching tetrahedra instead of surface nets to extract the isosurface.

#### `isosurface.createSurfaceNetsEditor(dims, values, bounds)`

A persistent, incrementally editable Surface Nets mesh.  `values` is an
`Float64Array` of scalar samples in x-fastest order with length
`dims[0] * dims[1] * dims[2]`; the editor keeps its own copy.  Extraction
follows exactly the same rules as `surfaceNets` (zero level, negative
classification, averaged edge intersections, original winding), but every
vertex keeps the stable id of its cell and every face a stable id, so local
edits produce a patch instead of a full rebuild.

* `snapshot()` returns `{ vertices, triangles }`, both sorted by id.  Each
  vertex is `{ id, position, normal }` (triangle-free boundary vertices are
  included); each triangle is `{ id, vertices: [a, b, c] }` referencing stable
  vertex ids.
* `update(changes)` takes an array of `{ index, value }` sample edits
  (duplicated indices resolve to the last value) and returns
  `{ verticesUpsert, verticesRemove, trianglesUpsert, trianglesRemove, work }`.
  Upserts carry complete records, removes carry ids, and every array is
  sorted and duplicate-free.  `work.cellIds` / `work.edgeIds` are the exact
  local dependency sets that were recomputed.  An empty batch, or one whose
  final values match the field, does no work.

```javascript
var isosurface = require("isosurface")

var dims   = [32, 32, 32]
var values = new Float64Array(dims[0] * dims[1] * dims[2])
for (var i = 0; i < values.length; ++i) values[i] = Math.random() * 2 - 1

var editor = isosurface.createSurfaceNetsEditor(dims, values,
  [[-10, -10, -10], [10, 10, 10]])

var mesh = editor.snapshot()
console.log(mesh.vertices.length, mesh.triangles.length)

//Edit a handful of samples and apply only the local patch.
var patch = editor.update([
  { index: 123, value:  0.75 },
  { index: 456, value: -0.25 },
  { index: 456, value: -0.9 }   // duplicated index: last write wins
])

//Renderer-side application order: remove faces, remove vertices, then
//upsert vertices and faces.
patch.trianglesRemove.forEach(function(id) { /* delete face */ })
patch.verticesRemove.forEach(function(id) { /* delete vertex */ })
patch.verticesUpsert.forEach(function(v) { /* insert/replace vertex v.id */ })
patch.trianglesUpsert.forEach(function(t) { /* insert/replace face t.id */ })

//The resulting buffers are identical to a fresh full snapshot.
var rebuilt = editor.snapshot()
```


# Credits
(c) 2012-2014 Mikola Lysenko.  MIT License