import { test } from 'node:test';
import assert from 'node:assert/strict';

import { AnimationClip } from '../src/animation/AnimationClip.js';
import { AnimationUtils } from '../src/animation/AnimationUtils.js';
import { AnimationMixer } from '../src/animation/AnimationMixer.js';
import { Object3D } from '../src/core/Object3D.js';
import { Quaternion } from '../src/math/Quaternion.js';
import { Color } from '../src/math/Color.js';
import { NumberKeyframeTrack } from '../src/animation/tracks/NumberKeyframeTrack.js';
import { VectorKeyframeTrack } from '../src/animation/tracks/VectorKeyframeTrack.js';
import { ColorKeyframeTrack } from '../src/animation/tracks/ColorKeyframeTrack.js';
import { QuaternionKeyframeTrack } from '../src/animation/tracks/QuaternionKeyframeTrack.js';
import { BooleanKeyframeTrack } from '../src/animation/tracks/BooleanKeyframeTrack.js';
import { StringKeyframeTrack } from '../src/animation/tracks/StringKeyframeTrack.js';
import { CubicSegmentKeyframeTrack } from '../src/animation/tracks/CubicSegmentKeyframeTrack.js';
import {
	InterpolateLinear,
	InterpolateDiscrete,
	InterpolateSmooth,
	InterpolateBezier,
	LoopOnce,
	NormalAnimationBlendMode,
	AdditiveAnimationBlendMode
} from '../src/constants.js';

const NUMERIC_TOLERANCE = 2e-5;
const QUATERNION_TOLERANCE = 2e-6;

// deterministic pseudo random numbers, so the suite never depends on Math.random
function createLcg( seed ) {

	let state = seed;

	return function random() {

		state = ( state * 1103515245 + 12345 ) & 0x7fffffff;
		return state / 0x7fffffff;

	};

}

function numericError( actual, expected ) {

	return Math.abs( actual - expected ) / Math.max( 1, Math.abs( expected ) );

}

function quaternionPoseError( a, b ) {

	const qa = new Quaternion( a[ 0 ], a[ 1 ], a[ 2 ], a[ 3 ] ).normalize();
	const qb = new Quaternion( b[ 0 ], b[ 1 ], b[ 2 ], b[ 3 ] ).normalize();

	return 1 - Math.abs( qa.dot( qb ) );

}

/**
 * Local sample times covering the clip origin, the clip end, uniform grid
 * points, jittered points and both sides of every source keyframe inside
 * the cut.
 */
function createSampleTimes( sourceTimes, startTime, endTime, interpolation ) {

	const duration = endTime - startTime;
	const points = new Set();

	const add = ( time ) => {

		if ( time >= 0 && time <= duration ) {

			points.add( Number( time.toFixed( 9 ) ) );

		}

	};

	add( 0 );
	add( duration );

	for ( let i = 0; i <= 48; ++ i ) {

		add( duration * i / 48 );

	}

	for ( const time of sourceTimes ) {

		if ( time > startTime && time < endTime ) {

			const localTime = time - startTime;
			add( localTime );
			add( Math.max( 0, localTime - 1e-4 ) );
			add( Math.min( duration, localTime + 1e-4 ) );

		}

	}

	const random = createLcg( 987654321 );

	for ( let i = 0; i < 40; ++ i ) {

		add( random() * duration );

	}

	let result = [ ...points ].sort( ( a, b ) => a - b );

	// Discrete tracks store times as Float32 in both clips, so subtracting the
	// start time shifts an interior key by at most a few ULPs (~1e-7s).
	// Sampling inside that rounding band would compare two different Float32
	// boundaries; step behavior itself is asserted structurally elsewhere.
	if ( interpolation === InterpolateDiscrete ) {

		result = result.filter( ( localTime ) => {

			const sourceTime = startTime + localTime;

			return sourceTimes.every( ( keyTime ) => Math.abs( sourceTime - keyTime ) > 2e-3 );

		} );

	}

	return result;

}

/**
 * Compares a cut track against the fixed curve produced by the source
 * track's own interpolant over the whole closed cut interval.
 */
function assertTrackMatchesSource( track, cutTrack, startTime, endTime ) {

	const sourceInterpolant = track.createInterpolant();
	const cutInterpolant = cutTrack.createInterpolant();
	const sampleTimes = createSampleTimes(
		Array.from( track.times ),
		startTime,
		endTime,
		track.getInterpolation()
	);

	for ( const localTime of sampleTimes ) {

		const expected = sourceInterpolant.evaluate( startTime + localTime );
		const actual = cutInterpolant.evaluate( localTime );

		if ( track.ValueTypeName === 'bool' || track.ValueTypeName === 'string' ) {

			assert.strictEqual(
				actual[ 0 ], expected[ 0 ],
				`${ track.ValueTypeName } track "${ track.name }" differs at local time ${ localTime }`
			);

		} else if ( track.ValueTypeName === 'quaternion' ) {

			const error = quaternionPoseError( actual, expected );

			assert.ok(
				error <= QUATERNION_TOLERANCE,
				`quaternion track "${ track.name }" pose error ${ error } at local time ${ localTime }`
			);

		} else {

			for ( let c = 0; c !== expected.length; ++ c ) {

				assert.ok(
					numericError( actual[ c ], expected[ c ] ) <= NUMERIC_TOLERANCE,
					`numeric track "${ track.name }" component ${ c } differs at local time ${ localTime }: ` +
					`${ actual[ c ] } vs ${ expected[ c ] }`
				);

			}

		}

	}

}

function makeBezierTrack( name, times, values ) {

	const track = new NumberKeyframeTrack( name, times, values, InterpolateBezier );
	const nKeys = times.length;
	const inTangents = new Float32Array( nKeys * 2 );
	const outTangents = new Float32Array( nKeys * 2 );

	// deliberately non-uniform, asymmetric control point times
	const outFractions = [ 0.25, 0.7, 0.15, 0.8, 0.5 ];
	const inFractions = [ 0, 0.3, 0.8, 0.2, 0.65 ];

	for ( let i = 0; i !== nKeys; ++ i ) {

		if ( i === 0 ) {

			inTangents[ 0 ] = times[ 0 ];
			inTangents[ 1 ] = values[ 0 ];

		} else {

			const interval = times[ i ] - times[ i - 1 ];
			inTangents[ i * 2 ] = times[ i - 1 ] + inFractions[ i ] * interval;
			inTangents[ i * 2 + 1 ] = values[ i ] - 1.5;

		}

		if ( i === nKeys - 1 ) {

			outTangents[ i * 2 ] = times[ i ];
			outTangents[ i * 2 + 1 ] = values[ i ];

		} else {

			const interval = times[ i + 1 ] - times[ i ];
			outTangents[ i * 2 ] = times[ i ] + outFractions[ i ] * interval;
			outTangents[ i * 2 + 1 ] = values[ i ] + 2.0;

		}

	}

	track.settings = { inTangents, outTangents };

	return track;

}

function buildMultiTrackClip() {

	const tracks = [
		new NumberKeyframeTrack(
			'.position[x]',
			[ 0, 1.3, 2.7, 4.1, 6 ],
			[ 0.5, 2.25, - 1.5, 3.75, 1.0 ],
			InterpolateLinear
		),
		new NumberKeyframeTrack(
			'.position[y]',
			[ 0.5, 1.8, 3.3, 5.0 ],
			[ 10, 20, 30, 40 ],
			InterpolateDiscrete
		),
		makeBezierTrack(
			'.position[z]',
			[ 0, 1.5, 3.2, 5.0, 6 ],
			[ 0, 4, - 3, 2.5, - 1 ]
		),
		new VectorKeyframeTrack(
			'.scale',
			[ 0.2, 1.1, 2.6, 4.4, 5.8 ],
			[
				1, 1, 1,
				1.5, 0.75, 1.25,
				0.5, 2.0, 1.0,
				2.0, 1.25, 0.75,
				1.1, 1.1, 1.1
			],
			InterpolateSmooth
		),
		new ColorKeyframeTrack(
			'.material.color',
			[ 0, 2, 4, 6 ],
			[
				1, 0, 0,
				0, 1, 0,
				0, 0, 1,
				1, 1, 0
			],
			InterpolateLinear
		),
		new QuaternionKeyframeTrack(
			'.quaternion',
			[ 0, 1.2, 2.8, 4.6, 6 ],
			[
				0, 0, 0, 1,
				0.7071, 0, 0, 0.7071,
				- 0.7071, - 0, - 0, - 0.7071, // antipode of the previous key
				0, 0.7071, 0, 0.7071,
				0, 0, 1, 0
			]
		),
		new BooleanKeyframeTrack(
			'.visible',
			[ 0, 1, 2.5, 4.5, 6 ],
			[ true, false, true, false, true ]
		),
		new StringKeyframeTrack(
			'.name',
			[ 0.3, 1.7, 3.9, 5.2 ],
			[ 'alpha', 'beta', 'gamma', 'delta' ]
		)
	];

	return new AnimationClip( 'multiTrack', 6, tracks );

}

const CUTS = [
	{ name: 'wide interior cut', startTime: 0.7, endTime: 5.3 },
	{ name: 'cut landing on linear keys', startTime: 1.3, endTime: 2.7 },
	{ name: 'cut without internal keys', startTime: 3.55, endTime: 3.95 },
	{ name: 'cut starting at zero', startTime: 0, endTime: 2 },
	{ name: 'cut ending at the duration', startTime: 4.9, endTime: 6 }
];

test( 'exact-curves', () => {

	const clip = buildMultiTrackClip();

	for ( const cutSpec of CUTS ) {

		const cut = AnimationUtils.subclipExact(
			clip,
			`cut-${ cutSpec.name }`,
			cutSpec.startTime,
			cutSpec.endTime
		);

		// every source track survives, even without keyframes in the interval
		assert.equal( cut.tracks.length, clip.tracks.length, cutSpec.name );
		assert.deepEqual(
			cut.tracks.map( ( track ) => track.name ),
			clip.tracks.map( ( track ) => track.name ),
			cutSpec.name
		);

		// the clip origin is startTime and the duration is exact
		assert.equal( cut.duration, cutSpec.endTime - cutSpec.startTime, cutSpec.name );

		for ( let i = 0; i !== clip.tracks.length; ++ i ) {

			const track = clip.tracks[ i ];
			const cutTrack = cut.tracks[ i ];

			assert.equal( cutTrack.name, track.name );

			// the cut owns its arrays, nothing is shared with the source
			assert.notEqual( cutTrack.times, track.times );
			assert.notEqual( cutTrack.values, track.values );

			if ( track.settings !== undefined && track.settings.inTangents !== undefined ) {

				assert.ok( cutTrack.settings !== undefined, cutSpec.name );
				assert.notEqual( cutTrack.settings.inTangents, track.settings.inTangents );
				assert.notEqual( cutTrack.settings.outTangents, track.settings.outTangents );

			}

			assertTrackMatchesSource( track, cutTrack, cutSpec.startTime, cutSpec.endTime );

			// explicit boundary checks, including the closed interval ends
			const sourceAtStart = track.createInterpolant().evaluate( cutSpec.startTime );
			const sourceAtEnd = track.createInterpolant().evaluate( cutSpec.endTime );
			const cutAtStart = cutTrack.createInterpolant().evaluate( 0 );
			const cutAtEnd = cutTrack.createInterpolant().evaluate( cut.duration );

			if ( track.ValueTypeName === 'bool' || track.ValueTypeName === 'string' ) {

				assert.strictEqual( cutAtStart[ 0 ], sourceAtStart[ 0 ] );
				assert.strictEqual( cutAtEnd[ 0 ], sourceAtEnd[ 0 ] );

			} else if ( track.ValueTypeName === 'quaternion' ) {

				assert.ok( quaternionPoseError( cutAtStart, sourceAtStart ) <= QUATERNION_TOLERANCE );
				assert.ok( quaternionPoseError( cutAtEnd, sourceAtEnd ) <= QUATERNION_TOLERANCE );

			} else {

				for ( let c = 0; c !== sourceAtStart.length; ++ c ) {

					assert.ok( numericError( cutAtStart[ c ], sourceAtStart[ c ] ) <= NUMERIC_TOLERANCE );
					assert.ok( numericError( cutAtEnd[ c ], sourceAtEnd[ c ] ) <= NUMERIC_TOLERANCE );

				}

			}

		}

	}

	// a discrete key exactly on the start/end cuts takes the key's new value
	const discreteTrack = new NumberKeyframeTrack(
		'.position[y]',
		[ 0, 1, 2, 3 ],
		[ 10, 20, 30, 40 ],
		InterpolateDiscrete
	);
	const discreteClip = new AnimationClip( 'discrete', 3, [ discreteTrack ] );
	const discreteCut = AnimationUtils.subclipExact( discreteClip, 'discreteCut', 1, 3 );
	const discreteInterpolant = discreteCut.tracks[ 0 ].createInterpolant();

	assert.equal( discreteInterpolant.evaluate( 0 )[ 0 ], 20 );
	assert.equal( discreteInterpolant.evaluate( 3 - 1 )[ 0 ], 40 );
	assert.equal( discreteInterpolant.evaluate( 0.999 )[ 0 ], 20 );
	assert.equal( discreteInterpolant.evaluate( 1 )[ 0 ], 30 );

	// the Bezier cut keeps its non-uniform 2D control points instead of resampling
	const bezierCut = AnimationUtils.subclipExact( clip, 'bezierCut', 0.7, 5.3 );
	const bezierTrack = bezierCut.tracks.find( ( track ) => track.name === '.position[z]' );

	assert.equal( bezierTrack.getInterpolation(), InterpolateBezier );
	assert.ok( bezierTrack.settings.inTangents instanceof Float32Array );
	assert.ok( bezierTrack.times.length <= clip.tracks[ 2 ].times.length + 1 );

} );

test( 'smooth-context', () => {

	// strongly non-uniform key spacing, so neighbor influence is not the
	// uniform-grid special case
	const smoothTrack = new NumberKeyframeTrack(
		'.position[x]',
		[ 0, 0.4, 1.7, 2.0, 5.0, 5.6 ],
		[ 2, - 3, 1.5, 4, - 2, 0.8 ],
		InterpolateSmooth
	);
	const smoothClip = new AnimationClip( 'smooth', 5.6, [ smoothTrack ] );

	const startTime = 0.9;
	const endTime = 4.7;
	const cut = AnimationUtils.subclipExact( smoothClip, 'smoothCut', startTime, endTime );
	const cutTrack = cut.tracks[ 0 ];

	assert.ok( cutTrack instanceof CubicSegmentKeyframeTrack );

	// keys outside the cut must not be copied into the cut, yet their shape
	// influence survives through the stored cubic segments
	for ( const time of cutTrack.times ) {

		assert.ok( time >= 0 && time <= endTime - startTime );

	}

	assertTrackMatchesSource( smoothTrack, cutTrack, startTime, endTime );

	// dense sampling very close to the cut positions, where dropping the
	// outside neighbors would be most visible
	const sourceInterpolant = smoothTrack.createInterpolant();
	const cutInterpolant = cutTrack.createInterpolant();

	for ( const offset of [ 1e-2, 1e-3, 1e-4, 1e-5 ] ) {

		const expectedStart = sourceInterpolant.evaluate( startTime + offset )[ 0 ];
		const actualStart = cutInterpolant.evaluate( offset )[ 0 ];
		assert.ok( numericError( actualStart, expectedStart ) <= NUMERIC_TOLERANCE );

		const expectedEnd = sourceInterpolant.evaluate( endTime - offset )[ 0 ];
		const actualEnd = cutInterpolant.evaluate( endTime - startTime - offset )[ 0 ];
		assert.ok( numericError( actualEnd, expectedEnd ) <= NUMERIC_TOLERANCE );

	}

	// a two-key smooth track is curved (not a straight line) under
	// ZeroCurvatureEnding, and the cut must reproduce that exact curve
	const twoKeyTrack = new NumberKeyframeTrack(
		'.position[x]',
		[ 1, 3 ],
		[ 5, - 5 ],
		InterpolateSmooth
	);
	const twoKeyClip = new AnimationClip( 'twoKey', 4, [ twoKeyTrack ] );
	const twoKeyCut = AnimationUtils.subclipExact( twoKeyClip, 'twoKeyCut', 0.2, 3.0 );

	assertTrackMatchesSource( twoKeyTrack, twoKeyCut.tracks[ 0 ], 0.2, 3.0 );

	// cutting inside a single interval of a smooth track also stays exact
	const narrowCut = AnimationUtils.subclipExact( smoothClip, 'narrow', 1.75, 1.95 );
	assertTrackMatchesSource( smoothTrack, narrowCut.tracks[ 0 ], 1.75, 1.95 );

} );

test( 'quaternion-discrete', () => {

	const quaternionTrack = new QuaternionKeyframeTrack(
		'.quaternion',
		[ 0, 1.2, 2.8, 4.6, 6 ],
		[
			0, 0, 0, 1,
			0.7071, 0, 0, 0.7071,
			- 0.7071, 0, 0, - 0.7071, // antipode: shortest-arc slerp must pass through it
			0, 0.7071, 0, 0.7071,
			0, 0, 1, 0
		]
	);
	const quaternionClip = new AnimationClip( 'quaternion', 6, [ quaternionTrack ] );

	for ( const cutSpec of [
		{ startTime: 0.6, endTime: 5.4 },
		{ startTime: 2.0, endTime: 3.6 }, // crosses the antipodal key
		{ startTime: 3.0, endTime: 3.5 } // no internal key, held end value
	] ) {

		const cut = AnimationUtils.subclipExact(
			quaternionClip,
			'quaternionCut',
			cutSpec.startTime,
			cutSpec.endTime
		);

		assertTrackMatchesSource( quaternionTrack, cut.tracks[ 0 ], cutSpec.startTime, cutSpec.endTime );

	}

	// discrete quaternions keep the staircase behavior and take the new
	// value on keys that coincide with the cuts
	const discreteQuaternions = new QuaternionKeyframeTrack(
		'.quaternion',
		[ 0, 1, 2 ],
		[
			0, 0, 0, 1,
			1, 0, 0, 0,
			0, 1, 0, 0
		],
		InterpolateDiscrete
	);
	const discreteClip = new AnimationClip( 'discreteQuaternion', 2, [ discreteQuaternions ] );
	const discreteCut = AnimationUtils.subclipExact( discreteClip, 'cut', 1, 2 );
	const sourceInterpolant = discreteQuaternions.createInterpolant();
	const cutInterpolant = discreteCut.tracks[ 0 ].createInterpolant();

	for ( const localTime of createSampleTimes( [ 0, 1, 2 ], 1, 2, InterpolateDiscrete ) ) {

		const error = quaternionPoseError(
			cutInterpolant.evaluate( localTime ),
			sourceInterpolant.evaluate( 1 + localTime )
		);

		assert.ok( error <= QUATERNION_TOLERANCE );

	}

	assert.ok(
		quaternionPoseError( cutInterpolant.evaluate( 0 ), sourceInterpolant.evaluate( 1 ) ) <= QUATERNION_TOLERANCE
	);
	assert.ok(
		quaternionPoseError( cutInterpolant.evaluate( 1 ), sourceInterpolant.evaluate( 2 ) ) <= QUATERNION_TOLERANCE
	);

} );

test( 'mixer-roundtrip', () => {

	const clip = buildMultiTrackClip();
	const startTime = 0.7;
	const endTime = 5.3;
	const cut = AnimationUtils.subclipExact( clip, 'cut', startTime, endTime );

	// real JSON serialization round trip
	const json = JSON.parse( JSON.stringify( AnimationClip.toJSON( cut ) ) );
	const parsedCut = AnimationClip.parse( json );

	assert.equal( parsedCut.duration, cut.duration );
	assert.equal( parsedCut.tracks.length, cut.tracks.length );

	const smoothCutTrack = cut.tracks.find( ( track ) => track.name === '.scale' );
	const parsedSmoothTrack = parsedCut.tracks.find( ( track ) => track.name === '.scale' );
	assert.ok( parsedSmoothTrack instanceof CubicSegmentKeyframeTrack );
	assertTrackMatchesSource(
		clip.tracks.find( ( track ) => track.name === '.scale' ),
		parsedSmoothTrack,
		startTime,
		endTime
	);

	// clone keeps the curve and owns its arrays
	const clonedCut = cut.clone();
	const clonedSmoothTrack = clonedCut.tracks.find( ( track ) => track.name === '.scale' );
	assert.ok( clonedSmoothTrack instanceof CubicSegmentKeyframeTrack );
	assert.notEqual( clonedSmoothTrack.coefficients, smoothCutTrack.coefficients );
	assertTrackMatchesSource(
		clip.tracks.find( ( track ) => track.name === '.scale' ),
		clonedSmoothTrack,
		startTime,
		endTime
	);

	// actual mixer playback on Object3D roots; the plain material object is
	// bindable by the color track (Color has fromArray/toArray)
	const sourceRoot = new Object3D();
	const cutRoot = new Object3D();
	const parsedRoot = new Object3D();
	const cloneRoot = new Object3D();

	for ( const root of [ sourceRoot, cutRoot, parsedRoot, cloneRoot ] ) {

		root.material = { color: new Color() };

	}

	const sourceMixer = new AnimationMixer( sourceRoot );
	const cutMixer = new AnimationMixer( cutRoot );
	const parsedMixer = new AnimationMixer( parsedRoot );
	const cloneMixer = new AnimationMixer( cloneRoot );

	function configureAction( action ) {

		action.loop = LoopOnce;
		action.clampWhenFinished = true;
		action.zeroSlopeAtStart = false;
		action.zeroSlopeAtEnd = false;
		action.play();

	}

	configureAction( sourceMixer.clipAction( clip ) );
	configureAction( cutMixer.clipAction( cut ) );
	configureAction( parsedMixer.clipAction( parsedCut ) );
	configureAction( cloneMixer.clipAction( clonedCut ) );

	const deltas = [ 0.131, 0.097, 0.214, 0.055, 0.188, 0.142, 0.073, 0.166, 0.119, 0.203, 0.317, 0.256, 0.411, 0.364, 0.509, 0.488, 0.527, 0.442 ];
	let elapsed = 0;

	for ( const delta of deltas ) {

		sourceMixer.update( delta );
		cutMixer.update( delta );
		parsedMixer.update( delta );
		cloneMixer.update( delta );

		elapsed += delta;
		const localTime = Math.min( elapsed, cut.duration );
		const sourceTime = startTime + localTime;

		// source action sampled at startTime + t, cut actions sampled at t
		const sourceScale = clip.tracks.find( ( track ) => track.name === '.scale' )
			.createInterpolant().evaluate( sourceTime );
		const sourceX = clip.tracks.find( ( track ) => track.name === '.position[x]' )
			.createInterpolant().evaluate( sourceTime )[ 0 ];
		const sourceQuaternion = clip.tracks.find( ( track ) => track.name === '.quaternion' )
			.createInterpolant().evaluate( sourceTime );
		const sourceVisible = clip.tracks.find( ( track ) => track.name === '.visible' )
			.createInterpolant().evaluate( sourceTime )[ 0 ];
		const sourceName = clip.tracks.find( ( track ) => track.name === '.name' )
			.createInterpolant().evaluate( sourceTime )[ 0 ];
		const sourceColor = clip.tracks.find( ( track ) => track.name === '.material.color' )
			.createInterpolant().evaluate( sourceTime );

		for ( const root of [ cutRoot, parsedRoot, cloneRoot ] ) {

			assert.ok( numericError( root.position.x, sourceX ) <= NUMERIC_TOLERANCE );

			const actualColor = [ root.material.color.r, root.material.color.g, root.material.color.b ];

			for ( let c = 0; c !== 3; ++ c ) {

				assert.ok(
					numericError( root.scale.getComponent( c ), sourceScale[ c ] ) <= NUMERIC_TOLERANCE
				);
				assert.ok(
					numericError( actualColor[ c ], sourceColor[ c ] ) <= NUMERIC_TOLERANCE
				);

			}

			const poseError = quaternionPoseError(
				[ root.quaternion.x, root.quaternion.y, root.quaternion.z, root.quaternion.w ],
				sourceQuaternion
			);
			assert.ok( poseError <= QUATERNION_TOLERANCE );

			assert.strictEqual( root.visible, sourceVisible );
			assert.strictEqual( root.name, sourceName );

		}

	}

	// clampWhenFinished keeps the end pose after the clip has finished
	cutMixer.update( 1.0 );
	parsedMixer.update( 1.0 );
	cloneMixer.update( 1.0 );

	const endSource = clip.tracks.find( ( track ) => track.name === '.position[x]' )
		.createInterpolant().evaluate( endTime )[ 0 ];

	for ( const root of [ cutRoot, parsedRoot, cloneRoot ] ) {

		assert.ok( numericError( root.position.x, endSource ) <= NUMERIC_TOLERANCE );

	}

	// the blend mode is part of the standalone clip and survives the cut
	const additiveClip = new AnimationClip(
		'additive',
		2,
		[ new NumberKeyframeTrack( '.position[x]', [ 0, 1, 2 ], [ 0, 1, 2 ] ) ],
		AdditiveAnimationBlendMode
	);
	const additiveCut = AnimationUtils.subclipExact( additiveClip, 'additiveCut', 0.4, 1.6 );

	assert.equal( additiveCut.blendMode, AdditiveAnimationBlendMode );
	assert.notEqual( additiveCut.blendMode, NormalAnimationBlendMode );

	const additiveRoot = new Object3D();
	const additiveMixer = new AnimationMixer( additiveRoot );
	const additiveAction = additiveMixer.clipAction( additiveCut );
	configureAction( additiveAction );
	additiveMixer.update( 0.5 );
	assert.ok( Number.isFinite( additiveRoot.position.x ) );

} );

test( 'invalid-and-ownership', () => {

	const clip = buildMultiTrackClip();

	// parameter validation
	function assertThrows( callback, messageFragment ) {

		assert.throws( callback, ( error ) => {

			assert.ok( error instanceof Error );
			assert.ok( error.message.length > 0 );

			if ( messageFragment !== undefined ) {

				assert.ok( error.message.includes( messageFragment ) );

			}

			return true;

		} );

	}

	assertThrows( () => AnimationUtils.subclipExact( clip, 'x', - 1, 2 ) );
	assertThrows( () => AnimationUtils.subclipExact( clip, 'x', 2, 2 ) );
	assertThrows( () => AnimationUtils.subclipExact( clip, 'x', 2, 1 ) );
	assertThrows( () => AnimationUtils.subclipExact( clip, 'x', 0, clip.duration + 0.1 ) );
	assertThrows( () => AnimationUtils.subclipExact( clip, 'x', NaN, 2 ) );
	assertThrows( () => AnimationUtils.subclipExact( clip, 'x', 0, Infinity ) );
	assertThrows( () => AnimationUtils.subclipExact( clip, 'x', - Infinity, 1 ) );
	assertThrows( () => AnimationUtils.subclipExact( null, 'x', 0, 1 ) );
	assertThrows( () => AnimationUtils.subclipExact( {}, 'x', 0, 1 ) );

	// invalid input must not be modified
	const track = clip.tracks[ 0 ];
	const timesSnapshot = Array.from( track.times );
	const valuesSnapshot = Array.from( track.values );

	try {

		AnimationUtils.subclipExact( clip, 'x', 3, 2 );

	} catch {}

	assert.deepEqual( Array.from( track.times ), timesSnapshot );
	assert.deepEqual( Array.from( track.values ), valuesSnapshot );

	// user track subclasses are rejected instead of being silently degraded
	class CustomTrack extends NumberKeyframeTrack {

		constructor( ...args ) {

			super( ...args );

		}

	}

	const customTrack = new CustomTrack( '.position[x]', [ 0, 1 ], [ 0, 1 ] );
	const customClip = new AnimationClip( 'custom', 1, [ customTrack ] );
	assertThrows( () => AnimationUtils.subclipExact( customClip, 'x', 0.2, 0.8 ) );

	// loader-style custom createInterpolant is rejected, never downgraded to linear
	const hackedTrack = new NumberKeyframeTrack( '.position[x]', [ 0, 1, 2 ], [ 0, 1, 2 ] );
	const originalFactory = hackedTrack.createInterpolant;

	hackedTrack.createInterpolant = function ( result ) {

		return originalFactory.call( this, result );

	};

	const hackedClip = new AnimationClip( 'hacked', 2, [ hackedTrack ] );
	assertThrows( () => AnimationUtils.subclipExact( hackedClip, 'x', 0.2, 1.8 ) );

	// GLTF cubic spline markers are also rejected
	const gltfTrack = new NumberKeyframeTrack( '.position[x]', [ 0, 1, 2 ], [ 0, 1, 2 ] );
	const gltfFactory = function ( result ) {

		return originalFactory.call( this, result );

	};
	gltfFactory.isInterpolantFactoryMethodGLTFCubicSpline = true;
	gltfTrack.createInterpolant = gltfFactory;
	const gltfClip = new AnimationClip( 'gltf', 2, [ gltfTrack ] );
	assertThrows( () => AnimationUtils.subclipExact( gltfClip, 'x', 0.2, 1.8 ) );

	// mutation isolation between source, cut and sibling cuts
	const sourceTrack = new NumberKeyframeTrack(
		'.position[x]',
		[ 0, 0.7, 2.1, 4, 5.5 ],
		[ 1, 3, - 2, 0.5, 4 ],
		InterpolateSmooth
	);
	const sourceClip = new AnimationClip( 'ownership', 5.5, [ sourceTrack ] );
	const sourceInterpolant = sourceTrack.createInterpolant();
	const expectedAtTwo = sourceInterpolant.evaluate( 2.0 )[ 0 ];

	const cutA = AnimationUtils.subclipExact( sourceClip, 'a', 1.23, 4.8 );
	const cutB = AnimationUtils.subclipExact( sourceClip, 'b', 1.0, 2.0 );
	const cutBValue = cutB.tracks[ 0 ].createInterpolant().evaluate( 0.5 )[ 0 ];

	cutA.tracks[ 0 ].times[ 0 ] = 99;
	cutA.tracks[ 0 ].values[ 0 ] = 99;
	cutA.tracks[ 0 ].coefficients[ 0 ] = 99;

	assert.equal( sourceInterpolant.evaluate( 2.0 )[ 0 ], expectedAtTwo );
	assert.equal( cutB.tracks[ 0 ].createInterpolant().evaluate( 0.5 )[ 0 ], cutBValue );

	// mutating the source after cutting does not reach the cut
	const cutAValueBefore = cutA.tracks[ 0 ].createInterpolant().evaluate( 1.0 )[ 0 ];
	sourceTrack.times[ 1 ] = 99;
	sourceTrack.values[ 1 ] = 99;
	assert.equal( cutA.tracks[ 0 ].createInterpolant().evaluate( 1.0 )[ 0 ], cutAValueBefore );

	// Bezier tangent arrays are independently owned as well
	const bezierSource = makeBezierTrack( '.position[z]', [ 0, 1.5, 3.2, 5 ], [ 0, 4, - 3, 2.5 ] );
	const bezierClip = new AnimationClip( 'bezierOwnership', 5, [ bezierSource ] );
	const bezierCut = AnimationUtils.subclipExact( bezierClip, 'bezierCut', 0.6, 4.4 );
	const bezierValue = bezierCut.tracks[ 0 ].createInterpolant().evaluate( 1.0 )[ 0 ];

	bezierCut.tracks[ 0 ].settings.inTangents[ 0 ] = 99;
	bezierCut.tracks[ 0 ].settings.outTangents[ 1 ] = 99;

	assert.equal( bezierCut.tracks[ 0 ].createInterpolant().evaluate( 1.0 )[ 0 ], bezierValue );
	assert.notEqual( bezierSource.settings.inTangents[ 0 ], 99 );
	assert.notEqual( bezierSource.settings.outTangents[ 1 ], 99 );

} );
