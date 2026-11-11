import assert from 'node:assert/strict';
import * as THREE from 'three';
import { loadControls } from './build.mjs';

const Controls = await loadControls();
Controls.install( { THREE } );

const EPS = 1e-6;
const approx = ( a, b, tolerance = 1e-5 ) => Math.abs( a - b ) <= tolerance;

// corners of an arbitrarily oriented, elongated box (long axis is not world-aligned)
function makeLongObliqueBox( center = new THREE.Vector3() ) {

	const half = new THREE.Vector3( 10, 0.6, 0.4 );
	const rotation = new THREE.Matrix4().makeRotationFromEuler( new THREE.Euler( 0.6, 0.4, 0.2 ) );
	const points = [];
	for ( const sx of [ - 1, 1 ] ) {
		for ( const sy of [ - 1, 1 ] ) {
			for ( const sz of [ - 1, 1 ] ) {

				points.push(
					new THREE.Vector3( sx * half.x, sy * half.y, sz * half.z )
						.applyMatrix4( rotation )
						.add( center ),
				);

			}
		}
	}
	return points;

}

function aabbCenter( points ) {

	const box = new THREE.Box3().setFromPoints( points );
	return box.getCenter( new THREE.Vector3() );

}

// max NDC occupancy, depth span and final target for one framing
function evaluate( label, controls, camera, points ) {

	camera.updateMatrixWorld();

	let maxNdc = 0;
	let minDepth = Infinity;
	let maxDepth = - Infinity;
	for ( const point of points ) {

		const ndc = point.clone().project( camera );
		maxNdc = Math.max( maxNdc, Math.abs( ndc.x ), Math.abs( ndc.y ) );

		const cameraSpace = point.clone().applyMatrix4( camera.matrixWorldInverse );
		const depth = - cameraSpace.z;
		minDepth = Math.min( minDepth, depth );
		maxDepth = Math.max( maxDepth, depth );

	}

	const target = controls.getTarget( new THREE.Vector3() );
	console.log(
		`${ label }\n` +
		`  target=(${ target.x.toFixed( 4 ) }, ${ target.y.toFixed( 4 ) }, ${ target.z.toFixed( 4 ) }) ` +
		`maxNdc=${ maxNdc.toFixed( 6 ) } nearDepth=${ minDepth.toFixed( 6 ) } farDepth=${ maxDepth.toFixed( 6 ) }`,
	);
	return { maxNdc, minDepth, maxDepth, target };

}

async function settle( controls, camera, promise = undefined ) {

	for ( let i = 0; i < 600; i ++ ) {

		controls.update( 1 / 60 );
		camera.updateMatrixWorld();
		await Promise.resolve();

	}
	if ( promise ) await promise;

}

{
	// 1. perspective, elongated oblique box: per-point constraints vs bounding sphere
	const camera = new THREE.PerspectiveCamera( 60, 1.5, 0.1, 1000 );
	camera.position.set( 8, 5, 11 );
	const controls = new Controls( camera );
	controls.rotateTo( 0.7, 1.15, false ); // oblique, non-axis-snapped viewing angle

	const points = makeLongObliqueBox( new THREE.Vector3( 3, - 2, 5 ) );
	const snapshot = JSON.stringify( points.map( ( p ) => p.toArray() ) );
	const expectedCenter = aabbCenter( points );

	await controls.fitToPoints( points, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();

	const report = evaluate( '[1] perspective oblique long box (padding=0)', controls, camera, points );
	assert.ok( report.maxNdc <= 1 + 1e-5, `points must stay inside NDC square, got ${ report.maxNdc }` );
	assert.ok( report.maxNdc > 1 - 1e-4, 'tight fit should touch the NDC border without padding' );
	assert.ok( report.minDepth >= camera.near - EPS, 'nearest point must be behind the near plane' );
	assert.ok( report.maxDepth <= camera.far + EPS, 'farthest point must be in front of the far plane' );
	assert.ok( report.maxDepth - report.minDepth > 5, 'the long box must have a noticeable depth span' );
	assert.ok( approx( report.target.x, expectedCenter.x ) && approx( report.target.y, expectedCenter.y ) && approx( report.target.z, expectedCenter.z ) );

	const spherical = controls.getSpherical( new THREE.Spherical() );
	const sphereRadius = new THREE.Box3().setFromPoints( points ).getBoundingSphere( new THREE.Sphere() ).radius;
	const sphereFitDistance = controls.getDistanceToFitSphere( sphereRadius );
	console.log(
		`  per-point radius=${ spherical.radius.toFixed( 6 ) } ` +
		`bounding-sphere radius would be=${ sphereFitDistance.toFixed( 6 ) }`,
	);
	assert.ok(
		spherical.radius < sphereFitDistance,
		'per-point fitting must frame an elongated part tighter than a bounding sphere',
	);
	assert.equal( controls.getFocalOffset( new THREE.Vector3() ).length(), 0, 'focal offset must be cleared' );
	assert.equal( JSON.stringify( points.map( ( p ) => p.toArray() ) ), snapshot, 'input vectors must not be mutated' );

	// padding keeps every point inside the smaller [-1+p, 1-p] square and pulls the camera back
	await controls.fitToPoints( points, false, { ndcPadding: 0.1 } );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const padded = evaluate( '[1b] perspective oblique long box (padding=0.1)', controls, camera, points );
	assert.ok( padded.maxNdc <= 0.9 + 1e-5 );
	assert.ok( padded.maxNdc > 0.9 - 1e-4, 'padding fit should touch the padded border' );
	const paddedRadius = controls.getSpherical( new THREE.Spherical() ).radius;
	assert.ok( paddedRadius > spherical.radius, 'padding requires a larger orbit radius' );

	// perspective zoom must be preserved and the effective (narrower) FOV used for planning
	controls.zoomTo( 2, false );
	await controls.fitToPoints( points, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const zoomedReport = evaluate( '[1c] perspective oblique long box (zoom=2)', controls, camera, points );
	assert.ok( zoomedReport.maxNdc <= 1 + 1e-5 && zoomedReport.maxNdc > 1 - 1e-4 );
	assert.ok( approx( camera.zoom, 2 ), 'perspective zoom must be left untouched' );
	assert.ok(
		controls.getSpherical( new THREE.Spherical() ).radius > spherical.radius,
		'a narrower effective FOV (zoom 2) requires pulling the camera back',
	);
	controls.dispose();

}

{
	// 2. Z-up world keeps the same contract
	const camera = new THREE.PerspectiveCamera( 55, 1.2, 0.1, 500 );
	camera.up.set( 0, 0, 1 );
	camera.position.set( 6, - 4, 9 );
	const controls = new Controls( camera );
	controls.rotateTo( - 0.5, 1.0, false );

	const points = makeLongObliqueBox( new THREE.Vector3( 0, 0, 2 ) );
	const expectedCenter = aabbCenter( points );

	await controls.fitToPoints( points, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();

	const report = evaluate( '[2] Z-up perspective', controls, camera, points );
	assert.ok( report.maxNdc <= 1 + 1e-5 );
	assert.ok( report.minDepth >= camera.near - EPS && report.maxDepth <= camera.far + EPS );
	assert.ok( approx( report.target.x, expectedCenter.x ) && approx( report.target.y, expectedCenter.y ) && approx( report.target.z, expectedCenter.z ) );
	assert.deepEqual( camera.up.toArray(), [ 0, 0, 1 ], 'camera.up must not change' );
	controls.dispose();

}

{
	// 3. a rotateTo transition still in flight: framing must use the planned end orientation
	const camera = new THREE.PerspectiveCamera( 60, 1.5, 0.1, 1000 );
	camera.position.set( 8, 5, 11 );
	const controls = new Controls( camera );

	controls.rotateTo( 2.1, 0.8, true );
	controls.update( 1 / 60 ); // one frame only: the current quaternion is still mid-transition
	const midSpherical = controls.getSpherical( new THREE.Spherical(), false );
	assert.ok( ! approx( midSpherical.theta, 2.1 ), 'rotation transition must still be in progress' );

	const points = makeLongObliqueBox( new THREE.Vector3( - 4, 1, 2 ) );
	await controls.fitToPoints( points, false );
	// settle the rotation transition that was already in flight; the framing itself
	// was computed from the planned end orientation rather than this frame's quaternion
	await settle( controls, camera );
	camera.updateMatrixWorld();

	const endSpherical = controls.getSpherical( new THREE.Spherical(), true );
	assert.ok( approx( endSpherical.theta, 2.1 ), 'planned end azimuth must be preserved' );
	assert.ok( approx( endSpherical.phi, 0.8 ), 'planned end polar must be preserved' );

	const report = evaluate( '[3] fit while a rotate transition is in flight', controls, camera, points );
	assert.ok( report.maxNdc <= 1 + 1e-5 );
	assert.ok( approx( report.maxNdc, 1, 1e-4 ), 'once at rest the end orientation must frame the points tightly' );
	assert.ok( report.minDepth >= camera.near - EPS && report.maxDepth <= camera.far + EPS );
	controls.dispose();

}

{
	// 4. orthographic camera: zoom boundaries and zero screen-space span
	const camera = new THREE.OrthographicCamera( - 3, 3, 2, - 2, 0.1, 100 );
	camera.position.set( 0, 0, 10 );
	const controls = new Controls( camera );
	controls.dollyTo( 10, false );
	controls.rotateTo( 0.6, 1.1, false );

	const points = makeLongObliqueBox( new THREE.Vector3( 1, 1, 0 ) );

	// first find the zoom an unconstrained fit requires
	await controls.fitToPoints( points, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const requiredZoom = camera.zoom;
	assert.ok( requiredZoom > 0 && Number.isFinite( requiredZoom ) );
	const unconstrained = evaluate( '[4] orthographic oblique long box', controls, camera, points );
	assert.ok( unconstrained.maxNdc <= 1 + 1e-5 && unconstrained.maxNdc > 1 - 1e-4 );
	assert.ok( unconstrained.minDepth >= camera.near - EPS && unconstrained.maxDepth <= camera.far + EPS );
	const radiusKept = controls.getSpherical( new THREE.Spherical() ).radius;
	assert.ok( approx( radiusKept, 10 ), 'orthographic mode must keep the orbit radius' );

	// maxZoom below the required zoom only zooms out further: still feasible,
	// the chosen zoom is clamped to maxZoom and the points occupy a smaller NDC area
	controls.minZoom = 0.0001;
	controls.maxZoom = requiredZoom * 0.5;
	await controls.fitToPoints( points, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	assert.ok( approx( camera.zoom, requiredZoom * 0.5 ), 'zoom must clamp to maxZoom when maxZoom is below the ideal fit' );
	const zoomedOut = evaluate( '[4b] orthographic maxZoom below the ideal fit', controls, camera, points );
	assert.ok( zoomedOut.maxNdc <= 0.5 + 1e-4 && zoomedOut.maxNdc > 0.5 - 1e-4 );

	// minZoom above the required zoom forces the view to be too tight -> reject
	controls.minZoom = requiredZoom * 2;
	controls.maxZoom = Infinity;
	await assert.rejects( controls.fitToPoints( points, false ), /zoom limits/ );

	// feasible limits again -> normal framing, zoom clamped inside the range
	controls.minZoom = 0.01;
	controls.maxZoom = Infinity;

	// zero screen span, unbounded maxZoom: keep max(minZoom, planned zoom)
	const single = [ new THREE.Vector3( 1, 1, 0 ) ];
	controls.zoomTo( 1, false );
	await controls.fitToPoints( single, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const singleReport = evaluate( '[4b] orthographic single point, infinite maxZoom', controls, camera, single );
	assert.ok( Number.isFinite( camera.zoom ) && camera.zoom > 0 );
	assert.ok( approx( camera.zoom, Math.max( controls.minZoom, 1 ) ) );
	assert.ok( singleReport.minDepth >= camera.near - EPS && singleReport.maxDepth <= camera.far + EPS );

	// zero screen span, bounded maxZoom: take maxZoom
	controls.maxZoom = 7;
	await controls.fitToPoints( single, false );
	controls.update( 1 / 60 );
	assert.ok( approx( camera.zoom, 7 ), 'zero-span points with a finite maxZoom must use maxZoom' );

	// a non-finite zoom configuration must be refused rather than producing NaN
	controls.maxZoom = Infinity;
	controls.minZoom = Infinity;
	await assert.rejects( controls.fitToPoints( single, false ), /zoom/ );
	controls.minZoom = 0.01;

	// a line lying exactly on the view direction has depth span but no screen span.
	// use an exact axis view so the zero screen span is not polluted by float noise.
	controls.rotateTo( 0, Math.PI / 2, false );
	const target = controls.getTarget( new THREE.Vector3() );
	const depthLine = [
		target.clone().add( new THREE.Vector3( 0, 0, - 2 ) ),
		target.clone(),
		target.clone().add( new THREE.Vector3( 0, 0, 2 ) ),
	];
	await controls.fitToPoints( depthLine, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const lineReport = evaluate( '[4c] orthographic view-axis line (zero screen span)', controls, camera, depthLine );
	assert.ok( Number.isFinite( camera.zoom ) && camera.zoom > 0 );
	assert.ok( lineReport.maxDepth - lineReport.minDepth > 3.9, 'depth distribution must be considered' );
	assert.ok( lineReport.minDepth >= camera.near - EPS && lineReport.maxDepth <= camera.far + EPS );

	// depth span that cannot fit the fixed orthographic frustum -> reject.
	// radius 4 keeps the target in front of the near plane, while the second point
	// lands at distance 104, beyond the far plane (100)
	controls.dollyTo( 4, false );
	const tooDeep = [
		target.clone(),
		target.clone().add( new THREE.Vector3( 0, 0, - 100 ) ),
	];
	await assert.rejects( controls.fitToPoints( tooDeep, false ), /clipping-plane/ );
	controls.dispose();

	// an asymmetric orthographic frustum is explicitly unsupported
	const asymmetricCamera = new THREE.OrthographicCamera( - 3, 4, 2, - 2, 0.1, 100 );
	asymmetricCamera.position.set( 0, 0, 10 );
	const asymmetricControls = new Controls( asymmetricCamera );
	await assert.rejects(
		asymmetricControls.fitToPoints( [ new THREE.Vector3( 0, 0, 0 ) ], false ),
		/symmetric orthographic frustum/,
	);
	asymmetricControls.dispose();

}

{
	// 5. rejected calls leave camera, planned end values and event state untouched
	const camera = new THREE.PerspectiveCamera( 60, 1.5, 0.1, 1000 );
	camera.position.set( 8, 5, 11 );
	const controls = new Controls( camera );
	controls.rotateTo( 0.9, 1.2, false );
	controls.setFocalOffset( 0.2, - 0.3, 0.1, false );
	controls.update( 1 / 60 );

	const points = makeLongObliqueBox( new THREE.Vector3( 2, 2, 2 ) );
	const inputSnapshot = JSON.stringify( points.map( ( p ) => p.toArray() ) );

	const targetBefore = controls.getTarget( new THREE.Vector3() ).toArray();
	const sphericalBefore = controls.getSpherical( new THREE.Spherical() );
	const sphericalBeforeSnapshot = [ sphericalBefore.radius, sphericalBefore.phi, sphericalBefore.theta ];
	const zoomBefore = controls._zoomEnd;
	const focalBefore = controls.getFocalOffset( new THREE.Vector3() ).toArray();
	const restedBefore = controls._hasRested;
	const restListenersBefore = controls._listeners.rest?.length ?? 0;
	const firedEvents = [];
	const recordEvent = ( event ) => firedEvents.push( event.type );
	controls.addEventListener( 'transitionstart', recordEvent );
	controls.addEventListener( 'wake', recordEvent );
	controls.addEventListener( 'update', recordEvent );

	const invalidPadding = [ - 0.1, 1, NaN, 1.5 ];
	for ( const padding of invalidPadding ) {

		await assert.rejects( controls.fitToPoints( points, false, { ndcPadding: padding } ), /ndcPadding/ );

	}
	await assert.rejects( controls.fitToPoints( [], false ), /non-empty/ );
	await assert.rejects(
		controls.fitToPoints( [ new THREE.Vector3( 0, 0, NaN ) ], false ),
		/finite coordinates/,
	);

	// points whose depth span (1500) exceeds the whole near-far range (999.9):
	// no orbit radius can keep both of them clipped at the same time
	const tooDeep = [ new THREE.Vector3( 0, 0, 0 ), new THREE.Vector3( 0, 0, 1500 ) ];
	await assert.rejects( controls.fitToPoints( tooDeep, false ), /clipping-plane|distance/ );

	// required radius beyond maxDistance
	controls.maxDistance = 5;
	await assert.rejects( controls.fitToPoints( points, false ), /distance/ );
	controls.maxDistance = Infinity;

	// unsupported camera configurations
	const parent = new THREE.Object3D();
	parent.add( camera );
	await assert.rejects( controls.fitToPoints( points, false ), /parent/ );
	parent.remove( camera );

	camera.filmOffset = 1;
	await assert.rejects( controls.fitToPoints( points, false ), /film or view/ );
	camera.filmOffset = 0;

	camera.view = { enabled: true, fullWidth: 1, fullHeight: 1, offsetX: 0, offsetY: 0, width: 1, height: 1 };
	await assert.rejects( controls.fitToPoints( points, false ), /film or view/ );
	camera.view = null;

	assert.deepEqual( controls.getTarget( new THREE.Vector3() ).toArray(), targetBefore, 'planned target must be unchanged after rejection' );
	const sphericalAfter = controls.getSpherical( new THREE.Spherical() );
	assert.deepEqual(
		[ sphericalAfter.radius, sphericalAfter.phi, sphericalAfter.theta ],
		sphericalBeforeSnapshot,
		'planned spherical must be unchanged after rejection',
	);
	assert.equal( controls._zoomEnd, zoomBefore, 'planned zoom must be unchanged after rejection' );
	assert.deepEqual( controls.getFocalOffset( new THREE.Vector3() ).toArray(), focalBefore, 'focal offset must be unchanged after rejection' );
	assert.equal( controls._hasRested, restedBefore, 'rest event state must be unchanged after rejection' );
	assert.equal( controls._listeners.rest?.length ?? 0, restListenersBefore, 'no rest promise listeners may leak from a rejection' );
	assert.deepEqual( firedEvents, [], 'rejected calls must not dispatch any transition events' );
	assert.equal( JSON.stringify( points.map( ( p ) => p.toArray() ) ), inputSnapshot, 'rejected calls must not mutate input vectors' );

	// 6. normal framing still works after the failures
	await controls.fitToPoints( points, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const report = evaluate( '[6] normal framing after rejected calls', controls, camera, points );
	assert.ok( report.maxNdc <= 1 + 1e-5 );
	assert.ok( report.minDepth >= camera.near - EPS && report.maxDepth <= camera.far + EPS );
	controls.dispose();

}

{
	// 7. single point and coplanar points always yield finite perspective results
	const camera = new THREE.PerspectiveCamera( 60, 1.5, 0.1, 1000 );
	camera.position.set( 4, 4, 4 );
	const controls = new Controls( camera );

	const single = [ new THREE.Vector3( 7, - 3, 2 ) ];
	await controls.fitToPoints( single, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const singleReport = evaluate( '[7] perspective single point', controls, camera, single );
	assert.ok( Number.isFinite( controls.getSpherical( new THREE.Spherical() ).radius ) );
	assert.ok( singleReport.minDepth >= camera.near - EPS && singleReport.maxDepth <= camera.far + EPS );
	assert.ok( approx( singleReport.target.x, single[ 0 ].x ) && approx( singleReport.target.y, single[ 0 ].y ) && approx( singleReport.target.z, single[ 0 ].z ) );

	const coplanar = [
		new THREE.Vector3( - 2, 0, 1 ),
		new THREE.Vector3( 2, 0, - 1 ),
		new THREE.Vector3( 0, 0, 3 ),
		new THREE.Vector3( 1, 0, - 3 ),
	];
	await controls.fitToPoints( coplanar, false );
	controls.update( 1 / 60 );
	camera.updateMatrixWorld();
	const planarReport = evaluate( '[7b] perspective coplanar points', controls, camera, coplanar );
	assert.ok( Number.isFinite( controls.getSpherical( new THREE.Spherical() ).radius ) );
	assert.ok( planarReport.maxNdc <= 1 + 1e-5 );
	assert.ok( planarReport.minDepth >= camera.near - EPS && planarReport.maxDepth <= camera.far + EPS );
	controls.dispose();

}

{
	// 8. enableTransition=true rides the existing smooth update / rest promise machinery
	const camera = new THREE.PerspectiveCamera( 60, 1.5, 0.1, 1000 );
	camera.position.set( 8, 5, 11 );
	const controls = new Controls( camera );
	controls.rotateTo( 0.7, 1.15, false );

	const points = makeLongObliqueBox( new THREE.Vector3( 3, - 2, 5 ) );
	const radiusBefore = controls.getSpherical( new THREE.Spherical(), false ).radius;

	const promise = controls.fitToPoints( points, true );
	// the method itself must not snap the camera or run an animation loop
	assert.equal( controls.getSpherical( new THREE.Spherical(), false ).radius, radiusBefore );

	await settle( controls, camera, promise );

	const report = evaluate( '[8] transitioned fit after rest', controls, camera, points );
	assert.ok( report.maxNdc <= 1 + 1e-5 );
	assert.ok( report.minDepth >= camera.near - EPS && report.maxDepth <= camera.far + EPS );
	assert.ok( approx( report.target.x, aabbCenter( points ).x ) );
	controls.dispose();

}

console.log( 'ALL FIT-TO-POINTS CHECKS PASSED' );
