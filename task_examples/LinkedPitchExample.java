package task_examples;

import be.tarsos.dsp.LinkedPitchShifter;
import be.tarsos.dsp.LinkedPitchShifter.Result;

/**
 * Standalone example for {@link LinkedPitchShifter}.
 * <p>
 * Generates a short deterministic six-channel track (each channel carries its
 * own frequency and phase offset, including one anti-phase pair) and pitch
 * shifts the whole interleaved buffer in one linked pass so the stereo image
 * is preserved. The class is also exercised by {@code task_tests.LinkedPitchTest}.
 */
public final class LinkedPitchExample {

	public static final int SAMPLE_RATE = 48000;
	public static final int FFT_SIZE = 1024;
	public static final int HOP = 256;
	public static final int CHANNELS = 6;
	public static final int FRAMES = 4096;

	private LinkedPitchExample() {
	}

	/**
	 * Generates the six-channel demonstration track.
	 *
	 * @return interleaved float PCM, {@link #FRAMES} frames times {@link #CHANNELS}
	 */
	public static float[] generateTrack() {
		float[] track = new float[FRAMES * CHANNELS];
		double[] frequencies = {220.0, 277.18, 329.63, 392.0, 440.0, 554.37};
		double[] phases = {0.0, 0.4, 0.9, 1.6, 2.2, 2.9};
		for (int frame = 0; frame < FRAMES; frame++) {
			for (int channel = 0; channel < CHANNELS; channel++) {
				double envelope = 0.35 + 0.15 * Math.sin(2.0 * Math.PI * frame / FRAMES);
				double value = envelope
						* Math.sin(2.0 * Math.PI * frequencies[channel] * frame / SAMPLE_RATE
								+ phases[channel]);
				// Channels 4 and 5 form an anti-phase pair.
				if (channel == 5) {
					value = -value;
				}
				track[frame * CHANNELS + channel] = (float) value;
			}
		}
		return track;
	}

	/**
	 * Pitch shifts the generated track.
	 *
	 * @param factor pitch factor in [0.5, 2]
	 * @return interleaved output with exactly {@link #FRAMES} frames
	 */
	public static float[] shiftTrack(double factor) {
		LinkedPitchShifter shifter = new LinkedPitchShifter(SAMPLE_RATE, CHANNELS, FFT_SIZE, HOP);
		shifter.setFactorAt(0L, factor);
		float[] input = generateTrack();
		float[] output = new float[FRAMES * CHANNELS];
		int inputFrames = 0;
		int outputFrames = 0;
		while (true) {
			Result result = shifter.process(input, inputFrames * CHANNELS,
					FRAMES - inputFrames, output, outputFrames * CHANNELS,
					FRAMES - outputFrames, true);
			inputFrames += result.consumed;
			outputFrames += result.produced;
			if (result.done) {
				break;
			}
			if (result.consumed == 0 && result.produced == 0) {
				throw new IllegalStateException("pitch shifter made no progress");
			}
		}
		if (inputFrames != FRAMES || outputFrames != FRAMES
				|| shifter.getInputFrames() != FRAMES
				|| shifter.getOutputFrames() != FRAMES) {
			throw new IllegalStateException("unexpected frame accounting");
		}
		return output;
	}

	public static void main(String[] args) {
		float[] input = generateTrack();
		for (double factor : new double[] {1.0, 0.75, 1.5}) {
			float[] output = shiftTrack(factor);
			double inputEnergy = 0.0;
			double outputEnergy = 0.0;
			for (float sample : input) {
				inputEnergy += sample * sample;
			}
			for (float sample : output) {
				if (!Float.isFinite(sample)) {
					throw new IllegalStateException("non-finite output sample");
				}
				outputEnergy += sample * sample;
			}
			System.out.printf(java.util.Locale.ROOT,
					"factor=%.2f channels=%d frames=%d inputEnergy=%.6f outputEnergy=%.6f%n",
					factor, CHANNELS, FRAMES, inputEnergy, outputEnergy);
		}
	}
}
