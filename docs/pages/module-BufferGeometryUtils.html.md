# BufferGeometryUtils

## Import

BufferGeometryUtils is an addon, and must be imported explicitly, see [Installation#Addons](https://threejs.org/manual/#installation#addons).

```js
import * as BufferGeometryUtils from 'three/addons/utils/BufferGeometryUtils.js';
```

## Methods

### .bakeAffineGeometry( geometry : BufferGeometry, matrix : Matrix4 ) : BufferGeometry (inner)

Bakes an affine transformation into a new [BufferGeometry](BufferGeometry.html), including its morph targets, and returns the new geometry. The source geometry and the matrix are not modified, and the returned geometry owns independent arrays.

Only triangular meshes are supported, indexed or non-indexed. The geometry may carry `position` and `normal` morph attributes, but no skinning or instanced attributes. Plain [BufferAttribute](BufferAttribute.html) and [InterleavedBufferAttribute](InterleavedBufferAttribute.html) inputs are both accepted; interleaved storage may be de-interleaved in the result. Relative morph targets (`morphTargetsRelative = true`) are treated as displacement vectors, so the translation part of the matrix is never applied to them, while absolute targets are transformed as endpoints. For any finite morph weights, blending the baked geometry with the same weights gives the same positions as blending the source geometry first and then applying the matrix.

Positions and normals (base and morph) are written to `Float32` arrays. Base normals are unit length; morph normals use the inverse transpose without per-target normalization, which keeps linear normal blending correct under non-uniform scale and shear. Blended normals are compared after normalization; only states with a non-zero blended normal are considered, and artist-authored morph normals are never recomputed. Other attributes keep their typed array type, item size and `normalized` flag.

When the determinant of the matrix is negative, every triangle exchanges its second and third corner so the original front faces stay front faces: the index is rewritten for indexed geometry, and every per-vertex attribute (including morph records) is permuted for non-indexed geometry. Triangle order, groups, draw ranges and material groups are preserved. Existing tangents are re-orthogonalized against the transformed tangent plane, stay unit and orthogonal to their normal, and their `.w` reflects the new handedness; no tangent is generated when the source has none. UVs, colors and custom attributes do not participate in the geometric transform, but stay aligned with their vertices when a non-indexed reflection permutes corners.

Existing bounding boxes and spheres are rebuilt through the geometry's own computation entry points; absent bounds stay lazy.

The method throws when the geometry lacks a `position` attribute, has inconsistent attribute lengths, illegal triangle indices, skinning/instanced data, unsupported attribute types or morph kinds, or when the matrix is non-affine, singular, or contains non-finite values. This is a CPU-only utility with no browser or GPU dependency.

**geometry**

The geometry to bake. It is not modified.

**matrix**

The affine matrix to bake. It is not modified.

**Returns:** A new independent geometry with the transformation baked in.

### .computeMikkTSpaceTangents( geometry : BufferGeometry, MikkTSpace : Object, negateSign : boolean ) : BufferGeometry (inner)

Computes vertex tangents using the MikkTSpace algorithm. MikkTSpace generates the same tangents consistently, and is used in most modelling tools and normal map bakers. Use MikkTSpace for materials with normal maps, because inconsistent tangents may lead to subtle visual issues in the normal map, particularly around mirrored UV seams.

In comparison to this method, [BufferGeometry#computeTangents](BufferGeometry.html#computeTangents) (a custom algorithm) generates tangents that probably will not match the tangents in other software. The custom algorithm is sufficient for general use with a custom material, and may be faster than MikkTSpace.

Returns the original BufferGeometry. Indexed geometries will be de-indexed. Requires position, normal, and uv attributes. Tangents are stored like the normals: normalized Int8 or Int16 when the normals are, Float32 otherwise.

**geometry**

The geometry to compute tangents for.

**MikkTSpace**

Instance of `examples/jsm/libs/mikktspace.module.js`, or `mikktspace` npm package. The bundled JavaScript implementation is ready immediately. Await `MikkTSpace.ready` before using a WebAssembly implementation.

**negateSign**

Whether to negate the sign component (.w) of each tangent. Required for normal map conventions in some formats, including glTF.

Default is `true`.

**Returns:** The updated geometry.

### .computeMorphedAttributes( object : Mesh | Line | Points ) : Object (inner)

Calculates the morphed attributes of a morphed/skinned BufferGeometry.

Helpful for Raytracing or Decals (i.e. a `DecalGeometry` applied to a morphed Object with a `BufferGeometry` will use the original `BufferGeometry`, not the morphed/skinned one, generating an incorrect result. Using this function to create a shadow `Object3`D the `DecalGeometry` can be correctly generated).

**object**

The 3D object to compute morph attributes for.

**Returns:** An object with original position/normal attributes and morphed ones.

### .deepCloneAttribute( attribute : BufferAttribute ) : BufferAttribute (inner)

Performs a deep clone of the given buffer attribute.

**attribute**

The attribute to clone.

**Returns:** The cloned attribute.

### .deinterleaveAttribute( attribute : InterleavedBufferAttribute ) : BufferAttribute (inner)

Returns a new, non-interleaved version of the given attribute.

**attribute**

The interleaved attribute.

**Returns:** The non-interleaved attribute.

### .deinterleaveGeometry( geometry : BufferGeometry ) (inner)

Deinterleaves all attributes on the given geometry.

**geometry**

The geometry to deinterleave.

### .estimateBytesUsed( geometry : BufferGeometry ) : number (inner)

Returns the amount of bytes used by all attributes to represent the geometry.

**geometry**

The geometry.

**Returns:** The estimate bytes used.

### .interleaveAttributes( attributes : Array.<BufferAttribute> ) : Array.<InterleavedBufferAttribute> (inner)

Interleaves a set of attributes and returns a new array of corresponding attributes that share a single [InterleavedBuffer](InterleavedBuffer.html) instance. All attributes must have compatible types.

**attributes**

The attributes to interleave.

**Returns:** An array of interleaved attributes. If interleave does not succeed, the method returns `null`.

### .mergeAttributes( attributes : Array.<BufferAttribute> ) : BufferAttribute (inner)

Merges a set of attributes into a single instance. All attributes must have compatible properties and types. Instances of [InterleavedBufferAttribute](InterleavedBufferAttribute.html) are not supported.

**attributes**

The attributes to merge.

**Returns:** The merged attribute. Returns `null` if the merge does not succeed.

### .mergeGeometries( geometries : Array.<BufferGeometry>, useGroups : boolean ) : BufferGeometry (inner)

Merges a set of geometries into a single instance. All geometries must have compatible attributes.

**geometries**

The geometries to merge.

**useGroups**

Whether to use groups or not.

Default is `false`.

**Returns:** The merged geometry. Returns `null` if the merge does not succeed.

### .mergeGroups( geometry : BufferGeometry ) : BufferGeometry (inner)

Merges the [BufferGeometry#groups](BufferGeometry.html#groups) for the given geometry.

**geometry**

The geometry to modify.

**Returns:**

*   The updated geometry

### .mergeVertices( geometry : BufferGeometry, tolerance : number ) : BufferGeometry (inner)

Returns a new geometry with vertices for which all similar vertex attributes (within tolerance) are merged.

**geometry**

The geometry to merge vertices for.

**tolerance**

The tolerance value.

Default is `1e-4`.

**Returns:**

*   The new geometry with merged vertices.

### .remapAttribute( attribute : BufferAttribute | InterleavedBufferAttribute, sourceIndices : Array.<number> | TypedArray, buffers : Map.<InterleavedBuffer, InterleavedBuffer> ) : BufferAttribute | InterleavedBufferAttribute (inner)

Copies attribute entries in the order specified by the source vertex indices. Preserves raw values, attribute type and metadata. Reuse the same buffer map across calls to preserve shared interleaved buffers for the same mapping.

**attribute**

The source attribute.

**sourceIndices**

The source index for each output vertex.

**buffers**

Shared interleaved buffer copies.

**Returns:** The remapped attribute.

### .toCreasedNormals( geometry : BufferGeometry, creaseAngle : number ) : BufferGeometry (inner)

Modifies the supplied geometry if it is non-indexed, otherwise creates a new, non-indexed geometry. Returns the geometry with smooth normals everywhere except faces that meet at an angle greater than the crease angle.

**geometry**

The geometry to modify.

**creaseAngle**

The crease angle in radians.

Default is `Math.PI/3`.

**Returns:**

*   The updated geometry

### .toTrianglesDrawMode( geometry : BufferGeometry, drawMode : number ) : BufferGeometry (inner)

Converts the given geometry to the `TrianglesDrawMode` draw mode, which corresponds to the `gl.TRIANGLES` primitive in WebGL. The conversion only rewrites the index, so the geometry is modified in place and returned.

**geometry**

The geometry to convert.

**drawMode**

The current draw mode.

**Returns:** The converted geometry using `TrianglesDrawMode`.

## Source

[examples/jsm/utils/BufferGeometryUtils.js](https://github.com/mrdoob/three.js/blob/master/examples/jsm/utils/BufferGeometryUtils.js)