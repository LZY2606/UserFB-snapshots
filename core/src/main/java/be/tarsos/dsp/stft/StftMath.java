/*
*      _______                       _____   _____ _____
*     |__   __|                     |  __ \ / ____|  __ \
*        | | __ _ _ __ ___  ___  ___| |  | | (___ | |__) |
*        | |/ _` | '__/ __|/ _ \/ __| |\___ \  |  ___/
*        | | (_| | |  \__ \ (_) \__ \ |__| |____) | |
*        |_|\__,_|_|  |___/\___/|___/_____/|_____/|_|
*
* -------------------------------------------------------------
*
* TarsosDSP is developed by Joren Six at IPEM, University Ghent
*
* -------------------------------------------------------------
*
*/

package be.tarsos.dsp.stft;

import be.tarsos.dsp.util.fft.FloatFFT;

/**
 * Shared building blocks for the STFT based pitch shifters.
 * <p>
 * All methods work on the packed real FFT layout produced by
 * {@link FloatFFT#realForward(float[])} for an even transform size
 * {@code N}: {@code data[0]} holds the real DC coefficient,
 * {@code data[1]} holds the real Nyquist coefficient and, for
 * {@code 0 < k < N/2}, the complex bin {@code k} is stored as
 * {@code (data[2*k], data[2*k+1])}.
 * <p>
 * The window used everywhere is the periodic Hann window
 * {@code 0.5 - 0.5*cos(2*pi*i/N)}.
 *
 * @author Joren Six
 */
public final class StftMath {

	private static final double TWO_PI = 2.0 * Math.PI;

	private StftMath() {
	}

	/**
	 * Fills {@code window} with the periodic Hann window of length {@code n}.
	 *
	 * @param n      the window length
	 * @param window destination array of length at least {@code n}
	 */
	public static void periodicHann(final int n, final float[] window) {
		for (int i = 0; i < n; i++) {
			window[i] = (float) (0.5 - 0.5 * Math.cos(TWO_PI * i / n));
		}
	}

	/**
	 * Unpacks the packed real FFT output into separate real and imaginary
	 * arrays covering the full spectrum. The DC and Nyquist bins are treated
	 * as purely real, as required for the transform of a real signal.
	 *
	 * @param packed packed FFT output, length {@code n}
	 * @param re     destination real parts, length {@code n}
	 * @param im     destination imaginary parts, length {@code n}
	 * @param n      FFT size (even)
	 */
	public static void unpackReal(final float[] packed, final double[] re, final double[] im,
			final int n) {
		final int half = n / 2;
		re[0] = packed[0];
		im[0] = 0.0;
		re[half] = packed[1];
		im[half] = 0.0;
		for (int k = 1; k < half; k++) {
			re[k] = packed[2 * k];
			im[k] = packed[2 * k + 1];
			re[n - k] = packed[2 * k];
			im[n - k] = -packed[2 * k + 1];
		}
	}

	/**
	 * Packs the positive frequency half of a full complex spectrum back into
	 * the packed real FFT layout. DC and Nyquist must be purely real; the
	 * conjugate symmetry of the negative frequencies is assumed.
	 *
	 * @param re     full spectrum real parts, length {@code n}
	 * @param im     full spectrum imaginary parts, length {@code n}
	 * @param packed destination packed array, length {@code n}
	 * @param n      FFT size (even)
	 */
	public static void packReal(final double[] re, final double[] im, final float[] packed,
			final int n) {
		final int half = n / 2;
		packed[0] = (float) re[0];
		packed[1] = (float) re[half];
		for (int k = 1; k < half; k++) {
			packed[2 * k] = (float) re[k];
			packed[2 * k + 1] = (float) im[k];
		}
	}

	/**
	 * Wraps a phase difference into the {@code [-pi, pi)} interval using the
	 * same even-rounding convention as the original Bernsee phase vocoder.
	 *
	 * @param delta the raw phase difference
	 * @return the difference reduced by an even multiple of {@code pi}
	 */
	public static double principal(final double delta) {
		long qpd = (long) (delta / Math.PI);
		if (qpd >= 0) {
			qpd += qpd & 1L;
		} else {
			qpd -= qpd & 1L;
		}
		return delta - Math.PI * qpd;
	}

	/**
	 * Estimates the unwrapped phase advance between two analysis windows for
	 * one source bin. The measured advance comes from the argument of the
	 * cross-channel inner product
	 * {@code sum_c X[k,c] * conj(Xprev[k,c])}; when that product vanishes the
	 * expected linear advance {@code 2*pi*k*hop/n} is used instead.
	 *
	 * @param re       current window real parts for every channel, laid out as
	 *                 {@code [channel][bin]}
	 * @param im       current window imaginary parts
	 * @param prevRe   previous window real parts
	 * @param prevIm   previous window imaginary parts
	 * @param channels number of channels
	 * @param k        source bin
	 * @param hop      analysis hop size in samples
	 * @param n        FFT size
	 * @return the unwrapped phase advance in radians
	 */
	public static double linkedPhaseAdvance(final double[][] re, final double[][] im,
			final double[][] prevRe, final double[][] prevIm, final int channels, final int k,
			final int hop, final int n) {
		double innerRe = 0.0;
		double innerIm = 0.0;
		for (int c = 0; c < channels; c++) {
			final double r = re[c][k];
			final double i = im[c][k];
			final double pr = prevRe[c][k];
			final double pi = prevIm[c][k];
			innerRe += r * pr + i * pi;
			innerIm += i * pr - r * pi;
		}
		final double expected = TWO_PI * k * hop / n;
		if (innerRe == 0.0 && innerIm == 0.0) {
			return expected;
		}
		final double measured = Math.atan2(innerIm, innerRe);
		return expected + principal(measured - expected);
	}

	/**
	 * Applies the periodic Hann window to a time domain buffer in place.
	 * @param data   buffer of length {@code n}
	 * @param window periodic Hann window
	 * @param n      window length
	 */
	public static void applyWindowInPlace(final float[] data, final float[] window, final int n) {
		for (int i = 0; i < n; i++) {
			data[i] *= window[i];
		}
	}

	/**
	 * Maps a source bin to its target bin for the given pitch factor.
	 *
	 * @param sourceBin the source bin
	 * @param factor   the pitch factor
	 * @return {@code floor(sourceBin * factor)}
	 */
	public static int mappedBin(final int sourceBin, final double factor) {
		return (int) Math.floor(sourceBin * factor);
	}

	/**
	 * Reduces an angle to the {@code [-pi, pi)} interval.
	 *
	 * @param angle angle in radians
	 * @return the equivalent angle inside {@code [-pi, pi)}
	 */
	public static double wrapPi(final double angle) {
		double wrapped = angle % TWO_PI;
		if (wrapped >= Math.PI) {
			wrapped -= TWO_PI;
		} else if (wrapped < -Math.PI) {
			wrapped += TWO_PI;
		}
		return wrapped;
	}
}
