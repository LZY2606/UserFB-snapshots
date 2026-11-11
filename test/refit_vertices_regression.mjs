import assert from 'node:assert/strict';
import {
	BufferAttribute,
	BufferGeometry,
	DoubleSide,
	InterleavedBuffer,
	InterleavedBufferAttribute,
	Mesh,
	MeshBasicMaterial,
	Raycaster,
	Vector3,
	Box3,
} from 'three';
import { MeshBVH } from '../src/core/MeshBVH.js';

// ---------------------------------------------------------------------------
// geometry fixtures
// ---------------------------------------------------------------------------
//
// Vertex grid of 3 rows x 6 columns (asymmetric positions) forming 2 x 5 quads,
// plus one dangling vertex referenced by no triangle.
//
//   row 0 quads (cols 0..4) -> group 0 (indices 0..29)
//   row 1 quads (cols 0..2) -> group 1 (indices 30..47)
//   row 1 quads (cols 3..4) -> gap between groups, excluded from the BVH
//
// Middle-row vertices (cols 0..3) are shared between the two material groups.
// Triangle order is scrambled within each group range so the BVH build must
// reorder triangles and the indirect buffer is not an identity permutation.

const COLS = 6;
const ROWS = 3;
const DANGLING = ROWS * COLS; // vertex 18, referenced by no triangle
const VERT_COUNT = ROWS * COLS + 1;
const SHARED_VERTEX = 1 * COLS + 2; // vertex 8: used by triangles of both groups
const GAP_ONLY_VERTEX = 2 * COLS + 5; // vertex 17: only used by gap triangles
const GROUP1_ONLY_VERTEX = 2 * COLS + 0; // vertex 12: only used by group 1 triangles

function vid( r, c ) {

	return r * COLS + c;

}

function makeGeometry() {

	const pos = new Float32Array( VERT_COUNT * 3 );
	const uv = new Float32Array( VERT_COUNT * 2 );
	for ( let r = 0; r < ROWS; r ++ ) {

		for ( let c = 0; c < COLS; c ++ ) {

			const i = vid( r, c );
			pos[ i * 3 + 0 ] = c * 1.7 + ( c % 2 ) * 0.31;
			pos[ i * 3 + 1 ] = r * 1.3 + ( ( r + c ) % 2 ) * 0.17;
			pos[ i * 3 + 2 ] = ( ( r * 7 + c * 3 ) % 5 ) * 0.11;
			uv[ i * 2 + 0 ] = c / ( COLS - 1 );
			uv[ i * 2 + 1 ] = r / ( ROWS - 1 );

		}

	}

	// dangling vertex, far away and referenced by no triangle
	pos[ DANGLING * 3 + 0 ] = 100;
	pos[ DANGLING * 3 + 1 ] = 100;
	pos[ DANGLING * 3 + 2 ] = 100;

	const group0Quads = [ 0, 1, 2, 3, 4 ].map( c => [ 0, c ] );
	const group1Quads = [ 0, 1, 2 ].map( c => [ 1, c ] );
	const gapQuads = [ 3, 4 ].map( c => [ 1, c ] );

	// scramble the quad order within each group range
	const ordered = [
		...[ 2, 0, 4, 1, 3 ].map( i => group0Quads[ i ] ),
		...[ 1, 2, 0 ].map( i => group1Quads[ i ] ),
		...[ 1, 0 ].map( i => gapQuads[ i ] ),
	];

	const index = [];
	for ( const [ r, c ] of ordered ) {

		const a = vid( r, c );
		const b = vid( r, c + 1 );
		const d = vid( r + 1, c + 1 );
		const e = vid( r + 1, c );
		index.push( a, b, d, a, d, e );

	}

	const g = new BufferGeometry();
	g.setAttribute( 'position', new BufferAttribute( pos, 3 ) );
	g.setAttribute( 'uv', new BufferAttribute( uv, 2 ) );
	g.setIndex( index );
	g.addGroup( 0, 30, 0 );
	g.addGroup( 30, 18, 1 );
	return g;

}

function makeInterleavedGeometry() {

	const base = makeGeometry();
	const src = base.attributes.position;
	const stride = 4;
	const offset = 1;
	const interleaved = new InterleavedBuffer( new Float32Array( src.count * stride ), stride );
	for ( let i = 0; i < src.count; i ++ ) {

		interleaved.array[ i * stride + offset + 0 ] = src.getX( i );
		interleaved.array[ i * stride + offset + 1 ] = src.getY( i );
		interleaved.array[ i * stride + offset + 2 ] = src.getZ( i );

	}

	const g = new BufferGeometry();
	g.setAttribute( 'position', new InterleavedBufferAttribute( interleaved, 3, offset ) );
	g.setAttribute( 'uv', base.attributes.uv.clone() );
	g.setIndex( new BufferAttribute( base.index.array.slice(), 1 ) );
	g.addGroup( 0, 30, 0 );
	g.addGroup( 30, 18, 1 );
	return g;

}

function makeNonIndexedGeometry() {

	const triCount = 6;
	const pos = new Float32Array( triCount * 9 );
	for ( let t = 0; t < triCount; t ++ ) {

		const o = t * 9;
		pos[ o + 0 ] = t * 1.3; pos[ o + 1 ] = 0; pos[ o + 2 ] = ( t % 3 ) * 0.2;
		pos[ o + 3 ] = t * 1.3 + 1.1; pos[ o + 4 ] = 0.2; pos[ o + 5 ] = 0;
		pos[ o + 6 ] = t * 1.3 + 0.4; pos[ o + 7 ] = 1.2; pos[ o + 8 ] = ( t % 2 ) * 0.3;

	}

	const g = new BufferGeometry();
	g.setAttribute( 'position', new BufferAttribute( pos, 3 ) );
	return g;

}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

function triangleVertices( g, tri ) {

	const index = g.index ? g.index.array : null;
	const result = [];
	for ( let j = 0; j < 3; j ++ ) {

		result.push( index ? index[ tri * 3 + j ] : tri * 3 + j );

	}

	return result;

}

// number of BVH leaves containing at least one triangle that references the vertex
function leavesForVertex( bvh, vertex ) {

	const g = bvh.geometry;
	const indirect = bvh._indirectBuffer;
	let count = 0;
	for ( let root = 0; root < bvh._roots.length; root ++ ) {

		bvh.traverse( ( depth, isLeaf, bounds, offset, cnt ) => {

			if ( ! isLeaf ) return;
			for ( let i = offset; i < offset + cnt; i ++ ) {

				const tri = indirect ? indirect[ i ] : i;
				if ( triangleVertices( g, tri ).includes( vertex ) ) {

					count ++;
					break;

				}

			}

		}, root );

	}

	return count;

}

function totalLeafCount( bvh ) {

	let count = 0;
	for ( let root = 0; root < bvh._roots.length; root ++ ) {

		bvh.traverse( ( depth, isLeaf ) => {

			if ( isLeaf ) count ++;

		}, root );

	}

	return count;

}

function assertSameBounds( bvhA, bvhB, label ) {

	assert.equal( bvhA._roots.length, bvhB._roots.length, label );
	for ( let i = 0; i < bvhA._roots.length; i ++ ) {

		const a = new Uint32Array( bvhA._roots[ i ] );
		const b = new Uint32Array( bvhB._roots[ i ] );
		assert.equal( a.length, b.length, `${ label } root ${ i }` );
		for ( let j = 0; j < a.length; j ++ ) {

			assert.equal( a[ j ], b[ j ], `${ label } root ${ i } word ${ j }` );

		}

	}

}

function findTriangleWithVertex( g, vertex ) {

	const triCount = ( g.index ? g.index.count : g.attributes.position.count ) / 3;
	for ( let t = 0; t < triCount; t ++ ) {

		if ( triangleVertices( g, t ).includes( vertex ) ) return t;

	}

	return - 1;

}

function triangleCentroid( g, tri ) {

	const pos = g.attributes.position;
	const c = new Vector3();
	for ( const v of triangleVertices( g, tri ) ) {

		c.x += pos.getX( v ) / 3;
		c.y += pos.getY( v ) / 3;
		c.z += pos.getZ( v ) / 3;

	}

	return c;

}

// compare the BVH against the unmodified three.js Mesh.raycast ground truth
function assertRaycastMatches( bvh, g, origin, label ) {

	const mesh = new Mesh( g, new MeshBasicMaterial( { side: DoubleSide } ) );
	mesh.updateMatrixWorld( true );
	const raycaster = new Raycaster( origin, new Vector3( 0, 0, - 1 ) );
	const expected = raycaster.intersectObject( mesh )[ 0 ];
	const hit = bvh.raycastFirst( raycaster.ray, DoubleSide );
	assert.ok( expected, `${ label }: three.js raycast should hit` );
	assert.ok( hit, `${ label }: bvh raycastFirst should hit` );
	assert.ok( Math.abs( hit.distance - expected.distance ) < 1e-7, `${ label }: distance` );
	assert.equal( hit.faceIndex, expected.faceIndex, `${ label }: faceIndex` );
	assert.ok( hit.uv.distanceTo( expected.uv ) < 1e-7, `${ label }: uv` );
	return expected;

}

// ---------------------------------------------------------------------------
// section A: shared vertex across two material groups, bounds and raycast parity
// ---------------------------------------------------------------------------
for ( const indirect of [ false, true ] ) {

	const options = { indirect, targetLeafSize: 1 };
	const g = makeGeometry();
	const bvh = new MeshBVH( g, options );
	const g2 = makeGeometry();
	const bvh2 = new MeshBVH( g2, options );

	// direct mode produces one root per group range; indirect mode produces a
	// single root over the compacted indirect buffer
	assert.equal( bvh._roots.length, indirect ? 1 : 2, 'unexpected root count' );

	if ( indirect ) {

		const buffer = bvh._indirectBuffer;
		let identity = true;
		for ( let i = 0; i < buffer.length; i ++ ) {

			if ( buffer[ i ] !== i ) {

				identity = false;
				break;

			}

		}

		assert.ok( ! identity, 'indirect triangle order should not be the identity permutation' );

	}

	// normalize both trees with a full refit so build-time epsilon padding on leaf
	// bounds is gone and the sparse update can be compared exactly
	bvh.refit();
	bvh2.refit();

	// snapshot render-side data that refitVertices must not rewrite
	const uvBefore = g.attributes.uv.array.slice();
	const indexBefore = g.index.array.slice();
	const groupsBefore = JSON.stringify( g.groups );

	// move the shared vertex out of its old bounds in both geometries
	const sv = SHARED_VERTEX;
	const newX = g.attributes.position.getX( sv ) + 5;
	const newZ = g.attributes.position.getZ( sv ) + 5;
	g.attributes.position.setX( sv, newX );
	g.attributes.position.setZ( sv, newZ );
	g2.attributes.position.setX( sv, newX );
	g2.attributes.position.setZ( sv, newZ );

	const expectedLeaves = leavesForVertex( bvh, sv );
	assert.ok( expectedLeaves > 1, 'shared vertex should map to multiple leaves' );

	const updated = bvh.refitVertices( [ sv ] );
	assert.equal( updated, expectedLeaves, 'return value must be the deduplicated leaf count' );

	// every BVH node bound must match a full refit
	bvh2.refit();
	assertSameBounds( bvh, bvh2, `indirect=${ indirect } refitVertices vs refit` );

	// render-side data untouched
	assert.deepStrictEqual( g.attributes.uv.array, uvBefore );
	assert.deepStrictEqual( g.index.array, indexBefore );
	assert.equal( JSON.stringify( g.groups ), groupsBefore );

	// raycast against the moved surface: aim at the centroid of a triangle that
	// references the moved vertex. The centroid lies outside the stale leaf bounds
	// so the hit is only found if the bounds were actually updated.
	const tri = findTriangleWithVertex( g, sv );
	const centroid = triangleCentroid( g, tri );
	const expected = assertRaycastMatches( bvh, g, new Vector3( centroid.x, centroid.y, 30 ), `indirect=${ indirect }` );
	assert.ok( triangleVertices( g, expected.faceIndex ).includes( sv ), 'hit triangle should reference the moved vertex' );
	assert.ok( expected.point.z > 1, 'hit should be on the moved surface' );

	console.log( `shared vertex refit PASS indirect=${ indirect } leaves=${ updated }/${ totalLeafCount( bvh ) }` );

}

// ---------------------------------------------------------------------------
// section B: consecutive sparse updates must not read unrelated vertex positions
// ---------------------------------------------------------------------------
for ( const indirect of [ false, true ] ) {

	const options = { indirect, targetLeafSize: 1 };
	const g = makeGeometry();
	const bvh = new MeshBVH( g, options );
	const pos = g.attributes.position;

	// first move: builds the dependency map
	pos.setZ( 0, 5 );
	const n1 = bvh.refitVertices( [ 0 ] );
	assert.equal( n1, leavesForVertex( bvh, 0 ) );
	assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 5 ) < 1e-6 );

	// second move: count position reads
	pos.setZ( 1, 6 );
	const reads = new Set();
	let getXCalls = 0;
	let getYCalls = 0;
	let getZCalls = 0;
	const originalGetX = pos.getX;
	const originalGetY = pos.getY;
	const originalGetZ = pos.getZ;
	pos.getX = function ( i ) { getXCalls ++; reads.add( i ); return originalGetX.call( this, i ); };
	pos.getY = function ( i ) { getYCalls ++; reads.add( i ); return originalGetY.call( this, i ); };
	pos.getZ = function ( i ) { getZCalls ++; reads.add( i ); return originalGetZ.call( this, i ); };

	let n2;
	try {

		n2 = bvh.refitVertices( [ 1 ] );

	} finally {

		pos.getX = originalGetX;
		pos.getY = originalGetY;
		pos.getZ = originalGetZ;

	}

	assert.equal( n2, leavesForVertex( bvh, 1 ) );
	assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 6 ) < 1e-6 );

	// vertex 1 is only referenced by the two quads at row 0, cols 0..1, so only the
	// vertices of those four triangles may be read
	const allowed = new Set( [ 0, 1, 2, 6, 7, 8 ] );
	assert.ok( reads.size > 0, 'expected some position reads' );
	for ( const r of reads ) {

		assert.ok( allowed.has( r ), `vertex ${ r } lives only in unrelated leaves and must not be read` );

	}

	assert.ok( ! reads.has( GROUP1_ONLY_VERTEX ), 'group 1 vertex must not be read' );
	assert.ok( ! reads.has( GAP_ONLY_VERTEX ), 'gap vertex must not be read' );
	assert.ok( ! reads.has( DANGLING ), 'dangling vertex must not be read' );
	assert.equal( getXCalls, getYCalls );
	assert.equal( getYCalls, getZCalls );
	assert.ok( getXCalls <= 3 * 3 * n2, 'reads must scale with the affected leaves' );

	console.log( `sparse position reads PASS indirect=${ indirect } getXCalls=${ getXCalls } verticesRead=${ reads.size }` );

}

// ---------------------------------------------------------------------------
// section C: duplicates, empty input, uncovered vertices, generators, bad input
// ---------------------------------------------------------------------------
for ( const indirect of [ false, true ] ) {

	const options = { indirect, targetLeafSize: 1 };
	const g = makeGeometry();
	const bvh = new MeshBVH( g, options );
	const pos = g.attributes.position;

	// empty and uncovered inputs
	assert.equal( bvh.refitVertices( [] ), 0, 'empty input' );
	assert.equal( bvh.refitVertices( [ DANGLING ] ), 0, 'vertex referenced by no triangle' );
	assert.equal( bvh.refitVertices( [ GAP_ONLY_VERTEX ] ), 0, 'vertex only used by gap triangles' );

	// duplicate vertices are processed once
	pos.setZ( SHARED_VERTEX, 2.5 );
	const expectedLeaves = leavesForVertex( bvh, SHARED_VERTEX );
	assert.equal( bvh.refitVertices( [ SHARED_VERTEX, SHARED_VERTEX, SHARED_VERTEX ] ), expectedLeaves );

	// one-shot generators are consumed exactly once
	pos.setZ( SHARED_VERTEX, 3.5 );
	function* oneShot() {

		yield SHARED_VERTEX;

	}

	assert.equal( bvh.refitVertices( oneShot() ), expectedLeaves );
	assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 3.5 ) < 1e-6 );

	// invalid indices are rejected with RangeError
	const count = pos.count;
	for ( const bad of [ - 1, count, count + 100, 1.5, NaN, Infinity, - Infinity, '3', null, undefined ] ) {

		assert.throws( () => bvh.refitVertices( [ bad ] ), RangeError, `bad index ${ bad }` );

	}

	// a failed validation must not leave partially updated nodes behind
	const boxBefore = bvh.getBoundingBox( new Box3() );
	pos.setZ( SHARED_VERTEX, 9 );
	assert.throws( () => bvh.refitVertices( [ SHARED_VERTEX, - 1 ] ), RangeError );
	assert.ok( bvh.getBoundingBox( new Box3() ).equals( boxBefore ), 'bounds must be untouched after RangeError' );
	assert.equal( bvh.refitVertices( [ SHARED_VERTEX ] ), expectedLeaves );
	assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 9 ) < 1e-6 );

	console.log( `input validation PASS indirect=${ indirect }` );

}

// ---------------------------------------------------------------------------
// section D: index version / replacement resync and deserialize
// ---------------------------------------------------------------------------
for ( const indirect of [ false, true ] ) {

	const options = { indirect, targetLeafSize: 1 };
	const g = makeGeometry();
	const bvh = new MeshBVH( g, options );

	// establish the dependency map, then change connectivity in place
	bvh.refitVertices( [ 0 ] );
	const indexAttr = g.index;
	indexAttr.array[ 0 ] = ( indexAttr.array[ 0 ] + 5 ) % 18;
	indexAttr.needsUpdate = true;

	// empty input still triggers the resync and returns all recomputed leaves
	const totalLeaves = totalLeafCount( bvh );
	assert.equal( bvh.refitVertices( [] ), totalLeaves, 'index version bump must trigger a full resync' );
	assertRaycastMatches( bvh, g, new Vector3( 3, 1, 30 ), `indirect=${ indirect } after version bump` );

	// sparse updates resume afterwards
	const pos = g.attributes.position;
	pos.setZ( 3, 4 );
	const sparseCount = bvh.refitVertices( [ 3 ] );
	assert.equal( sparseCount, leavesForVertex( bvh, 3 ) );
	assert.ok( sparseCount < totalLeaves, 'sparse update should resume after resync' );
	assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 4 ) < 1e-6 );

	// replacing the index attribute (same length) also triggers a resync
	const newIndex = indexAttr.array.slice();
	newIndex[ 3 ] = ( newIndex[ 3 ] + 7 ) % 18;
	g.setIndex( new BufferAttribute( newIndex, 1 ) );
	assert.equal( bvh.refitVertices( [] ), totalLeaves, 'index replacement must trigger a full resync' );
	assertRaycastMatches( bvh, g, new Vector3( 6, 0.5, 30 ), `indirect=${ indirect } after index replacement` );

	// a deserialized BVH supports refitVertices on the first call
	const data = MeshBVH.serialize( bvh );
	const gDeser = makeGeometry();
	const bvhDeser = MeshBVH.deserialize( data, gDeser );

	// serialized data does not include vertex positions so sync them explicitly
	gDeser.attributes.position.array.set( g.attributes.position.array );
	pos.setZ( 4, 1.5 );
	gDeser.attributes.position.setZ( 4, 1.5 );
	const countLive = bvh.refitVertices( [ 4 ] );
	const countDeser = bvhDeser.refitVertices( [ 4 ] );
	assert.equal( countDeser, countLive );
	assertSameBounds( bvh, bvhDeser, `indirect=${ indirect } deserialized` );

	console.log( `index resync + deserialize PASS indirect=${ indirect } totalLeaves=${ totalLeaves }` );

}

// ---------------------------------------------------------------------------
// section E: topology changes require a BVH rebuild
// ---------------------------------------------------------------------------
for ( const indirect of [ false, true ] ) {

	const options = { indirect, targetLeafSize: 1 };

	// drawRange change
	{

		const g = makeGeometry();
		const bvh = new MeshBVH( g, options );
		bvh.refitVertices( [ 0 ] );
		g.setDrawRange( 0, 30 );
		assert.throws( () => bvh.refitVertices( [ 0 ] ), /rebuilt/, 'drawRange change' );

	}

	// group change
	{

		const g = makeGeometry();
		const bvh = new MeshBVH( g, options );
		bvh.refitVertices( [ 0 ] );
		g.addGroup( 48, 12, 2 );
		assert.throws( () => bvh.refitVertices( [ 0 ] ), /rebuilt/, 'group change' );

	}

	// vertex count change
	{

		const g = makeGeometry();
		const bvh = new MeshBVH( g, options );
		bvh.refitVertices( [ 0 ] );
		g.setAttribute( 'position', new BufferAttribute( new Float32Array( ( VERT_COUNT + 3 ) * 3 ), 3 ) );
		assert.throws( () => bvh.refitVertices( [ 0 ] ), /rebuilt/, 'vertex count change' );

	}

	// index removal
	{

		const g = makeGeometry();
		const bvh = new MeshBVH( g, options );
		bvh.refitVertices( [ 0 ] );
		g.setIndex( null );
		assert.throws( () => bvh.refitVertices( [ 0 ] ), /rebuilt/, 'index removal' );

	}

	// index length change
	{

		const g = makeGeometry();
		const bvh = new MeshBVH( g, options );
		bvh.refitVertices( [ 0 ] );
		g.setIndex( new BufferAttribute( new Uint32Array( 30 ), 1 ) );
		assert.throws( () => bvh.refitVertices( [ 0 ] ), /rebuilt/, 'index length change' );

	}

	// replacing the position attribute with one of the same length is allowed
	{

		const g = makeGeometry();
		const bvh = new MeshBVH( g, options );
		bvh.refitVertices( [ 0 ] );
		const replacement = g.attributes.position.clone();
		replacement.setZ( 2, 1.25 );
		g.setAttribute( 'position', replacement );
		assert.equal( bvh.refitVertices( [ 2 ] ), leavesForVertex( bvh, 2 ) );
		assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 1.25 ) < 1e-6 );

	}

	console.log( `rebuild detection PASS indirect=${ indirect }` );

}

// ---------------------------------------------------------------------------
// section F: interleaved position attribute
// ---------------------------------------------------------------------------
for ( const indirect of [ false, true ] ) {

	const options = { indirect, targetLeafSize: 1 };
	const g = makeInterleavedGeometry();
	const bvh = new MeshBVH( g, options );
	const g2 = makeInterleavedGeometry();
	const bvh2 = new MeshBVH( g2, options );

	bvh.refit();
	bvh2.refit();

	const sv = SHARED_VERTEX;
	g.attributes.position.setZ( sv, 4.5 );
	g2.attributes.position.setZ( sv, 4.5 );

	const updated = bvh.refitVertices( [ sv ] );
	assert.equal( updated, leavesForVertex( bvh, sv ) );
	bvh2.refit();
	assertSameBounds( bvh, bvh2, `interleaved indirect=${ indirect }` );
	assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 4.5 ) < 1e-6 );

	console.log( `interleaved position PASS indirect=${ indirect }` );

}

// ---------------------------------------------------------------------------
// section G: indirect mode with non-indexed geometry
// ---------------------------------------------------------------------------
{

	const g = makeNonIndexedGeometry();
	const bvh = new MeshBVH( g, { indirect: true, targetLeafSize: 1 } );
	const g2 = makeNonIndexedGeometry();
	const bvh2 = new MeshBVH( g2, { indirect: true, targetLeafSize: 1 } );

	assert.equal( g.index, null );

	bvh.refit();
	bvh2.refit();

	g.attributes.position.setZ( 4, 3 );
	g2.attributes.position.setZ( 4, 3 );

	const updated = bvh.refitVertices( [ 4 ] );
	assert.equal( updated, leavesForVertex( bvh, 4 ) );
	bvh2.refit();
	assertSameBounds( bvh, bvh2, 'indirect non-indexed' );
	assert.ok( Math.abs( bvh.getBoundingBox( new Box3() ).max.z - 3 ) < 1e-6 );

	// adding an index afterwards is a topology change
	g.setIndex( new BufferAttribute( new Uint32Array( 18 ), 1 ) );
	assert.throws( () => bvh.refitVertices( [ 0 ] ), /rebuilt/, 'index addition' );

	console.log( 'indirect non-indexed PASS' );

}

console.log( 'vertex refit PASS' );
