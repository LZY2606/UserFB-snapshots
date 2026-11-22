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

package be.tarsos.dsp;

import java.util.Arrays;
import java.util.Map;
import java.util.TreeMap;

import be.tarsos.dsp.stft.StftMath;
import be.tarsos.dsp.util.fft.FloatFFT;

/**
 * A multichannel linked phase vocoder pitch shifter.
 * <p>
 * All channels share one phase history per source bin: the common phase
 * advance is estimated from the cross-channel complex inner product
 * {@code sum_c X_m[k,c] * conj(X_(m-1)[k,c])} and the same extra rotation is
 * applied to every channel. Channels are never summed and no single channel
 * is used as a reference, so the stereo image (relative levels and phase
 * differences) is preserved.
 * <p>
 * The processor works on interleaved float PCM, keeps the duration constant
 * and stores only the windows currently in flight: the working memory grows
 * with the channel count, the FFT size and the number of scheduled factors,
 * never with the length of the recording.
 * <p>
 * Analysis and synthesis both use the periodic Hann window; overlap-add is
 * normalized by the actual sum of squared windows covering each output
 * sample. The analysis framing carries an {@code N-H} sample look-behind, so
 * the first windows are negative-start pre-roll windows whose leading slots
 * are zero; with a factor of one the very same synthesis path reconstructs
 * the input signal, including the first and last samples.
 *
 * @author Joren Six
 */
public class LinkedPitchShifter {

	/**
	 * Result of one {@link LinkedPitchShifter#process} call. All counts are
	 * in complete audio frames.
	 */
	public static final class Result {
		/** Number of input frames accepted during the call. */
		public final int consumed;
		/** Number of output frames delivered during the call. */
		public final int produced;
		/** True once the end of the stream has been reached and all output has been delivered. */
		public final boolean done;

		public Result(int consumed, int produced, boolean done) {
			this.consumed = consumed;
			this.produced = produced;
			this.done = done;
		}
	}

	private static final double[] SAMPLE_RATES = {44100.0, 48000.0};
	private static final int[] CHANNEL_COUNTS = {1, 2, 6};

	private final int channels;
	private final int n;
	private final int hop;

	private final FloatFFT fft;
	private final float[] window;

	/** Factor schedule: synthesis window start frame (multiple of hop) -> factor. */
	private final TreeMap<Long, Double> schedule = new TreeMap<>();

	// Stream state
	private boolean started;
	private boolean eof;
	private long totalInputFrames;
	private long totalOutputFrames;
	private long streamLength;

	/** Index of the next window to analyze. */
	private int windowIndex;

	/**
	 * Input ring of N interleaved frames indexed by absolute frame number
	 * modulo N. Acceptance is gated on the next window's span so a frame a
	 * window still needs is never overwritten.
	 */
	private final float[] inputRing;

	// Analysis spectra per channel: full length N real/imaginary arrays
	private final double[][] prevRe;
	private final double[][] prevIm;
	private final double[][] curRe;
	private final double[][] curIm;
	private final double[][] targetRe;
	private final double[][] targetIm;
	private final float[] fftBuffer;

	/** Shared extra rotation angle per source bin, reduced to [-pi, pi). */
	private final double[] rotation;

	// Overlap-add output ring (interleaved) with per-slot window-square energy
	// and the logical frame generation currently occupying each slot.
	private final float[] outputRing;
	private final float[] normRing;
	private final long[] slotFrame;
	private long nextOutputFrame;
	private long coveredThrough = Long.MIN_VALUE;

	/**
	 * Creates a new linked multichannel pitch shifter.
	 *
	 * @param sampleRate sample rate in Hz; only 44100 and 48000 are supported
	 * @param channels   number of interleaved channels; 1, 2 or 6
	 * @param fftSize    FFT/window size; 1024 or 2048
	 * @param hop        hop size in frames; 256 for FFT size 1024, 512 for 2048
	 */
	public LinkedPitchShifter(double sampleRate, int channels, int fftSize, int hop) {
		if (!contains(SAMPLE_RATES, sampleRate)) {
			throw new IllegalArgumentException("unsupported sample rate: " + sampleRate);
		}
		if (!containsInt(CHANNEL_COUNTS, channels)) {
			throw new IllegalArgumentException("unsupported channel count: " + channels);
		}
		if (!((fftSize == 1024 && hop == 256) || (fftSize == 2048 && hop == 512))) {
			throw new IllegalArgumentException(
					"unsupported fftSize/hop combination: " + fftSize + "/" + hop);
		}
		this.channels = channels;
		this.n = fftSize;
		this.hop = hop;

		this.fft = new FloatFFT(fftSize);
		this.window = new float[fftSize];
		StftMath.periodicHann(fftSize, window);

		this.inputRing = new float[fftSize * channels];
		this.outputRing = new float[fftSize * channels];
		this.normRing = new float[fftSize];
		this.slotFrame = new long[fftSize];
		Arrays.fill(slotFrame, Long.MIN_VALUE);

		this.prevRe = new double[channels][];
		this.prevIm = new double[channels][];
		this.curRe = new double[channels][];
		this.curIm = new double[channels][];
		this.targetRe = new double[channels][];
		this.targetIm = new double[channels][];
		for (int c = 0; c < channels; c++) {
			prevRe[c] = new double[fftSize];
			prevIm[c] = new double[fftSize];
			curRe[c] = new double[fftSize];
			curIm[c] = new double[fftSize];
			targetRe[c] = new double[fftSize];
			targetIm[c] = new double[fftSize];
		}
		this.fftBuffer = new float[fftSize];
		this.rotation = new double[fftSize / 2 + 1];
		this.streamLength = 0;
	}

	private static boolean contains(double[] values, double value) {
		for (double v : values) {
			if (v == value) {
				return true;
			}
		}
		return false;
	}

	private static boolean containsInt(int[] values, int value) {
		for (int v : values) {
			if (v == value) {
				return true;
			}
		}
		return false;
	}

	/**
	 * Schedules a pitch factor. The schedule must be completed before the
	 * first non-empty input is accepted. A frame position takes the factor of
	 * the latest schedule entry not after it; the default factor is one.
	 * Negative-start pre-roll windows use the factor scheduled at frame zero.
	 *
	 * @param frame  synthesis window start frame; must be non-negative and a multiple of the hop
	 * @param factor pitch factor in the closed interval [0.5, 2]
	 * @throws IllegalArgumentException if the frame or factor is invalid
	 * @throws IllegalStateException    if the stream has already started
	 */
	public void setFactorAt(long frame, double factor) {
		if (started) {
			throw new IllegalStateException("factor schedule is locked once the stream has started");
		}
		if (frame < 0 || frame % hop != 0) {
			throw new IllegalArgumentException(
					"frame must be non-negative and a multiple of " + hop + ": " + frame);
		}
		if (!Double.isFinite(factor) || factor < 0.5 || factor > 2.0) {
			throw new IllegalArgumentException(
					"factor must be finite and within [0.5, 2]: " + factor);
		}
		schedule.put(frame, factor);
	}

	/**
	 * @return the number of real input frames accepted so far
	 */
	public long getInputFrames() {
		return totalInputFrames;
	}

	/**
	 * @return the number of output frames delivered so far
	 */
	public long getOutputFrames() {
		return totalOutputFrames;
	}

	/**
	 * Clears all buffered audio, phase history, end-of-input state and frame
	 * counters. Configuration and the factor schedule are kept.
	 */
	public void reset() {
		started = false;
		eof = false;
		totalInputFrames = 0;
		totalOutputFrames = 0;
		streamLength = 0;
		windowIndex = 0;
		nextOutputFrame = 0;
		coveredThrough = Long.MIN_VALUE;
		Arrays.fill(inputRing, 0.0f);
		Arrays.fill(outputRing, 0.0f);
		Arrays.fill(normRing, 0.0f);
		Arrays.fill(slotFrame, Long.MIN_VALUE);
		Arrays.fill(rotation, 0.0);
		for (int c = 0; c < channels; c++) {
			Arrays.fill(prevRe[c], 0.0);
			Arrays.fill(prevIm[c], 0.0);
			Arrays.fill(curRe[c], 0.0);
			Arrays.fill(curIm[c], 0.0);
			Arrays.fill(targetRe[c], 0.0);
			Arrays.fill(targetIm[c], 0.0);
		}
	}

	/**
	 * Logical frame at which window m starts: {@code m*hop - (n-hop)}. The
	 * first windows start before frame zero and contain leading zeros.
	 */
	private long windowStart(int m) {
		return (long) m * hop - (n - hop);
	}

	/**
	 * Number of real input frames that must be accepted before window m can
	 * run: its span only reaches frame {@code start+n-1}.
	 */
	private long requiredInput(int m) {
		return Math.max(0L, windowStart(m) + n);
	}

	private double factorForWindow(int m) {
		long frame = Math.max(0L, windowStart(m));
		Map.Entry<Long, Double> entry = schedule.floorEntry(frame);
		return entry == null ? 1.0 : entry.getValue();
	}

	/**
	 * Feeds interleaved PCM frames and pulls pitch shifted frames.
	 * <p>
	 * The method may accept only part of the offered input; the caller keeps
	 * the unconsumed suffix and presents it again. Frame counts are in
	 * complete audio frames, offsets in array elements. A zero-length side
	 * may pass a null array.
	 *
	 * @param input        interleaved input samples, or null if frames is zero
	 * @param inputOffset  first element of the input in {@code input}
	 * @param frames       number of input frames offered
	 * @param output       destination interleaved samples, or null if capacity is zero
	 * @param outputOffset first element of the output in {@code output}
	 * @param capacity     maximum number of output frames to produce
	 * @param endOfInput   true when the offered input contains the final frames
	 * @return accepted and produced frame counts and the finished flag
	 */
	public Result process(float[] input, int inputOffset, int frames, float[] output,
			int outputOffset, int capacity, boolean endOfInput) {
		if (frames < 0 || capacity < 0 || inputOffset < 0 || outputOffset < 0) {
			throw new IllegalArgumentException("negative count or offset");
		}
		if (frames > 0) {
			if (input == null) {
				throw new IllegalArgumentException("input array is null for non-empty input");
			}
			if ((long) inputOffset + (long) frames * channels > input.length) {
				throw new IllegalArgumentException("input range exceeds array bounds");
			}
		}
		if (capacity > 0) {
			if (output == null) {
				throw new IllegalArgumentException("output array is null for non-zero capacity");
			}
			if ((long) outputOffset + (long) capacity * channels > output.length) {
				throw new IllegalArgumentException("output range exceeds array bounds");
			}
		}
		if (eof && frames > 0) {
			throw new IllegalStateException("input offered after end of input was latched");
		}
		if (capacity == 0) {
			// No room: nothing is consumed and no state, including EOF, advances.
			// The finished status is still reported as a pure observation.
			return new Result(0, 0, isDone());
		}

		int inPos = inputOffset;
		int inRemain = frames;
		int outPos = outputOffset;
		int outRemain = capacity;
		int consumed = 0;
		int produced = 0;

		boolean progress = true;
		while (progress) {
			progress = false;

			// Deliver every fully covered output frame that fits in the caller's buffer.
			long readyLimit = eof ? Math.min(coveredThrough, streamLength - 1) : coveredThrough;
			if (outRemain > 0 && nextOutputFrame <= readyLimit) {
				int take = (int) Math.min(outRemain, readyLimit - nextOutputFrame + 1);
				deliver(output, outPos, take);
				outPos += take * channels;
				outRemain -= take;
				produced += take;
				progress = true;
			}

			// Run the next window once its span is available and every output
			// ring slot it touches has been delivered to the caller.
			if (windowReady()) {
				runWindow();
				progress = true;
			}

			// Accept input only up to the frame the next window can reach; the
			// N-slot ring then never evicts a frame the pending window needs.
			if (!eof && inRemain > 0) {
				long limit = requiredInput(windowIndex);
				if (totalInputFrames < limit) {
					int take = (int) Math.min(inRemain, limit - totalInputFrames);
					for (int f = 0; f < take; f++) {
						long frame = totalInputFrames + f;
						int slot = (int) Math.floorMod(frame, (long) n);
						System.arraycopy(input, inPos + f * channels, inputRing,
								slot * channels, channels);
					}
					totalInputFrames += take;
					inPos += take * channels;
					inRemain -= take;
					consumed += take;
					started = true;
					progress = true;
				}
			}

			// EOF latches only when every frame offered in this call has been accepted.
			if (!eof && inRemain == 0 && endOfInput) {
				eof = true;
				streamLength = totalInputFrames;
				progress = true;
			}
		}

		boolean done = isDone();
		return new Result(consumed, produced, done);
	}

	private boolean isDone() {
		boolean windowsRemaining = eof && streamLength > 0
				&& windowStart(windowIndex) < streamLength;
		return eof && !windowsRemaining && nextOutputFrame >= streamLength;
	}

	private boolean windowReady() {
		if (outputRingBlocked()) {
			return false;
		}
		long start = windowStart(windowIndex);
		if (eof) {
			// At EOF every window intersecting the real input interval runs,
			// possibly partial with zero extended tails.
			return streamLength > 0 && start < streamLength;
		}
		return totalInputFrames >= requiredInput(windowIndex);
	}

	/**
	 * A new window reuses ring slots of frames directly before its own range;
	 * those frames must already have been delivered to the caller.
	 */
	private boolean outputRingBlocked() {
		return coveredThrough >= 0 && nextOutputFrame <= coveredThrough;
	}

	private void deliver(float[] output, int offset, int frameCount) {
		for (int f = 0; f < frameCount; f++) {
			long logicalFrame = nextOutputFrame + f;
			int slot = (int) Math.floorMod(logicalFrame, (long) n);
			float scale = 0.0f;
			if (slotFrame[slot] == logicalFrame && normRing[slot] > 0.0f) {
				scale = 1.0f / normRing[slot];
			}
			int dst = offset + f * channels;
			for (int c = 0; c < channels; c++) {
				output[dst + c] = outputRing[slot * channels + c] * scale;
				outputRing[slot * channels + c] = 0.0f;
			}
			normRing[slot] = 0.0f;
			slotFrame[slot] = Long.MIN_VALUE;
		}
		nextOutputFrame += frameCount;
		totalOutputFrames += frameCount;
	}

	/**
	 * Analyzes one window, applies the linked phase rotation and overlap-adds
	 * the synthesized window for every channel.
	 */
	private void runWindow() {
		final int m = windowIndex;
		final long start = windowStart(m);
		final double factor = factorForWindow(m);
		final int half = n / 2;

		for (int c = 0; c < channels; c++) {
			// Gather the window from the absolute-frame ring; frames before
			// zero and past the input end are zero extended.
			for (int i = 0; i < n; i++) {
				long frame = start + i;
				float sample = 0.0f;
				if (frame >= 0 && frame < totalInputFrames) {
					int slot = (int) Math.floorMod(frame, (long) n);
					sample = inputRing[slot * channels + c];
				}
				fftBuffer[i] = sample * window[i];
			}
			fft.realForward(fftBuffer);
			StftMath.unpackReal(fftBuffer, curRe[c], curIm[c], n);
		}

		// Linked phase advance and one shared rotation per source bin. The
		// first window starts with rotation zero; DC and Nyquist stay real.
		if (m != 0) {
			for (int k = 1; k < half; k++) {
				final double advance = StftMath.linkedPhaseAdvance(curRe, curIm, prevRe, prevIm,
						channels, k, hop, n);
				rotation[k] = StftMath.wrapPi(rotation[k] + (factor - 1.0) * advance);
			}
		}

		for (int c = 0; c < channels; c++) {
			Arrays.fill(targetRe[c], 0.0);
			Arrays.fill(targetIm[c], 0.0);
			for (int k = 0; k <= half; k++) {
				double r = curRe[c][k];
				double im = curIm[c][k];
				if (k != 0 && k != half) {
					final double angle = rotation[k];
					if (angle != 0.0) {
						final double cos = Math.cos(angle);
						final double sin = Math.sin(angle);
						double rotatedRe = r * cos - im * sin;
						double rotatedIm = r * sin + im * cos;
						r = rotatedRe;
						im = rotatedIm;
					}
				}
				int target = StftMath.mappedBin(k, factor);
				if (target <= half) {
					// Complex accumulation: colliding source bins add, never overwrite.
					targetRe[c][target] += r;
					targetIm[c][target] += im;
				}
			}
			// Negative frequencies by conjugate symmetry. DC and Nyquist stay real.
			for (int k = 1; k < half; k++) {
				targetRe[c][n - k] = targetRe[c][k];
				targetIm[c][n - k] = -targetIm[c][k];
			}
			StftMath.packReal(targetRe[c], targetIm[c], fftBuffer, n);
			// realInverse(true) undoes the forward transform gain and returns
			// the time domain samples in their original units.
			fft.realInverse(fftBuffer, true);
			StftMath.applyWindowInPlace(fftBuffer, window, n);

			for (int i = 0; i < n; i++) {
				long logicalFrame = start + i;
				int slot = (int) Math.floorMod(logicalFrame, (long) n);
				if (slotFrame[slot] != logicalFrame) {
					slotFrame[slot] = logicalFrame;
					normRing[slot] = 0.0f;
					for (int cc = 0; cc < channels; cc++) {
						outputRing[slot * channels + cc] = 0.0f;
					}
				}
				outputRing[slot * channels + c] += fftBuffer[i];
				if (c == 0) {
					normRing[slot] += window[i] * window[i];
				}
			}
		}

		// Current spectra become the previous window's history.
		for (int c = 0; c < channels; c++) {
			double[] tmp = prevRe[c];
			prevRe[c] = curRe[c];
			curRe[c] = tmp;
			tmp = prevIm[c];
			prevIm[c] = curIm[c];
			curIm[c] = tmp;
		}

		windowIndex++;
		// Frames up to start+hop-1 have now received every covering window.
		coveredThrough = start + hop - 1;
	}
}
