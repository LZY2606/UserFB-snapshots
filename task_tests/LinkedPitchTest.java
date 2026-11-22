package task_tests;

import be.tarsos.dsp.AudioEvent;
import be.tarsos.dsp.AudioProcessor;
import be.tarsos.dsp.LinkedPitchShifter;
import be.tarsos.dsp.LinkedPitchShifter.Result;
import be.tarsos.dsp.PitchShifter;
import be.tarsos.dsp.io.TarsosDSPAudioFormat;
import be.tarsos.dsp.stft.StftMath;
import task_examples.LinkedPitchExample;
import task_support.LegacyPitchShifter;
import task_support.TaskCheck;

/**
 * Real assertions for {@link LinkedPitchShifter}, the shared {@link StftMath}
 * production code and the refactored legacy {@link PitchShifter}. Cases are
 * registered through {@link TaskCheck} in the four required groups.
 */
public final class LinkedPitchTest {

	private static final int SR = 48000;
	private static final int N = 1024;
	private static final int H = 256;

	private LinkedPitchTest() {
	}

	public static void register() {
		registerPhase();
		registerStream();
		registerBoundary();
		registerLegacy();
	}

	// ------------------------------------------------------------------
	// phase group
	// ------------------------------------------------------------------

	private static void registerPhase() {
		TaskCheck.add("phase", "inner-product-unwrap-dft", () -> {
			int n = 64;
			int hop = 16;
			int channels = 2;
			int bin = 3;
			float[][] w0 = new float[channels][n];
			float[][] w1 = new float[channels][n];
			double[] amps = {1.0, 0.7};
			double[] phaseOffsets = {0.4, -1.1};
			for (int t = 0; t < n; t++) {
				for (int c = 0; c < channels; c++) {
					w0[c][t] = (float) (amps[c] * Math.cos(
							2 * Math.PI * bin * t / n + phaseOffsets[c]));
					int t2 = t + hop;
					w1[c][t] = (float) (amps[c] * Math.cos(
							2 * Math.PI * bin * t2 / n + phaseOffsets[c]));
				}
			}
			double[][] prevRe = new double[channels][n];
			double[][] prevIm = new double[channels][n];
			double[][] curRe = new double[channels][n];
			double[][] curIm = new double[channels][n];
			for (int c = 0; c < channels; c++) {
				naiveDft(w0[c], prevRe[c], prevIm[c]);
				naiveDft(w1[c], curRe[c], curIm[c]);
			}
			double expected = 2 * Math.PI * bin * hop / n;
			double advance = StftMath.linkedPhaseAdvance(curRe, curIm, prevRe, prevIm,
					channels, bin, hop, n);
			TaskCheck.check(Math.abs(StftMath.principal(advance - expected)) < 1e-12,
					"cross-channel inner product must recover the bin-aligned phase advance");

			// A zero inner product falls back to the expected linear advance.
			double[][] zeroRe = new double[channels][n];
			double[][] zeroIm = new double[channels][n];
			double fallback = StftMath.linkedPhaseAdvance(zeroRe, zeroIm, prevRe, prevIm,
					channels, bin, hop, n);
			TaskCheck.check(Math.abs(fallback - expected) < 1e-12,
					"zero inner product must use the expected advance");

			// Unwrapping folds large jumps onto an even multiple of pi.
			TaskCheck.check(Math.abs(StftMath.principal(3 * Math.PI) + Math.PI) < 1e-12,
					"principal value must wrap 3*pi to -pi");
			TaskCheck.check(Math.abs(StftMath.principal(0.5 * Math.PI) - 0.5 * Math.PI) < 1e-12,
					"principal value must leave small angles untouched");

			// Independent cross-check of the inner product itself.
			double manualRe = 0.0;
			double manualIm = 0.0;
			for (int c = 0; c < channels; c++) {
				manualRe += curRe[c][bin] * prevRe[c][bin] + curIm[c][bin] * prevIm[c][bin];
				manualIm += curIm[c][bin] * prevRe[c][bin] - curRe[c][bin] * prevIm[c][bin];
			}
			double manualAngle = Math.atan2(manualIm, manualRe);
			TaskCheck.check(Math.abs(StftMath.principal(manualAngle - expected)) < 1e-10,
					"manual cross-channel inner product angle matches the expected advance");
			TaskCheck.check(manualRe * manualRe + manualIm * manualIm > 1.0,
					"bin-aligned tone must give a strong cross-channel inner product");
		});

		TaskCheck.add("phase", "rotation-and-mapping-collision", () -> {
			// Source-bin mapping including collisions.
			TaskCheck.check(StftMath.mappedBin(2, 1.5) == 3, "floor(2*1.5) == 3");
			TaskCheck.check(StftMath.mappedBin(3, 1.5) == 4, "floor(3*1.5) == 4");
			TaskCheck.check(StftMath.mappedBin(4, 0.75) == 3, "floor(4*0.75) == 3");
			TaskCheck.check(StftMath.mappedBin(5, 0.75) == 3,
					"floor(5*0.75) == 3 collides with bin 4");
			TaskCheck.check(StftMath.mappedBin(0, 1.5) == 0, "DC stays at DC");
			TaskCheck.check(StftMath.mappedBin(N / 2, 1.0) == N / 2, "Nyquist identity mapping");

			// One shared rotation preserves the inter-channel phase relation.
			int n = 32;
			int channels = 3;
			double[][] re = new double[channels][n];
			double[][] im = new double[channels][n];
			double[] rel = new double[channels];
			double angle = 0.83;
			for (int c = 0; c < channels; c++) {
				re[c][7] = 1.0 + 0.25 * c;
				im[c][7] = -0.5 + 0.3 * c;
				rel[c] = Math.atan2(im[c][7], re[c][7]);
			}
			double cos = Math.cos(angle);
			double sin = Math.sin(angle);
			for (int c = 0; c < channels; c++) {
				double r = re[c][7];
				double i = im[c][7];
				re[c][7] = r * cos - i * sin;
				im[c][7] = r * sin + i * cos;
			}
			for (int c = 1; c < channels; c++) {
				double after = Math.atan2(im[c][7], re[c][7]) - Math.atan2(im[0][7], re[0][7]);
				double before = rel[c] - rel[0];
				TaskCheck.check(Math.abs(StftMath.principal(after - before)) < 1e-12,
						"shared rotation must keep channel phase difference");
			}
			TaskCheck.check(Math.abs(StftMath.wrapPi(3 * Math.PI) + Math.PI) < 1e-12,
					"rotation angles are reduced to [-pi, pi)");

			// Colliding bins must add complex energy instead of disappearing.
			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, 2, N, H);
			shifter.setFactorAt(0L, 0.75);
			float[] input = dualToneStereo(4096, SR * 4.0 / N, SR * 5.0 / N, 0.6, 0.2,
					0.3, -0.5);
			float[] output = runAll(shifter, input, 4096, 2, 257, 257);
			double energy = 0.0;
			for (float v : output) {
				energy += v * v;
			}
			TaskCheck.check(energy > 1.0, "colliding source bins must retain output energy");
		});

		TaskCheck.add("phase", "frequency-and-stereo-image", () -> {
			int frames = 8192;
			double f1 = SR * 8.0 / N;
			double f2 = SR * 20.0 / N;
			float[] input = dualToneStereo(frames, f1, f2, 0.6, 0.2, 0.3, 0.5);
			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, 2, N, H);
			double factor = 1.5;
			shifter.setFactorAt(0L, factor);
			float[] output = runAll(shifter, input, frames, 2, 113, 257);

			for (double sourceFreq : new double[] {f1, f2}) {
				double targetFreq = sourceFreq * factor;
				int centerBin = (int) Math.round(targetFreq * N / SR);
				int bestBin = -1;
				double bestEnergy = -1.0;
				for (int b = centerBin - 3; b <= centerBin + 3; b++) {
					double freq = SR * (double) b / N;
					double e = 0.0;
					for (int c = 0; c < 2; c++) {
						double[] z = complexAmplitude(output, frames, 2, c, freq);
						e += z[0] * z[0] + z[1] * z[1];
					}
					if (e > bestEnergy) {
						bestEnergy = e;
						bestBin = b;
					}
				}
				TaskCheck.check(bestEnergy > 1e4,
						"dominant energy must appear near the shifted target frequency");
				double bestFreq = SR * bestBin / N;
				TaskCheck.check(Math.abs(bestFreq - targetFreq) <= SR / (double) N,
						"dominant bin must be adjacent to the target frequency");

				double[] inL = complexAmplitude(input, frames, 2, 0, sourceFreq);
				double[] inR = complexAmplitude(input, frames, 2, 1, sourceFreq);
				double[] outL = complexAmplitude(output, frames, 2, 0, bestFreq);
				double[] outR = complexAmplitude(output, frames, 2, 1, bestFreq);
				double ratioIn = Math.hypot(inL[0], inL[1]) / Math.hypot(inR[0], inR[1]);
				double ratioOut = Math.hypot(outL[0], outL[1]) / Math.hypot(outR[0], outR[1]);
				TaskCheck.check(Math.abs(ratioOut - ratioIn) / ratioIn <= 0.03,
						"channel complex amplitude ratio must stay within 3%");
				double phaseIn = Math.atan2(inL[1], inL[0]) - Math.atan2(inR[1], inR[0]);
				double phaseOut = Math.atan2(outL[1], outL[0]) - Math.atan2(outR[1], outR[0]);
				TaskCheck.check(Math.abs(StftMath.principal(phaseOut - phaseIn)) <= 0.03,
						"inter-channel phase difference must stay within 0.03 rad");
			}
		});

		TaskCheck.add("phase", "antiphase-dominance-and-example", () -> {
			int frames = 6000;
			double freq = SR * 10.0 / N;
			float[] anti = new float[frames * 2];
			for (int t = 0; t < frames; t++) {
				float v = (float) (0.4 * Math.sin(2 * Math.PI * freq * t / SR));
				anti[2 * t] = v;
				anti[2 * t + 1] = -v;
			}
			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, 2, N, H);
			shifter.setFactorAt(0L, 1.25);
			float[] antiOut = runAll(shifter, anti, frames, 2, 17, 257);
			double maxAsym = 0.0;
			double maxAmp = 0.0;
			for (int t = 0; t < frames; t++) {
				maxAsym = Math.max(maxAsym, Math.abs(antiOut[2 * t] + antiOut[2 * t + 1]));
				maxAmp = Math.max(maxAmp, Math.abs(antiOut[2 * t]));
				maxAmp = Math.max(maxAmp, Math.abs(antiOut[2 * t + 1]));
			}
			TaskCheck.check(maxAmp > 0.1, "anti-phase track must remain audible");
			TaskCheck.check(maxAsym <= 1e-4 * Math.max(1.0, maxAmp),
					"anti-phase channels must stay anti-phase");

			// Smoothly swapping dominant channel: per-channel energies must swap
			// monotonically without any reset producing NaNs or discontinuities.
			float[] swap = new float[frames * 2];
			for (int t = 0; t < frames; t++) {
				double a = (double) t / frames;
				swap[2 * t] = (float) ((1.0 - a) * 0.5 * Math.sin(2 * Math.PI * freq * t / SR));
				swap[2 * t + 1] = (float) (a * 0.5 * Math.sin(2 * Math.PI * freq * t / SR + 0.25));
			}
			LinkedPitchShifter shifter2 = new LinkedPitchShifter(SR, 2, N, H);
			shifter2.setFactorAt(0L, 1.5);
			float[] swapOut = runAll(shifter2, swap, frames, 2, 113, 17);
			double[] leftEnergy = new double[3];
			double[] rightEnergy = new double[3];
			for (int t = 0; t < frames; t++) {
				int third = Math.min(2, 3 * t / frames);
				leftEnergy[third] += swapOut[2 * t] * swapOut[2 * t];
				rightEnergy[third] += swapOut[2 * t + 1] * swapOut[2 * t + 1];
				TaskCheck.check(Float.isFinite(swapOut[2 * t]) && Float.isFinite(swapOut[2 * t + 1]),
						"dominance swap must not produce non-finite samples");
			}
			TaskCheck.check(leftEnergy[0] > leftEnergy[2],
					"left channel energy must decline as it loses dominance");
			TaskCheck.check(rightEnergy[2] > rightEnergy[0],
					"right channel energy must grow as it gains dominance");

			// The shipped six-channel example is compiled and actually run.
			float[] exampleOut = LinkedPitchExample.shiftTrack(0.75);
			TaskCheck.check(exampleOut.length == LinkedPitchExample.FRAMES
					* LinkedPitchExample.CHANNELS, "example must keep the frame count");
			double exampleEnergy = 0.0;
			for (float v : exampleOut) {
				TaskCheck.check(Float.isFinite(v), "example output must be finite");
				exampleEnergy += v * v;
			}
			TaskCheck.check(exampleEnergy > 1.0, "example output must contain signal");
			LinkedPitchExample.main(new String[0]);
		});
	}

	// ------------------------------------------------------------------
	// stream group
	// ------------------------------------------------------------------

	private static void registerStream() {
		TaskCheck.add("stream", "fragmentation-bit-identical", () -> {
			int frames = 6000;
			int channels = 2;
			float[] input = pseudoRandomTrack(frames, channels, 7);
			long[] frames1 = {0L, 1024L, 3072L};
			double[] factors = {1.0, 1.5, 0.75};
			float[] reference = runScheduled(frames, channels, input, 99999, 99999,
					frames1, factors);
			float[][] variants = {
					runScheduled(frames, channels, input, 1, 1, frames1, factors),
					runScheduled(frames, channels, input, 7, 17, frames1, factors),
					runScheduled(frames, channels, input, 113, 257, frames1, factors),
			};
			for (float[] variant : variants) {
				TaskCheck.check(variant.length == reference.length, "fragmented runs keep length");
				for (int i = 0; i < reference.length; i++) {
					TaskCheck.check(Float.floatToIntBits(variant[i])
							== Float.floatToIntBits(reference[i]),
							"chunking must not change any output float");
				}
			}
			LinkedPitchShifter a = new LinkedPitchShifter(SR, channels, N, H);
			LinkedPitchShifter b = new LinkedPitchShifter(SR, channels, N, H);
			for (int i = 0; i < frames1.length; i++) {
				a.setFactorAt(frames1[i], factors[i]);
				b.setFactorAt(frames1[i], factors[i]);
			}
			runAll(a, input, frames, channels, 7, 17);
			runAll(b, input, frames, channels, 113, 257);
			TaskCheck.check(a.getInputFrames() == b.getInputFrames()
					&& a.getInputFrames() == frames, "input frame counters must match");
			TaskCheck.check(a.getOutputFrames() == b.getOutputFrames()
					&& a.getOutputFrames() == frames, "output frame counters must match");
		});

		TaskCheck.add("stream", "backpressure-and-partial-accept", () -> {
			int frames = 5000;
			int channels = 2;
			float[] input = pseudoRandomTrack(frames, channels, 11);
			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, channels, N, H);
			shifter.setFactorAt(0L, 1.25);
			float[] output = new float[(frames + N) * channels];

			// Zero capacity must neither consume nor advance any state.
			Result blocked = shifter.process(input, 0, frames, output, 0, 0, true);
			TaskCheck.check(blocked.consumed == 0 && blocked.produced == 0 && !blocked.done,
					"capacity zero consumes and produces nothing");
			TaskCheck.check(shifter.getInputFrames() == 0 && shifter.getOutputFrames() == 0,
					"capacity zero leaves counters untouched");

			// Capacity one forces repeated backpressure; the big final batch is
			// accepted piecemeal, so EOF must not latch early.
			int inOff = 0;
			int outOff = 0;
			boolean sawPartial = false;
			int guard = 0;
			while (true) {
				int offered = frames - inOff;
				Result r = shifter.process(input, inOff * channels, offered,
						output, outOff * channels, 1, true);
				if (r.consumed < offered) {
					sawPartial = true;
				}
				inOff += r.consumed;
				outOff += r.produced;
				guard++;
				if (r.done) {
					break;
				}
				TaskCheck.check(guard < 100000, "backpressure loop must terminate");
			}
			TaskCheck.check(sawPartial, "tight capacity must force partial input acceptance");
			TaskCheck.check(inOff == frames && outOff == frames,
					"backpressure run still transfers every frame exactly once");
			TaskCheck.check(shifter.getInputFrames() == frames
					&& shifter.getOutputFrames() == frames, "counters after backpressure run");
		});

		TaskCheck.add("stream", "schedule-boundary-takes-effect", () -> {
			int frames = 4096;
			int channels = 1;
			float[] input = new float[frames];
			double freq = SR * 12.0 / N;
			for (int t = 0; t < frames; t++) {
				input[t] = (float) (0.5 * Math.sin(2 * Math.PI * freq * t / SR));
			}
			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, channels, N, H);
			shifter.setFactorAt(0L, 1.0);
			shifter.setFactorAt(2048L, 1.5);
			float[] output = runAll(shifter, input, frames, channels, 113, 257);
			TaskCheck.check(output.length == frames, "scheduled run keeps the duration");
			double lowHalfEnergy = bandEnergy(output, 0, 2048, freq, SR, channels);
			double highHalfEnergy = bandEnergy(output, 2048, frames, freq * 1.5, SR, channels);
			TaskCheck.check(lowHalfEnergy > 1.0 && highHalfEnergy > 1.0,
					"both schedule regions must contain signal");
			double shiftedLeak = bandEnergy(output, 2048, frames, freq, SR, channels);
			TaskCheck.check(shiftedLeak < highHalfEnergy,
					"the second region must move energy to the shifted frequency");

			// Re-scheduling the same frame uses the last factor.
			LinkedPitchShifter again = new LinkedPitchShifter(SR, channels, N, H);
			again.setFactorAt(0L, 1.5);
			again.setFactorAt(0L, 1.0);
			float[] rebuilt = runAll(again, input, frames, channels, 257, 257);
			double maxError = 0.0;
			for (int i = N; i < frames - N; i++) {
				maxError = Math.max(maxError, Math.abs(rebuilt[i] - input[i]));
			}
			TaskCheck.check(maxError <= 2e-5, "last factor at a frame must win");
		});

		TaskCheck.add("stream", "zero-frame-drain-and-eof-hold", () -> {
			int frames = 3000;
			int channels = 2;
			float[] input = pseudoRandomTrack(frames, channels, 23);
			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, channels, N, H);
			shifter.setFactorAt(0L, 1.0);
			float[] output = new float[(frames + N) * channels];

			// One tight call accepts the first window worth of frames, runs it
			// and delivers only one of its frames, leaving a guaranteed backlog.
			Result first = shifter.process(input, 0, 1500, output, 0, 1, false);
			TaskCheck.check(first.consumed >= H && first.produced == 1,
					"tight first call runs a window and holds back output");
			int inOff = first.consumed;
			int drained = first.produced;

			// Feed the rest of the first half under the same tight capacity.
			int feedGuard = 0;
			while (inOff < 1500) {
				Result next = shifter.process(input, inOff * channels, 1500 - inOff,
						output, drained * channels, 100, false);
				inOff += next.consumed;
				drained += next.produced;
				feedGuard++;
				TaskCheck.check(feedGuard < 100000, "partial feed loop must terminate");
			}
			TaskCheck.check(shifter.getInputFrames() == 1500, "input counter at 1500");

			// Zero-frame calls must deliver the pending output.
			Result drain = shifter.process(null, 0, 0, output, drained * channels, 1000, false);
			TaskCheck.check(drain.consumed == 0 && drain.produced > 0,
					"zero-frame call drains pending output");
			drained += drain.produced;

			// A final batch that is only partially accepted must keep EOF pending
			// until the remaining frames come back with endOfInput=true.
			int guard = 0;
			while (inOff < frames) {
				Result r = shifter.process(input, inOff * channels, frames - inOff,
						output, drained * channels, 37, true);
				inOff += r.consumed;
				drained += r.produced;
				guard++;
				TaskCheck.check(guard < 100000, "drain loop must terminate");
			}
			Result finish;
			do {
				finish = shifter.process(null, 0, 0, output, drained * channels, 512, true);
				drained += finish.produced;
			} while (!finish.done);
			TaskCheck.check(drained == frames, "EOF held until every frame was accepted");
			TaskCheck.check(shifter.getInputFrames() == frames
					&& shifter.getOutputFrames() == frames, "drain counters");

			// Repeated draining after completion yields nothing.
			Result repeat = shifter.process(null, 0, 0, output, 0, 256, true);
			TaskCheck.check(repeat.produced == 0 && repeat.consumed == 0 && repeat.done,
					"completed streams produce no more output");
		});
	}

	// ------------------------------------------------------------------
	// boundary group
	// ------------------------------------------------------------------

	private static void registerBoundary() {
		TaskCheck.add("boundary", "empty-and-short-signals", () -> {
			LinkedPitchShifter empty = new LinkedPitchShifter(SR, 2, N, H);
			empty.setFactorAt(0L, 1.5);
			Result r = empty.process(null, 0, 0, new float[16], 0, 8, true);
			TaskCheck.check(r.consumed == 0 && r.produced == 0 && r.done,
					"empty stream finishes with zero frames");
			TaskCheck.check(empty.getInputFrames() == 0 && empty.getOutputFrames() == 0,
					"empty stream counters stay zero");
			Result again = empty.process(null, 0, 0, new float[16], 0, 8, true);
			TaskCheck.check(again.done && again.produced == 0, "empty stream repeats as done");

			for (int frames : new int[] {1, 7, 255, 256, 257, 1023}) {
				float[] input = new float[frames * 2];
				for (int i = 0; i < input.length; i++) {
					input[i] = (float) (0.3 * Math.sin(0.07 * i) + 0.1 * Math.cos(0.013 * i));
				}
				LinkedPitchShifter shifter = new LinkedPitchShifter(SR, 2, N, H);
				shifter.setFactorAt(0L, 1.0);
				float[] output = runAll(shifter, input, frames, 2, 1, 1);
				TaskCheck.check(output.length == input.length, "short signal keeps frame count");
				double maxError = 0.0;
				for (int i = 0; i < input.length; i++) {
					maxError = Math.max(maxError, Math.abs(output[i] - input[i]));
				}
				TaskCheck.check(maxError <= 2e-5,
						"short signal factor-one reconstruction within 2e-5, frames=" + frames);
			}
		});

		TaskCheck.add("boundary", "head-tail-pulses-and-nonhop-length", () -> {
			int frames = 3001; // deliberately not a multiple of the hop
			int channels = 2;
			float[] input = pseudoRandomTrack(frames, channels, 31);
			for (int c = 0; c < channels; c++) {
				input[c] = 0.5f - 0.1f * c;
				input[(frames - 1) * channels + c] = 0.4f * (c == 0 ? 1 : -1);
			}
			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, channels, N, H);
			shifter.setFactorAt(0L, 1.0);
			float[] output = runAll(shifter, input, frames, channels, 113, 257);
			TaskCheck.check(Math.abs(output[0] - input[0]) <= 2e-5, "first pulse reconstructed");
			TaskCheck.check(Math.abs(output[1] - input[1]) <= 2e-5,
					"first pulse second channel reconstructed");
			TaskCheck.check(
					Math.abs(output[(frames - 1) * channels] - input[(frames - 1) * channels])
							<= 2e-5,
					"last pulse reconstructed");
			double maxError = 0.0;
			for (int i = 0; i < input.length; i++) {
				maxError = Math.max(maxError, Math.abs(output[i] - input[i]));
			}
			TaskCheck.check(maxError <= 2e-5, "non-hop-length factor-one reconstruction");

			LinkedPitchShifter shifted = new LinkedPitchShifter(SR, channels, 2048, 512);
			shifted.setFactorAt(0L, 1.5);
			float[] shiftedOut = runAll(shifted, input, frames, channels, 17, 13);
			TaskCheck.check(shiftedOut.length == input.length, "shifted non-hop length kept");
			for (float v : shiftedOut) {
				TaskCheck.check(Float.isFinite(v), "shifted non-hop output finite");
			}
		});

		TaskCheck.add("boundary", "illegal-calls-preserve-state", () -> {
			expectIae(() -> new LinkedPitchShifter(44101, 2, N, H));
			expectIae(() -> new LinkedPitchShifter(44100, 3, N, H));
			expectIae(() -> new LinkedPitchShifter(48000, 2, 1024, 512));
			expectIae(() -> new LinkedPitchShifter(48000, 2, 2048, 256));

			LinkedPitchShifter shifter = new LinkedPitchShifter(SR, 2, N, H);
			expectIae(() -> shifter.setFactorAt(-1L, 1.0));
			expectIae(() -> shifter.setFactorAt(7L, 1.0));
			expectIae(() -> shifter.setFactorAt(0L, 0.4));
			expectIae(() -> shifter.setFactorAt(0L, 2.01));
			expectIae(() -> shifter.setFactorAt(0L, Double.NaN));
			expectIae(() -> shifter.setFactorAt(0L, Double.POSITIVE_INFINITY));
			TaskCheck.check(shifter.getInputFrames() == 0 && shifter.getOutputFrames() == 0,
					"illegal schedule calls leave counters untouched");

			// A valid schedule still works after the rejected attempts.
			shifter.setFactorAt(0L, 1.0);
			float[] input = pseudoRandomTrack(300, 2, 41);
			float[] output = new float[400 * 2];
			expectIae(() -> shifter.process(input, 0, 301, output, 0, 100, false));
			expectIae(() -> shifter.process(input, -1, 10, output, 0, 100, false));
			expectIae(() -> shifter.process(null, 0, 1, output, 0, 100, false));
			expectIae(() -> shifter.process(input, 0, 10, null, 0, 10, false));
			expectIae(() -> shifter.process(input, 0, 10, output, 0, -1, false));
			TaskCheck.check(shifter.getInputFrames() == 0,
					"illegal process calls leave state untouched");

			// Schedule locks once the first non-empty input is accepted.
			shifter.process(input, 0, 10, output, 0, 100, false);
			expectIse(() -> shifter.setFactorAt(256L, 1.5));

			// After EOF, non-empty input is rejected.
			LinkedPitchShifter small = new LinkedPitchShifter(SR, 1, N, H);
			small.setFactorAt(0L, 1.0);
			float[] one = new float[] {0.25f};
			float[] oneOut = new float[64];
			Result done;
			do {
				done = small.process(one, 0, 1, oneOut, 0, 64, true);
			} while (!done.done);
			expectIse(() -> small.process(one, 0, 1, oneOut, 0, 64, true));
			TaskCheck.check(small.getInputFrames() == 1 && small.getOutputFrames() == 1,
					"single frame accounted before the post-EOF rejection");
		});

		TaskCheck.add("boundary", "reset-keeps-config-and-schedule", () -> {
			int frames = 3000;
			int channels = 2;
			float[] input = pseudoRandomTrack(frames, channels, 53);
			long[] scheduleFrames = {0L, 1024L};
			double[] scheduleFactors = {1.0, 1.5};

			LinkedPitchShifter shifter = new LinkedPitchShifter(44100, channels, 2048, 512);
			for (int i = 0; i < scheduleFrames.length; i++) {
				shifter.setFactorAt(scheduleFrames[i], scheduleFactors[i]);
			}
			float[] firstRun = runAll(shifter, input, frames, channels, 113, 257);
			TaskCheck.check(shifter.getInputFrames() == frames
					&& shifter.getOutputFrames() == frames, "pre-reset counters");
			shifter.reset();
			TaskCheck.check(shifter.getInputFrames() == 0 && shifter.getOutputFrames() == 0,
					"reset zeroes the counters");

			// The retained schedule must reproduce the first run bit for bit.
			float[] secondRun = runAll(shifter, input, frames, channels, 1, 17);
			TaskCheck.check(secondRun.length == firstRun.length, "reset run keeps length");
			for (int i = 0; i < firstRun.length; i++) {
				TaskCheck.check(Float.floatToIntBits(firstRun[i])
						== Float.floatToIntBits(secondRun[i]),
						"reset with retained schedule is bit identical");
			}

			// EOF and audio state are cleared: new input is accepted normally.
			shifter.reset();
			float[] more = pseudoRandomTrack(500, channels, 61);
			float[] moreOut = runAll(shifter, more, 500, channels, 257, 257);
			TaskCheck.check(moreOut.length == 500 * channels, "stream reusable after reset");

			// A freshly built instance with the same schedule matches as well.
			LinkedPitchShifter fresh = new LinkedPitchShifter(44100, channels, 2048, 512);
			for (int i = 0; i < scheduleFrames.length; i++) {
				fresh.setFactorAt(scheduleFrames[i], scheduleFactors[i]);
			}
			float[] freshOut = runAll(fresh, input, frames, channels, 7, 13);
			for (int i = 0; i < firstRun.length; i++) {
				TaskCheck.check(Float.floatToIntBits(freshOut[i])
						== Float.floatToIntBits(firstRun[i]),
						"fresh instance matches the reset instance");
			}
		});
	}

	// ------------------------------------------------------------------
	// legacy group
	// ------------------------------------------------------------------

	private static void registerLegacy() {
		TaskCheck.add("legacy", "refactored-pitchshifter-matches-legacy", () -> {
			int size = N;
			int overlap = N - H;
			TarsosDSPAudioFormat format = new TarsosDSPAudioFormat(48000, 16, 1, true, false);
			int hops = 64;
			for (double factor : new double[] {1.0, 0.75, 1.5}) {
				PitchShifter refactored = new PitchShifter(factor, 48000, size, overlap);
				LegacyPitchShifter legacy = new LegacyPitchShifter(factor, 48000, size, overlap);
				float[] refactoredOut = renderLegacyPath(refactored, format, hops, size, factor);
				float[] legacyOut = renderLegacyPath(legacy, format, hops, size, factor);
				TaskCheck.check(refactoredOut.length == legacyOut.length,
						"legacy path length matches for factor " + factor);
				double maxError = 0.0;
				double energy = 0.0;
				for (int i = 0; i < legacyOut.length; i++) {
					maxError = Math.max(maxError, Math.abs(refactoredOut[i] - legacyOut[i]));
					energy += legacyOut[i] * legacyOut[i];
					TaskCheck.check(Float.isFinite(refactoredOut[i]),
							"refactored legacy output finite");
				}
				TaskCheck.check(maxError <= 2e-6,
						"refactored PitchShifter within 2e-6 of the legacy algorithm, factor "
								+ factor);
				TaskCheck.check(energy > 1e-6, "legacy reference produces audible output");
			}
		});
	}

	// ------------------------------------------------------------------
	// helpers
	// ------------------------------------------------------------------

	private static float[] renderLegacyPath(AudioProcessor processor, TarsosDSPAudioFormat format,
			int hops, int size, double factor) {
		float[] result = new float[hops * H];
		int write = 0;
		for (int m = 0; m < hops; m++) {
			AudioEvent event = new AudioEvent(format);
			float[] input = new float[size];
			for (int i = 0; i < size; i++) {
				int t = m * H + i;
				input[i] = (float) (0.008 * Math.sin(2 * Math.PI * 12 * t / size)
						+ 0.004 * Math.cos(2 * Math.PI * 40 * t / size)
						+ 0.0005 * Math.sin(0.000003 * t * t));
			}
			event.setFloatBuffer(input);
			event.setOverlap(size - H);
			event.setBytesProcessed((long) m * H * 2);
			TaskCheck.check(processor.process(event), "legacy processor keeps processing");
			float[] out = event.getFloatBuffer();
			for (int i = size - H; i < size; i++) {
				result[write++] = out[i];
			}
		}
		processor.processingFinished();
		TaskCheck.check(factor >= 0.5 && factor <= 2.0, "legacy factors stay in range");
		return result;
	}

	private static float[] runAll(LinkedPitchShifter shifter, float[] input, int frames,
			int channels, int inChunk, int outChunk) {
		float[] output = new float[(frames + 2 * N) * channels];
		int inOff = 0;
		int outOff = 0;
		int guard = 0;
		while (true) {
			int offer = Math.min(inChunk, frames - inOff);
			int capacity = Math.min(outChunk,
					(output.length / channels) - outOff);
			boolean eoi = inOff + offer == frames;
			Result r = shifter.process(input, inOff * channels, offer, output,
					outOff * channels, capacity, eoi);
			inOff += r.consumed;
			outOff += r.produced;
			guard++;
			if (r.done) {
				break;
			}
			if (r.consumed == 0 && r.produced == 0) {
				throw new IllegalStateException("shifter stalled");
			}
			if (guard > 1000000) {
				throw new IllegalStateException("runAll guard exceeded");
			}
		}
		TaskCheck.check(inOff == frames, "all input frames accepted");
		TaskCheck.check(outOff == frames, "exactly the input frame count delivered");
		float[] trimmed = new float[frames * channels];
		System.arraycopy(output, 0, trimmed, 0, trimmed.length);
		return trimmed;
	}

	private static float[] runScheduled(int frames, int channels, float[] input, int inChunk,
			int outChunk, long[] scheduleFrames, double[] factors) {
		LinkedPitchShifter shifter = new LinkedPitchShifter(SR, channels, N, H);
		for (int i = 0; i < scheduleFrames.length; i++) {
			shifter.setFactorAt(scheduleFrames[i], factors[i]);
		}
		return runAll(shifter, input, frames, channels, inChunk, outChunk);
	}

	private static float[] dualToneStereo(int frames, double f1, double f2, double l1,
			double r1, double l2, double r2) {
		float[] track = new float[frames * 2];
		for (int t = 0; t < frames; t++) {
			track[2 * t] = (float) (l1 * Math.cos(2 * Math.PI * f1 * t / SR + 0.3)
					+ l2 * Math.cos(2 * Math.PI * f2 * t / SR + 1.1));
			track[2 * t + 1] = (float) (r1 * Math.cos(2 * Math.PI * f1 * t / SR - 0.9)
					+ r2 * Math.cos(2 * Math.PI * f2 * t / SR + 2.0));
		}
		return track;
	}

	private static float[] pseudoRandomTrack(int frames, int channels, long seed) {
		java.util.Random random = new java.util.Random(seed);
		float[] track = new float[frames * channels];
		for (int i = 0; i < track.length; i++) {
			track[i] = (float) (0.2 * Math.sin(0.013 * i) + 0.05 * random.nextGaussian());
		}
		return track;
	}

	private static double[] complexAmplitude(float[] interleaved, int frames, int channels,
			int channel, double freq) {
		double re = 0.0;
		double im = 0.0;
		// Skip the windowed edges so the measurement sees the steady state.
		for (int t = N; t < frames - N; t++) {
			double phase = -2 * Math.PI * freq * t / SR;
			float value = interleaved[t * channels + channel];
			re += value * Math.cos(phase);
			im += value * Math.sin(phase);
		}
		return new double[] {re, im};
	}

	private static double bandEnergy(float[] interleaved, int fromFrame, int toFrame,
			double freq, int sampleRate, int channels) {
		double energy = 0.0;
		for (int c = 0; c < channels; c++) {
			double re = 0.0;
			double im = 0.0;
			for (int t = fromFrame; t < toFrame; t++) {
				double phase = -2 * Math.PI * freq * t / sampleRate;
				float value = interleaved[t * channels + c];
				re += value * Math.cos(phase);
				im += value * Math.sin(phase);
			}
			energy += re * re + im * im;
		}
		return energy;
	}

	private static void naiveDft(float[] x, double[] re, double[] im) {
		int n = x.length;
		for (int k = 0; k < n; k++) {
			re[k] = 0.0;
			im[k] = 0.0;
			for (int t = 0; t < n; t++) {
				double phase = -2 * Math.PI * k * t / n;
				re[k] += x[t] * Math.cos(phase);
				im[k] += x[t] * Math.sin(phase);
			}
		}
	}

	private static void expectIae(ThrowingRunnable runnable) {
		boolean thrown = false;
		try {
			runnable.run();
		} catch (IllegalArgumentException e) {
			thrown = true;
		} catch (Exception e) {
			throw new AssertionError("wrong exception type: " + e.getClass(), e);
		}
		TaskCheck.check(thrown, "expected IllegalArgumentException");
	}

	private static void expectIse(ThrowingRunnable runnable) {
		boolean thrown = false;
		try {
			runnable.run();
		} catch (IllegalStateException e) {
			thrown = true;
		} catch (Exception e) {
			throw new AssertionError("wrong exception type: " + e.getClass(), e);
		}
		TaskCheck.check(thrown, "expected IllegalStateException");
	}

	@FunctionalInterface
	private interface ThrowingRunnable {
		void run() throws Exception;
	}
}
