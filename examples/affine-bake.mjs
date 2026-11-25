// Standalone Node example for BufferGeometryUtils.bakeAffineGeometry().
//
// Usage: node --experimental-loader ./support/loader.mjs examples/affine-bake.mjs <output-directory>
//
// Builds a small indexed triangle mesh with relative position/normal morph
// targets, bakes a non-uniform affine transform (with translation, shear and a
// negative determinant) into it, writes the baked base mesh as Wavefront OBJ
// and prints geometry statistics. Only Node built-in modules are used.

import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import {
	BufferGeometry,
	Float32BufferAttribute,
	Matrix4,
	Vector3,
	Mesh,
} from 'three';
import { bakeAffineGeometry } from './jsm/utils/BufferGeometryUtils.js';

const outputDir = process.argv[ 2 ];

if ( outputDir === undefined ) {

	console.error( 'usage: affine-bake.mjs <output-directory>' );
	process.exit( 2 );

}

// --- build the source geometry ---------------------------------------------

const geometry = new BufferGeometry();

geometry.setAttribute( 'position', new Float32BufferAttribute( [
	0, 0, 0,
	1, 0, 0,
	1, 1, 0,
	0, 1, 0,
	0, 0, 1,
	1, 0, 1,
], 3 ) );

geometry.setAttribute( 'normal', new Float32BufferAttribute( [
	0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1,
	0, 1, 0, 0, 1, 0,
], 3 ) );

geometry.setAttribute( 'uv', new Float32BufferAttribute( [
	0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0,
], 2 ) );

geometry.setIndex( [ 0, 1, 2, 0, 2, 3, 4, 5, 1, 4, 1, 0 ] );

geometry.morphTargetsRelative = true;

geometry.morphAttributes.position = [
	new Float32BufferAttribute( [
		0.1, 0, 0, 0.1, 0, 0, 0.1, 0.2, 0, 0.1, 0.2, 0,
		0, 0, 0.3, 0, 0, 0.3,
	], 3 ),
	new Float32BufferAttribute( [
		0, 0.2, 0.1, 0, 0.2, 0.1, 0, 0, 0.1, 0, 0, 0.1,
		0.2, 0.1, 0, 0.2, 0.1, 0,
	], 3 ),
];

geometry.morphAttributes.normal = [
	new Float32BufferAttribute( [
		0.2, 0, 1, 0.2, 0, 1, 0.2, 0, 1, 0.2, 0, 1,
		0, 1, 0.2, 0, 1, 0.2,
	], 3 ),
];

// Affine matrix with non-uniform scale, shear, translation and a negative
// determinant so the reflection corner swap is exercised.

const matrix = new Matrix4().set(
	- 2.0, 0.4, 0.1, 5.0,
	0.0, 1.5, - 0.3, - 1.0,
	0.5, 0.2, 0.8, 2.0,
	0, 0, 0, 1
);

const baked = bakeAffineGeometry( geometry, matrix );

// Sanity check through the same runtime path a Mesh would use.

const sourceMesh = new Mesh( geometry );
const bakedMesh = new Mesh( baked );
const weights = [ 0.75, - 0.5 ];
sourceMesh.morphTargetInfluences[ 0 ] = weights[ 0 ];
sourceMesh.morphTargetInfluences[ 1 ] = weights[ 1 ];
bakedMesh.morphTargetInfluences[ 0 ] = weights[ 0 ];
bakedMesh.morphTargetInfluences[ 1 ] = weights[ 1 ];

let maxPositionError = 0;

for ( let i = 0; i < geometry.getAttribute( 'position' ).count; i ++ ) {

	const expected = sourceMesh.getVertexPosition( i, new Vector3() ).applyMatrix4( matrix );
	const actual = bakedMesh.getVertexPosition( i, new Vector3() );
	maxPositionError = Math.max( maxPositionError, expected.distanceTo( actual ) );

}

// --- minimal Wavefront OBJ writer (baked base mesh) ------------------------

function writeOBJ( geom ) {

	const position = geom.getAttribute( 'position' );
	const normal = geom.getAttribute( 'normal' );
	const uv = geom.getAttribute( 'uv' );
	const lines = [ '# baked with BufferGeometryUtils.bakeAffineGeometry()' ];

	for ( let i = 0; i < position.count; i ++ ) {

		lines.push( `v ${ position.getX( i ) } ${ position.getY( i ) } ${ position.getZ( i ) }` );

	}

	if ( uv !== undefined ) {

		for ( let i = 0; i < uv.count; i ++ ) {

			lines.push( `vt ${ uv.getX( i ) } ${ uv.getY( i ) }` );

		}

	}

	if ( normal !== undefined ) {

		for ( let i = 0; i < normal.count; i ++ ) {

			lines.push( `vn ${ normal.getX( i ) } ${ normal.getY( i ) } ${ normal.getZ( i ) }` );

		}

	}

	const faceCount = geom.getIndex() !== null
		? geom.getIndex().count / 3
		: position.count / 3;

	for ( let face = 0; face < faceCount; face ++ ) {

		const corners = [];

		for ( let corner = 0; corner < 3; corner ++ ) {

			const vertex = geom.getIndex() !== null
				? geom.getIndex().getX( face * 3 + corner )
				: face * 3 + corner;

			// OBJ indices are 1-based; write v/vt/vn triplets when available.

			const v = vertex + 1;
			const t = uv !== undefined ? v : '';
			const n = normal !== undefined ? v : '';
			corners.push( [ v, t, n ].filter( part => part !== '' ).join( '/' ) );

		}

		lines.push( `f ${ corners.join( ' ' ) }` );

	}

	return lines.join( '\n' ) + '\n';

}

// --- statistics -------------------------------------------------------------

baked.computeBoundingBox();
baked.computeBoundingSphere();

const statistics = {
	determinant: matrix.determinant(),
	reflected: matrix.determinant() < 0,
	indexed: baked.getIndex() !== null,
	vertexCount: baked.getAttribute( 'position' ).count,
	triangleCount: baked.getIndex() !== null
		? baked.getIndex().count / 3
		: baked.getAttribute( 'position' ).count / 3,
	attributes: Object.keys( baked.attributes ),
	morphTargets: Object.fromEntries(
		Object.entries( baked.morphAttributes ).map( ( [ name, list ] ) => [ name, list.length ] )
	),
	morphTargetsRelative: baked.morphTargetsRelative,
	groups: baked.groups.length,
	maxMorphPositionError: maxPositionError,
	boundingBox: {
		min: baked.boundingBox.min.toArray(),
		max: baked.boundingBox.max.toArray(),
	},
	boundingSphere: {
		center: baked.boundingSphere.center.toArray(),
		radius: baked.boundingSphere.radius,
	},
};

mkdirSync( outputDir, { recursive: true } );
writeFileSync( join( outputDir, 'result.obj' ), writeOBJ( baked ) );

console.log( JSON.stringify( statistics, null, 2 ) );
console.log( `wrote ${ join( outputDir, 'result.obj' ) }` );
