import { runGroup, check } from '../task_support/check.mjs';
import {
	SceneRaycastIndex,
	Object3D,
	Group,
	Mesh,
	BufferGeometry,
	Float32BufferAttribute,
	MeshBasicMaterial,
	Raycaster,
	Vector3,
	DoubleSide,
	FrontSide,
	BackSide,
	Points,
	Line,
	SkinnedMesh,
	InstancedMesh
} from '../src/Three.Core.js';

// --- helpers ---------------------------------------------------------------

function triGeometry( s = 1 ) {

	const g = new BufferGeometry();
	g.setAttribute( 'position', new Float32BufferAttribute( [ - s, - s, 0, s, - s, 0, 0, s, 0 ], 3 ) );
	return g;

}

function quadGeometry() {

	// two triangles sharing the diagonal from (0,0,0) to (1,1,0)
	const g = new BufferGeometry();
	g.setAttribute( 'position', new Float32BufferAttribute( [
		0, 0, 0, 1, 0, 0, 1, 1, 0,
		0, 0, 0, 1, 1, 0, 0, 1, 0
	], 3 ) );
	return g;

}

function compareHits( a, b ) {

	if ( a.distance !== b.distance ) return a.distance - b.distance;
	if ( a.object.id !== b.object.id ) return a.object.id - b.object.id;

	const af = a.faceIndex === undefined ? -1 : a.faceIndex;
	const bf = b.faceIndex === undefined ? -1 : b.faceIndex;
	return af - bf;

}

function baselineHits( root, raycaster ) {

	const hits = raycaster.intersectObject( root, true );
	hits.sort( compareHits );
	return hits;

}

function checkSameHits( actual, expected, message ) {

	check( actual.length === expected.length, `${message}: hit count ${actual.length} !== ${expected.length}` );

	const n = Math.min( actual.length, expected.length );

	for ( let i = 0; i < n; i ++ ) {

		const a = actual[ i ];
		const e = expected[ i ];

		check( a.object === e.object, `${message}: hit ${i} object mismatch` );
		check( Math.abs( a.distance - e.distance ) <= 1e-9, `${message}: hit ${i} distance ${a.distance} !== ${e.distance}` );
		check( a.faceIndex === e.faceIndex, `${message}: hit ${i} faceIndex ${a.faceIndex} !== ${e.faceIndex}` );

		const am = a.face ? a.face.materialIndex : -1;
		const em = e.face ? e.face.materialIndex : -1;
		check( am === em, `${message}: hit ${i} materialIndex ${am} !== ${em}` );

	}

}

function throws( fn ) {

	try {

		fn();

	} catch ( e ) {

		return true;

	}

	return false;

}

// --- lifecycle ---------------------------------------------------------------

runGroup( 'lifecycle', () => {

	const root = new Object3D();
	const a = new Group();
	const b = new Group();
	root.add( a, b );

	const geo = triGeometry();
	const mat = new MeshBasicMaterial( { side: DoubleSide } );
	const one = new Mesh( geo, mat );
	const two = new Mesh( geo, mat );
	a.add( one );
	b.add( two );
	b.position.x = 4;
	root.updateMatrixWorld( true );

	const index = new SceneRaycastIndex( root );

	const ray = new Raycaster( new Vector3( 0, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 20 );
	checkSameHits( index.intersectRay( ray ), baselineHits( root, ray ), 'initial query' );
	check( index.intersectRay( ray ).length === 1, 'initially only the first mesh is hit' );

	// removal of a whole subtree
	root.remove( b );
	index.sync();
	root.updateMatrixWorld( true );
	checkSameHits( index.intersectRay( ray ), baselineHits( root, ray ), 'after subtree removal' );

	// re-attach the subtree elsewhere in the same root (reparent)
	b.add( one );
	root.add( b );
	index.sync();
	root.updateMatrixWorld( true );
	checkSameHits( index.intersectRay( ray ), baselineHits( root, ray ), 'after reparent' );
	check( index.intersectRay( ray ).length === 0, 'reparented mesh no longer at the origin' );

	const ray4 = new Raycaster( new Vector3( 4.25, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 20 );
	checkSameHits( index.intersectRay( ray4 ), baselineHits( root, ray4 ), 'reparented meshes at new location' );
	check( index.intersectRay( ray4 ).length === 2, 'both meshes of the reparented subtree are hit' );

	// move the whole subtree out of the root and back in
	root.remove( b );
	index.sync();
	root.updateMatrixWorld( true );
	checkSameHits( index.intersectRay( ray4 ), baselineHits( root, ray4 ), 'after moving subtree out' );
	check( index.intersectRay( ray4 ).length === 0, 'moved-out subtree is not picked' );

	root.add( b );
	index.sync();
	root.updateMatrixWorld( true );
	checkSameHits( index.intersectRay( ray4 ), baselineHits( root, ray4 ), 'after re-attaching subtree' );
	check( index.intersectRay( ray4 ).length === 2, 're-attached subtree is picked again' );

	// a parent layer mismatch must not mask the child meshes
	b.layers.set( 2 );
	checkSameHits( index.intersectRay( ray4 ), baselineHits( root, ray4 ), 'parent layer mismatch' );
	check( index.intersectRay( ray4 ).length === 2, 'children survive a parent layer mismatch' );

	// the mesh's own layer does mask it
	one.layers.set( 3 );
	checkSameHits( index.intersectRay( ray4 ), baselineHits( root, ray4 ), 'mesh layer mismatch' );
	check( index.intersectRay( ray4 ).length === 1, 'mesh on a foreign layer is skipped' );
	one.layers.set( 0 );
	b.layers.set( 0 );

	// visible=false keeps the original picking behavior (still picked)
	root.visible = false;
	checkSameHits( index.intersectRay( ray4 ), baselineHits( root, ray4 ), 'invisible root' );
	check( index.intersectRay( ray4 ).length === 2, 'invisible objects stay pickable' );
	root.visible = true;

	// matrixAutoUpdate=false with a hand-written matrix
	const manual = new Mesh( triGeometry(), mat );
	manual.matrixAutoUpdate = false;
	manual.matrix.makeTranslation( - 6, 0, 0 );
	a.add( manual );
	index.sync();
	root.updateMatrixWorld( true );

	const rayManual = new Raycaster( new Vector3( - 6, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 20 );
	checkSameHits( index.intersectRay( rayManual ), baselineHits( root, rayManual ), 'hand-written matrix' );
	check( index.intersectRay( rayManual ).length === 1, 'hand-written matrix mesh is hit' );

	manual.matrix.makeTranslation( - 8, 0, 0 );
	index.markDirty( manual );
	index.sync();
	root.updateMatrixWorld( true );
	check( index.intersectRay( rayManual ).length === 0, 'old hand-written position is stale after move' );

	const rayManual2 = new Raycaster( new Vector3( - 8, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 20 );
	checkSameHits( index.intersectRay( rayManual2 ), baselineHits( root, rayManual2 ), 'moved hand-written matrix' );
	check( index.intersectRay( rayManual2 ).length === 1, 'moved hand-written matrix mesh is hit' );

	// marking an ancestor dirty updates its current descendants
	a.position.x = 2;
	index.markDirty( a );
	index.sync();
	root.updateMatrixWorld( true );
	checkSameHits( index.intersectRay( rayManual ), baselineHits( root, rayManual ), 'dirty ancestor moves descendant' );
	check( index.intersectRay( rayManual ).length === 1, 'descendant followed the dirty ancestor' );

	// dispose never disposes caller-owned geometry or material
	let geoDisposed = false;
	let matDisposed = false;
	geo.addEventListener( 'dispose', () => geoDisposed = true );
	mat.addEventListener( 'dispose', () => matDisposed = true );

	index.dispose();
	index.dispose(); // repeated dispose is safe
	check( geoDisposed === false && matDisposed === false, 'dispose does not dispose geometry or material' );
	check( throws( () => index.sync() ), 'sync after dispose throws' );
	check( throws( () => index.intersectRay( ray ) ), 'query after dispose throws' );
	check( throws( () => index.markDirty( a ) ), 'markDirty after dispose throws' );
	check( throws( () => index.markGeometryDirty( geo ) ), 'markGeometryDirty after dispose throws' );
	check( typeof index.stats.boundsTests === 'number', 'stats stay readable after dispose' );

	// unsupported content is rejected explicitly
	const r1 = new Object3D();
	r1.add( new SkinnedMesh( triGeometry(), mat ) );
	check( throws( () => new SceneRaycastIndex( r1 ) ), 'SkinnedMesh is rejected' );

	const r2 = new Object3D();
	r2.add( new InstancedMesh( triGeometry(), mat, 1 ) );
	check( throws( () => new SceneRaycastIndex( r2 ) ), 'InstancedMesh is rejected' );

	const r3 = new Object3D();
	r3.add( new Points( triGeometry() ) );
	check( throws( () => new SceneRaycastIndex( r3 ) ), 'Points are rejected' );

	const r4 = new Object3D();
	r4.add( new Line( triGeometry() ) );
	check( throws( () => new SceneRaycastIndex( r4 ) ), 'Line is rejected' );

	const morphGeo = triGeometry();
	morphGeo.morphAttributes.position = [ new Float32BufferAttribute( [ 0, 0, 0, 1, 0, 0, 0, 1, 0 ], 3 ) ];
	const r5 = new Object3D();
	r5.add( new Mesh( morphGeo, mat ) );
	check( throws( () => new SceneRaycastIndex( r5 ) ), 'morph targets are rejected' );

	// unsupported content arriving through childadded is rejected too,
	// and the index keeps refusing instead of silently missing it
	const r6 = new Object3D();
	const index6 = new SceneRaycastIndex( r6 );
	check( throws( () => r6.add( new Points( triGeometry() ) ) ), 'runtime add of Points is rejected' );
	check( throws( () => index6.sync() ), 'index stays rejected after unsupported content' );
	check( throws( () => index6.intersectRay( ray ) ), 'queries stay rejected after unsupported content' );
	index6.dispose();

	// marking objects outside the index is an explicit error
	const r7 = new Object3D();
	const index7 = new SceneRaycastIndex( r7 );
	check( throws( () => index7.markDirty( new Object3D() ) ), 'markDirty on a non-member throws' );
	index7.dispose();

} );

// --- geometry ----------------------------------------------------------------

runGroup( 'geometry', () => {

	const root = new Object3D();
	const a = new Group();
	const b = new Group();
	root.add( a, b );

	const shared = triGeometry();
	const mat = new MeshBasicMaterial( { side: DoubleSide } );
	const m1 = new Mesh( shared, mat );
	const m2 = new Mesh( shared, mat );
	a.add( m1 );
	b.add( m2 );
	b.position.x = 10;
	root.updateMatrixWorld( true );

	const index = new SceneRaycastIndex( root );

	const rayA = new Raycaster( new Vector3( 0, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	const rayB = new Raycaster( new Vector3( 10, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	check( index.intersectRay( rayA ).length === 1 && index.intersectRay( rayB ).length === 1, 'shared geometry hit in both branches' );

	// edit the vertices of the shared geometry: move the triangle up by 4
	const pos = shared.getAttribute( 'position' );
	for ( let i = 0; i < pos.count; i ++ ) pos.setY( i, pos.getY( i ) + 4 );
	pos.needsUpdate = true;

	index.markGeometryDirty( shared );
	index.resetStats();
	index.sync();

	let st = index.stats;
	check( st.leafUpdates === 2, `all users of the shared geometry are updated (leafUpdates=${st.leafUpdates})` );
	check( st.sceneVisits <= 8, `geometry update stays local (sceneVisits=${st.sceneVisits})` );

	root.updateMatrixWorld( true );
	check( index.intersectRay( rayA ).length === 0, 'old vertex positions no longer hit in branch a' );
	check( index.intersectRay( rayB ).length === 0, 'old vertex positions no longer hit in branch b' );

	const rayA2 = new Raycaster( new Vector3( 0, 4, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	const rayB2 = new Raycaster( new Vector3( 10, 4, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	checkSameHits( index.intersectRay( rayA2 ), baselineHits( root, rayA2 ), 'edited geometry in branch a' );
	checkSameHits( index.intersectRay( rayB2 ), baselineHits( root, rayB2 ), 'edited geometry in branch b' );
	check( index.intersectRay( rayA2 ).length === 1 && index.intersectRay( rayB2 ).length === 1, 'both branches hit after the geometry edit' );

	// emptying the geometry must not leave ghost hits behind
	shared.setAttribute( 'position', new Float32BufferAttribute( [], 3 ) );
	index.markGeometryDirty( shared );
	index.sync();
	root.updateMatrixWorld( true );

	index.resetStats();
	check( index.intersectRay( rayA2 ).length === 0, 'empty geometry leaves no ghost hit' );
	check( index.stats.objectRaycasts === 0, 'empty geometry is not even raycast' );
	checkSameHits( index.intersectRay( rayA2 ), baselineHits( root, rayA2 ), 'empty geometry matches baseline' );

	// drawRange behaves like the original Raycaster
	const quadRoot = new Object3D();
	const quadGeo = quadGeometry();
	const quad = new Mesh( quadGeo, new MeshBasicMaterial( { side: DoubleSide } ) );
	quadRoot.add( quad );
	quadRoot.updateMatrixWorld( true );
	const quadIndex = new SceneRaycastIndex( quadRoot );

	const rayQ1 = new Raycaster( new Vector3( 0.75, 0.25, 5 ), new Vector3( 0, 0, - 1 ), 0, 20 );
	const rayQ2 = new Raycaster( new Vector3( 0.25, 0.75, 5 ), new Vector3( 0, 0, - 1 ), 0, 20 );
	check( quadIndex.intersectRay( rayQ1 ).length === 1 && quadIndex.intersectRay( rayQ2 ).length === 1, 'both quad triangles hit' );

	quadGeo.setDrawRange( 0, 3 ); // only the first triangle
	quadRoot.updateMatrixWorld( true );
	checkSameHits( quadIndex.intersectRay( rayQ1 ), baselineHits( quadRoot, rayQ1 ), 'drawRange keeps the first triangle' );
	checkSameHits( quadIndex.intersectRay( rayQ2 ), baselineHits( quadRoot, rayQ2 ), 'drawRange drops the second triangle' );
	check( quadIndex.intersectRay( rayQ2 ).length === 0, 'second triangle excluded by drawRange' );
	quadIndex.dispose();

	// material arrays with geometry groups
	const gRoot = new Object3D();
	const gGeo = quadGeometry();
	gGeo.addGroup( 0, 3, 0 );
	gGeo.addGroup( 3, 3, 1 );
	const gMesh = new Mesh( gGeo, [
		new MeshBasicMaterial( { side: FrontSide } ),
		new MeshBasicMaterial( { side: BackSide } )
	] );
	gRoot.add( gMesh );
	gRoot.updateMatrixWorld( true );
	const gIndex = new SceneRaycastIndex( gRoot );

	checkSameHits( gIndex.intersectRay( rayQ1 ), baselineHits( gRoot, rayQ1 ), 'material array: front group' );
	checkSameHits( gIndex.intersectRay( rayQ2 ), baselineHits( gRoot, rayQ2 ), 'material array: back group' );
	check( gIndex.intersectRay( rayQ1 ).length === 1, 'front-sided material hit from the front' );
	check( gIndex.intersectRay( rayQ2 ).length === 0, 'back-sided material not hit from the front' );

	const rayBehind = new Raycaster( new Vector3( 0.25, 0.75, - 5 ), new Vector3( 0, 0, 1 ), 0, 20 );
	checkSameHits( gIndex.intersectRay( rayBehind ), baselineHits( gRoot, rayBehind ), 'material array: from behind' );
	check( gIndex.intersectRay( rayBehind ).length === 1, 'back-sided material hit from behind' );
	gIndex.dispose();

	// reflection, non-uniform scale and rotation
	const tRoot = new Object3D();
	const tMesh = new Mesh( triGeometry(), new MeshBasicMaterial( { side: FrontSide } ) );
	tRoot.add( tMesh );
	tRoot.updateMatrixWorld( true );
	const tIndex = new SceneRaycastIndex( tRoot );

	const rayT = new Raycaster( new Vector3( 0, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	check( tIndex.intersectRay( rayT ).length === 1, 'front face hit before reflection' );

	tMesh.scale.set( - 1, 1, 1 ); // reflection mirrors the local x axis
	tMesh.position.set( 5, 0, 0 );
	tIndex.markDirty( tMesh );
	tIndex.sync();
	tRoot.updateMatrixWorld( true );

	const rayR = new Raycaster( new Vector3( 5.3, 0, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	checkSameHits( tIndex.intersectRay( rayR ), baselineHits( tRoot, rayR ), 'reflection' );
	check( tIndex.intersectRay( rayR ).length === 1, 'reflected mesh hit at its mirrored position' );
	check( tIndex.intersectRay( rayT ).length === 0, 'reflected mesh no longer at the origin' );

	tMesh.scale.set( - 2, 0.5, 3 );
	tMesh.rotation.set( 0, Math.PI / 6, Math.PI / 4 );
	tMesh.position.set( 1, 2, 3 );
	tMesh.material = new MeshBasicMaterial( { side: DoubleSide } );
	tIndex.markDirty( tMesh );
	tIndex.sync();
	tRoot.updateMatrixWorld( true );

	const centroid = new Vector3( 0, - 1 / 3, 0 ).applyMatrix4( tMesh.matrixWorld );
	const rayC = new Raycaster( new Vector3( centroid.x, centroid.y, 10 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	checkSameHits( tIndex.intersectRay( rayC ), baselineHits( tRoot, rayC ), 'non-uniform scale and rotation' );
	check( tIndex.intersectRay( rayC ).length === 1, 'transformed mesh hit through its centroid' );

	const rayMiss = new Raycaster( new Vector3( 100, 100, 10 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	checkSameHits( tIndex.intersectRay( rayMiss ), baselineHits( tRoot, rayMiss ), 'transformed mesh missed elsewhere' );
	tIndex.dispose();

	// a dirty geometry without any users is a no-op
	const emptyIndex = new SceneRaycastIndex( new Object3D() );
	emptyIndex.resetStats();
	emptyIndex.markGeometryDirty( triGeometry() );
	emptyIndex.sync();
	check( emptyIndex.stats.leafUpdates === 0, 'geometry without users updates no leaves' );
	emptyIndex.dispose();

	index.dispose();

} );

// --- query -------------------------------------------------------------------

runGroup( 'query', () => {

	const mat = new MeshBasicMaterial( { side: DoubleSide } );
	const geo = triGeometry();

	// results are sorted by distance
	const root = new Object3D();
	const meshes = [];
	for ( const d of [ 5, 1, 3 ] ) {

		const m = new Mesh( geo, mat );
		m.position.z = - d;
		root.add( m );
		meshes.push( m );

	}
	root.updateMatrixWorld( true );
	const index = new SceneRaycastIndex( root );

	const ray = new Raycaster( new Vector3( 0, 0, 10 ), new Vector3( 0, 0, - 1 ), 0, 100 );
	const hits = index.intersectRay( ray );
	check( hits.length === 3, 'all three meshes hit' );
	check( hits[ 0 ].distance < hits[ 1 ].distance && hits[ 1 ].distance < hits[ 2 ].distance, 'sorted by distance' );
	check( hits[ 0 ].object === meshes[ 1 ] && hits[ 1 ].object === meshes[ 2 ] && hits[ 2 ].object === meshes[ 0 ], 'closest first' );
	checkSameHits( hits, baselineHits( root, ray ), 'distance order matches baseline' );

	// equal distances are ordered by object id
	const root2 = new Object3D();
	const ma = new Mesh( geo, mat );
	const mb = new Mesh( geo, mat );
	root2.add( mb );
	root2.add( ma );
	root2.updateMatrixWorld( true );
	const index2 = new SceneRaycastIndex( root2 );

	const ray2 = new Raycaster( new Vector3( 0, 0, 10 ), new Vector3( 0, 0, - 1 ), 0, 100 );
	const hits2 = index2.intersectRay( ray2 );
	check( hits2.length === 2, 'both coincident meshes hit' );
	check( hits2[ 0 ].object === ma && hits2[ 1 ].object === mb, 'ties broken by object id' );
	checkSameHits( hits2, baselineHits( root2, ray2 ), 'coincident hits match baseline' );
	index2.dispose();

	// a ray exactly on a shared edge keeps both intersections, ordered by faceIndex
	const root3 = new Object3D();
	root3.add( new Mesh( quadGeometry(), mat ) );
	root3.updateMatrixWorld( true );
	const index3 = new SceneRaycastIndex( root3 );

	const rayEdge = new Raycaster( new Vector3( 0.5, 0.5, 5 ), new Vector3( 0, 0, - 1 ), 0, 20 );
	const hits3 = index3.intersectRay( rayEdge );
	check( hits3.length === 2, 'edge hit reported by both triangles' );
	check( hits3[ 0 ].faceIndex === 0 && hits3[ 1 ].faceIndex === 1, 'ties broken by faceIndex' );
	checkSameHits( hits3, baselineHits( root3, rayEdge ), 'edge hits match baseline' );
	index3.dispose();

	// near/far behave like the original Raycaster
	const rayNF = new Raycaster( new Vector3( 0, 0, 10 ), new Vector3( 0, 0, - 1 ), 12, 14 );
	const hitsNF = index.intersectRay( rayNF );
	check( hitsNF.length === 1 && hitsNF[ 0 ].object === meshes[ 2 ], 'near/far window respected' );
	checkSameHits( hitsNF, baselineHits( root, rayNF ), 'near/far matches baseline' );

	// raycaster layers filter individual meshes
	meshes[ 0 ].layers.set( 1 );
	const rayL = new Raycaster( new Vector3( 0, 0, 10 ), new Vector3( 0, 0, - 1 ), 0, 100 );
	rayL.layers.set( 1 );
	const hitsL = index.intersectRay( rayL );
	check( hitsL.length === 1 && hitsL[ 0 ].object === meshes[ 0 ], 'raycaster layers filter meshes' );
	checkSameHits( hitsL, baselineHits( root, rayL ), 'layers match baseline' );
	meshes[ 0 ].layers.set( 0 );

	// intersections are appended to the given target, which is also returned
	const t2 = index.intersectRay( ray2 === null ? ray : ray, [] );
	const before = t2.length;
	const ret = index.intersectRay( ray, t2 );
	check( ret === t2, 'the target array is returned' );
	check( t2.length === before * 2, 'intersections are appended to the target' );

	// querying with pending changes is an explicit error, not a silent scan
	meshes[ 1 ].position.x = 3;
	index.markDirty( meshes[ 1 ] );
	check( throws( () => index.intersectRay( ray ) ), 'query with pending changes throws' );
	index.sync();
	root.updateMatrixWorld( true );
	checkSameHits( index.intersectRay( ray ), baselineHits( root, ray ), 'query works again after sync' );

	// many randomized rays against transformed meshes must match the Raycaster
	const rRoot = new Object3D();
	let seed = 1234567;
	const rand = () => ( seed = ( seed * 1103515245 + 12345 ) & 0x7fffffff ) / 0x7fffffff;

	const rMeshes = [];
	for ( let i = 0; i < 40; i ++ ) {

		const m = new Mesh( triGeometry( 1 + rand() * 2 ), mat );
		m.position.set( ( rand() - 0.5 ) * 40, ( rand() - 0.5 ) * 40, ( rand() - 0.5 ) * 40 );
		m.rotation.set( rand() * Math.PI, rand() * Math.PI, rand() * Math.PI );
		m.scale.set( 0.5 + rand() * 2, 0.5 + rand() * 2, 0.5 + rand() * 2 );
		rRoot.add( m );
		rMeshes.push( m );

	}
	root.updateMatrixWorld( true );
	rRoot.updateMatrixWorld( true );
	const rIndex = new SceneRaycastIndex( rRoot );

	let totalHits = 0;
	for ( let i = 0; i < 64; i ++ ) {

		let origin, direction, near = 0, far = Infinity;

		if ( i % 2 === 0 ) {

			// aim at the centroid of a random mesh: guaranteed to produce a hit
			const m = rMeshes[ Math.floor( rand() * rMeshes.length ) ];
			const s = m.geometry.getAttribute( 'position' ).getY( 2 );
			const target = new Vector3( 0, - s / 3, 0 ).applyMatrix4( m.matrixWorld );
			origin = new Vector3( ( rand() - 0.5 ) * 60, ( rand() - 0.5 ) * 60, ( rand() - 0.5 ) * 60 );
			direction = target.sub( origin ).normalize();

		} else {

			origin = new Vector3( ( rand() - 0.5 ) * 60, ( rand() - 0.5 ) * 60, ( rand() - 0.5 ) * 60 );
			direction = new Vector3( rand() - 0.5, rand() - 0.5, rand() - 0.5 ).normalize();
			near = rand() * 2;
			far = 20 + rand() * 40;

		}

		const rc = new Raycaster( origin, direction, near, far );
		const actual = rIndex.intersectRay( rc );
		checkSameHits( actual, baselineHits( rRoot, rc ), `random ray ${i}` );
		totalHits += actual.length;

	}
	check( totalHits > 0, 'random rays actually hit something' );

	rIndex.dispose();
	index.dispose();

} );

// --- work --------------------------------------------------------------------

runGroup( 'work', () => {

	const root = new Object3D();
	const mat = new MeshBasicMaterial( { side: DoubleSide } );

	const N = 16; // 16 x 16 = 256 spatially separated meshes
	const meshes = [];
	for ( let i = 0; i < N; i ++ ) {

		for ( let j = 0; j < N; j ++ ) {

			const m = new Mesh( triGeometry(), mat );
			m.position.set( i * 10, j * 10, 0 );
			root.add( m );
			meshes.push( m );

		}

	}
	root.updateMatrixWorld( true );

	const index = new SceneRaycastIndex( root );
	const leafCount = meshes.length;
	check( leafCount === 256, 'scene has 256 meshes' );

	// a single local ray must skip most of the scene in the broad phase
	index.resetStats();
	const ray = new Raycaster( new Vector3( 3 * 10 + 0.1, 5 * 10 + 0.1, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	const hits = index.intersectRay( ray );
	check( hits.length === 1, 'local ray hits exactly one mesh' );
	checkSameHits( hits, baselineHits( root, ray ), 'local ray matches baseline' );

	let st = index.stats;
	check( st.boundsTests < leafCount / 2, `boundsTests ${st.boundsTests} < ${leafCount / 2}` );
	check( st.objectRaycasts >= 1 && st.objectRaycasts <= 8, `objectRaycasts ${st.objectRaycasts} only follows candidates` );
	check( st.sceneVisits === 0, 'queries never visit the scene' );

	// a ray that misses everything performs no object raycasts at all
	index.resetStats();
	const miss = index.intersectRay( new Raycaster( new Vector3( - 1000, - 1000, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 ) );
	st = index.stats;
	check( miss.length === 0, 'miss ray has no hits' );
	check( st.objectRaycasts === 0, 'miss ray performs no object raycasts' );
	check( st.boundsTests < leafCount / 2, `miss ray prunes early (boundsTests=${st.boundsTests})` );

	// dirtying a single leaf must not enumerate its unchanged siblings
	index.resetStats();
	const moved = meshes[ 137 ];
	moved.position.z = 7;
	index.markDirty( moved );
	index.sync();
	st = index.stats;
	check( st.leafUpdates === 1, `single dirty leaf updated once (leafUpdates=${st.leafUpdates})` );
	check( st.sceneVisits < leafCount / 2, `dirty update stays local (sceneVisits=${st.sceneVisits})` );
	check( st.boundsTests === 0 && st.objectRaycasts === 0, 'sync performs no raycast work' );

	root.updateMatrixWorld( true );
	const rayMoved = new Raycaster( new Vector3( moved.position.x + 0.1, moved.position.y + 0.1, 20 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	checkSameHits( index.intersectRay( rayMoved ), baselineHits( root, rayMoved ), 'moved leaf matches baseline' );
	const movedHits = index.intersectRay( rayMoved );
	check( movedHits.length === 1 && Math.abs( movedHits[ 0 ].distance - 13 ) < 1e-9, 'moved leaf hit at its new depth' );

	// a sync without changes does not traverse the scene or recompute leaves
	index.resetStats();
	index.sync();
	st = index.stats;
	check( st.leafUpdates === 0 && st.sceneVisits === 0, 'no-change sync does nothing' );
	check( st.boundsTests === 0 && st.objectRaycasts === 0, 'no-change sync performs no raycast work' );

	// adding and removing members is maintained incrementally
	index.resetStats();
	const extra = new Mesh( triGeometry(), mat );
	extra.position.set( 1000, 1000, 0 );
	root.add( extra );
	index.sync();
	st = index.stats;
	check( st.leafUpdates === 1, `added leaf updated once (leafUpdates=${st.leafUpdates})` );
	check( st.sceneVisits < leafCount / 2, `add stays local (sceneVisits=${st.sceneVisits})` );

	root.updateMatrixWorld( true );
	const rayExtra = new Raycaster( new Vector3( 1000.1, 1000.1, 5 ), new Vector3( 0, 0, - 1 ), 0, 50 );
	checkSameHits( index.intersectRay( rayExtra ), baselineHits( root, rayExtra ), 'added mesh matches baseline' );
	check( index.intersectRay( rayExtra ).length === 1, 'added mesh is pickable' );

	index.resetStats();
	root.remove( extra );
	index.sync();
	st = index.stats;
	check( st.leafUpdates === 0, 'removal recomputes no leaves' );
	check( st.sceneVisits < leafCount / 2, `removal stays local (sceneVisits=${st.sceneVisits})` );
	root.updateMatrixWorld( true );
	check( index.intersectRay( rayExtra ).length === 0, 'removed mesh is no longer hit' );

	// stats are exposed read-only
	const snapshot = index.stats;
	check( Object.isFrozen( snapshot ), 'stats snapshot is read-only' );
	check(
		typeof snapshot.boundsTests === 'number' &&
		typeof snapshot.objectRaycasts === 'number' &&
		typeof snapshot.leafUpdates === 'number' &&
		typeof snapshot.sceneVisits === 'number',
		'stats exposes all counters'
	);

	index.dispose();

} );
