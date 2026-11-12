import * as AnimationUtils from '../AnimationUtils.js';
import { KeyframeTrack } from '../KeyframeTrack.js';
import { PiecewiseCubicInterpolant } from '../../math/interpolants/PiecewiseCubicInterpolant.js';

/**
 * A keyframe track whose values are described by a sequence of cubic Hermite
 * segments instead of plain keyframe samples.
 *
 * The track stores one value per breakpoint in the inherited `times` and
 * `values` arrays, while the `coefficients` array holds the four polynomial
 * coefficients per segment and value component. The breakpoint values are kept
 * in sync with the coefficients and are used for clamping outside the time
 * range and for serialization bookkeeping.
 *
 * Instances are self-contained: they never reference the curve they were
 * derived from, so clips built from these tracks can be cloned and serialized
 * like any other clip.
 *
 * @augments KeyframeTrack
 */
class CubicSegmentKeyframeTrack extends KeyframeTrack {

	/**
	 * Constructs a new cubic segment keyframe track.
	 *
	 * @param {string} name - The keyframe track's name.
	 * @param {Array<number>} times - The segment breakpoint times.
	 * @param {Array<number>} values - The curve values at the breakpoints.
	 * @param {Array<number>} coefficients - Per segment/per component `[ A, B, C, D ]` coefficients.
	 */
	constructor( name, times, values, coefficients ) {

		super( name, times, values );

		/**
		 * The cubic Hermite coefficients, with four entries per segment and
		 * value component.
		 *
		 * @type {Float32Array}
		 */
		this.coefficients = AnimationUtils.convertArray( coefficients, Float32Array );

		this.createInterpolant = this.InterpolantFactoryMethodCubicSegments;

	}

	/**
	 * Factory method for creating a new piecewise cubic interpolant.
	 *
	 * @static
	 * @param {TypedArray} [result] - The result buffer.
	 * @return {PiecewiseCubicInterpolant} The new interpolant.
	 */
	InterpolantFactoryMethodCubicSegments( result ) {

		return new PiecewiseCubicInterpolant( this.times, this.values, this.coefficients, this.getValueSize(), result );

	}

	/**
	 * Scales all keyframe times and the stored polynomial coefficients by the
	 * given factor, so the curve keeps its shape in the scaled time domain.
	 *
	 * @param {number} timeScale - The time scale.
	 * @return {CubicSegmentKeyframeTrack} A reference to this keyframe track.
	 */
	scale( timeScale ) {

		if ( timeScale !== 1.0 ) {

			super.scale( timeScale );

			const coefficients = this.coefficients;
			const factorB = 1.0 / timeScale;
			const factorC = factorB / timeScale;
			const factorD = factorC / timeScale;

			for ( let i = 0, n = coefficients.length; i !== n; i += 4 ) {

				coefficients[ i + 1 ] *= factorB;
				coefficients[ i + 2 ] *= factorC;
				coefficients[ i + 3 ] *= factorD;

			}

		}

		return this;

	}

	/**
	 * Removes keyframes before and after the given time range, together with
	 * the cubic segments they belong to.
	 *
	 * @param {number} startTime - The start time.
	 * @param {number} endTime - The end time.
	 * @return {CubicSegmentKeyframeTrack} A reference to this keyframe track.
	 */
	trim( startTime, endTime ) {

		const times = this.times;
		const nKeys = times.length;

		let from = 0;
		let to = nKeys - 1;

		while ( from !== nKeys && times[ from ] < startTime ) {

			++ from;

		}

		while ( to !== - 1 && times[ to ] > endTime ) {

			-- to;

		}

		++ to; // inclusive -> exclusive bound

		if ( from !== 0 || to !== nKeys ) {

			// empty tracks are forbidden, so keep at least one keyframe and its value
			if ( from >= to ) {

				to = Math.max( to, 1 );
				from = to - 1;

			}

			const stride = this.getValueSize();

			this.times = times.slice( from, to );
			this.values = this.values.slice( from * stride, to * stride );
			this.coefficients = this.coefficients.slice( from * stride * 4, ( to - 1 ) * stride * 4 );

		}

		return this;

	}

	/**
	 * The cubic representation has no redundant keyframes in the sense of the
	 * base implementation, so optimizing is a no-op.
	 *
	 * @return {CubicSegmentKeyframeTrack} A reference to this keyframe track.
	 */
	optimize() {

		return this;

	}

	/**
	 * Returns a new keyframe track with copied values from this instance.
	 *
	 * @return {CubicSegmentKeyframeTrack} A clone of this instance.
	 */
	clone() {

		return new this.constructor( this.name, this.times.slice(), this.values.slice(), this.coefficients.slice() );

	}

}

/**
 * The value type name.
 *
 * @type {string}
 * @default 'cubicsegments'
 */
CubicSegmentKeyframeTrack.prototype.ValueTypeName = 'cubicsegments';

/**
 * Serializes the given cubic segment track.
 *
 * @static
 * @param {CubicSegmentKeyframeTrack} track - The track to serialize.
 * @return {Object} The serialized track as JSON.
 */
CubicSegmentKeyframeTrack.toJSON = function ( track ) {

	return {
		name: track.name,
		type: 'cubicsegments',
		times: AnimationUtils.convertArray( track.times, Array ),
		values: AnimationUtils.convertArray( track.values, Array ),
		coefficients: AnimationUtils.convertArray( track.coefficients, Array )
	};

};

/**
 * Parses a serialized cubic segment track.
 *
 * @static
 * @param {Object} json - The serialized track.
 * @return {CubicSegmentKeyframeTrack} The parsed track.
 */
CubicSegmentKeyframeTrack.parse = function ( json ) {

	return new CubicSegmentKeyframeTrack( json.name, json.times, json.values, json.coefficients );

};

export { CubicSegmentKeyframeTrack };
