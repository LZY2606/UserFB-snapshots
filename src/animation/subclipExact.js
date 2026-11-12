import {
	InterpolateLinear,
	InterpolateDiscrete,
	InterpolateSmooth,
	InterpolateBezier
} from '../constants.js';
import { AnimationClip } from './AnimationClip.js';
import { NumberKeyframeTrack } from './tracks/NumberKeyframeTrack.js';
import { VectorKeyframeTrack } from './tracks/VectorKeyframeTrack.js';
import { ColorKeyframeTrack } from './tracks/ColorKeyframeTrack.js';
import { QuaternionKeyframeTrack } from './tracks/QuaternionKeyframeTrack.js';
import { BooleanKeyframeTrack } from './tracks/BooleanKeyframeTrack.js';
import { StringKeyframeTrack } from './tracks/StringKeyframeTrack.js';
import { CubicSegmentKeyframeTrack } from './tracks/CubicSegmentKeyframeTrack.js';

const builtinTrackTypes = new Set( [
	NumberKeyframeTrack,
	VectorKeyframeTrack,
	ColorKeyframeTrack,
	QuaternionKeyframeTrack,
	BooleanKeyframeTrack,
	StringKeyframeTrack
] );

const supportedInterpolations = new Set( [
	InterpolateLinear,
	InterpolateDiscrete,
	InterpolateSmooth,
	InterpolateBezier
] );

/**
 * Creates a new clip that reproduces the segment of the source clip between
 * `startTime` and `endTime` exactly, at every point in time.
 *
 * The new clip owns all of its curve data: no arrays are shared with the
 * source clip and nothing references the source tracks. The clip time origin
 * is `startTime`, so its duration is always `endTime - startTime`. Tracks
 * whose keyframes do not cover the whole range keep working by extending
 * their end values, and tracks without any keyframe inside the range are
 * retained as constant tracks.
 *
 * Curves are never uniformly resampled: smooth and Bezier curves are split
 * analytically into equivalent cubic curve pieces.
 *
 * @param {AnimationClip} sourceClip - The clip to cut.
 * @param {string} name - The name of the new clip.
 * @param {number} startTime - Start time in seconds, inclusive.
 * @param {number} endTime - End time in seconds, inclusive.
 * @return {AnimationClip} The new, self-contained sub clip.
 */
function subclipExact( sourceClip, name, startTime, endTime ) {

	if ( sourceClip === null || typeof sourceClip !== 'object' || Array.isArray( sourceClip.tracks ) === false ) {

		throw new Error( 'THREE.AnimationUtils.subclipExact: sourceClip is not an AnimationClip' );

	}

	if ( Number.isFinite( startTime ) === false || Number.isFinite( endTime ) === false ) {

		throw new Error( 'THREE.AnimationUtils.subclipExact: startTime and endTime must be finite numbers' );

	}

	if ( startTime < 0 || startTime >= endTime || endTime > sourceClip.duration ) {

		throw new Error( 'THREE.AnimationUtils.subclipExact: requires 0 <= startTime < endTime <= sourceClip.duration' );

	}

	const duration = endTime - startTime;
	const tracks = [];

	for ( let i = 0, n = sourceClip.tracks.length; i !== n; ++ i ) {

		tracks.push( trimTrack( sourceClip.tracks[ i ], startTime, endTime, duration ) );

	}

	return new AnimationClip( name, duration, tracks, sourceClip.blendMode );

}

function trimTrack( track, startTime, endTime, duration ) {

	if ( track instanceof CubicSegmentKeyframeTrack ) {

		return trimCubicSegmentTrack( track, startTime, endTime );

	}

	if ( builtinTrackTypes.has( track.constructor ) === false ||
		track.createInterpolant === undefined ||
		track.createInterpolant.isInterpolantFactoryMethodGLTFCubicSpline === true ) {

		throw new Error( 'THREE.AnimationUtils.subclipExact: unsupported track type or custom interpolant on track "' + track.name + '"' );

	}

	const interpolation = track.getInterpolation();

	if ( supportedInterpolations.has( interpolation ) === false ) {

		throw new Error( 'THREE.AnimationUtils.subclipExact: unsupported interpolation on track "' + track.name + '"' );

	}

	if ( track.ValueTypeName === 'bool' || track.ValueTypeName === 'string' || interpolation === InterpolateDiscrete ) {

		return trimDiscreteTrack( track, startTime, endTime, duration, interpolation );

	}

	if ( interpolation === InterpolateBezier && track.settings !== undefined &&
		track.settings.inTangents !== undefined && track.settings.outTangents !== undefined ) {

		return trimBezierTrack( track, startTime, endTime );

	}

	if ( interpolation === InterpolateSmooth ) {

		return trimSmoothTrack( track, startTime, endTime, duration );

	}

	// Linear interpolation, and Bezier tracks without tangent data (which the
	// Bezier interpolant itself falls back to linear): keep the exact curve
	// pieces with anchor keyframes at the cut positions.
	return trimLinearTrack( track, startTime, endTime, interpolation );

}

/**
 * Builds the sorted source-time breakpoints inside the cut range, always
 * starting with `startTime` and ending with `endTime`. Source keyframes that
 * coincide with a cut are not duplicated.
 */
function buildBreakpoints( times, startTime, endTime ) {

	const breakpoints = [ startTime ];

	for ( let i = 0, n = times.length; i !== n; ++ i ) {

		const time = times[ i ];

		if ( time > startTime && time < endTime ) {

			breakpoints.push( time );

		}

	}

	if ( breakpoints[ breakpoints.length - 1 ] !== endTime ) {

		breakpoints.push( endTime );

	}

	return breakpoints;

}

function shiftTimes( times, startTime ) {

	const result = new Float32Array( times.length );

	for ( let i = 0, n = times.length; i !== n; ++ i ) {

		result[ i ] = times[ i ] - startTime;

	}

	return result;

}

function evaluateSource( track, time ) {

	return track.createInterpolant().evaluate( time ).slice();

}

function trimDiscreteTrack( track, startTime, endTime, duration, interpolation ) {

	const times = track.times;
	const stride = track.getValueSize();
	const ValueBufferType = track.values.constructor;

	const outTimes = [ 0 ];
	const outValues = [];

	// anchor at the clip origin; evaluating the source interpolant also gives
	// the new value when a discrete key sits exactly on the start cut
	appendValue( outValues, evaluateSource( track, startTime ) );

	let endValue = null;

	for ( let i = 0, n = times.length; i !== n; ++ i ) {

		const time = times[ i ];

		if ( time > startTime && time < endTime ) {

			outTimes.push( time - startTime );

			for ( let c = 0; c !== stride; ++ c ) {

				outValues.push( track.values[ i * stride + c ] );

			}

		} else if ( time === endTime ) {

			// a key exactly on the end cut: its value is the new step value
			endValue = track.values.slice( i * stride, ( i + 1 ) * stride );
			break;

		}

	}

	if ( endValue === null ) {

		// no key on the cut: hold whatever the source curve has at the end,
		// which also covers tracks whose keys end before the cut
		endValue = evaluateSource( track, endTime );

	}

	// the end anchor is stored at the largest Float32 time not above the
	// exact clip duration, so evaluating at the duration takes the end value
	outTimes.push( float32AtOrBelow( duration ) );
	appendValue( outValues, endValue );

	const storedTimes = Float32Array.from( outTimes );

	// round interior keys into Float32 while keeping the times strictly
	// increasing; sub-ULP collisions can only occur between a key and the
	// anchor that immediately follows it
	for ( let i = storedTimes.length - 2; i >= 1; -- i ) {

		if ( storedTimes[ i ] >= storedTimes[ i + 1 ] ) {

			storedTimes[ i ] = previousFloat32( storedTimes[ i + 1 ] );

		}

	}

	const TrackType = track.constructor;

	return new TrackType(
		track.name,
		storedTimes,
		convertValues( outValues, ValueBufferType ),
		interpolation
	);

}

/**
 * Returns the largest Float32 value that is not greater than the input.
 */
function float32AtOrBelow( value ) {

	const float32View = new Float32Array( 1 );
	const uint32View = new Uint32Array( float32View.buffer );

	float32View[ 0 ] = value;

	if ( float32View[ 0 ] <= value ) {

		return float32View[ 0 ];

	}

	return previousFloat32( float32View[ 0 ] );

}

function previousFloat32( value ) {

	const float32View = new Float32Array( 1 );
	const uint32View = new Uint32Array( float32View.buffer );

	float32View[ 0 ] = value;
	uint32View[ 0 ] -= 1;

	return float32View[ 0 ];

}

function appendValue( outValues, value ) {

	for ( let i = 0, n = value.length; i !== n; ++ i ) {

		outValues.push( value[ i ] );

	}

}

function convertValues( values, type ) {

	if ( typeof type.BYTES_PER_ELEMENT === 'number' ) {

		return new type( values );

	}

	return values;

}

function trimLinearTrack( track, startTime, endTime, interpolation ) {

	const stride = track.getValueSize();
	const breakpoints = buildBreakpoints( track.times, startTime, endTime );
	const sourceInterpolant = track.createInterpolant();

	const outTimes = shiftTimes( breakpoints, startTime );
	const outValues = new track.values.constructor( breakpoints.length * stride );

	for ( let i = 0, n = breakpoints.length; i !== n; ++ i ) {

		const value = sourceInterpolant.evaluate( breakpoints[ i ] );

		for ( let c = 0; c !== stride; ++ c ) {

			outValues[ i * stride + c ] = value[ c ];

		}

	}

	const TrackType = track.constructor;

	return new TrackType( track.name, outTimes, outValues, interpolation );

}

/**
 * Converts a smooth (Catmull-Rom, ZeroCurvatureEnding) track into equivalent
 * cubic Hermite segments and keeps only the pieces covered by the cut. The
 * derivative at every source keyframe follows the same neighbor slopes as
 * `CubicInterpolant`, so keys outside the cut keep shaping the curve near
 * the cuts.
 */
function trimSmoothTrack( track, startTime, endTime, duration ) {

	const times = track.times;
	const values = track.values;
	const stride = track.getValueSize();
	const nKeys = times.length;

	if ( nKeys === 1 ) {

		return constantTrack( track, duration, stride, values );

	}

	// first derivatives w.r.t. time at every source keyframe: the interior
	// tangents are the average of the adjacent segment slopes, the end
	// tangents (ZeroCurvatureEnding) equal the end segment slopes
	const tangents = new Float64Array( nKeys * stride );

	for ( let c = 0; c !== stride; ++ c ) {

		tangents[ c ] =
			( values[ stride + c ] - values[ c ] ) / ( times[ 1 ] - times[ 0 ] );

		tangents[ ( nKeys - 1 ) * stride + c ] =
			( values[ ( nKeys - 1 ) * stride + c ] - values[ ( nKeys - 2 ) * stride + c ] ) /
			( times[ nKeys - 1 ] - times[ nKeys - 2 ] );

	}

	for ( let k = 1; k !== nKeys - 1; ++ k ) {

		const leftSlope = new Float64Array( stride );
		const rightSlope = new Float64Array( stride );

		for ( let c = 0; c !== stride; ++ c ) {

			leftSlope[ c ] =
				( values[ k * stride + c ] - values[ ( k - 1 ) * stride + c ] ) /
				( times[ k ] - times[ k - 1 ] );

			rightSlope[ c ] =
				( values[ ( k + 1 ) * stride + c ] - values[ k * stride + c ] ) /
				( times[ k + 1 ] - times[ k ] );

			tangents[ k * stride + c ] = 0.5 * ( leftSlope[ c ] + rightSlope[ c ] );

		}

	}

	// Hermite coefficients A, B, C, D for every source segment
	const segments = new Array( nKeys - 1 );

	for ( let i = 0; i !== nKeys - 1; ++ i ) {

		const t0 = times[ i ];
		const dt = times[ i + 1 ] - t0;
		const coefficients = new Float64Array( stride * 4 );

		for ( let c = 0; c !== stride; ++ c ) {

			const v0 = values[ i * stride + c ];
			const v1 = values[ ( i + 1 ) * stride + c ];
			const m0 = tangents[ i * stride + c ];
			const m1 = tangents[ ( i + 1 ) * stride + c ];

			const offset = c * 4;

			coefficients[ offset ] = v0;
			coefficients[ offset + 1 ] = m0;
			coefficients[ offset + 2 ] = ( 3 * ( v1 - v0 ) / dt - ( 2 * m0 + m1 ) ) / dt;
			coefficients[ offset + 3 ] = ( 2 * ( v0 - v1 ) / dt + ( m0 + m1 ) ) / dt / dt;

		}

		segments[ i ] = { t0, t1: times[ i + 1 ], coefficients };

	}

	return buildCubicSegmentTrack( track, segments, times, startTime, endTime, stride );

}

function trimCubicSegmentTrack( track, startTime, endTime ) {

	const times = track.times;
	const stride = track.getValueSize();
	const segments = new Array( times.length - 1 );

	for ( let i = 0; i !== times.length - 1; ++ i ) {

		segments[ i ] = {
			t0: times[ i ],
			t1: times[ i + 1 ],
			coefficients: Float64Array.from( track.coefficients.subarray( i * stride * 4, ( i + 1 ) * stride * 4 ) )
		};

	}

	return buildCubicSegmentTrack( track, segments, times, startTime, endTime, stride );

}

/**
 * Splits the given cubic segments at the cut positions and assembles a
 * self-contained CubicSegmentKeyframeTrack. Pieces outside the source key
 * range are constant because the source interpolant clamps its end values.
 */
function buildCubicSegmentTrack( track, segments, sourceTimes, startTime, endTime, stride ) {

	const breakpoints = buildBreakpoints( sourceTimes, startTime, endTime );
	const outTimes = shiftTimes( breakpoints, startTime );
	const outValues = new Float32Array( breakpoints.length * stride );
	const outCoefficients = new Float32Array( ( breakpoints.length - 1 ) * stride * 4 );

	const domainStart = sourceTimes[ 0 ];
	const domainEnd = sourceTimes[ sourceTimes.length - 1 ];

	let segmentIndex = 0;

	for ( let p = 0; p !== breakpoints.length - 1; ++ p ) {

		const pieceStart = breakpoints[ p ];
		const pieceEnd = breakpoints[ p + 1 ];
		const dstBase = p * stride * 4;

		if ( pieceEnd <= domainStart || pieceStart >= domainEnd ) {

			const valueIndex = pieceEnd <= domainStart ? 0 : ( sourceTimes.length - 1 ) * stride;

			// source interior breakpoints split the domain boundary, so a piece
			// never straddles it; fill the whole piece with one held value
			for ( let c = 0; c !== stride; ++ c ) {

				const offset = dstBase + c * 4;

				outCoefficients[ offset ] = track.values[ valueIndex + c ];
				outCoefficients[ offset + 1 ] = 0;
				outCoefficients[ offset + 2 ] = 0;
				outCoefficients[ offset + 3 ] = 0;

			}

		} else {

			while ( segments[ segmentIndex ].t1 <= pieceStart ) {

				++ segmentIndex;

			}

			const segment = segments[ segmentIndex ];

			// split at the piece origin, then at the piece end, both relative
			// to the source segment origin
			const piece = splitCubicSegment(
				segment.coefficients,
				stride,
				pieceStart - segment.t0,
				pieceEnd - segment.t0
			);

			for ( let c = 0; c !== stride; ++ c ) {

				const srcOffset = c * 4;
				const dstOffset = dstBase + c * 4;

				outCoefficients[ dstOffset ] = piece[ srcOffset ];
				outCoefficients[ dstOffset + 1 ] = piece[ srcOffset + 1 ];
				outCoefficients[ dstOffset + 2 ] = piece[ srcOffset + 2 ];
				outCoefficients[ dstOffset + 3 ] = piece[ srcOffset + 3 ];

			}

		}

		for ( let c = 0; c !== stride; ++ c ) {

			outValues[ p * stride + c ] = outCoefficients[ dstBase + c * 4 ];

		}

	}

	// value at the final breakpoint
	const lastPieceBase = ( breakpoints.length - 2 ) * stride * 4;

	for ( let c = 0; c !== stride; ++ c ) {

		const offset = lastPieceBase + c * 4;
		const h = outTimes[ outTimes.length - 1 ] - outTimes[ outTimes.length - 2 ];

		const a = outCoefficients[ offset ];
		const b = outCoefficients[ offset + 1 ];
		const c2 = outCoefficients[ offset + 2 ];
		const d = outCoefficients[ offset + 3 ];

		outValues[ ( breakpoints.length - 1 ) * stride + c ] =
			a + h * ( b + h * ( c2 + h * d ) );

	}

	return new CubicSegmentKeyframeTrack( track.name, outTimes, outValues, outCoefficients );

}

/**
 * Returns the coefficients of the sub-segment `[ from, to ]` of a cubic
 * segment whose local time origin is the segment start. Splitting is purely
 * algebraic, so the curve shape is preserved exactly.
 */
function splitCubicSegment( coefficients, stride, from, to ) {

	const result = new Float64Array( stride * 4 );

	for ( let c = 0; c !== stride; ++ c ) {

		const offset = c * 4;

		const a = coefficients[ offset ];
		const b = coefficients[ offset + 1 ];
		const c2 = coefficients[ offset + 2 ];
		const d = coefficients[ offset + 3 ];

		const valueAt = ( h ) => a + h * ( b + h * ( c2 + h * d ) );
		const slopeAt = ( h ) => b + h * ( 2 * c2 + 3 * d * h );

		const startValue = valueAt( from );
		const startSlope = slopeAt( from );
		const endValue = valueAt( to );
		const endSlope = slopeAt( to );

		const dt = to - from;
		const dst = c * 4;

		result[ dst ] = startValue;
		result[ dst + 1 ] = startSlope;
		result[ dst + 2 ] = ( 3 * ( endValue - startValue ) / dt - ( 2 * startSlope + endSlope ) ) / dt;
		result[ dst + 3 ] = ( 2 * ( startValue - endValue ) / dt + ( startSlope + endSlope ) ) / dt / dt;

	}

	return result;

}

/**
 * Trims a Bezier track with explicit 2D control points. Each affected cubic
 * Bezier segment is split with de Casteljau's algorithm, which preserves the
 * exact time/value curve shape including non-uniform control point times.
 */
function trimBezierTrack( track, startTime, endTime ) {

	const times = track.times;
	const values = track.values;
	const stride = track.getValueSize();
	const inTangents = track.settings.inTangents;
	const outTangents = track.settings.outTangents;
	const tangentStride = stride * 2;

	// collect the sub-curves intersecting the cut; every piece keeps its
	// four 2D control points per component
	const pieces = [];

	for ( let i = 0, n = times.length - 1; i !== n; ++ i ) {

		const t0 = times[ i ];
		const t1 = times[ i + 1 ];

		if ( t1 <= startTime || t0 >= endTime ) {

			continue;

		}

		const piece = {
			t0: Math.max( t0, startTime ),
			t1: Math.min( t1, endTime ),
			controls: new Array( stride )
		};

		for ( let c = 0; c !== stride; ++ c ) {

			const outOffset = i * tangentStride + c * 2;
			const inOffset = ( i + 1 ) * tangentStride + c * 2;

			const points = [
				[ t0, values[ i * stride + c ] ],
				[ outTangents[ outOffset ], outTangents[ outOffset + 1 ] ],
				[ inTangents[ inOffset ], inTangents[ inOffset + 1 ] ],
				[ t1, values[ ( i + 1 ) * stride + c ] ]
			];

			let remaining = points;

			if ( piece.t0 > t0 ) {

				const s = solveBezierParameter( piece.t0, points );
				remaining = splitBezier( points, s ).right;

			}

			if ( piece.t1 < t1 ) {

				const s = solveBezierParameter( piece.t1, remaining );
				remaining = splitBezier( remaining, s ).left;

			}

			piece.controls[ c ] = remaining;

		}

		pieces.push( piece );

	}

	// ranges without source segments hold the clamped end value
	if ( pieces.length === 0 ) {

		const value = evaluateSource( track, startTime );
		pieces.push( constantBezierPiece( startTime, endTime, value, stride ) );

	} else {

		const firstPiece = pieces[ 0 ];

		if ( firstPiece.t0 > startTime ) {

			const value = [];

			for ( let c = 0; c !== stride; ++ c ) {

				value.push( firstPiece.controls[ c ][ 0 ][ 1 ] );

			}

			pieces.unshift( constantBezierPiece( startTime, firstPiece.t0, value, stride ) );

		}

		const lastPiece = pieces[ pieces.length - 1 ];

		if ( lastPiece.t1 < endTime ) {

			const value = [];

			for ( let c = 0; c !== stride; ++ c ) {

				value.push( lastPiece.controls[ c ][ 3 ][ 1 ] );

			}

			pieces.push( constantBezierPiece( lastPiece.t1, endTime, value, stride ) );

		}

	}

	// stitch the pieces into keyframes with in/out tangents
	const nKeys = pieces.length + 1;
	const outTimes = new Float32Array( nKeys );
	const outValues = new track.values.constructor( nKeys * stride );
	const outInTangents = new Float32Array( nKeys * tangentStride );
	const outOutTangents = new Float32Array( nKeys * tangentStride );

	for ( let p = 0; p !== pieces.length; ++ p ) {

		outTimes[ p ] = pieces[ p ].t0 - startTime;
		outTimes[ p + 1 ] = pieces[ p ].t1 - startTime;

		for ( let c = 0; c !== stride; ++ c ) {

			const controls = pieces[ p ].controls[ c ];

			outValues[ p * stride + c ] = controls[ 0 ][ 1 ];
			outValues[ ( p + 1 ) * stride + c ] = controls[ 3 ][ 1 ];

			// out tangent of the piece start key (control point P1)
			const outOffset = ( p * stride + c ) * 2;
			outOutTangents[ outOffset ] = controls[ 1 ][ 0 ] - startTime;
			outOutTangents[ outOffset + 1 ] = controls[ 1 ][ 1 ];

			// in tangent of the piece end key (control point P2)
			const inOffset = ( ( p + 1 ) * stride + c ) * 2;
			outInTangents[ inOffset ] = controls[ 2 ][ 0 ] - startTime;
			outInTangents[ inOffset + 1 ] = controls[ 2 ][ 1 ];

		}

	}

	// unused tangent slots point at their own keyframes, keeping the data valid
	for ( let c = 0; c !== stride; ++ c ) {

		const firstIn = c * 2;
		outInTangents[ firstIn ] = outTimes[ 0 ];
		outInTangents[ firstIn + 1 ] = outValues[ c ];

		const lastOut = ( ( nKeys - 1 ) * stride + c ) * 2;
		outOutTangents[ lastOut ] = outTimes[ nKeys - 1 ];
		outOutTangents[ lastOut + 1 ] = outValues[ ( nKeys - 1 ) * stride + c ];

	}

	const TrackType = track.constructor;
	const result = new TrackType( track.name, outTimes, outValues, InterpolateBezier );

	result.settings = {
		inTangents: outInTangents,
		outTangents: outOutTangents
	};

	return result;

}

/**
 * Builds a degenerate Bezier piece whose value is constant over the range.
 */
function constantBezierPiece( t0, t1, value, stride ) {

	const piece = { t0, t1, controls: new Array( stride ) };

	for ( let c = 0; c !== stride; ++ c ) {

		const y = value[ c ];

		piece.controls[ c ] = [
			[ t0, y ],
			[ t0, y ],
			[ t1, y ],
			[ t1, y ]
		];

	}

	return piece;

}

function constantTrack( track, duration, stride, values ) {

	const outTimes = new Float32Array( [ 0, duration ] );
	const outValues = new track.values.constructor( stride * 2 );

	for ( let i = 0; i !== 2; ++ i ) {

		for ( let c = 0; c !== stride; ++ c ) {

			outValues[ i * stride + c ] = values[ c ];

		}

	}

	const TrackType = track.constructor;

	return new TrackType( track.name, outTimes, outValues, InterpolateLinear );

}

/**
 * Splits a 2D cubic Bezier defined by four points at parameter s.
 *
 * @return {Object} The two sub-curves in `left` and `right` point arrays.
 */
function splitBezier( points, s ) {

	const q1 = lerpPoint( points[ 0 ], points[ 1 ], s );
	const r1 = lerpPoint( points[ 1 ], points[ 2 ], s );
	const r2 = lerpPoint( points[ 2 ], points[ 3 ], s );
	const q2 = lerpPoint( q1, r1, s );
	const q1r = lerpPoint( r1, r2, s );
	const q3 = lerpPoint( q2, q1r, s );

	return {
		left: [ points[ 0 ], q1, q2, q3 ],
		right: [ q3, q1r, r2, points[ 3 ] ]
	};

}

function lerpPoint( a, b, s ) {

	const k = 1 - s;

	return [
		a[ 0 ] * k + b[ 0 ] * s,
		a[ 1 ] * k + b[ 1 ] * s
	];

}

/**
 * Finds the Bezier curve parameter at which the X (time) coordinate equals
 * the requested time. The control point times are not assumed to be uniform.
 */
function solveBezierParameter( x, points ) {

	const x0 = points[ 0 ][ 0 ];
	const x1 = points[ 1 ][ 0 ];
	const x2 = points[ 2 ][ 0 ];
	const x3 = points[ 3 ][ 0 ];

	let s = ( x - x0 ) / ( x3 - x0 );

	for ( let i = 0; i < 16; ++ i ) {

		const error = cubicBezier1D( s, x0, x1, x2, x3 ) - x;

		if ( Math.abs( error ) < 1e-12 ) {

			break;

		}

		const slope = cubicBezierSlope1D( s, x0, x1, x2, x3 );

		if ( Math.abs( slope ) < 1e-12 ) {

			break;

		}

		s = Math.max( 0, Math.min( 1, s - error / slope ) );

	}

	return s;

}

function cubicBezier1D( s, p0, p1, p2, p3 ) {

	const k = 1 - s;

	return k * k * k * p0 + 3 * k * k * s * p1 + 3 * k * s * s * p2 + s * s * s * p3;

}

function cubicBezierSlope1D( s, p0, p1, p2, p3 ) {

	const k = 1 - s;

	return 3 * k * k * ( p1 - p0 ) + 6 * k * s * ( p2 - p1 ) + 3 * s * s * ( p3 - p2 );

}

export { subclipExact };
