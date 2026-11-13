import {
	BufferAttribute,
	BufferGeometry,
	Float32BufferAttribute,
	InstancedBufferAttribute,
	InstancedBufferGeometry,
	InterleavedBuffer,
	InterleavedBufferAttribute,
	Int8BufferAttribute,
	Matrix3,
	Matrix4,
	Mesh,
	Uint16BufferAttribute,
	Uint8BufferAttribute,
	Vector3,
} from 'three';
import { bakeAffineGeometry } from '../examples/jsm/utils/BufferGeometryUtils.js';

const EPS = 2e-4;

const approx = ( a, b, eps = EPS ) => Math.abs( a - b ) <= eps;

const approxVec = ( a, b, eps = EPS ) =>
	approx( a.x, b.x, eps ) && approx( a.y, b.y, eps ) && approx( a.z, b.z, eps );

// Quad in the XY plane, +Z facing. Indexed uses 4 shared vertices; non-indexed
// expands the two triangles into 6 vertices.

const QUAD_POSITIONS = [
	[ 0, 0, 0 ], [ 1, 0, 0 ], [ 1, 1, 0 ], [ 0, 1, 0 ],
];

const QUAD_UVS = [
	[ 0, 0 ], [ 1, 0 ], [ 1, 1 ], [ 0, 1 ],
];

const QUAD_INDEX = [ 0, 1, 2, 0, 2, 3 ];

function expandQuad( perVertex ) {

	return [ perVertex[ 0 ], perVertex[ 1 ], perVertex[ 2 ], perVertex[ 0 ], perVertex[ 2 ], perVertex[ 3 ] ];

}

function flat( values ) {

	const out = [];

	for ( const value of values ) out.push( ... value );

	return out;

}

// Two morph position targets pointing in clearly different directions.
const MORPHS_POSITION = [
	[ [ 0, 0, 1 ], [ 1, 0, 2 ], [ 0, 1, 3 ], [ 1, 1, 4 ] ],
	[ [ 0, 2, - 1 ], [ - 2, 0, 1 ], [ 1, - 1, 2 ], [ - 1, 1, - 2 ] ],
];

// Two morph normal targets with different directions.
const MORPHS_NORMAL = [
	[ [ 0.5, 0, 0.2 ], [ 0.4, 0.1, 0.3 ], [ 0.6, - 0.1, 0.2 ], [ 0.3, 0.2, 0.4 ] ],
	[ [ 0, 0.6, - 0.3 ], [ 0.1, 0.5, - 0.2 ], [ - 0.2, 0.7, 0.1 ], [ 0.2, 0.4, - 0.4 ] ],
];

const WEIGHTS = [
	[ 0, 0 ],
	[ 1, 0 ],
	[ 0, 1 ],
	[ 0.5, - 0.7 ],
	[ - 1.3, 2.1 ],
	[ 0.25, 0.25 ],
];

function buildQuad( options = {} ) {

	const {
		indexed = true,
		relative = true,
		withNormal = true,
		withTangent = true,
		withMorphNormals = true,
	} = options;

	const geometry = new BufferGeometry();
	const count = indexed ? 4 : 6;
	const positions = indexed ? QUAD_POSITIONS : expandQuad( QUAD_POSITIONS );
	const uvs = indexed ? QUAD_UVS : expandQuad( QUAD_UVS );

	geometry.setAttribute( 'position', new Float32BufferAttribute( flat( positions ), 3 ) );
	geometry.setAttribute( 'uv', new Float32BufferAttribute( flat( uvs ), 2 ) );
	geometry.setAttribute( 'color', new Float32BufferAttribute( flat( positions.map( ( p ) => [ p[ 0 ], p[ 1 ], 1 ] ) ), 3 ) );

	// Quantized, non-normalized custom attribute: type must survive the bake.
	const weights = new Uint16Array( count * 2 );

	for ( let i = 0; i < count; i ++ ) {

		weights[ i * 2 + 0 ] = 100 + i;
		weights[ i * 2 + 1 ] = 1000 + i;

	}

	geometry.setAttribute( 'weight', new BufferAttribute( weights, 2, false ) );

	if ( withNormal ) {

		const normals = positions.map( () => [ 0, 0, 1 ] );
		geometry.setAttribute( 'normal', new Float32BufferAttribute( flat( normals ), 3 ) );

	}

	if ( withTangent ) {

		const tangents = positions.map( ( _, i ) => [ 1, 0, 0, i % 2 === 0 ? 1 : - 1 ] );
		geometry.setAttribute( 'tangent', new Float32BufferAttribute( flat( tangents ), 4 ) );

	}

	if ( indexed ) geometry.setIndex( QUAD_INDEX );

	geometry.addGroup( 0, 3, 0 );
	geometry.addGroup( 3, 3, 1 );
	geometry.setDrawRange( 0, 6 );

	geometry.morphTargetsRelative = relative;
	geometry.morphAttributes.position = MORPHS_POSITION.map( ( target ) => {

		const data = indexed ? target : expandQuad( target );
		return new Float32BufferAttribute( flat( data ), 3 );

	} );

	if ( withMorphNormals && withNormal ) {

		geometry.morphAttributes.normal = MORPHS_NORMAL.map( ( target ) => {

			const data = indexed ? target : expandQuad( target );
			return new Float32BufferAttribute( flat( data ), 3 );

		} );

	}

	return geometry;

}

// Interleaved variant: position and a normalized Int8 custom tag share one
// InterleavedBuffer; normal/tangent/morph records live in separate attributes.
function buildInterleavedQuad( options = {} ) {

	const { indexed = true, relative = true } = options;
	const geometry = new BufferGeometry();
	const count = indexed ? 4 : 6;
	const positions = indexed ? QUAD_POSITIONS : expandQuad( QUAD_POSITIONS );

	// stride 6: xyz position + xyz normalized custom tag.
	const buffer = new Int8Array( count * 6 );
	const tags = [];

	for ( let i = 0; i < count; i ++ ) {

		const tag = [ ( i % 2 ) * 2 - 1, ( i % 3 ) - 1, i > 2 ? 1 : - 1 ];
		tags.push( tag );
		buffer[ i * 6 + 0 ] = positions[ i ][ 0 ];
		buffer[ i * 6 + 1 ] = positions[ i ][ 1 ];
		buffer[ i * 6 + 2 ] = positions[ i ][ 2 ];
		buffer[ i * 6 + 3 ] = tag[ 0 ];
		buffer[ i * 6 + 4 ] = tag[ 1 ];
		buffer[ i * 6 + 5 ] = tag[ 2 ];

	}

	const interleaved = new InterleavedBuffer( buffer, 6 );
	geometry.setAttribute( 'position', new InterleavedBufferAttribute( interleaved, 3, 0 ) );
	geometry.setAttribute( 'tag', new InterleavedBufferAttribute( interleaved, 3, 3, true ) );

	const normals = positions.map( () => [ 0, 0, 1 ] );
	geometry.setAttribute( 'normal', new Float32BufferAttribute( flat( normals ), 3 ) );
	const tangents = positions.map( ( _, i ) => [ 1, 0, 0, i % 2 === 0 ? 1 : - 1 ] );
	geometry.setAttribute( 'tangent', new Float32BufferAttribute( flat( tangents ), 4 ) );

	if ( indexed ) geometry.setIndex( QUAD_INDEX );

	geometry.morphTargetsRelative = relative;
	geometry.morphAttributes.position = MORPHS_POSITION.map( ( target ) => {

		const data = indexed ? target : expandQuad( target );
		return new Float32BufferAttribute( flat( data ), 3 );

	} );
	geometry.morphAttributes.normal = MORPHS_NORMAL.map( ( target ) => {

		const data = indexed ? target : expandQuad( target );
		return new Float32BufferAttribute( flat( data ), 3 );

	} );

	geometry.userData.expectedTags = tags;

	return geometry;

}

function matrixScaleTranslate() {

	return new Matrix4().makeScale( 2, 3, 1 ).setPosition( 4, 5, 6 );

}

function matrixShear() {

	const matrix = new Matrix4().makeShear( 0.3, 0.1, 0.2, 0.05, 0.15, 0.1 );
	matrix.multiply( new Matrix4().makeScale( 1.5, 0.7, 2.0 ) );
	matrix.setPosition( - 2, 3, 1 );
	return matrix;

}

function matrixReflection() {

	return new Matrix4().makeScale( - 1, 1, 1 );

}

function matrixReflectionScaled() {

	return new Matrix4().makeScale( - 2, 3, 1 ).setPosition( 7, - 2, 4 );

}

// Independent reference: blend source normals with the engine's morph rule,
// then apply the inverse-transpose normal matrix and normalize once.
function referenceNormal( geometry, vertex, influences, normalMatrix ) {

	const base = new Vector3().fromBufferAttribute( geometry.attributes.normal, vertex );
	const records = geometry.morphAttributes.normal || [];
	const blend = new Vector3();

	if ( geometry.morphTargetsRelative ) {

		blend.copy( base );

		for ( let i = 0; i < records.length; i ++ ) {

			if ( influences[ i ] === 0 ) continue;

			const delta = new Vector3().fromBufferAttribute( records[ i ], vertex );
			blend.addScaledVector( delta, influences[ i ] );

		}

	} else {

		const sum = influences.reduce( ( acc, value ) => acc + value, 0 );
		blend.copy( base ).multiplyScalar( 1 - sum );

		for ( let i = 0; i < records.length; i ++ ) {

			if ( influences[ i ] === 0 ) continue;

			const target = new Vector3().fromBufferAttribute( records[ i ], vertex );
			blend.addScaledVector( target, influences[ i ] );

		}

	}

	return blend.applyMatrix3( normalMatrix ).normalize();

}

function bakedNormal( geometry, vertex, influences ) {

	const base = new Vector3().fromBufferAttribute( geometry.attributes.normal, vertex );
	const records = geometry.morphAttributes.normal || [];
	const blend = new Vector3();

	if ( geometry.morphTargetsRelative ) {

		blend.copy( base );

		for ( let i = 0; i < records.length; i ++ ) {

			if ( influences[ i ] === 0 ) continue;

			blend.addScaledVector( new Vector3().fromBufferAttribute( records[ i ], vertex ), influences[ i ] );

		}

	} else {

		const sum = influences.reduce( ( acc, value ) => acc + value, 0 );
		blend.copy( base ).multiplyScalar( 1 - sum );

		for ( let i = 0; i < records.length; i ++ ) {

			if ( influences[ i ] === 0 ) continue;

			blend.addScaledVector( new Vector3().fromBufferAttribute( records[ i ], vertex ), influences[ i ] );

		}

	}

	return blend.normalize();

}

function faceNormal( geometry, face ) {

	const position = geometry.attributes.position;
	const index = geometry.index;
	const pick = ( corner ) => index ? index.array[ face * 3 + corner ] : face * 3 + corner;
	const a = new Vector3().fromBufferAttribute( position, pick( 0 ) );
	const b = new Vector3().fromBufferAttribute( position, pick( 1 ) );
	const c = new Vector3().fromBufferAttribute( position, pick( 2 ) );

	return new Vector3().subVectors( b, a ).cross( new Vector3().subVectors( c, a ) ).normalize();

}

// Non-indexed reflection swaps triangle corners 2 and 3, so output vertex v
// carries the data of a different source vertex.
function sourceVertexForBaked( vertex, reflected ) {

	if ( ! reflected ) return vertex;

	const mod = vertex % 3;

	if ( mod === 1 ) return vertex + 1;
	if ( mod === 2 ) return vertex - 1;

	return vertex;

}

function snapshot( geometry ) {

	return {
		position: geometry.attributes.position.array.slice(),
		morph: ( geometry.morphAttributes.position || [] ).map( ( attribute ) => attribute.array.slice() ),
		normal: geometry.attributes.normal ? geometry.attributes.normal.array.slice() : null,
	};

}

export function registerCases( registry ) {

	// ---------------------------------------------------------------------
	// position: identity must hold through Mesh.getVertexPosition.
	// ---------------------------------------------------------------------

	registry.add( 'position', 'relative-indexed-scale-translate', async ( { check } ) => {

		const source = buildQuad( { indexed: true, relative: true } );
		const matrix = matrixScaleTranslate();
		const baked = bakeAffineGeometry( source, matrix );
		const sourceMesh = new Mesh( source );
		const bakedMesh = new Mesh( baked );

		for ( const influences of WEIGHTS ) {

			sourceMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			sourceMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];
			bakedMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			bakedMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];

			for ( let vertex = 0; vertex < 4; vertex ++ ) {

				const expected = sourceMesh.getVertexPosition( vertex, new Vector3() ).applyMatrix4( matrix );
				const actual = bakedMesh.getVertexPosition( vertex, new Vector3() );
				check( approxVec( actual, expected ), `vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	registry.add( 'position', 'absolute-indexed-morph-endpoints', async ( { check } ) => {

		const source = buildQuad( { indexed: true, relative: false } );
		const matrix = matrixShear();
		const baked = bakeAffineGeometry( source, matrix );
		const sourceMesh = new Mesh( source );
		const bakedMesh = new Mesh( baked );

		for ( const influences of WEIGHTS ) {

			sourceMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			sourceMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];
			bakedMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			bakedMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];

			for ( let vertex = 0; vertex < 4; vertex ++ ) {

				const expected = sourceMesh.getVertexPosition( vertex, new Vector3() ).applyMatrix4( matrix );
				const actual = bakedMesh.getVertexPosition( vertex, new Vector3() );
				check( approxVec( actual, expected, 5e-4 ), `absolute vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	registry.add( 'position', 'non-indexed-shear-negative-weights', async ( { check } ) => {

		const source = buildQuad( { indexed: false, relative: true } );
		const matrix = matrixShear();
		const baked = bakeAffineGeometry( source, matrix );
		const sourceMesh = new Mesh( source );
		const bakedMesh = new Mesh( baked );

		for ( const influences of [ [ - 1.7, 2.3 ], [ 0.5, - 0.5 ], [ 2, - 2 ] ] ) {

			sourceMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			sourceMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];
			bakedMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			bakedMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = sourceMesh.getVertexPosition( vertex, new Vector3() ).applyMatrix4( matrix );
				const actual = bakedMesh.getVertexPosition( vertex, new Vector3() );
				check( approxVec( actual, expected, 5e-4 ), `non-indexed vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	registry.add( 'position', 'relative-displacement-drops-translation', async ( { check } ) => {

		const source = buildQuad( { indexed: true, relative: true } );
		const matrix = matrixScaleTranslate();
		const baked = bakeAffineGeometry( source, matrix );
		const linear = new Matrix4().makeScale( 2, 3, 1 );

		for ( let target = 0; target < 2; target ++ ) {

			for ( let vertex = 0; vertex < 4; vertex ++ ) {

				const displacement = new Vector3().fromBufferAttribute( source.morphAttributes.position[ target ], vertex );
				const expected = displacement.applyMatrix4( linear );
				const actual = new Vector3().fromBufferAttribute( baked.morphAttributes.position[ target ], vertex );
				check( approxVec( actual, expected ), `morph ${ target } vertex ${ vertex } keeps translation out` );

			}

		}

		check( ! approxVec(
			new Vector3().fromBufferAttribute( baked.morphAttributes.position[ 0 ], 1 ),
			new Vector3().fromBufferAttribute( source.morphAttributes.position[ 0 ], 1 )
		), 'baked target differs from source target' );

	} );

	registry.add( 'position', 'interleaved-position-through-mesh', async ( { check } ) => {

		const source = buildInterleavedQuad( { indexed: true, relative: true } );
		const matrix = matrixReflectionScaled();
		const baked = bakeAffineGeometry( source, matrix );
		const sourceMesh = new Mesh( source );
		const bakedMesh = new Mesh( baked );

		for ( const influences of WEIGHTS ) {

			sourceMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			sourceMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];
			bakedMesh.morphTargetInfluences[ 0 ] = influences[ 0 ];
			bakedMesh.morphTargetInfluences[ 1 ] = influences[ 1 ];

			for ( let vertex = 0; vertex < 4; vertex ++ ) {

				const expected = sourceMesh.getVertexPosition( vertex, new Vector3() ).applyMatrix4( matrix );
				const actual = bakedMesh.getVertexPosition( vertex, new Vector3() );
				check( approxVec( actual, expected, 5e-4 ), `interleaved vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	registry.add( 'position', 'reflection-positions-point-transform', async ( { check } ) => {

		for ( const indexed of [ true, false ] ) {

			const source = buildQuad( { indexed, relative: true } );
			const matrix = matrixReflection();
			const baked = bakeAffineGeometry( source, matrix );
			const count = indexed ? 4 : 6;

			if ( indexed ) {

				for ( let vertex = 0; vertex < count; vertex ++ ) {

					const expected = new Vector3().fromBufferAttribute( source.attributes.position, vertex ).applyMatrix4( matrix );
					const actual = new Vector3().fromBufferAttribute( baked.attributes.position, vertex );
					check( approxVec( actual, expected ), `indexed base position ${ vertex }` );

				}

			} else {

				// Corner swaps reorder the non-indexed vertices, so compare as a point set.
				const key = ( vector ) => `${ vector.x.toFixed( 4 ) },${ vector.y.toFixed( 4 ) },${ vector.z.toFixed( 4 ) }`;
				const expected = new Set();

				for ( let vertex = 0; vertex < count; vertex ++ ) {

					expected.add( key( new Vector3().fromBufferAttribute( source.attributes.position, vertex ).applyMatrix4( matrix ) ) );

				}

				for ( let vertex = 0; vertex < count; vertex ++ ) {

					check( expected.has( key( new Vector3().fromBufferAttribute( baked.attributes.position, vertex ) ) ),
						`non-indexed reflected position ${ vertex } belongs to the transformed point set` );

				}

			}

		}

	} );

	// ---------------------------------------------------------------------
	// normal: independent linear blend + inverse transpose reference.
	// ---------------------------------------------------------------------

	registry.add( 'normal', 'two-target-directions-differ', async ( { check } ) => {

		const source = buildQuad( { indexed: true } );
		const first = new Vector3().fromBufferAttribute( source.morphAttributes.normal[ 0 ], 0 );
		const second = new Vector3().fromBufferAttribute( source.morphAttributes.normal[ 1 ], 0 );
		check( first.dot( second ) / ( first.length() * second.length() ) < 0.8, 'morph normal targets point different ways' );

	} );

	registry.add( 'normal', 'non-uniform-scale-indexed', async ( { check } ) => {

		const source = buildQuad( { indexed: true, relative: true } );
		const matrix = matrixScaleTranslate();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		for ( const influences of WEIGHTS ) {

			for ( let vertex = 0; vertex < 4; vertex ++ ) {

				const reference = referenceNormal( source, vertex, influences, normalMatrix );
				check( reference.lengthSq() > 0, `reference blend nonzero vertex ${ vertex } weights ${ influences }` );
				const actual = bakedNormal( baked, vertex, influences );
				check( actual.dot( reference ) > 1 - 1e-5, `normal vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	registry.add( 'normal', 'shear-non-indexed-negative-weights', async ( { check } ) => {

		const source = buildQuad( { indexed: false, relative: true } );
		const matrix = matrixShear();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		for ( const influences of [ [ - 1.4, 1.6 ], [ 0.3, - 0.9 ], [ - 2, - 1 ] ] ) {

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const reference = referenceNormal( source, vertex, influences, normalMatrix );
				check( reference.lengthSq() > 0, `shear reference nonzero vertex ${ vertex }` );
				const actual = bakedNormal( baked, vertex, influences );
				check( actual.dot( reference ) > 1 - 1e-5, `shear normal vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	registry.add( 'normal', 'absolute-morph-normal-rule', async ( { check } ) => {

		const source = buildQuad( { indexed: true, relative: false } );
		const matrix = matrixReflectionScaled();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		for ( const influences of [ [ 0.7, 0.2 ], [ - 0.5, 1.2 ], [ 1.5, - 0.5 ] ] ) {

			for ( let vertex = 0; vertex < 4; vertex ++ ) {

				const reference = referenceNormal( source, vertex, influences, normalMatrix );
				check( reference.lengthSq() > 0, `absolute reference nonzero vertex ${ vertex }` );
				const actual = bakedNormal( baked, vertex, influences );
				check( actual.dot( reference ) > 1 - 1e-5, `absolute normal vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	registry.add( 'normal', 'base-unit-targets-not-renormalized', async ( { check } ) => {

		const source = buildQuad( { indexed: true, relative: true } );
		const matrix = matrixScaleTranslate();
		const baked = bakeAffineGeometry( source, matrix );

		for ( let vertex = 0; vertex < 4; vertex ++ ) {

			const base = new Vector3().fromBufferAttribute( baked.attributes.normal, vertex );
			check( approx( base.length(), 1, 1e-6 ), `base normal unit vertex ${ vertex }` );

		}

		const targetNormal = new Vector3().fromBufferAttribute( baked.morphAttributes.normal[ 0 ], 0 );
		check( ! approx( targetNormal.length(), 1, 1e-3 ), 'morph normal keeps transform scale instead of being renormalized' );

	} );

	registry.add( 'normal', 'interleaved-reflection-normal', async ( { check } ) => {

		const source = buildInterleavedQuad( { indexed: false, relative: true } );
		const matrix = matrixReflectionScaled();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		for ( const influences of [ [ 0.5, - 0.7 ], [ - 1.1, 0.9 ] ] ) {

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const sourceVertex = sourceVertexForBaked( vertex, true );
				const reference = referenceNormal( source, sourceVertex, influences, normalMatrix );
				const actual = bakedNormal( baked, vertex, influences );
				check( actual.dot( reference ) > 1 - 1e-5, `interleaved normal vertex ${ vertex } weights ${ influences }` );

			}

		}

	} );

	// ---------------------------------------------------------------------
	// records: transformed records, reflection, winding, tangents, groups.
	// ---------------------------------------------------------------------

	registry.add( 'records', 'indexed-reflection-swaps-index-corners', async ( { check } ) => {

		const source = buildQuad( { indexed: true, relative: true } );
		const baked = bakeAffineGeometry( source, matrixReflection() );

		check( baked.index !== null, 'baked geometry stays indexed' );

		for ( let face = 0; face < 2; face ++ ) {

			const a = QUAD_INDEX[ face * 3 + 0 ];
			const b = QUAD_INDEX[ face * 3 + 1 ];
			const c = QUAD_INDEX[ face * 3 + 2 ];
			const out = baked.index.array;
			check( out[ face * 3 + 0 ] === a && out[ face * 3 + 1 ] === c && out[ face * 3 + 2 ] === b,
				`face ${ face } swaps only corners 2 and 3` );

		}

	} );

	registry.add( 'records', 'non-indexed-reflection-swaps-every-attribute', async ( { check } ) => {

		const source = buildQuad( { indexed: false, relative: true } );
		const baked = bakeAffineGeometry( source, matrixReflectionScaled() );

		check( baked.index === null, 'baked geometry stays non-indexed' );

		for ( let face = 0; face < 2; face ++ ) {

			const b = face * 3 + 1;
			const c = face * 3 + 2;

			check(
				baked.attributes.uv.getX( b ) === source.attributes.uv.getX( c ) &&
				baked.attributes.uv.getY( b ) === source.attributes.uv.getY( c ),
				`face ${ face } uv corner b carries source corner c`
			);

			check(
				baked.attributes.weight.array[ b * 2 ] === source.attributes.weight.array[ c * 2 ] &&
				baked.attributes.weight.array[ b * 2 + 1 ] === source.attributes.weight.array[ c * 2 + 1 ],
				`face ${ face } custom weight corner swap`
			);

			// Relative morph position records are displacements (linear part only) and must
			// travel with their corner through the swap.
			const reflectionLinear = new Matrix4().makeScale( - 2, 3, 1 );
			const expectedMorphPosition = new Vector3()
				.fromBufferAttribute( source.morphAttributes.position[ 0 ], c )
				.applyMatrix4( reflectionLinear );
			check(
				approxVec(
					new Vector3().fromBufferAttribute( baked.morphAttributes.position[ 0 ], b ),
					expectedMorphPosition,
					5e-4
				),
				`face ${ face } morph position travels with its corner`
			);

			// The morph normal record must travel with its corner through the
			// inverse-transpose transform and the per-vertex normal scale.
			const normalMatrix = new Matrix3().getNormalMatrix( matrixReflectionScaled() );
			const sourceNormalC = new Vector3().fromBufferAttribute( source.attributes.normal, c );
			const scale = 1 / sourceNormalC.applyMatrix3( normalMatrix ).length();
			const expectedMorphC = new Vector3()
				.fromBufferAttribute( source.morphAttributes.normal[ 1 ], c )
				.applyMatrix3( normalMatrix ).multiplyScalar( scale );
			check(
				approxVec(
					new Vector3().fromBufferAttribute( baked.morphAttributes.normal[ 1 ], b ),
					expectedMorphC,
					5e-4
				),
				`face ${ face } morph normal travels with its corner`
			);

		}

	} );

	registry.add( 'records', 'winding-front-face-preserved', async ( { check } ) => {

		for ( const indexed of [ true, false ] ) {

			for ( const matrix of [ matrixShear(), matrixReflectionScaled() ] ) {

				const source = buildQuad( { indexed, relative: true } );
				const baked = bakeAffineGeometry( source, matrix );
				const normalMatrix = new Matrix3().getNormalMatrix( matrix );

				for ( let face = 0; face < 2; face ++ ) {

					const sourceFace = faceNormal( source, face ).applyMatrix3( normalMatrix ).normalize();
					const bakedFace = faceNormal( baked, face );
					check( bakedFace.dot( sourceFace ) > 1 - 1e-4,
						`${ indexed ? 'indexed' : 'non-indexed' } face ${ face } front face preserved` );

				}

			}

		}

	} );

	registry.add( 'records', 'groups-drawrange-triangle-order-kept', async ( { check } ) => {

		const source = buildQuad( { indexed: true } );
		const baked = bakeAffineGeometry( source, matrixShear() );

		check( baked.groups.length === 2, 'group count preserved' );
		check( baked.groups[ 0 ].start === 0 && baked.groups[ 0 ].count === 3 && baked.groups[ 0 ].materialIndex === 0, 'group 0' );
		check( baked.groups[ 1 ].start === 3 && baked.groups[ 1 ].count === 3 && baked.groups[ 1 ].materialIndex === 1, 'group 1' );
		check( baked.drawRange.start === 0 && baked.drawRange.count === 6, 'draw range preserved' );
		check( baked.morphTargetsRelative === source.morphTargetsRelative, 'relative flag preserved' );

		const absolute = buildQuad( { indexed: true, relative: false } );
		const bakedAbsolute = bakeAffineGeometry( absolute, matrixShear() );
		check( bakedAbsolute.morphTargetsRelative === false, 'absolute flag preserved' );

	} );

	registry.add( 'records', 'tangent-plane-orthogonality-and-handedness', async ( { check } ) => {

		const source = buildQuad( { indexed: true } );
		const positive = bakeAffineGeometry( source, matrixShear() );
		const reflected = bakeAffineGeometry( source, matrixReflectionScaled() );

		for ( let vertex = 0; vertex < 4; vertex ++ ) {

			const tangent = new Vector3(
				positive.attributes.tangent.getX( vertex ),
				positive.attributes.tangent.getY( vertex ),
				positive.attributes.tangent.getZ( vertex )
			);
			const normal = new Vector3().fromBufferAttribute( positive.attributes.normal, vertex );

			check( approx( tangent.length(), 1, 1e-6 ), `positive tangent unit vertex ${ vertex }` );
			check( Math.abs( tangent.dot( normal ) ) < 1e-5, `tangent orthogonal to normal vertex ${ vertex }` );

			const sourceTangent = new Vector3(
				source.attributes.tangent.getX( vertex ),
				source.attributes.tangent.getY( vertex ),
				source.attributes.tangent.getZ( vertex )
			);
			// Directions use the linear part only (w = 0), matching Vector3.transformDirection,
			// then Gram-Schmidt onto the baked unit normal plane.
			const expectedTangent = sourceTangent.transformDirection( matrixShear() );
			expectedTangent.addScaledVector( normal, - expectedTangent.dot( normal ) ).normalize();
			check( approxVec( tangent, expectedTangent, 1e-5 ), `tangent follows transformed tangent plane vertex ${ vertex }` );

			check(
				positive.attributes.tangent.getW( vertex ) === source.attributes.tangent.getW( vertex ),
				`positive determinant keeps tangent w vertex ${ vertex }`
			);
			check(
				reflected.attributes.tangent.getW( vertex ) === - source.attributes.tangent.getW( vertex ),
				`negative determinant flips tangent w vertex ${ vertex }`
			);

		}

	} );

	registry.add( 'records', 'interleaved-custom-tag-and-reflection', async ( { check } ) => {

		const source = buildInterleavedQuad( { indexed: false, relative: true } );
		const matrix = matrixReflection();
		const baked = bakeAffineGeometry( source, matrix );

		check( baked.attributes.tag.isInterleavedBufferAttribute !== true, 'interleaved data is de-interleaved' );
		check( baked.attributes.tag.array.constructor === Int8Array || baked.attributes.tag.array.constructor.name === 'Int8Array', 'tag keeps Int8 storage' );
		check( baked.attributes.tag.normalized === true, 'tag keeps normalized flag' );

		for ( let face = 0; face < 2; face ++ ) {

			const b = face * 3 + 1;
			const c = face * 3 + 2;
			const bakedTag = baked.attributes.tag;
			const sourceRaw = source.attributes.tag.data.array;
			const stride = source.attributes.tag.data.stride;
			const offset = source.attributes.tag.offset;
			const matches = [ 0, 1, 2 ].every( ( component ) =>
				bakedTag.array[ b * 3 + component ] === sourceRaw[ c * stride + offset + component ]
			);

			check( matches, `face ${ face } normalized tag swaps with its corner` );

		}

	} );

	registry.add( 'records', 'rejection-paths', async ( { check } ) => {

		const expectThrow = ( fn, label ) => {

			try {

				fn();
				check( false, `${ label } should throw` );

			} catch ( error ) {

				check( error instanceof Error && error.message.includes( 'bakeAffineGeometry' ), `${ label } throws` );

			}

		};

		const good = () => buildQuad( { indexed: true } );

		let missing = new BufferGeometry();
		missing.setAttribute( 'uv', new Float32BufferAttribute( [ 0, 0 ], 2 ) );
		expectThrow( () => bakeAffineGeometry( missing, matrixShear() ), 'missing position' );

		const mismatched = good();
		mismatched.morphAttributes.position = [ new Float32BufferAttribute( [ 0, 0, 0 ], 3 ) ];
		expectThrow( () => bakeAffineGeometry( mismatched, matrixShear() ), 'morph length mismatch' );

		const badIndex = good();
		badIndex.setIndex( [ 0, 1, 99 ] );
		expectThrow( () => bakeAffineGeometry( badIndex, matrixShear() ), 'illegal triangle index' );

		expectThrow( () => bakeAffineGeometry( good(), new Matrix4().makeScale( 1, 0, 2 ) ), 'singular matrix' );

		const nonAffine = new Matrix4().makeScale( 2, 2, 2 );
		nonAffine.elements[ 3 ] = 0.5;
		expectThrow( () => bakeAffineGeometry( good(), nonAffine ), 'non-affine matrix' );

		const nonFinite = good();
		nonFinite.attributes.position.array[ 4 ] = Number.NaN;
		expectThrow( () => bakeAffineGeometry( nonFinite, matrixShear() ), 'non-finite value' );

		const morphColor = good();
		morphColor.morphAttributes.color = [ new Float32BufferAttribute( new Array( 12 ).fill( 0 ), 3 ) ];
		expectThrow( () => bakeAffineGeometry( morphColor, matrixShear() ), 'unsupported morph kind' );

		const instanced = good();
		instanced.setAttribute( 'weight', new InstancedBufferAttribute( new Float32Array( 8 ), 2 ) );
		expectThrow( () => bakeAffineGeometry( instanced, matrixShear() ), 'instanced attribute' );

		const instancedGeometry = new InstancedBufferGeometry();
		instancedGeometry.setAttribute( 'position', new Float32BufferAttribute( flat( QUAD_POSITIONS ), 3 ) );
		expectThrow( () => bakeAffineGeometry( instancedGeometry, matrixShear() ), 'instanced geometry' );

		const noNormal = buildQuad( { indexed: true, withNormal: false, withMorphNormals: false } );
		noNormal.morphAttributes.normal = [ new Float32BufferAttribute( new Array( 12 ).fill( 0.1 ), 3 ) ];
		expectThrow( () => bakeAffineGeometry( noNormal, matrixShear() ), 'morph normal without base normal' );

	} );

	// ---------------------------------------------------------------------
	// ownership: source immutability, type preservation, bounds, isolation.
	// ---------------------------------------------------------------------

	registry.add( 'ownership', 'source-geometry-and-matrix-unchanged', async ( { check } ) => {

		const source = buildQuad( { indexed: true } );
		const before = snapshot( source );
		const matrix = matrixShear();
		const matrixBefore = matrix.elements.slice();
		bakeAffineGeometry( source, matrix );

		check( source.attributes.position.array.every( ( value, i ) => value === before.position[ i ] ), 'base position untouched' );
		check( source.morphAttributes.position.every( ( attribute, t ) =>
			attribute.array.every( ( value, i ) => value === before.morph[ t ][ i ] ) ), 'morph positions untouched' );
		check( source.attributes.normal.array.every( ( value, i ) => value === before.normal[ i ] ), 'normals untouched' );
		check( matrix.elements.every( ( value, i ) => value === matrixBefore[ i ] ), 'matrix untouched' );

	} );

	registry.add( 'ownership', 'sibling-geometry-sharing-buffers-isolated', async ( { check } ) => {

		const source = buildQuad( { indexed: true } );
		const sibling = new BufferGeometry();
		sibling.setAttribute( 'position', source.getAttribute( 'position' ) );
		sibling.setAttribute( 'normal', source.getAttribute( 'normal' ) );
		const sharedBefore = source.getAttribute( 'position' ).array.slice();
		bakeAffineGeometry( source, matrixShear() );
		check( sibling.getAttribute( 'position' ).array.every( ( value, i ) => value === sharedBefore[ i ] ),
			'sibling sharing the source attribute is unaffected' );

	} );

	registry.add( 'ownership', 'returned-arrays-independent', async ( { check } ) => {

		const source = buildQuad( { indexed: true } );
		const baked = bakeAffineGeometry( source, matrixShear() );

		check( baked.attributes.position.array !== source.attributes.position.array, 'distinct position storage' );
		check( baked.morphAttributes.position[ 0 ].array !== source.morphAttributes.position[ 0 ].array, 'distinct morph storage' );
		check( baked instanceof BufferGeometry && baked !== source, 'returns a new BufferGeometry' );

		baked.attributes.position.array[ 0 ] = 999;
		baked.morphAttributes.position[ 0 ].array[ 1 ] = 999;
		check( source.attributes.position.array[ 0 ] !== 999, 'mutating baked base position cannot reach source' );
		check( source.morphAttributes.position[ 0 ].array[ 1 ] !== 999, 'mutating baked morph cannot reach source' );

	} );

	registry.add( 'ownership', 'interleaved-source-backing-buffer-unchanged', async ( { check } ) => {

		const source = buildInterleavedQuad( { indexed: true } );
		const backingBefore = source.attributes.position.data.array.slice();
		bakeAffineGeometry( source, matrixReflectionScaled() );
		check( source.attributes.position.data.array.every( ( value, i ) => value === backingBefore[ i ] ),
			'interleaved backing buffer untouched' );
		check( source.attributes.tag.isInterleavedBufferAttribute === true, 'source keeps its interleaved layout' );

	} );

	registry.add( 'ownership', 'non-geometric-type-preservation', async ( { check } ) => {

		const source = buildQuad( { indexed: false } );
		const baked = bakeAffineGeometry( source, matrixShear() );
		const weight = baked.attributes.weight;

		check( weight.array.constructor === Uint16Array, 'Uint16 storage preserved' );
		check( weight.itemSize === 2, 'itemSize preserved' );
		check( weight.normalized === false, 'normalized flag preserved' );

		const normalized = new BufferGeometry();
		normalized.setAttribute( 'position', new Float32BufferAttribute( flat( expandQuad( QUAD_POSITIONS ) ), 3 ) );
		const tags = new Uint8Array( expandQuad( QUAD_POSITIONS ).length * 2 );
		for ( let i = 0; i < tags.length; i ++ ) tags[ i ] = i * 7;
		normalized.setAttribute( 'tag', new Uint8BufferAttribute( tags, 2, true ) );
		const bakedNormalized = bakeAffineGeometry( normalized, matrixShear() );
		check( bakedNormalized.attributes.tag.array.constructor === Uint8Array, 'Uint8 storage preserved' );
		check( bakedNormalized.attributes.tag.normalized === true, 'normalized custom flag preserved' );
		check( Array.from( bakedNormalized.attributes.tag.array ).every( ( value, i ) => value === tags[ i ] ),
			'raw normalized bytes preserved' );

	} );

	registry.add( 'ownership', 'bounds-rebuilt-or-kept-lazy', async ( { check } ) => {

		const source = buildQuad( { indexed: true } );
		const lazy = bakeAffineGeometry( source, matrixShear() );
		check( lazy.boundingBox === null && lazy.boundingSphere === null, 'absent bounds stay lazy' );

		source.computeBoundingBox();
		source.computeBoundingSphere();
		const matrix = matrixReflectionScaled();
		const baked = bakeAffineGeometry( source, matrix );

		check( baked.boundingBox !== null && baked.boundingSphere !== null, 'existing bounds are rebuilt' );
		const expectedBox = source.boundingBox.clone().applyMatrix4( matrix );
		check( approxVec( baked.boundingBox.min, expectedBox.min, 1e-4 ), 'bounding box min' );
		check( approxVec( baked.boundingBox.max, expectedBox.max, 1e-4 ), 'bounding box max' );

		const center = baked.boundingSphere.center;
		const radius = baked.boundingSphere.radius;
		check( Number.isFinite( radius ) && radius > 0, 'finite bounding sphere radius' );
		let contained = true;
		for ( let vertex = 0; vertex < 4; vertex ++ ) {

			const point = new Vector3().fromBufferAttribute( baked.attributes.position, vertex );
			if ( point.distanceTo( center ) > radius + 1e-4 ) contained = false;

		}
		check( contained, 'bounding sphere contains every baked vertex' );

	} );

	registry.add( 'ownership', 'tangent-absence-and-attribute-set', async ( { check } ) => {

		const source = buildQuad( { indexed: true, withTangent: false } );
		const baked = bakeAffineGeometry( source, matrixShear() );
		check( baked.attributes.tangent === undefined, 'no tangent generated when source has none' );
		check( baked.attributes.uv !== undefined && baked.attributes.color !== undefined && baked.attributes.weight !== undefined,
			'uv, color and custom attributes are carried over' );
		check( baked.attributes.position.array.constructor === Float32Array, 'position output is Float32' );

		const quantized = new BufferGeometry();
		quantized.setAttribute( 'position', new Int8BufferAttribute( flat( QUAD_POSITIONS ), 3 ) );
		quantized.setIndex( QUAD_INDEX );
		const bakedQuantized = bakeAffineGeometry( quantized, matrixScaleTranslate() );
		check( bakedQuantized.attributes.position.array.constructor === Float32Array, 'quantized position promoted to Float32' );

	} );

}
