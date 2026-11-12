import { Interpolant } from '../Interpolant.js';

/**
 * An interpolant over a sequence of cubic Hermite segments.
 *
 * Each segment stores four polynomial coefficients per value component,
 * so the value at local offset `h = t - t0` is:
 *
 * `f( t ) = A + h * ( B + h * ( C + h * D ) )`
 *
 * Unlike {@link CubicInterpolant}, the shape of every segment is fixed once the
 * interpolant is constructed; evaluating it never reads neighboring keyframes.
 * That allows an exact sub-clip to own a self-contained copy of the curve
 * pieces it covers.
 *
 * @augments Interpolant
 */
class PiecewiseCubicInterpolant extends Interpolant {

	/**
	 * Constructs a new piecewise cubic interpolant.
	 *
	 * @param {TypedArray} parameterPositions - The segment breakpoint positions.
	 * @param {TypedArray} sampleValues - The curve values at the breakpoints, used for clamping outside the parameter range.
	 * @param {TypedArray} coefficients - Per segment/per component `[ A, B, C, D ]` coefficients.
	 * @param {number} sampleSize - The sample size.
	 * @param {TypedArray} [resultBuffer] - The result buffer.
	 */
	constructor( parameterPositions, sampleValues, coefficients, sampleSize, resultBuffer ) {

		super( parameterPositions, sampleValues, sampleSize, resultBuffer );

		/**
		 * The cubic Hermite coefficients. The layout per segment `i` and
		 * component `c` is `coefficients[ ( i * valueSize + c ) * 4 + k ]`
		 * with `k` being `0` to `3` for `A` to `D`.
		 *
		 * @type {TypedArray}
		 */
		this.coefficients = coefficients;

	}

	interpolate_( i1, t0, t ) {

		const result = this.resultBuffer;
		const stride = this.valueSize;
		const coefficients = this.coefficients;

		const segmentBase = ( i1 - 1 ) * stride * 4;
		const h = t - t0;

		for ( let c = 0; c !== stride; ++ c ) {

			const offset = segmentBase + c * 4;

			const a = coefficients[ offset ];
			const b = coefficients[ offset + 1 ];
			const c2 = coefficients[ offset + 2 ];
			const d = coefficients[ offset + 3 ];

			result[ c ] = a + h * ( b + h * ( c2 + h * d ) );

		}

		return result;

	}

}

export { PiecewiseCubicInterpolant };
