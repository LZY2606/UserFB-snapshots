import {
	BufferGeometry,
	Float32BufferAttribute,
	GLBufferAttribute,
	InstancedBufferAttribute,
	InstancedBufferGeometry,
	InterleavedBuffer,
	InterleavedBufferAttribute,
	Matrix3,
	Matrix4,
	Uint8BufferAttribute,
	Vector3,
	Mesh,
} from 'three';
import { bakeAffineGeometry } from '../examples/jsm/utils/BufferGeometryUtils.js';

// --- shared helpers ---------------------------------------------------------

const EPS = 1e-5;

function close( a, b, eps = EPS ) {

	return Math.abs( a - b ) <= eps * Math.max( 1, Math.abs( a ), Math.abs( b ) );

}

function vecClose( a, b, eps = EPS ) {

	return close( a.x, b.x, eps ) && close( a.y, b.y, eps ) && close( a.z, b.z, eps );

}

function snapshot( attribute ) {

	return Array.from(
		attribute.isInterleavedBufferAttribute === true ? attribute.data.array : attribute.array
	);

}

// A general affine matrix: non-uniform scale, shear, rotation and translation.
function makeAffineMatrix() {

	return new Matrix4().set(
		2.0, 0.3, 0.1, 4.0,
		- 0.2, 1.5, 0.4, - 1.0,
		0.5, - 0.3, 0.7, 2.5,
		0, 0, 0, 1
	);

}

function makeReflectionMatrix() {

	return new Matrix4().set(
		- 1.0, 0.2, 0.1, 3.0,
		0.0, 1.0, - 0.3, 0.5,
		0.4, 0.0, 1.0, - 2.0,
		0, 0, 0, 1
	);

}

// Triangle soup: two triangles, 6 vertices.
const POSITIONS = [
	1, 0, 0, 0, 1, 0, 0, 0, 1,
	1, 1, 0, 1, 1, 1, 0, 1, 1,
];

const NORMALS = [
	0, 0, 1, 0, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 0, 1, 0, 0,
];

const TANGENTS = [
	1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1,
	0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
];

// Two morph records pointing in different directions.
const MORPH_A = [
	0.5, 0.1, 0.2, 0.5, 0.1, 0.2, 0.5, 0.1, 0.2,
	- 0.3, 0.4, 0.1, - 0.3, 0.4, 0.1, - 0.3, 0.4, 0.1,
];

const MORPH_B = [
	- 0.2, 0.6, - 0.4, - 0.2, 0.6, - 0.4, - 0.2, 0.6, - 0.4,
	0.3, - 0.5, 0.7, 0.3, - 0.5, 0.7, 0.3, - 0.5, 0.7,
];

const MORPH_NORMAL_A = [
	1, 0, 0, 1, 0, 0, 1, 0, 0,
	0, 1, 0, 0, 1, 0, 0, 1, 0,
];

const MORPH_NORMAL_B = [
	0, 1, 1, 0, 1, 1, 0, 1, 1,
	1, 0, 1, 1, 0, 1, 1, 0, 1,
];

const WEIGHT_SETS = [
	[ 0.5, 0.25 ],
	[ - 0.75, 1.25 ],
	[ 1.25, - 0.5 ],
	[ - 1.0, 2.0 ],
	[ 0.0, - 1.5 ],
];

function buildGeometry( { indexed = true, relative = true, interleaved = false, tangent = true } = {} ) {

	const geometry = new BufferGeometry();

	if ( interleaved ) {

		// position and normal share one interleaved buffer; a second buffer holds a
		// normalized custom integer attribute and a float custom attribute.

		const stride = 6;
		const data = new Float32Array( 6 * stride );

		for ( let i = 0; i < 6; i ++ ) {

			data[ i * stride + 0 ] = POSITIONS[ i * 3 + 0 ];
			data[ i * stride + 1 ] = POSITIONS[ i * 3 + 1 ];
			data[ i * stride + 2 ] = POSITIONS[ i * 3 + 2 ];
			data[ i * stride + 3 ] = NORMALS[ i * 3 + 0 ];
			data[ i * stride + 4 ] = NORMALS[ i * 3 + 1 ];
			data[ i * stride + 5 ] = NORMALS[ i * 3 + 2 ];

		}

		const buffer = new InterleavedBuffer( data, stride );
		geometry.setAttribute( 'position', new InterleavedBufferAttribute( buffer, 3, 0 ) );
		geometry.setAttribute( 'normal', new InterleavedBufferAttribute( buffer, 3, 3 ) );

		const customStride = 5;
		const customData = new Uint8Array( 6 * customStride );

		for ( let i = 0; i < customData.length; i ++ ) customData[ i ] = 10 + i;

		const customBuffer = new InterleavedBuffer( customData, customStride );
		geometry.setAttribute( 'customNorm', new InterleavedBufferAttribute( customBuffer, 4, 0, true ) );
		geometry.setAttribute( 'customByte', new InterleavedBufferAttribute( customBuffer, 1, 4 ) );

	} else {

		geometry.setAttribute( 'position', new Float32BufferAttribute( POSITIONS, 3 ) );
		geometry.setAttribute( 'normal', new Float32BufferAttribute( NORMALS, 3 ) );
		geometry.setAttribute( 'uv', new Float32BufferAttribute( [
			0, 0, 0.1, 0, 0.1, 0.1,
			1, 0, 1, 1, 0, 1,
		], 2 ) );
		geometry.setAttribute( 'color', new Uint8BufferAttribute( [
			255, 0, 0, 0, 255, 0, 0, 0, 255,
			10, 20, 30, 40, 50, 60, 70, 80, 90,
		], 3, true ) );

	}

	if ( tangent ) geometry.setAttribute( 'tangent', new Float32BufferAttribute( TANGENTS, 4 ) );

	if ( indexed ) geometry.setIndex( [ 0, 1, 2, 3, 4, 5 ] );

	geometry.morphTargetsRelative = relative;
	geometry.morphAttributes.position = [
		new Float32BufferAttribute( MORPH_A, 3 ),
		new Float32BufferAttribute( MORPH_B, 3 ),
	];
	geometry.morphAttributes.normal = [
		new Float32BufferAttribute( MORPH_NORMAL_A, 3 ),
		new Float32BufferAttribute( MORPH_NORMAL_B, 3 ),
	];

	return geometry;

}

// Independent reference: blend exactly like Mesh.getVertexPosition, then apply the
// matrix to the resulting endpoint.
function referencePosition( geometry, matrix, weights, vertex ) {

	const mesh = new Mesh( geometry );
	mesh.morphTargetInfluences[ 0 ] = weights[ 0 ];
	mesh.morphTargetInfluences[ 1 ] = weights[ 1 ];
	return mesh.getVertexPosition( vertex, new Vector3() ).applyMatrix4( matrix );

}

// Independent reference: linear blend following the source morph rule, inverse
// transpose transform, then unitization.
function referenceNormal( geometry, normalMatrix, weights, vertex ) {

	const relative = geometry.morphTargetsRelative;
	const base = new Vector3().fromBufferAttribute( geometry.getAttribute( 'normal' ), vertex );
	const blended = base.clone();

	for ( let t = 0; t < 2; t ++ ) {

		const record = new Vector3().fromBufferAttribute( geometry.morphAttributes.normal[ t ], vertex );

		if ( relative ) {

			blended.addScaledVector( record, weights[ t ] );

		} else {

			blended.addScaledVector( record.sub( base ), weights[ t ] );

		}

	}

	return blended.applyMatrix3( normalMatrix ).normalize();

}

function blendedBakedNormal( baked, weights, vertex ) {

	const relative = baked.morphTargetsRelative;
	const base = new Vector3().fromBufferAttribute( baked.getAttribute( 'normal' ), vertex );
	const blended = base.clone();

	for ( let t = 0; t < 2; t ++ ) {

		const record = new Vector3().fromBufferAttribute( baked.morphAttributes.normal[ t ], vertex );

		if ( relative ) {

			blended.addScaledVector( record, weights[ t ] );

		} else {

			blended.addScaledVector( record.sub( base ), weights[ t ] );

		}

	}

	return blended.normalize();

}

function faceNormal( geometry, face ) {

	const index = geometry.getIndex();
	const get = ( corner ) => {

		const vertex = index !== null ? index.getX( face * 3 + corner ) : face * 3 + corner;
		return new Vector3().fromBufferAttribute( geometry.getAttribute( 'position' ), vertex );

	};

	const a = get( 0 ), b = get( 1 ), c = get( 2 );
	return c.sub( b ).cross( a.sub( b ) ).normalize();

}

// --- registry ---------------------------------------------------------------

function registerCases( registry ) {

	// === position ===========================================================

	registry.add( 'position', 'relative-indexed-matches-getVertexPosition', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		const matrix = makeAffineMatrix();
		const baked = bakeAffineGeometry( source, matrix );
		const mesh = new Mesh( baked );

		for ( const weights of WEIGHT_SETS ) {

			mesh.morphTargetInfluences[ 0 ] = weights[ 0 ];
			mesh.morphTargetInfluences[ 1 ] = weights[ 1 ];

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = referencePosition( source, matrix, weights, vertex );
				const actual = mesh.getVertexPosition( vertex, new Vector3() );
				check( vecClose( actual, expected, 1e-4 ), `vertex ${ vertex } weights ${ weights }` );

			}

		}

	} );

	registry.add( 'position', 'relative-non-indexed-matches-getVertexPosition', ( { check } ) => {

		const source = buildGeometry( { indexed: false, relative: true } );
		const matrix = makeAffineMatrix();
		const baked = bakeAffineGeometry( source, matrix );
		const mesh = new Mesh( baked );

		for ( const weights of WEIGHT_SETS ) {

			mesh.morphTargetInfluences[ 0 ] = weights[ 0 ];
			mesh.morphTargetInfluences[ 1 ] = weights[ 1 ];

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = referencePosition( source, matrix, weights, vertex );
				const actual = mesh.getVertexPosition( vertex, new Vector3() );
				check( vecClose( actual, expected, 1e-4 ), `vertex ${ vertex } weights ${ weights }` );

			}

		}

	} );

	registry.add( 'position', 'absolute-targets-transform-as-endpoints', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: false } );
		const matrix = makeAffineMatrix();
		const baked = bakeAffineGeometry( source, matrix );
		const mesh = new Mesh( baked );

		for ( const weights of WEIGHT_SETS ) {

			mesh.morphTargetInfluences[ 0 ] = weights[ 0 ];
			mesh.morphTargetInfluences[ 1 ] = weights[ 1 ];

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = referencePosition( source, matrix, weights, vertex );
				const actual = mesh.getVertexPosition( vertex, new Vector3() );
				check( vecClose( actual, expected, 1e-4 ), `absolute vertex ${ vertex } weights ${ weights }` );

			}

		}

	} );

	registry.add( 'position', 'translation-never-baked-into-relative-records', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		const matrix = new Matrix4().makeTranslation( 7, - 3, 2 );
		const baked = bakeAffineGeometry( source, matrix );

		const record = baked.morphAttributes.position[ 0 ];
		const sourceRecord = source.morphAttributes.position[ 0 ];

		for ( let i = 0; i < record.count; i ++ ) {

			check( close( record.getX( i ), sourceRecord.getX( i ) ), `morph x ${ i } untouched by translation` );
			check( close( record.getY( i ), sourceRecord.getY( i ) ), `morph y ${ i } untouched by translation` );
			check( close( record.getZ( i ), sourceRecord.getZ( i ) ), `morph z ${ i } untouched by translation` );

		}

		const base = baked.getAttribute( 'position' );
		check( close( base.getX( 0 ), POSITIONS[ 0 ] + 7 ), 'base position received translation x' );
		check( close( base.getY( 0 ), POSITIONS[ 1 ] - 3 ), 'base position received translation y' );
		check( close( base.getZ( 0 ), POSITIONS[ 2 ] + 2 ), 'base position received translation z' );

	} );

	registry.add( 'position', 'interleaved-input-bakes-correctly', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true, interleaved: true } );
		const matrix = makeAffineMatrix();
		const baked = bakeAffineGeometry( source, matrix );
		const mesh = new Mesh( baked );

		for ( const weights of [ WEIGHT_SETS[ 1 ], WEIGHT_SETS[ 3 ] ] ) {

			mesh.morphTargetInfluences[ 0 ] = weights[ 0 ];
			mesh.morphTargetInfluences[ 1 ] = weights[ 1 ];

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = referencePosition( source, matrix, weights, vertex );
				const actual = mesh.getVertexPosition( vertex, new Vector3() );
				check( vecClose( actual, expected, 1e-4 ), `interleaved vertex ${ vertex }` );

			}

		}

	} );

	// === normal =============================================================

	registry.add( 'normal', 'relative-indexed-normal-blend', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		const matrix = makeAffineMatrix();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		for ( const weights of WEIGHT_SETS ) {

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = referenceNormal( source, normalMatrix, weights, vertex );
				const actual = blendedBakedNormal( baked, weights, vertex );
				check( vecClose( actual, expected, 1e-4 ), `normal vertex ${ vertex } weights ${ weights }` );

			}

		}

	} );

	registry.add( 'normal', 'relative-non-indexed-normal-blend-negative-weights', ( { check } ) => {

		const source = buildGeometry( { indexed: false, relative: true } );
		const matrix = makeAffineMatrix();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		for ( const weights of [ WEIGHT_SETS[ 1 ], WEIGHT_SETS[ 2 ], WEIGHT_SETS[ 4 ] ] ) {

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = referenceNormal( source, normalMatrix, weights, vertex );
				const actual = blendedBakedNormal( baked, weights, vertex );
				check( vecClose( actual, expected, 1e-4 ), `non-indexed normal ${ vertex } weights ${ weights }` );

			}

		}

	} );

	registry.add( 'normal', 'absolute-normal-targets-follow-difference-rule', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: false } );
		const matrix = makeAffineMatrix();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		for ( const weights of WEIGHT_SETS ) {

			for ( let vertex = 0; vertex < 6; vertex ++ ) {

				const expected = referenceNormal( source, normalMatrix, weights, vertex );
				const actual = blendedBakedNormal( baked, weights, vertex );
				check( vecClose( actual, expected, 1e-4 ), `absolute normal ${ vertex } weights ${ weights }` );

			}

		}

	} );

	registry.add( 'normal', 'base-normals-unitized-but-morph-records-not', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		const matrix = makeAffineMatrix();
		const baked = bakeAffineGeometry( source, matrix );
		const normal = baked.getAttribute( 'normal' );

		for ( let i = 0; i < normal.count; i ++ ) {

			const length = Math.hypot( normal.getX( i ), normal.getY( i ), normal.getZ( i ) );
			check( close( length, 1, 1e-6 ), `base normal ${ i } is unit length` );

		}

		// Under non-uniform scale/shear the transformed morph records are generally
		// not unit length, which is required for correct linear blending.
		const record = baked.morphAttributes.normal[ 0 ];
		let foundNonUnit = false;

		for ( let i = 0; i < record.count; i ++ ) {

			const length = Math.hypot( record.getX( i ), record.getY( i ), record.getZ( i ) );
			if ( Math.abs( length - 1 ) > 1e-3 ) foundNonUnit = true;

		}

		check( foundNonUnit, 'morph normal records are allowed to be non-unit' );

	} );

	registry.add( 'normal', 'two-targets-point-in-different-directions', ( { check } ) => {

		const source = buildGeometry( { indexed: false, relative: true } );
		const a = new Vector3().fromBufferAttribute( source.morphAttributes.normal[ 0 ], 0 );
		const b = new Vector3().fromBufferAttribute( source.morphAttributes.normal[ 1 ], 0 );
		check( a.dot( b ) < 0.999, 'the two morph normal targets differ in direction' );

		const matrix = makeReflectionMatrix();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );
		const weights = [ - 0.75, 1.25 ];

		for ( let vertex = 0; vertex < 6; vertex ++ ) {

			const expected = referenceNormal( source, normalMatrix, weights, vertex );
			const actual = blendedBakedNormal( baked, weights, vertex );
			check( vecClose( actual, expected, 1e-4 ), `reflected normal ${ vertex }` );

		}

	} );

	// === records ============================================================

	registry.add( 'records', 'indexed-reflection-swaps-second-and-third-corner', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		const matrix = makeReflectionMatrix();
		check( matrix.determinant() < 0, 'fixture matrix really has negative determinant' );
		const baked = bakeAffineGeometry( source, matrix );
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );

		const sourceIndex = source.getIndex();
		const bakedIndex = baked.getIndex();

		for ( let face = 0; face < 2; face ++ ) {

			check( bakedIndex.getX( face * 3 ) === sourceIndex.getX( face * 3 ), `face ${ face } corner 0 kept` );
			check( bakedIndex.getX( face * 3 + 1 ) === sourceIndex.getX( face * 3 + 2 ), `face ${ face } corners 1/2 swapped` );
			check( bakedIndex.getX( face * 3 + 2 ) === sourceIndex.getX( face * 3 + 1 ), `face ${ face } corners 2/1 swapped` );

			// Winding is restored: the baked triangle's geometric normal points along
			// the inverse-transpose-transformed source normal instead of its opposite.

			const sourceFaceNormal = faceNormal( source, face );
			const expectedDirection = sourceFaceNormal.applyMatrix3( normalMatrix ).normalize();
			check( vecClose( faceNormal( baked, face ), expectedDirection, 1e-4 ), `face ${ face } keeps front facing` );

		}

	} );

	registry.add( 'records', 'non-indexed-reflection-swaps-every-attribute', ( { check } ) => {

		const source = buildGeometry( { indexed: false, relative: true, interleaved: true } );
		const matrix = makeReflectionMatrix();
		const normalMatrix = new Matrix3().getNormalMatrix( matrix );
		const baked = bakeAffineGeometry( source, matrix );

		const swapMap = ( vertex ) => {

			const face = Math.floor( vertex / 3 );
			const corner = vertex % 3;

			if ( corner === 1 ) return face * 3 + 2;
			if ( corner === 2 ) return face * 3 + 1;
			return vertex;

		};		const expectedPoint = new Vector3();
		const expectedDirection = new Vector3();

		for ( let vertex = 0; vertex < 6; vertex ++ ) {

			const from = swapMap( vertex );

			// Geometric attributes: transformed value of the corner that moved here.

			expectedPoint.fromBufferAttribute( source.getAttribute( 'position' ), from ).applyMatrix4( matrix );
			check(
				vecClose( new Vector3().fromBufferAttribute( baked.getAttribute( 'position' ), vertex ), expectedPoint, 1e-4 ),
				`position vertex ${ vertex } carries swapped source corner ${ from }`
			);

			expectedDirection.fromBufferAttribute( source.getAttribute( 'normal' ), from ).applyMatrix3( normalMatrix ).normalize();
			check(
				vecClose( new Vector3().fromBufferAttribute( baked.getAttribute( 'normal' ), vertex ), expectedDirection, 1e-4 ),
				`normal vertex ${ vertex } carries swapped source corner ${ from }`
			);

			// Non-geometric attributes: raw record of the swapped corner, including
			// the normalized interleaved integer attribute.

			const custom = baked.getAttribute( 'customNorm' );
			const sourceCustom = source.getAttribute( 'customNorm' );

			for ( let component = 0; component < 4; component ++ ) {

				check(
					close( custom.getComponent( vertex, component ), sourceCustom.getComponent( from, component ) ),
					`normalized custom attr vertex ${ vertex } swapped from ${ from } component ${ component }`
				);

			}

			for ( let t = 0; t < 2; t ++ ) {

				const bakedRecord = baked.morphAttributes.position[ t ];
				const sourceRecord = source.morphAttributes.position[ t ];
				expectedDirection.set(
					sourceRecord.getX( from ),
					sourceRecord.getY( from ),
					sourceRecord.getZ( from )
				).applyMatrix4( new Matrix4().copy( matrix ).setPosition( 0, 0, 0 ) );

				check(
					vecClose(
						new Vector3( bakedRecord.getX( vertex ), bakedRecord.getY( vertex ), bakedRecord.getZ( vertex ) ),
						expectedDirection,
						1e-4
					),
					`morph position record ${ t } vertex ${ vertex } swapped and transformed`
				);

			}

		}

		// Same winding check as the indexed case.

		for ( let face = 0; face < 2; face ++ ) {

			const expected = faceNormal( source, face ).applyMatrix3( normalMatrix ).normalize();
			check( vecClose( faceNormal( baked, face ), expected, 1e-4 ), `non-indexed face ${ face } keeps front facing` );

		}

	} );

	registry.add( 'records', 'groups-drawrange-and-triangle-order-preserved', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		source.clearGroups();
		source.addGroup( 0, 3, 0 );
		source.addGroup( 3, 3, 1 );
		source.setDrawRange( 1, 4 );
		const baked = bakeAffineGeometry( source, makeReflectionMatrix() );

		check( baked.groups.length === 2, 'both groups survive' );
		check( baked.groups[ 0 ].start === 0 && baked.groups[ 0 ].count === 3 && baked.groups[ 0 ].materialIndex === 0, 'group 0 unchanged' );
		check( baked.groups[ 1 ].start === 3 && baked.groups[ 1 ].count === 3 && baked.groups[ 1 ].materialIndex === 1, 'group 1 unchanged' );
		check( baked.drawRange.start === 1 && baked.drawRange.count === 4, 'draw range unchanged' );

		const sourceIndex = source.getIndex();
		const bakedIndex = baked.getIndex();
		check( bakedIndex.count === sourceIndex.count, 'triangle order/count unchanged' );

	} );

	registry.add( 'records', 'tangent-handedness-and-orthogonality', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		const reflection = makeReflectionMatrix();
		const bakedReflected = bakeAffineGeometry( source, reflection );
		const tangent = bakedReflected.getAttribute( 'tangent' );
		const normal = bakedReflected.getAttribute( 'normal' );

		for ( let i = 0; i < 6; i ++ ) {

			check( tangent.getW( i ) === - source.getAttribute( 'tangent' ).getW( i ), `tangent ${ i } w flips under reflection` );

			const t = new Vector3( tangent.getX( i ), tangent.getY( i ), tangent.getZ( i ) );
			const n = new Vector3( normal.getX( i ), normal.getY( i ), normal.getZ( i ) );
			check( close( t.length(), 1, 1e-6 ), `tangent ${ i } stays unit length` );
			check( close( t.dot( n ), 0, 1e-5 ), `tangent ${ i } stays orthogonal to its normal` );

		}

		const bakedDirect = bakeAffineGeometry( source, makeAffineMatrix() );
		let handednessKept = true;

		for ( let i = 0; i < 6; i ++ ) {

			if ( bakedDirect.getAttribute( 'tangent' ).getW( i ) !== source.getAttribute( 'tangent' ).getW( i ) ) handednessKept = false;

		}

		check( handednessKept, 'positive determinant keeps tangent handedness' );

		const noTangent = buildGeometry( { indexed: true, tangent: false } );
		const bakedNoTangent = bakeAffineGeometry( noTangent, makeAffineMatrix() );
		check( bakedNoTangent.getAttribute( 'tangent' ) === undefined, 'no tangent is generated when absent' );

	} );

	registry.add( 'records', 'bounds-rebuilt-or-kept-lazy', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		source.computeBoundingBox();
		source.computeBoundingSphere();
		const matrix = makeAffineMatrix();
		const baked = bakeAffineGeometry( source, matrix );

		check( baked.boundingBox !== null && baked.boundingSphere !== null, 'existing bounds are rebuilt' );

		const bakedPosition = baked.getAttribute( 'position' );

		// The rebuilt box must enclose every base position and every point reachable
		// by a single full-weight morph, all computed independently from the arrays.

		for ( let i = 0; i < bakedPosition.count; i ++ ) {

			const p = new Vector3().fromBufferAttribute( bakedPosition, i );
			check( baked.boundingBox.containsPoint( p ), `base position ${ i } inside rebuilt bounding box` );

			for ( const record of baked.morphAttributes.position ) {

				const morphed = p.clone().add( new Vector3().fromBufferAttribute( record, i ) );
				check( baked.boundingBox.containsPoint( morphed ), `morphed position ${ i } inside rebuilt bounding box` );

			}

		}

		// The sphere must enclose every baked base position and stay finite.

		check( Number.isFinite( baked.boundingSphere.radius ) && baked.boundingSphere.radius > 0, 'bounding sphere has a finite positive radius' );

		for ( let i = 0; i < bakedPosition.count; i ++ ) {

			const p = new Vector3().fromBufferAttribute( bakedPosition, i );
			check(
				p.distanceTo( baked.boundingSphere.center ) <= baked.boundingSphere.radius + 1e-3,
				`base position ${ i } inside the rebuilt bounding sphere`
			);

		}

		// Re-running the geometry's own computation entry points is stable.

		const boxBefore = baked.boundingBox.clone();
		const sphereBefore = baked.boundingSphere.clone();
		baked.computeBoundingBox();
		baked.computeBoundingSphere();
		check( vecClose( baked.boundingBox.min, boxBefore.min, 0 ), 'rebuilt bounding box is stable' );
		check( close( baked.boundingSphere.radius, sphereBefore.radius, 0 ), 'rebuilt bounding sphere is stable' );

		const lazy = buildGeometry( { indexed: true, relative: true } );
		const bakedLazy = bakeAffineGeometry( lazy, matrix );
		check( bakedLazy.boundingBox === null, 'absent bounding box stays lazy' );
		check( bakedLazy.boundingSphere === null, 'absent bounding sphere stays lazy' );

	} );

	registry.add( 'records', 'rejection-paths-throw', ( { check } ) => {

		const matrix = makeAffineMatrix();

		const missingPosition = new BufferGeometry();
		missingPosition.setAttribute( 'normal', new Float32BufferAttribute( NORMALS, 3 ) );
		check( assertThrow( () => bakeAffineGeometry( missingPosition, matrix ) ), 'missing position throws' );

		const mismatched = buildGeometry( { indexed: true } );
		mismatched.setAttribute( 'uv', new Float32BufferAttribute( [ 0, 0, 1, 1 ], 2 ) );
		check( assertThrow( () => bakeAffineGeometry( mismatched, matrix ) ), 'attribute length mismatch throws' );

		const illegalIndex = buildGeometry( { indexed: true } );
		illegalIndex.setIndex( [ 0, 1, 99 ] );
		check( assertThrow( () => bakeAffineGeometry( illegalIndex, matrix ) ), 'illegal triangle index throws' );

		const singular = new Matrix4().makeScale( 1, 0, 1 );
		check( assertThrow( () => bakeAffineGeometry( buildGeometry(), singular ) ), 'singular matrix throws' );

		const nonAffine = matrix.clone();
		nonAffine.elements[ 15 ] = 2;
		check( assertThrow( () => bakeAffineGeometry( buildGeometry(), nonAffine ) ), 'non-affine matrix throws' );

		const nonFinite = matrix.clone();
		nonFinite.elements[ 5 ] = Number.NaN;
		check( assertThrow( () => bakeAffineGeometry( buildGeometry(), nonFinite ) ), 'non-finite matrix throws' );

		const instanced = new InstancedBufferGeometry();
		instanced.setAttribute( 'position', new Float32BufferAttribute( POSITIONS, 3 ) );
		check( assertThrow( () => bakeAffineGeometry( instanced, matrix ) ), 'instanced geometry throws' );

		const instancedAttr = buildGeometry( { indexed: true } );
		instancedAttr.setAttribute( 'instance', new InstancedBufferAttribute( new Float32Array( 6 ), 1 ) );
		check( assertThrow( () => bakeAffineGeometry( instancedAttr, matrix ) ), 'instanced attribute throws' );

		const unsupportedMorph = buildGeometry( { indexed: true } );
		unsupportedMorph.morphAttributes.color = [ new Float32BufferAttribute( POSITIONS, 3 ) ];
		check( assertThrow( () => bakeAffineGeometry( unsupportedMorph, matrix ) ), 'unsupported morph kind throws' );

		const gl = buildGeometry( { indexed: true } );
		gl.setAttribute( 'position', new GLBufferAttribute( {}, 0x1406, 3, 4, 6 ) );
		check( assertThrow( () => bakeAffineGeometry( gl, matrix ) ), 'GLBufferAttribute throws' );

		const badSoup = new BufferGeometry();
		badSoup.setAttribute( 'position', new Float32BufferAttribute( [
			0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
		], 3 ) );
		check( assertThrow( () => bakeAffineGeometry( badSoup, matrix ) ), 'non-triangle non-indexed vertex count throws' );

	} );

	// === ownership ==========================================================

	registry.add( 'ownership', 'source-geometry-and-arrays-stay-unchanged', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true, interleaved: true } );
		const snapshots = {};

		for ( const name of Object.keys( source.attributes ) ) snapshots[ name ] = snapshot( source.attributes[ name ] );
		for ( const name of Object.keys( source.morphAttributes ) ) {

			snapshots[ `morph:${ name }` ] = source.morphAttributes[ name ].map( snapshot );

		}

		const indexSnapshot = Array.from( source.getIndex().array );
		bakeAffineGeometry( source, makeReflectionMatrix() );

		for ( const name of Object.keys( source.attributes ) ) {

			const now = snapshot( source.attributes[ name ] );
			check( now.length === snapshots[ name ].length && now.every( ( v, i ) => v === snapshots[ name ][ i ] ), `source attribute ${ name } unchanged` );

		}

		for ( const name of Object.keys( source.morphAttributes ) ) {

			source.morphAttributes[ name ].forEach( ( attribute, i ) => {

				const now = snapshot( attribute );
				const before = snapshots[ `morph:${ name }` ][ i ];
				check( now.every( ( v, j ) => v === before[ j ] ), `source morph ${ name }[${ i }] unchanged` );

			} );

		}

		check( Array.from( source.getIndex().array ).every( ( v, i ) => v === indexSnapshot[ i ] ), 'source index unchanged' );
		check( source.morphTargetsRelative === true, 'source morph flags unchanged' );

	} );

	registry.add( 'ownership', 'sibling-geometries-sharing-attributes-are-isolated', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: true } );
		const sibling = new BufferGeometry();
		sibling.setAttribute( 'position', source.getAttribute( 'position' ) );
		sibling.setAttribute( 'normal', source.getAttribute( 'normal' ) );
		const before = Array.from( source.getAttribute( 'position' ).array );

		const baked = bakeAffineGeometry( source, makeAffineMatrix() );
		baked.getAttribute( 'position' ).array[ 0 ] = 12345;

		check( Array.from( sibling.getAttribute( 'position' ).array ).every( ( v, i ) => v === before[ i ] ), 'sibling geometry keeps its shared data' );
		check( baked.getAttribute( 'position' ).array !== source.getAttribute( 'position' ).array, 'baked arrays are distinct allocations' );
		check( baked.morphAttributes.position[ 0 ].array !== source.morphAttributes.position[ 0 ].array, 'morph arrays are distinct allocations' );

	} );

	registry.add( 'ownership', 'matrix-is-not-modified', ( { check } ) => {

		const matrix = makeAffineMatrix();
		const before = Array.from( matrix.elements );
		bakeAffineGeometry( buildGeometry(), matrix );
		check( matrix.elements.every( ( v, i ) => v === before[ i ] ), 'matrix elements identical after bake' );

		const reflection = makeReflectionMatrix();
		const reflectionBefore = Array.from( reflection.elements );
		bakeAffineGeometry( buildGeometry(), reflection );
		check( reflection.elements.every( ( v, i ) => v === reflectionBefore[ i ] ), 'reflection matrix identical after bake' );

	} );

	registry.add( 'ownership', 'interleaved-input-is-deinterleaved-and-types-preserved', ( { check } ) => {

		const source = buildGeometry( { indexed: false, relative: true, interleaved: true } );
		const baked = bakeAffineGeometry( source, makeAffineMatrix() );

		for ( const name of [ 'position', 'normal', 'customNorm', 'customByte' ] ) {

			const attribute = baked.getAttribute( name );
			check( attribute.isInterleavedBufferAttribute !== true, `${ name } is plain after baking` );
			check( attribute.isBufferAttribute === true, `${ name } stays a BufferAttribute` );

		}

		const customNorm = baked.getAttribute( 'customNorm' );
		check( customNorm.array instanceof Uint8Array, 'custom attribute keeps its typed array type' );
		check( customNorm.itemSize === 4, 'custom attribute keeps itemSize' );
		check( customNorm.normalized === true, 'custom attribute keeps normalized flag' );

		const sourceCustom = source.getAttribute( 'customNorm' );

		for ( let i = 0; i < 6; i ++ ) {

			for ( let c = 0; c < 4; c ++ ) {

				check( close( customNorm.getComponent( i, c ), sourceCustom.getComponent( i, c ) ), `normalized value preserved ${ i }/${ c }` );

			}

		}

		const position = baked.getAttribute( 'position' );
		check( position.array instanceof Float32Array, 'position output is Float32' );

	} );

	registry.add( 'ownership', 'passthrough-attributes-keep-metadata', ( { check } ) => {

		const source = buildGeometry( { indexed: true, relative: false } );
		const baked = bakeAffineGeometry( source, makeAffineMatrix() );

		const uv = baked.getAttribute( 'uv' );
		const color = baked.getAttribute( 'color' );
		check( uv.array instanceof Float32Array && uv.itemSize === 2 && uv.normalized === false, 'uv type/itemSize/normalized preserved' );
		check( color.array instanceof Uint8Array && color.itemSize === 3 && color.normalized === true, 'color type/itemSize/normalized preserved' );

		for ( let i = 0; i < 6; i ++ ) {

			for ( let c = 0; c < 2; c ++ ) {

				check( close( uv.getComponent( i, c ), source.getAttribute( 'uv' ).getComponent( i, c ) ), `uv value ${ i }/${ c } preserved` );

			}

			for ( let c = 0; c < 3; c ++ ) {

				check( close( color.getComponent( i, c ), source.getAttribute( 'color' ).getComponent( i, c ) ), `color value ${ i }/${ c } preserved` );

			}

		}

		check( baked.morphTargetsRelative === false, 'absolute morph flag carried over' );
		check( baked.name === source.name, 'geometry name carried over' );

	} );

}

function assertThrow( fn ) {

	try {

		fn();
		return false;

	} catch {

		return true;

	}

}

export { registerCases };
