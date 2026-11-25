import {
	BufferAttribute,
	BufferGeometry,
	Float32BufferAttribute,
	InstancedBufferAttribute,
	InterleavedBuffer,
	InterleavedBufferAttribute,
	Matrix3,
	MathUtils,
	TriangleFanDrawMode,
	TriangleStripDrawMode,
	TrianglesDrawMode,
	Vector3,
	Vector4,
} from 'three';

/**
 * @module BufferGeometryUtils
 * @three_import import * as BufferGeometryUtils from 'three/addons/utils/BufferGeometryUtils.js';
 */

/**
 * Computes vertex tangents using the MikkTSpace algorithm. MikkTSpace generates the same tangents consistently,
 * and is used in most modelling tools and normal map bakers. Use MikkTSpace for materials with normal maps,
 * because inconsistent tangents may lead to subtle visual issues in the normal map, particularly around mirrored
 * UV seams.
 *
 * In comparison to this method, {@link BufferGeometry#computeTangents} (a custom algorithm) generates tangents that
 * probably will not match the tangents in other software. The custom algorithm is sufficient for general use with a
 * custom material, and may be faster than MikkTSpace.
 *
 * Returns the original BufferGeometry. Indexed geometries will be de-indexed. Requires position, normal, and uv attributes.
 * Tangents are stored like the normals: normalized Int8 or Int16 when the normals are, Float32 otherwise.
 *
 * @param {BufferGeometry} geometry - The geometry to compute tangents for.
 * @param {Object} MikkTSpace - Instance of `examples/jsm/libs/mikktspace.module.js`, or `mikktspace` npm package.
 * The bundled JavaScript implementation is ready immediately. Await `MikkTSpace.ready` before using a WebAssembly implementation.
 * @param {boolean} [negateSign=true] - Whether to negate the sign component (.w) of each tangent.
 * Required for normal map conventions in some formats, including glTF.
 * @return {BufferGeometry} The updated geometry.
 */
function computeMikkTSpaceTangents( geometry, MikkTSpace, negateSign = true ) {

	if ( ! MikkTSpace || ! MikkTSpace.isReady ) {

		throw new Error( 'THREE.BufferGeometryUtils: Initialized MikkTSpace library required.' );

	}

	if ( ! geometry.hasAttribute( 'position' ) || ! geometry.hasAttribute( 'normal' ) || ! geometry.hasAttribute( 'uv' ) ) {

		throw new Error( 'THREE.BufferGeometryUtils: Tangents require "position", "normal", and "uv" attributes.' );

	}

	function getAttributeArray( attribute ) {

		if ( attribute.normalized || attribute.isInterleavedBufferAttribute ) {

			const dstArray = new Float32Array( attribute.count * attribute.itemSize );

			for ( let i = 0, j = 0; i < attribute.count; i ++ ) {

				dstArray[ j ++ ] = attribute.getX( i );
				dstArray[ j ++ ] = attribute.getY( i );

				if ( attribute.itemSize > 2 ) {

					dstArray[ j ++ ] = attribute.getZ( i );

				}

			}

			return dstArray;

		}

		if ( attribute.array instanceof Float32Array ) {

			return attribute.array;

		}

		return new Float32Array( attribute.array );

	}

	// MikkTSpace algorithm requires non-indexed input.

	const _geometry = geometry.index ? geometry.toNonIndexed() : geometry;

	// Compute vertex tangents.

	const tangents = MikkTSpace.generateTangents(

		getAttributeArray( _geometry.attributes.position ),
		getAttributeArray( _geometry.attributes.normal ),
		getAttributeArray( _geometry.attributes.uv )

	);

	// Texture coordinate convention of glTF differs from the apparent
	// default of the MikkTSpace library; .w component must be flipped.

	if ( negateSign ) {

		for ( let i = 3; i < tangents.length; i += 4 ) {

			tangents[ i ] *= - 1;

		}

	}

	//

	// Quantized normals get tangents of the same precision.

	const normal = _geometry.attributes.normal;
	const normalArray = normal.array;

	if ( normal.normalized && ( normalArray instanceof Int8Array || normalArray instanceof Int16Array ) ) {

		const quantized = new normalArray.constructor( tangents.length );

		for ( let i = 0; i < tangents.length; i ++ ) {

			quantized[ i ] = MathUtils.normalize( tangents[ i ], quantized );

		}

		_geometry.setAttribute( 'tangent', new BufferAttribute( quantized, 4, true ) );

	} else {

		_geometry.setAttribute( 'tangent', new BufferAttribute( tangents, 4 ) );

	}

	if ( geometry !== _geometry ) {

		geometry.copy( _geometry );

	}

	return geometry;

}

/**
 * Merges a set of geometries into a single instance. All geometries must have compatible attributes.
 *
 * @param {Array<BufferGeometry>} geometries - The geometries to merge.
 * @param {boolean} [useGroups=false] - Whether to use groups or not.
 * @return {?BufferGeometry} The merged geometry. Returns `null` if the merge does not succeed.
 */
function mergeGeometries( geometries, useGroups = false ) {

	const isIndexed = geometries[ 0 ].index !== null;

	const attributesUsed = new Set( Object.keys( geometries[ 0 ].attributes ) );
	const morphAttributesUsed = new Set( Object.keys( geometries[ 0 ].morphAttributes ) );

	const attributes = {};
	const morphAttributes = {};

	const morphTargetsRelative = geometries[ 0 ].morphTargetsRelative;

	const mergedGeometry = new BufferGeometry();
	mergedGeometry.morphTargetsRelative = morphTargetsRelative;

	let offset = 0;

	for ( let i = 0; i < geometries.length; ++ i ) {

		const geometry = geometries[ i ];
		let attributesCount = 0;
		let morphAttributesCount = 0;

		// ensure that all geometries are indexed, or none

		if ( isIndexed !== ( geometry.index !== null ) ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '. All geometries must have compatible attributes; make sure index attribute exists among all geometries, or in none of them.' );
			return null;

		}

		// gather attributes, exit early if they're different

		for ( const name in geometry.attributes ) {

			if ( ! attributesUsed.has( name ) ) {

				console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '. All geometries must have compatible attributes; make sure "' + name + '" attribute exists among all geometries, or in none of them.' );
				return null;

			}

			if ( attributes[ name ] === undefined ) attributes[ name ] = [];

			attributes[ name ].push( geometry.attributes[ name ] );

			attributesCount ++;

		}

		// ensure geometries have the same number of attributes

		if ( attributesCount !== attributesUsed.size ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '. Make sure all geometries have the same number of attributes.' );
			return null;

		}

		// gather morph attributes, exit early if they're different

		if ( morphTargetsRelative !== geometry.morphTargetsRelative ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '. .morphTargetsRelative must be consistent throughout all geometries.' );
			return null;

		}

		for ( const name in geometry.morphAttributes ) {

			if ( ! morphAttributesUsed.has( name ) ) {

				console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '.  .morphAttributes must be consistent throughout all geometries.' );
				return null;

			}

			if ( geometry.morphAttributes[ name ].length !== geometries[ 0 ].morphAttributes[ name ].length ) {

				console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '. All geometries must have the same number of morph targets for the "' + name + '" morph attribute.' );
				return null;

			}

			if ( morphAttributes[ name ] === undefined ) morphAttributes[ name ] = [];

			morphAttributes[ name ].push( geometry.morphAttributes[ name ] );

			morphAttributesCount ++;

		}

		// ensure geometries have the same number of morph attributes

		if ( morphAttributesCount !== morphAttributesUsed.size ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '. Make sure all geometries have the same number of morph attributes.' );
			return null;

		}

		if ( useGroups ) {

			let count;

			if ( isIndexed ) {

				count = geometry.index.count;

			} else if ( geometry.attributes.position !== undefined ) {

				count = geometry.attributes.position.count;

			} else {

				console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed with geometry at index ' + i + '. The geometry must have either an index or a position attribute' );
				return null;

			}

			mergedGeometry.addGroup( offset, count, i );

			offset += count;

		}

	}

	// merge indices

	if ( isIndexed ) {

		let indexOffset = 0;
		const mergedIndex = [];

		for ( let i = 0; i < geometries.length; ++ i ) {

			const index = geometries[ i ].index;

			for ( let j = 0; j < index.count; ++ j ) {

				mergedIndex.push( index.getX( j ) + indexOffset );

			}

			indexOffset += geometries[ i ].attributes.position.count;

		}

		mergedGeometry.setIndex( mergedIndex );

	}

	// merge attributes

	for ( const name in attributes ) {

		const mergedAttribute = mergeAttributes( attributes[ name ] );

		if ( ! mergedAttribute ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed while trying to merge the ' + name + ' attribute.' );
			return null;

		}

		mergedGeometry.setAttribute( name, mergedAttribute );

	}

	// merge morph attributes

	for ( const name in morphAttributes ) {

		const numMorphTargets = morphAttributes[ name ][ 0 ].length;
		if ( numMorphTargets === 0 ) continue;

		mergedGeometry.morphAttributes = mergedGeometry.morphAttributes || {};
		mergedGeometry.morphAttributes[ name ] = [];

		for ( let i = 0; i < numMorphTargets; ++ i ) {

			const morphAttributesToMerge = [];

			for ( let j = 0; j < morphAttributes[ name ].length; ++ j ) {

				morphAttributesToMerge.push( morphAttributes[ name ][ j ][ i ] );

			}

			const mergedMorphAttribute = mergeAttributes( morphAttributesToMerge );

			if ( ! mergedMorphAttribute ) {

				console.error( 'THREE.BufferGeometryUtils: .mergeGeometries() failed while trying to merge the ' + name + ' morphAttribute.' );
				return null;

			}

			mergedGeometry.morphAttributes[ name ].push( mergedMorphAttribute );

		}

	}

	return mergedGeometry;

}

/**
 * Merges a set of attributes into a single instance. All attributes must have compatible properties and types.
 * Instances of {@link InterleavedBufferAttribute} are not supported.
 *
 * @param {Array<BufferAttribute>} attributes - The attributes to merge.
 * @return {?BufferAttribute} The merged attribute. Returns `null` if the merge does not succeed.
 */
function mergeAttributes( attributes ) {

	let TypedArray;
	let itemSize;
	let normalized;
	let gpuType = - 1;
	let arrayLength = 0;

	for ( let i = 0; i < attributes.length; ++ i ) {

		const attribute = attributes[ i ];

		if ( TypedArray === undefined ) TypedArray = attribute.array.constructor;
		if ( TypedArray !== attribute.array.constructor ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeAttributes() failed. BufferAttribute.array must be of consistent array types across matching attributes.' );
			return null;

		}

		if ( itemSize === undefined ) itemSize = attribute.itemSize;
		if ( itemSize !== attribute.itemSize ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeAttributes() failed. BufferAttribute.itemSize must be consistent across matching attributes.' );
			return null;

		}

		if ( normalized === undefined ) normalized = attribute.normalized;
		if ( normalized !== attribute.normalized ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeAttributes() failed. BufferAttribute.normalized must be consistent across matching attributes.' );
			return null;

		}

		if ( gpuType === - 1 ) gpuType = attribute.gpuType;
		if ( gpuType !== attribute.gpuType ) {

			console.error( 'THREE.BufferGeometryUtils: .mergeAttributes() failed. BufferAttribute.gpuType must be consistent across matching attributes.' );
			return null;

		}

		arrayLength += attribute.count * itemSize;

	}

	const array = new TypedArray( arrayLength );
	const result = new BufferAttribute( array, itemSize, normalized );
	let offset = 0;

	for ( let i = 0; i < attributes.length; ++ i ) {

		const attribute = attributes[ i ];
		if ( attribute.isInterleavedBufferAttribute ) {

			const tupleOffset = offset / itemSize;
			for ( let j = 0, l = attribute.count; j < l; j ++ ) {

				for ( let c = 0; c < itemSize; c ++ ) {

					const value = attribute.getComponent( j, c );
					result.setComponent( j + tupleOffset, c, value );

				}

			}

		} else {

			array.set( attribute.array, offset );

		}

		offset += attribute.count * itemSize;

	}

	if ( gpuType !== undefined ) {

		result.gpuType = gpuType;

	}

	return result;

}

/**
 * Performs a deep clone of the given buffer attribute.
 *
 * @param {BufferAttribute} attribute - The attribute to clone.
 * @return {BufferAttribute} The cloned attribute.
 */
function deepCloneAttribute( attribute ) {

	if ( attribute.isInstancedInterleavedBufferAttribute || attribute.isInterleavedBufferAttribute ) {

		return deinterleaveAttribute( attribute );

	}

	if ( attribute.isInstancedBufferAttribute ) {

		return new InstancedBufferAttribute().copy( attribute );

	}

	return new BufferAttribute().copy( attribute );

}

/**
 * Interleaves a set of attributes and returns a new array of corresponding attributes that share a
 * single {@link InterleavedBuffer} instance. All attributes must have compatible types.
 *
 * @param {Array<BufferAttribute>} attributes - The attributes to interleave.
 * @return {?Array<InterleavedBufferAttribute>} An array of interleaved attributes. If interleave does not succeed, the method returns `null`.
 */
function interleaveAttributes( attributes ) {

	// Interleaves the provided attributes into an InterleavedBuffer and returns
	// a set of InterleavedBufferAttributes for each attribute
	let TypedArray;
	let arrayLength = 0;
	let stride = 0;

	// calculate the length and type of the interleavedBuffer
	for ( let i = 0, l = attributes.length; i < l; ++ i ) {

		const attribute = attributes[ i ];

		if ( TypedArray === undefined ) TypedArray = attribute.array.constructor;
		if ( TypedArray !== attribute.array.constructor ) {

			console.error( 'AttributeBuffers of different types cannot be interleaved' );
			return null;

		}

		arrayLength += attribute.array.length;
		stride += attribute.itemSize;

	}

	// Create the set of buffer attributes
	const interleavedBuffer = new InterleavedBuffer( new TypedArray( arrayLength ), stride );
	let offset = 0;
	const res = [];
	const getters = [ 'getX', 'getY', 'getZ', 'getW' ];
	const setters = [ 'setX', 'setY', 'setZ', 'setW' ];

	for ( let j = 0, l = attributes.length; j < l; j ++ ) {

		const attribute = attributes[ j ];
		const itemSize = attribute.itemSize;
		const count = attribute.count;
		const iba = new InterleavedBufferAttribute( interleavedBuffer, itemSize, offset, attribute.normalized );
		res.push( iba );

		offset += itemSize;

		// Move the data for each attribute into the new interleavedBuffer
		// at the appropriate offset
		for ( let c = 0; c < count; c ++ ) {

			for ( let k = 0; k < itemSize; k ++ ) {

				iba[ setters[ k ] ]( c, attribute[ getters[ k ] ]( c ) );

			}

		}

	}

	return res;

}

/**
 * Returns a new, non-interleaved version of the given attribute.
 *
 * @param {InterleavedBufferAttribute} attribute - The interleaved attribute.
 * @return {BufferAttribute} The non-interleaved attribute.
 */
function deinterleaveAttribute( attribute ) {

	const cons = attribute.data.array.constructor;
	const count = attribute.count;
	const itemSize = attribute.itemSize;
	const normalized = attribute.normalized;

	const array = new cons( count * itemSize );
	let newAttribute;
	if ( attribute.isInstancedInterleavedBufferAttribute ) {

		newAttribute = new InstancedBufferAttribute( array, itemSize, normalized, attribute.meshPerAttribute );

	} else {

		newAttribute = new BufferAttribute( array, itemSize, normalized );

	}

	for ( let i = 0; i < count; i ++ ) {

		newAttribute.setX( i, attribute.getX( i ) );

		if ( itemSize >= 2 ) {

			newAttribute.setY( i, attribute.getY( i ) );

		}

		if ( itemSize >= 3 ) {

			newAttribute.setZ( i, attribute.getZ( i ) );

		}

		if ( itemSize >= 4 ) {

			newAttribute.setW( i, attribute.getW( i ) );

		}

	}

	return newAttribute;

}

/**
 * Deinterleaves all attributes on the given geometry.
 *
 * @param {BufferGeometry} geometry - The geometry to deinterleave.
 */
function deinterleaveGeometry( geometry ) {

	const attributes = geometry.attributes;
	const morphAttributes = geometry.morphAttributes;
	const attrMap = new Map();

	for ( const key in attributes ) {

		const attr = attributes[ key ];
		if ( attr.isInterleavedBufferAttribute ) {

			if ( ! attrMap.has( attr ) ) {

				attrMap.set( attr, deinterleaveAttribute( attr ) );

			}

			attributes[ key ] = attrMap.get( attr );

		}

	}

	for ( const key in morphAttributes ) {

		const morphAttribute = morphAttributes[ key ];

		for ( let i = 0, l = morphAttribute.length; i < l; i ++ ) {

			const attr = morphAttribute[ i ];
			if ( attr.isInterleavedBufferAttribute ) {

				if ( ! attrMap.has( attr ) ) {

					attrMap.set( attr, deinterleaveAttribute( attr ) );

				}

				morphAttribute[ i ] = attrMap.get( attr );

			}

		}

	}

}

/**
 * Returns the amount of bytes used by all attributes to represent the geometry.
 *
 * @param {BufferGeometry} geometry - The geometry.
 * @return {number} The estimate bytes used.
 */
function estimateBytesUsed( geometry ) {

	// Return the estimated memory used by this geometry in bytes
	// Calculate using itemSize, count, and BYTES_PER_ELEMENT to account
	// for InterleavedBufferAttributes.
	let mem = 0;
	for ( const name in geometry.attributes ) {

		const attr = geometry.getAttribute( name );
		mem += attr.count * attr.itemSize * attr.array.BYTES_PER_ELEMENT;

	}

	const indices = geometry.getIndex();
	mem += indices ? indices.count * indices.itemSize * indices.array.BYTES_PER_ELEMENT : 0;
	return mem;

}

/**
 * Returns a new geometry with vertices for which all similar vertex attributes (within tolerance) are merged.
 *
 * @param {BufferGeometry} geometry - The geometry to merge vertices for.
 * @param {number} [tolerance=1e-4] - The tolerance value.
 * @return {BufferGeometry} - The new geometry with merged vertices.
 */
function mergeVertices( geometry, tolerance = 1e-4 ) {

	tolerance = Math.max( tolerance, Number.EPSILON );

	// Generate an index buffer if the geometry doesn't have one, or optimize it
	// if it's already available.
	const hashToIndex = {};
	const indices = geometry.getIndex();
	const positions = geometry.getAttribute( 'position' );
	const vertexCount = indices ? indices.count : positions.count;

	// next value for triangle indices
	let nextIndex = 0;

	// attributes and new attribute arrays
	const attributeNames = Object.keys( geometry.attributes );
	const sourceIndices = [];
	const buffers = new Map();
	const newIndices = [];
	const getters = [ 'getX', 'getY', 'getZ', 'getW' ];

	// convert the error tolerance to an amount of decimal places to truncate to
	const halfTolerance = tolerance * 0.5;
	const exponent = Math.log10( 1 / tolerance );
	const hashMultiplier = Math.pow( 10, exponent );
	const hashAdditive = halfTolerance * hashMultiplier;
	for ( let i = 0; i < vertexCount; i ++ ) {

		const index = indices ? indices.getX( i ) : i;

		// Generate a hash for the vertex attributes at the current index 'i'
		let hash = '';
		for ( let j = 0, l = attributeNames.length; j < l; j ++ ) {

			const name = attributeNames[ j ];
			const attribute = geometry.getAttribute( name );
			const itemSize = attribute.itemSize;

			for ( let k = 0; k < itemSize; k ++ ) {

				// Math.trunc() preserves the full Number range, ~~ would wrap to int32
				hash += `${ Math.trunc( attribute[ getters[ k ] ]( index ) * hashMultiplier + hashAdditive ) },`;

			}

		}

		// Add another reference to the vertex if it's already
		// used by another index
		if ( hash in hashToIndex ) {

			newIndices.push( hashToIndex[ hash ] );

		} else {

			sourceIndices.push( index );

			hashToIndex[ hash ] = nextIndex;
			newIndices.push( nextIndex );
			nextIndex ++;

		}

	}

	// generate result BufferGeometry
	const result = geometry.clone();
	for ( const name in geometry.attributes ) {

		result.setAttribute( name, remapAttribute( geometry.attributes[ name ], sourceIndices, buffers ) );

	}

	for ( const name in geometry.morphAttributes ) {

		result.morphAttributes[ name ] = geometry.morphAttributes[ name ].map( attribute => remapAttribute( attribute, sourceIndices, buffers ) );

	}

	// indices

	result.setIndex( newIndices );

	return result;

}

/**
 * Converts the given geometry to the `TrianglesDrawMode` draw mode, which
 * corresponds to the `gl.TRIANGLES` primitive in WebGL. The conversion only
 * rewrites the index, so the geometry is modified in place and returned.
 *
 * @param {BufferGeometry} geometry - The geometry to convert.
 * @param {number} drawMode - The current draw mode.
 * @return {BufferGeometry} The converted geometry using `TrianglesDrawMode`.
 */
function toTrianglesDrawMode( geometry, drawMode ) {

	if ( drawMode === TrianglesDrawMode ) {

		console.warn( 'THREE.BufferGeometryUtils.toTrianglesDrawMode(): Geometry already defined as triangles.' );
		return geometry;

	}

	if ( drawMode === TriangleFanDrawMode || drawMode === TriangleStripDrawMode ) {

		let index = geometry.getIndex();

		// generate index if not present

		if ( index === null ) {

			const indices = [];

			const position = geometry.getAttribute( 'position' );

			if ( position !== undefined ) {

				for ( let i = 0; i < position.count; i ++ ) {

					indices.push( i );

				}

				geometry.setIndex( indices );
				index = geometry.getIndex();

			} else {

				console.error( 'THREE.BufferGeometryUtils.toTrianglesDrawMode(): Undefined position attribute. Processing not possible.' );
				return geometry;

			}

		}

		//

		const numberOfTriangles = index.count - 2;
		const newIndices = [];

		if ( drawMode === TriangleFanDrawMode ) {

			// gl.TRIANGLE_FAN

			for ( let i = 1; i <= numberOfTriangles; i ++ ) {

				newIndices.push( index.getX( 0 ) );
				newIndices.push( index.getX( i ) );
				newIndices.push( index.getX( i + 1 ) );

			}

		} else {

			// gl.TRIANGLE_STRIP

			for ( let i = 0; i < numberOfTriangles; i ++ ) {

				if ( i % 2 === 0 ) {

					newIndices.push( index.getX( i ) );
					newIndices.push( index.getX( i + 1 ) );
					newIndices.push( index.getX( i + 2 ) );

				} else {

					newIndices.push( index.getX( i + 2 ) );
					newIndices.push( index.getX( i + 1 ) );
					newIndices.push( index.getX( i ) );

				}

			}

		}

		if ( ( newIndices.length / 3 ) !== numberOfTriangles ) {

			console.error( 'THREE.BufferGeometryUtils.toTrianglesDrawMode(): Unable to generate correct amount of triangles.' );

		}

		// updated indices

		geometry.setIndex( newIndices );
		geometry.clearGroups();

		return geometry;

	} else {

		console.error( 'THREE.BufferGeometryUtils.toTrianglesDrawMode(): Unknown draw mode:', drawMode );
		return geometry;

	}

}

/**
 * Calculates the morphed attributes of a morphed/skinned BufferGeometry.
 *
 * Helpful for Raytracing or Decals (i.e. a `DecalGeometry` applied to a morphed Object with a `BufferGeometry`
 * will use the original `BufferGeometry`, not the morphed/skinned one, generating an incorrect result.
 * Using this function to create a shadow `Object3`D the `DecalGeometry` can be correctly generated).
 * If the geometry has no normal attribute, both returned normal attributes are `undefined`.
 *
 * @param {Mesh|Line|Points} object - The 3D object to compute morph attributes for.
 * @return {Object} An object with original position/normal attributes and morphed ones.
 */
function computeMorphedAttributes( object ) {

	const _vA = new Vector3();
	const _vB = new Vector3();
	const _vC = new Vector3();

	const _tempA = new Vector3();
	const _tempB = new Vector3();
	const _tempC = new Vector3();

	const _morphA = new Vector3();
	const _morphB = new Vector3();
	const _morphC = new Vector3();

	const _direction = new Vector4();

	function _calculateMorphedAttributeData(
		object,
		attribute,
		morphAttribute,
		morphTargetsRelative,
		a,
		b,
		c,
		modifiedAttributeArray
	) {

		_vA.fromBufferAttribute( attribute, a );
		_vB.fromBufferAttribute( attribute, b );
		_vC.fromBufferAttribute( attribute, c );

		const morphInfluences = object.morphTargetInfluences;

		if ( morphAttribute && morphInfluences ) {

			_morphA.set( 0, 0, 0 );
			_morphB.set( 0, 0, 0 );
			_morphC.set( 0, 0, 0 );

			for ( let i = 0, il = morphAttribute.length; i < il; i ++ ) {

				const influence = morphInfluences[ i ];
				const morph = morphAttribute[ i ];

				if ( influence === 0 ) continue;

				_tempA.fromBufferAttribute( morph, a );
				_tempB.fromBufferAttribute( morph, b );
				_tempC.fromBufferAttribute( morph, c );

				if ( morphTargetsRelative ) {

					_morphA.addScaledVector( _tempA, influence );
					_morphB.addScaledVector( _tempB, influence );
					_morphC.addScaledVector( _tempC, influence );

				} else {

					_morphA.addScaledVector( _tempA.sub( _vA ), influence );
					_morphB.addScaledVector( _tempB.sub( _vB ), influence );
					_morphC.addScaledVector( _tempC.sub( _vC ), influence );

				}

			}

			_vA.add( _morphA );
			_vB.add( _morphB );
			_vC.add( _morphC );

		}

		if ( object.isSkinnedMesh ) {

			if ( attribute === object.geometry.getAttribute( 'normal' ) ) {

				// normals are directions so they must be transformed with w = 0

				_vA.copy( object.applyBoneTransform( a, _direction.set( _vA.x, _vA.y, _vA.z, 0 ) ) );
				_vB.copy( object.applyBoneTransform( b, _direction.set( _vB.x, _vB.y, _vB.z, 0 ) ) );
				_vC.copy( object.applyBoneTransform( c, _direction.set( _vC.x, _vC.y, _vC.z, 0 ) ) );

			} else {

				object.applyBoneTransform( a, _vA );
				object.applyBoneTransform( b, _vB );
				object.applyBoneTransform( c, _vC );

			}

		}

		modifiedAttributeArray[ a * 3 + 0 ] = _vA.x;
		modifiedAttributeArray[ a * 3 + 1 ] = _vA.y;
		modifiedAttributeArray[ a * 3 + 2 ] = _vA.z;
		modifiedAttributeArray[ b * 3 + 0 ] = _vB.x;
		modifiedAttributeArray[ b * 3 + 1 ] = _vB.y;
		modifiedAttributeArray[ b * 3 + 2 ] = _vB.z;
		modifiedAttributeArray[ c * 3 + 0 ] = _vC.x;
		modifiedAttributeArray[ c * 3 + 1 ] = _vC.y;
		modifiedAttributeArray[ c * 3 + 2 ] = _vC.z;

	}

	const geometry = object.geometry;
	const material = object.material;

	let a, b, c;
	const index = geometry.index;
	const positionAttribute = geometry.attributes.position;
	const morphPosition = geometry.morphAttributes.position;
	const morphTargetsRelative = geometry.morphTargetsRelative;
	const normalAttribute = geometry.attributes.normal;
	const morphNormal = geometry.morphAttributes.normal;

	const groups = geometry.groups;
	const drawRange = geometry.drawRange;
	let i, j, il, jl;
	let group;
	let start, end;

	const modifiedPosition = new Float32Array( positionAttribute.count * positionAttribute.itemSize );
	const modifiedNormal = ( normalAttribute !== undefined ) ? new Float32Array( normalAttribute.count * normalAttribute.itemSize ) : undefined;

	if ( index !== null ) {

		// indexed buffer geometry

		if ( Array.isArray( material ) ) {

			for ( i = 0, il = groups.length; i < il; i ++ ) {

				group = groups[ i ];

				start = Math.max( group.start, drawRange.start );
				end = Math.min( ( group.start + group.count ), ( drawRange.start + drawRange.count ) );

				for ( j = start, jl = end; j < jl; j += 3 ) {

					a = index.getX( j );
					b = index.getX( j + 1 );
					c = index.getX( j + 2 );

					_calculateMorphedAttributeData(
						object,
						positionAttribute,
						morphPosition,
						morphTargetsRelative,
						a, b, c,
						modifiedPosition
					);

					if ( normalAttribute !== undefined ) {

						_calculateMorphedAttributeData(
							object,
							normalAttribute,
							morphNormal,
							morphTargetsRelative,
							a, b, c,
							modifiedNormal
						);

					}

				}

			}

		} else {

			start = Math.max( 0, drawRange.start );
			end = Math.min( index.count, ( drawRange.start + drawRange.count ) );

			for ( i = start, il = end; i < il; i += 3 ) {

				a = index.getX( i );
				b = index.getX( i + 1 );
				c = index.getX( i + 2 );

				_calculateMorphedAttributeData(
					object,
					positionAttribute,
					morphPosition,
					morphTargetsRelative,
					a, b, c,
					modifiedPosition
				);

				if ( normalAttribute !== undefined ) {

					_calculateMorphedAttributeData(
						object,
						normalAttribute,
						morphNormal,
						morphTargetsRelative,
						a, b, c,
						modifiedNormal
					);

				}

			}

		}

	} else {

		// non-indexed buffer geometry

		if ( Array.isArray( material ) ) {

			for ( i = 0, il = groups.length; i < il; i ++ ) {

				group = groups[ i ];

				start = Math.max( group.start, drawRange.start );
				end = Math.min( ( group.start + group.count ), ( drawRange.start + drawRange.count ) );

				for ( j = start, jl = end; j < jl; j += 3 ) {

					a = j;
					b = j + 1;
					c = j + 2;

					_calculateMorphedAttributeData(
						object,
						positionAttribute,
						morphPosition,
						morphTargetsRelative,
						a, b, c,
						modifiedPosition
					);

					if ( normalAttribute !== undefined ) {

						_calculateMorphedAttributeData(
							object,
							normalAttribute,
							morphNormal,
							morphTargetsRelative,
							a, b, c,
							modifiedNormal
						);

					}

				}

			}

		} else {

			start = Math.max( 0, drawRange.start );
			end = Math.min( positionAttribute.count, ( drawRange.start + drawRange.count ) );

			for ( i = start, il = end; i < il; i += 3 ) {

				a = i;
				b = i + 1;
				c = i + 2;

				_calculateMorphedAttributeData(
					object,
					positionAttribute,
					morphPosition,
					morphTargetsRelative,
					a, b, c,
					modifiedPosition
				);

				if ( normalAttribute !== undefined ) {

					_calculateMorphedAttributeData(
						object,
						normalAttribute,
						morphNormal,
						morphTargetsRelative,
						a, b, c,
						modifiedNormal
					);

				}

			}

		}

	}

	const morphedPositionAttribute = new Float32BufferAttribute( modifiedPosition, 3 );
	const morphedNormalAttribute = ( normalAttribute !== undefined ) ? new Float32BufferAttribute( modifiedNormal, 3 ) : undefined;

	return {

		positionAttribute: positionAttribute,
		normalAttribute: normalAttribute,
		morphedPositionAttribute: morphedPositionAttribute,
		morphedNormalAttribute: morphedNormalAttribute

	};

}

/**
 * Merges the {@link BufferGeometry#groups} for the given geometry.
 *
 * @param {BufferGeometry} geometry - The geometry to modify.
 * @return {BufferGeometry} - The updated geometry
 */
function mergeGroups( geometry ) {

	if ( geometry.groups.length === 0 ) {

		console.warn( 'THREE.BufferGeometryUtils.mergeGroups(): No groups are defined. Nothing to merge.' );
		return geometry;

	}

	let groups = geometry.groups;

	// sort groups by material index

	groups = groups.sort( ( a, b ) => {

		if ( a.materialIndex !== b.materialIndex ) return a.materialIndex - b.materialIndex;

		return a.start - b.start;

	} );

	// create index for non-indexed geometries

	if ( geometry.getIndex() === null ) {

		const positionAttribute = geometry.getAttribute( 'position' );
		const indices = [];

		for ( let i = 0; i < positionAttribute.count; i += 3 ) {

			indices.push( i, i + 1, i + 2 );

		}

		geometry.setIndex( indices );

	}

	// sort index

	const index = geometry.getIndex();

	const newIndices = [];

	for ( let i = 0; i < groups.length; i ++ ) {

		const group = groups[ i ];

		if ( group.count === Infinity ) group.count = index.count - group.start;

		const groupStart = group.start;
		const groupLength = groupStart + group.count;

		for ( let j = groupStart; j < groupLength; j ++ ) {

			newIndices.push( index.getX( j ) );

		}

	}

	geometry.dispose(); // Required to force buffer recreation
	geometry.setIndex( newIndices );

	// update groups indices

	let start = 0;

	for ( let i = 0; i < groups.length; i ++ ) {

		const group = groups[ i ];

		group.start = start;
		start += group.count;

	}

	// merge groups

	let currentGroup = groups[ 0 ];

	geometry.groups = [ currentGroup ];

	for ( let i = 1; i < groups.length; i ++ ) {

		const group = groups[ i ];

		if ( currentGroup.materialIndex === group.materialIndex ) {

			currentGroup.count += group.count;

		} else {

			currentGroup = group;
			geometry.groups.push( currentGroup );

		}

	}

	return geometry;

}

/**
 * Modifies the supplied geometry if it is non-indexed, otherwise creates a new,
 * non-indexed geometry. Returns the geometry with smooth normals everywhere except
 * faces that meet at an angle greater than the crease angle.
 *
 * @param {BufferGeometry} geometry - The geometry to modify.
 * @param {number} [creaseAngle=Math.PI/3] - The crease angle in radians.
 * @return {BufferGeometry} - The updated geometry
 */
function toCreasedNormals( geometry, creaseAngle = Math.PI / 3 /* 60 degrees */ ) {

	// BufferGeometry.toNonIndexed() warns if the geometry is non-indexed
	// and returns the original geometry
	const resultGeometry = geometry.index ? geometry.toNonIndexed() : geometry;
	const posAttr = resultGeometry.attributes.position;
	const vertexCount = posAttr.count;

	let positions;

	if ( posAttr.isBufferAttribute === true && posAttr.itemSize === 3 && posAttr.normalized === false ) {

		positions = posAttr.array;

	} else {

		// flatten the position buffer so the math below operates on plain numbers
		positions = new Float64Array( vertexCount * 3 );

		for ( let i = 0; i < vertexCount; i ++ ) {

			positions[ 3 * i + 0 ] = posAttr.getX( i );
			positions[ 3 * i + 1 ] = posAttr.getY( i );
			positions[ 3 * i + 2 ] = posAttr.getZ( i );

		}

	}

	const creaseDot = Math.cos( creaseAngle );
	const hashMultiplier = ( 1 + 1e-10 ) * 1e2;
	const faceCount = vertexCount / 3;

	// compute the normal of each face
	const faceNormals = new Float64Array( faceCount * 3 );
	for ( let f = 0; f < faceCount; f ++ ) {

		const f9 = 9 * f;
		const ax = positions[ f9 + 0 ], ay = positions[ f9 + 1 ], az = positions[ f9 + 2 ];
		const bx = positions[ f9 + 3 ], by = positions[ f9 + 4 ], bz = positions[ f9 + 5 ];
		const cx = positions[ f9 + 6 ], cy = positions[ f9 + 7 ], cz = positions[ f9 + 8 ];

		const v1x = cx - bx, v1y = cy - by, v1z = cz - bz;
		const v2x = ax - bx, v2y = ay - by, v2z = az - bz;

		const nx = v1y * v2z - v1z * v2y;
		const ny = v1z * v2x - v1x * v2z;
		const nz = v1x * v2y - v1y * v2x;

		const invLength = 1 / ( Math.sqrt( nx * nx + ny * ny + nz * nz ) || 1 );
		faceNormals[ 3 * f + 0 ] = nx * invLength;
		faceNormals[ 3 * f + 1 ] = ny * invLength;
		faceNormals[ 3 * f + 2 ] = nz * invLength;

	}

	// assign an id to each vertex, sharing the id between vertices with the same
	// quantized position via an open-addressed hash table (slots hold id + 1, 0 means empty)
	const vertexIds = new Int32Array( vertexCount );
	const quantized = new Float64Array( vertexCount * 3 );

	let tableSize = 1;
	while ( tableSize < vertexCount * 2 ) tableSize <<= 1;
	const tableMask = tableSize - 1;
	const table = new Int32Array( tableSize );

	let uniqueCount = 0;
	for ( let i = 0; i < vertexCount; i ++ ) {

		const i3 = 3 * i;
		const qx = Math.trunc( positions[ i3 + 0 ] * hashMultiplier );
		const qy = Math.trunc( positions[ i3 + 1 ] * hashMultiplier );
		const qz = Math.trunc( positions[ i3 + 2 ] * hashMultiplier );

		let slot = ( Math.imul( qx, 73856093 ) ^ Math.imul( qy, 19349663 ) ^ Math.imul( qz, 83492791 ) ) & tableMask;

		while ( true ) {

			const id = table[ slot ];

			if ( id === 0 ) {

				const q3 = 3 * uniqueCount;
				quantized[ q3 + 0 ] = qx;
				quantized[ q3 + 1 ] = qy;
				quantized[ q3 + 2 ] = qz;

				table[ slot ] = uniqueCount + 1;
				vertexIds[ i ] = uniqueCount ++;
				break;

			}

			const q3 = 3 * ( id - 1 );

			if ( quantized[ q3 + 0 ] === qx && quantized[ q3 + 1 ] === qy && quantized[ q3 + 2 ] === qz ) {

				vertexIds[ i ] = id - 1;
				break;

			}

			slot = ( slot + 1 ) & tableMask;

		}

	}

	// bucket the faces surrounding each unique vertex position
	const bucketOffsets = new Int32Array( uniqueCount + 1 );
	for ( let i = 0; i < vertexCount; i ++ ) bucketOffsets[ vertexIds[ i ] + 1 ] ++;
	for ( let i = 0; i < uniqueCount; i ++ ) bucketOffsets[ i + 1 ] += bucketOffsets[ i ];

	const bucketFaces = new Int32Array( vertexCount );
	const bucketCursors = bucketOffsets.slice( 0, uniqueCount );
	for ( let f = 0; f < faceCount; f ++ ) {

		const f3 = 3 * f;
		bucketFaces[ bucketCursors[ vertexIds[ f3 + 0 ] ] ++ ] = f;
		bucketFaces[ bucketCursors[ vertexIds[ f3 + 1 ] ] ++ ] = f;
		bucketFaces[ bucketCursors[ vertexIds[ f3 + 2 ] ] ++ ] = f;

	}

	// average the normals of the faces surrounding each vertex if they are within the
	// provided crease threshold
	const normalArray = new Float32Array( vertexCount * 3 );
	for ( let f = 0; f < faceCount; f ++ ) {

		const f3 = 3 * f;
		const nx = faceNormals[ f3 + 0 ];
		const ny = faceNormals[ f3 + 1 ];
		const nz = faceNormals[ f3 + 2 ];

		for ( let n = 0; n < 3; n ++ ) {

			const i = f3 + n;
			const id = vertexIds[ i ];

			let sumX = 0, sumY = 0, sumZ = 0;

			for ( let k = bucketOffsets[ id ], end = bucketOffsets[ id + 1 ]; k < end; k ++ ) {

				const o3 = 3 * bucketFaces[ k ];
				const ox = faceNormals[ o3 + 0 ];
				const oy = faceNormals[ o3 + 1 ];
				const oz = faceNormals[ o3 + 2 ];

				if ( nx * ox + ny * oy + nz * oz > creaseDot ) {

					sumX += ox;
					sumY += oy;
					sumZ += oz;

				}

			}

			const invLength = 1 / ( Math.sqrt( sumX * sumX + sumY * sumY + sumZ * sumZ ) || 1 );
			normalArray[ 3 * i + 0 ] = sumX * invLength;
			normalArray[ 3 * i + 1 ] = sumY * invLength;
			normalArray[ 3 * i + 2 ] = sumZ * invLength;

		}

	}

	resultGeometry.setAttribute( 'normal', new BufferAttribute( normalArray, 3, false ) );
	return resultGeometry;

}

/**
 * Copies attribute entries in the order specified by the source vertex indices.
 * Preserves raw values, attribute type and metadata. Reuse the same buffer map
 * across calls to preserve shared interleaved buffers for the same mapping.
 *
 * @param {BufferAttribute|InterleavedBufferAttribute} attribute - The source attribute.
 * @param {Array<number>|TypedArray} sourceIndices - The source index for each output vertex.
 * @param {Map<InterleavedBuffer, InterleavedBuffer>} [buffers] - Shared interleaved buffer copies.
 * @return {BufferAttribute|InterleavedBufferAttribute} The remapped attribute.
 */
function remapAttribute( attribute, sourceIndices, buffers = new Map() ) {

	const interleaved = attribute.isInterleavedBufferAttribute;
	const source = interleaved ? attribute.data : attribute;
	const stride = interleaved ? source.stride : attribute.itemSize;
	let data = interleaved ? buffers.get( source ) : undefined;

	if ( data === undefined ) {

		const array = new source.array.constructor( sourceIndices.length * stride );
		for ( let i = 0; i < sourceIndices.length; i ++ ) {

			for ( let j = 0; j < stride; j ++ ) array[ i * stride + j ] = source.array[ sourceIndices[ i ] * stride + j ];

		}

		data = interleaved
			? new source.constructor( array, stride, source.meshPerAttribute )
			: new attribute.constructor( array, attribute.itemSize, attribute.normalized, attribute.meshPerAttribute );
		data.setUsage( source.usage );
		if ( interleaved ) buffers.set( source, data );

	}

	const result = interleaved ? new attribute.constructor( data, attribute.itemSize, attribute.offset, attribute.normalized ) : data;
	result.name = attribute.name;
	if ( ! interleaved ) result.gpuType = attribute.gpuType;
	return result;

}

/**
 * Bakes an affine transformation into a new {@link BufferGeometry}, including its morph
 * targets, and returns the new geometry. The source geometry and the matrix are never
 * modified.
 *
 * Only triangular meshes are supported, indexed or non-indexed. The geometry may carry
 * `position` and `normal` morph attributes, but no skinning or instanced attributes.
 * Plain {@link BufferAttribute} and {@link InterleavedBufferAttribute} inputs are both
 * accepted; interleaved storage may be de-interleaved in the result. Relative morph
 * targets (`morphTargetsRelative = true`) are treated as displacement vectors, so the
 * translation part of the matrix is never applied to them, while absolute targets are
 * transformed as endpoints.
 *
 * Positions and normals (base and morph) are written to `Float32` arrays. Base normals
 * are unit length; morph normals are transformed with the inverse transpose but are not
 * individually normalized, which keeps linear morph blending correct under non-uniform
 * scale and shear. Other attributes keep their typed array type, item size and
 * `normalized` flag. When the determinant of the matrix is negative, every triangle flips
 * its second and third corner so the original front faces stay front faces: the index is
 * rewritten for indexed geometry, and every per-vertex attribute (including morph
 * records) is permuted for non-indexed geometry. Groups, draw ranges and material groups
 * are preserved. Existing tangents are re-orthogonalized against the transformed tangent
 * plane and their `.w` reflects the new handedness; no tangent is generated when the
 * source has none.
 *
 * Throws when the geometry lacks a `position` attribute, has inconsistent attribute
 * lengths, illegal triangle indices, skinning/instanced data, unsupported attribute
 * types, or when the matrix is non-affine, singular or contains non-finite values.
 *
 * @param {BufferGeometry} geometry - The geometry to bake. It is not modified.
 * @param {Matrix4} matrix - The affine matrix to bake. It is not modified.
 * @return {BufferGeometry} A new independent geometry with the transformation baked in.
 */
function bakeAffineGeometry( geometry, matrix ) {

	const ERROR_BASE = 'THREE.BufferGeometryUtils.bakeAffineGeometry():';

	function fail( message ) {

		throw new Error( `${ ERROR_BASE } ${ message }` );

	}

	if ( ! geometry || geometry.isBufferGeometry !== true ) {

		fail( 'first argument must be a BufferGeometry.' );

	}

	if ( ! matrix || matrix.isMatrix4 !== true ) {

		fail( 'second argument must be a Matrix4.' );

	}

	if ( geometry.isInstancedBufferGeometry === true ) {

		fail( 'instanced geometries are not supported.' );

	}

	if ( geometry.index !== null && geometry.index.isBufferAttribute !== true ) {

		fail( 'the index must be a plain BufferAttribute.' );

	}

	if ( geometry.indirect !== null ) {

		fail( 'geometries with indirect draw attributes are not supported.' );

	}

	const me = matrix.elements;

	for ( let i = 0; i < 16; i ++ ) {

		if ( typeof me[ i ] !== 'number' || ! Number.isFinite( me[ i ] ) ) {

			fail( 'matrix contains non-finite values.' );

		}

	}

	// An affine matrix has ( 0, 0, 0, 1 ) in its last row.

	if ( me[ 3 ] !== 0 || me[ 7 ] !== 0 || me[ 11 ] !== 0 || me[ 15 ] !== 1 ) {

		fail( 'matrix must be affine.' );

	}

	const determinant = matrix.determinant();

	if ( ! Number.isFinite( determinant ) || determinant === 0 ) {

		fail( 'matrix must be invertible (non-zero determinant).' );

	}

	const reflected = determinant < 0;

	const normalMatrix = new Matrix3().getNormalMatrix( matrix );

	const position = geometry.getAttribute( 'position' );

	if ( position === undefined ) {

		fail( 'geometry requires a "position" attribute.' );

	}

	if ( position.isGLBufferAttribute === true ) {

		fail( 'GLBufferAttribute is not supported.' );

	}

	if ( position.itemSize !== 3 ) {

		fail( 'the "position" attribute must use itemSize 3.' );

	}

	const vertexCount = position.count;

	if ( vertexCount === 0 ) {

		fail( 'the "position" attribute is empty.' );

	}

	// Validate every attribute before allocating output storage.

	const attributeNames = Object.keys( geometry.attributes );

	for ( const name of attributeNames ) {

		const attribute = geometry.attributes[ name ];

		if ( attribute.isBufferAttribute !== true && attribute.isInterleavedBufferAttribute !== true ) {

			fail( `attribute "${ name }" has an unsupported type.` );

		}

		if ( attribute.isInstancedBufferAttribute === true ||
			( attribute.isInterleavedBufferAttribute === true && attribute.data.isInstancedInterleavedBuffer === true ) ) {

			fail( 'instanced attributes are not supported.' );

		}

		if ( attribute.count !== vertexCount ) {

			fail( `attribute "${ name }" has ${ attribute.count } vertices, expected ${ vertexCount }.` );

		}

	}

	const morphNames = Object.keys( geometry.morphAttributes );

	for ( const name of morphNames ) {

		if ( name !== 'position' && name !== 'normal' ) {

			fail( `morph attribute "${ name }" is not supported; only "position" and "normal" morphs are.` );

		}

		for ( const attribute of geometry.morphAttributes[ name ] ) {

			if ( attribute.isBufferAttribute !== true && attribute.isInterleavedBufferAttribute !== true ) {

				fail( `morph "${ name }" has an unsupported attribute type.` );

			}

			if ( attribute.isInstancedBufferAttribute === true ||
				( attribute.isInterleavedBufferAttribute === true && attribute.data.isInstancedInterleavedBuffer === true ) ) {

				fail( 'instanced morph attributes are not supported.' );

			}

			if ( attribute.itemSize !== 3 || attribute.count !== vertexCount ) {

				fail( `morph "${ name }" must use itemSize 3 and match the position vertex count.` );

			}

		}

	}

	const morphPosition = geometry.morphAttributes.position || [];
	const morphNormal = geometry.morphAttributes.normal || [];
	const morphTargetsRelative = geometry.morphTargetsRelative;

	const normal = geometry.getAttribute( 'normal' );

	if ( normal !== undefined && normal.itemSize !== 3 ) {

		fail( 'the "normal" attribute must use itemSize 3.' );

	}

	const tangent = geometry.getAttribute( 'tangent' );

	if ( tangent !== undefined && tangent.itemSize !== 4 ) {

		fail( 'the "tangent" attribute must use itemSize 4.' );

	}

	const index = geometry.getIndex();
	let indexCount = 0;

	if ( index !== null ) {

		indexCount = index.count;

		if ( indexCount % 3 !== 0 ) {

			fail( 'index count must be a multiple of 3 for triangle geometry.' );

		}

		for ( let i = 0; i < indexCount; i ++ ) {

			const value = index.getX( i );

			if ( ! Number.isInteger( value ) || value < 0 || value >= vertexCount ) {

				fail( `illegal triangle index ${ value } at position ${ i }.` );

			}

		}

	} else if ( vertexCount % 3 !== 0 ) {

		fail( 'non-indexed geometry must have a vertex count divisible by 3.' );

	}

	// Reads every component of one vertex, honoring stride, offset and normalized.

	function readComponents( attribute, vertex ) {

		const itemSize = attribute.itemSize;
		const components = new Array( itemSize );

		for ( let c = 0; c < itemSize; c ++ ) {

			components[ c ] = attribute.getComponent( vertex, c );

			if ( ! Number.isFinite( components[ c ] ) ) {

				fail( 'source attribute contains non-finite values.' );

			}

		}

		return components;

	}

	// Allocates a plain output attribute that keeps the source typed array,
	// item size and normalized flag. Geometric attributes are handled separately
	// and may be promoted to Float32.

	function allocateAttribute( source, count ) {

		const itemSize = source.itemSize;
		const sourceArray = source.isInterleavedBufferAttribute === true ? source.data.array : source.array;

		return new BufferAttribute( new sourceArray.constructor( count * itemSize ), itemSize, source.normalized );

	}

	function transformPoint( x, y, z, target, offset ) {

		target[ offset ] = me[ 0 ] * x + me[ 4 ] * y + me[ 8 ] * z + me[ 12 ];
		target[ offset + 1 ] = me[ 1 ] * x + me[ 5 ] * y + me[ 9 ] * z + me[ 13 ];
		target[ offset + 2 ] = me[ 2 ] * x + me[ 6 ] * y + me[ 10 ] * z + me[ 14 ];

	}

	function transformVector( x, y, z, target, offset ) {

		target[ offset ] = me[ 0 ] * x + me[ 4 ] * y + me[ 8 ] * z;
		target[ offset + 1 ] = me[ 1 ] * x + me[ 5 ] * y + me[ 9 ] * z;
		target[ offset + 2 ] = me[ 2 ] * x + me[ 6 ] * y + me[ 10 ] * z;

	}

	function transformNormal( x, y, z, target, offset ) {

		const e = normalMatrix.elements;

		target[ offset ] = e[ 0 ] * x + e[ 3 ] * y + e[ 6 ] * z;
		target[ offset + 1 ] = e[ 1 ] * x + e[ 4 ] * y + e[ 7 ] * z;
		target[ offset + 2 ] = e[ 2 ] * x + e[ 5 ] * y + e[ 8 ] * z;

	}

	function assertFinite3( array, offset, message ) {

		if ( ! Number.isFinite( array[ offset ] ) ||
			! Number.isFinite( array[ offset + 1 ] ) ||
			! Number.isFinite( array[ offset + 2 ] ) ) {

			fail( message );

		}

	}

	const result = new BufferGeometry();
	result.morphTargetsRelative = morphTargetsRelative;

	// ---- position (base and morphs) -----------------------------------------

	const bakedPosition = new Float32Array( vertexCount * 3 );

	for ( let i = 0; i < vertexCount; i ++ ) {

		transformPoint( position.getX( i ), position.getY( i ), position.getZ( i ), bakedPosition, i * 3 );
		assertFinite3( bakedPosition, i * 3, 'baked position contains non-finite values.' );

	}

	result.setAttribute( 'position', new BufferAttribute( bakedPosition, 3 ) );

	const bakedMorphPosition = [];

	for ( const target of morphPosition ) {

		const array = new Float32Array( vertexCount * 3 );

		for ( let i = 0; i < vertexCount; i ++ ) {

			const x = target.getX( i ), y = target.getY( i ), z = target.getZ( i );

			if ( morphTargetsRelative ) {

				// Relative targets are displacements: the linear part only, never
				// the matrix translation.

				transformVector( x, y, z, array, i * 3 );

			} else {

				// Absolute targets are endpoints in the source frame and transform
				// exactly like base positions.

				transformPoint( x, y, z, array, i * 3 );

			}

			assertFinite3( array, i * 3, 'baked morph position contains non-finite values.' );

		}

		bakedMorphPosition.push( new BufferAttribute( array, 3 ) );

	}

	if ( bakedMorphPosition.length > 0 ) result.morphAttributes.position = bakedMorphPosition;

	// ---- normal (base and morphs) -------------------------------------------

	if ( normal !== undefined ) {

		const bakedNormal = new Float32Array( vertexCount * 3 );

		// Per-vertex inverse length of the inverse-transpose-transformed base normal.
		// Morph normal records at vertex i are scaled by the same factor as the base
		// normal, so every linear morph blend stays a scalar multiple of the
		// inverse-transpose-transformed source blend and normalizes to the same
		// direction, even under non-uniform scale and shear.

		const baseNormalInvLength = new Float32Array( vertexCount );

		for ( let i = 0; i < vertexCount; i ++ ) {

			const offset = i * 3;

			transformNormal( normal.getX( i ), normal.getY( i ), normal.getZ( i ), bakedNormal, offset );

			// The base normal stays unit length; artist-authored zero normals are
			// left untouched instead of producing NaN.

			const lengthSq = bakedNormal[ offset ] * bakedNormal[ offset ] +
				bakedNormal[ offset + 1 ] * bakedNormal[ offset + 1 ] +
				bakedNormal[ offset + 2 ] * bakedNormal[ offset + 2 ];

			if ( lengthSq > 0 ) {

				const invLength = 1 / Math.sqrt( lengthSq );
				baseNormalInvLength[ i ] = invLength;
				bakedNormal[ offset ] *= invLength;
				bakedNormal[ offset + 1 ] *= invLength;
				bakedNormal[ offset + 2 ] *= invLength;

			}

			assertFinite3( bakedNormal, offset, 'baked normal contains non-finite values.' );

		}

		result.setAttribute( 'normal', new BufferAttribute( bakedNormal, 3 ) );

		const bakedMorphNormal = [];

		for ( const target of morphNormal ) {

			const array = new Float32Array( vertexCount * 3 );

			for ( let i = 0; i < vertexCount; i ++ ) {

				const offset = i * 3;

				// Relative records are directions and absolute records are endpoint
				// normals; both transform with the inverse transpose. They are not
				// normalized individually: applying the base normal's per-vertex
				// scaling keeps the source linear-blend rule exact after baking.

				transformNormal( target.getX( i ), target.getY( i ), target.getZ( i ), array, offset );

				const invLength = baseNormalInvLength[ i ];

				if ( invLength !== 0 ) {

					array[ offset ] *= invLength;
					array[ offset + 1 ] *= invLength;
					array[ offset + 2 ] *= invLength;

				}

				assertFinite3( array, offset, 'baked morph normal contains non-finite values.' );

			}

			bakedMorphNormal.push( new BufferAttribute( array, 3 ) );

		}

		if ( bakedMorphNormal.length > 0 ) result.morphAttributes.normal = bakedMorphNormal;

	} else if ( morphNormal.length > 0 ) {

		fail( 'normal morph attributes require a base "normal" attribute.' );

	}

	// ---- tangent -------------------------------------------------------------

	if ( tangent !== undefined ) {

		const bakedTangent = new Float32Array( vertexCount * 4 );
		const tangentDirection = new Vector3();
		const scratch = [ 0, 0, 0 ];
		const baseNormal = new Vector3();

		for ( let i = 0; i < vertexCount; i ++ ) {

			const offset = i * 4;

			// Rotation/scale/shear part only: tangents are directions.

			transformVector( tangent.getX( i ), tangent.getY( i ), tangent.getZ( i ), scratch, 0 );
			tangentDirection.set( scratch[ 0 ], scratch[ 1 ], scratch[ 2 ] );

			if ( normal !== undefined ) {

				// Re-orthogonalize against the transformed base tangent plane so the
				// output tangent stays unit and orthogonal to the baked normal.

				baseNormal.fromBufferAttribute( result.attributes.normal, i );
				tangentDirection.addScaledVector( baseNormal, - baseNormal.dot( tangentDirection ) );

			}

			const lengthSq = tangentDirection.lengthSq();

			if ( lengthSq > 0 ) tangentDirection.multiplyScalar( 1 / Math.sqrt( lengthSq ) );

			bakedTangent[ offset ] = tangentDirection.x;
			bakedTangent[ offset + 1 ] = tangentDirection.y;
			bakedTangent[ offset + 2 ] = tangentDirection.z;
			bakedTangent[ offset + 3 ] = reflected ? - tangent.getW( i ) : tangent.getW( i );

			for ( let c = 0; c < 4; c ++ ) {

				if ( ! Number.isFinite( bakedTangent[ offset + c ] ) ) {

					fail( 'baked tangent contains non-finite values.' );

				}

			}

		}

		result.setAttribute( 'tangent', new BufferAttribute( bakedTangent, 4 ) );

	}

	// ---- remaining attributes ------------------------------------------------
	// UVs, colors and custom attributes do not participate in the geometric
	// transform. They are copied through the component API so interleaved
	// layout, stride, offset and normalized decoding are all honored.

	for ( const name of attributeNames ) {

		if ( name === 'position' || name === 'normal' || name === 'tangent' ) continue;

		const source = geometry.attributes[ name ];
		const output = allocateAttribute( source, vertexCount );

		for ( let i = 0; i < vertexCount; i ++ ) {

			const components = readComponents( source, i );

			for ( let c = 0; c < source.itemSize; c ++ ) {

				output.setComponent( i, c, components[ c ] );

			}

		}

		output.name = source.name;
		result.setAttribute( name, output );

	}

	// ---- index / reflection --------------------------------------------------

	if ( index !== null ) {

		const IndexedArray = index.array.constructor;
		const bakedIndex = new IndexedArray( indexCount );

		for ( let i = 0; i < indexCount; i ++ ) bakedIndex[ i ] = index.array[ i ];

		if ( reflected ) {

			// Only the second and third corner of each triangle are exchanged.

			for ( let i = 0; i < indexCount; i += 3 ) {

				const tmp = bakedIndex[ i + 1 ];
				bakedIndex[ i + 1 ] = bakedIndex[ i + 2 ];
				bakedIndex[ i + 2 ] = tmp;

			}

		}

		result.setIndex( new BufferAttribute( bakedIndex, 1, index.normalized ) );

	} else if ( reflected ) {

		// Non-indexed reflection: exchange corner two and three of every triangle
		// in every per-vertex attribute of the result, morph records included.

		const swapAttribute = ( attribute ) => {

			const itemSize = attribute.itemSize;

			for ( let face = 0; face < vertexCount; face += 3 ) {

				const a = ( face + 1 ) * itemSize;
				const b = ( face + 2 ) * itemSize;

				for ( let c = 0; c < itemSize; c ++ ) {

					const tmp = attribute.array[ a + c ];
					attribute.array[ a + c ] = attribute.array[ b + c ];
					attribute.array[ b + c ] = tmp;

				}

			}

		};

		for ( const name of Object.keys( result.attributes ) ) {

			swapAttribute( result.attributes[ name ] );

		}

		for ( const name of Object.keys( result.morphAttributes ) ) {

			for ( const attribute of result.morphAttributes[ name ] ) swapAttribute( attribute );

		}

	}

	// ---- groups, draw range, user data --------------------------------------
	// Triangle order and material grouping are unchanged; the reflection only
	// rewrites which corner each triangle uses, not where triangles start.

	for ( const group of geometry.groups ) {

		result.addGroup( group.start, group.count, group.materialIndex );

	}

	result.drawRange.start = geometry.drawRange.start;
	result.drawRange.count = geometry.drawRange.count;
	result.userData = geometry.userData;
	result.name = geometry.name;
	result._transformed = true;

	// Bounds are rebuilt through the geometry's own computation entry points only
	// when the source already had them; absent bounds stay lazy on the result too.

	if ( geometry.boundingBox !== null ) result.computeBoundingBox();
	if ( geometry.boundingSphere !== null ) result.computeBoundingSphere();

	return result;

}

export {
	computeMikkTSpaceTangents,
	mergeGeometries,
	mergeAttributes,
	deepCloneAttribute,
	deinterleaveAttribute,
	deinterleaveGeometry,
	interleaveAttributes,
	estimateBytesUsed,
	mergeVertices,
	remapAttribute,
	toTrianglesDrawMode,
	computeMorphedAttributes,
	mergeGroups,
	toCreasedNormals,
	bakeAffineGeometry
};
