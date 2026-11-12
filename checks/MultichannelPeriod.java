/* Multichannel pitch-period checks for the Java Sonic port.
   Copyright 2026
   This file is part of the Sonic Library.

   This file is licensed under the Apache 2.0 license.

   The private analysis routines are reached through reflection; no public API
   is added.  It mirrors checks/multichannel_period.c on identical integer
   inputs, so the two implementations must reach the same candidates and the
   same history decisions. */
package sonic;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

public class MultichannelPeriod {

    private static final int TEST_SAMPLE_RATE = 48000;
    private static final int TEST_WINDOW = 4096;
    private static final int MAX_CHANNELS = 6;
    private static final int SEQ_CAP = 512;

    private static int casesRun = 0;
    private static int casesFailed = 0;
    private static boolean firstFailureLogged = false;

    private final Method findPitchPeriod;
    private final Method findPitchPeriodInRange;
    private final Method findPeriodDiff;
    private final Field maxRequiredField;
    private final Field minPeriodField;
    private final Field maxPeriodField;
    private final Field prevPeriodField;
    private final Field prevMinDiffField;

    private MultichannelPeriod() throws Exception {
        findPitchPeriod = Sonic.class.getDeclaredMethod(
            "findPitchPeriod", short[].class, int.class, boolean.class);
        findPitchPeriod.setAccessible(true);
        findPitchPeriodInRange = Sonic.class.getDeclaredMethod(
            "findPitchPeriodInRange", short[].class, int.class, int.class,
            int.class);
        findPitchPeriodInRange.setAccessible(true);
        findPeriodDiff = Sonic.class.getDeclaredMethod(
            "findPeriodDiff", short[].class, int.class, int.class);
        findPeriodDiff.setAccessible(true);
        maxRequiredField = Sonic.class.getDeclaredField("maxRequired");
        maxRequiredField.setAccessible(true);
        minPeriodField = Sonic.class.getDeclaredField("minPeriod");
        minPeriodField.setAccessible(true);
        maxPeriodField = Sonic.class.getDeclaredField("maxPeriod");
        maxPeriodField.setAccessible(true);
        prevPeriodField = Sonic.class.getDeclaredField("prevPeriod");
        prevPeriodField.setAccessible(true);
        prevMinDiffField = Sonic.class.getDeclaredField("prevMinDiff");
        prevMinDiffField.setAccessible(true);
    }

    private static void fail(String format, Object... args) {
        casesFailed++;
        if (!firstFailureLogged) {
            firstFailureLogged = true;
            System.err.print("FIRST_DEVIATION [JAVA]: ");
            System.err.printf(format, args);
            System.err.println();
        }
    }

    private void check(boolean condition, String format, Object... args) {
        casesRun++;
        if (!condition) {
            fail(format, args);
            throw new TestFailure();
        }
    }

    private static class TestFailure extends RuntimeException {
        private static final long serialVersionUID = 1L;
    }

    private int maxRequired(Sonic stream) throws Exception {
        return maxRequiredField.getInt(stream);
    }

    private int minPeriod(Sonic stream) throws Exception {
        return minPeriodField.getInt(stream);
    }

    private int maxPeriod(Sonic stream) throws Exception {
        return maxPeriodField.getInt(stream);
    }

    private int prevPeriod(Sonic stream) throws Exception {
        return prevPeriodField.getInt(stream);
    }

    private int callFindPitchPeriod(Sonic stream, short[] samples,
                                    boolean preferNew) throws Exception {
        return (Integer) findPitchPeriod.invoke(stream, samples, 0, preferNew);
    }

    private int callFindPitchPeriodInRange(Sonic stream, short[] samples,
                                           int minP, int maxP) throws Exception {
        return (Integer) findPitchPeriodInRange.invoke(stream, samples, 0,
            minP, maxP);
    }

    private long callFindPeriodDiff(Sonic stream, short[] samples,
                                    int period) throws Exception {
        return (Long) findPeriodDiff.invoke(stream, samples, 0, period);
    }

    private static int absSampleDelta(short a, short b) {
        int d = (int) a - (int) b;
        return d < 0 ? -d : d;
    }

    private static long referencePeriodDiff(short[] samples, int numChannels,
                                            int period) {
        long diff = 0;
        for (int c = 0; c < numChannels; c++) {
            for (int i = 0; i < period; i++) {
                diff += absSampleDelta(samples[i * numChannels + c],
                    samples[(i + period) * numChannels + c]);
            }
        }
        return diff;
    }

    private static int referenceBestPeriod(short[] samples, int numChannels,
                                           int minPeriod, int maxPeriod) {
        long bestDiff = 0;
        int bestPeriod = 0;
        for (int p = minPeriod; p <= maxPeriod; p++) {
            long diff = referencePeriodDiff(samples, numChannels, p);
            if (bestPeriod == 0 || diff * bestPeriod < bestDiff * p) {
                bestDiff = diff;
                bestPeriod = p;
            }
        }
        return bestPeriod;
    }

    private long lcgState = 1;

    private void lcgReset() {
        lcgState = 1;
    }

    private short lcgSample(int amplitude) {
        lcgState = (lcgState * 1103515245L + 12345L) & 0x7fffffffL;
        long state = lcgState;
        return (short) ((((state >> 16) & 0x7fff) - 16384) * amplitude
                        / 16384);
    }

    private int phaseAcc;
    private int phaseInc;

    private void probeReset(int frequency, int sampleRate) {
        phaseAcc = 0;
        phaseInc = (int) (4294967296.0 * frequency / sampleRate + 0.5);
    }

    private short probeSample(int amplitude) {
        int tri1, tri2, tri3, value;
        phaseAcc += phaseInc;
        tri1 = ((phaseAcc >> 15) & 0xffff) - 32768;
        tri2 = (((phaseAcc << 1) >> 15) & 0xffff) - 32768;
        tri3 = (((phaseAcc << 2) >> 15) & 0xffff) - 32768;
        value = (int) (0.6 * tri1 + 0.3 * tri2 + 0.1 * tri3);
        if (value > 32767) {
            value = 32767;
        } else if (value < -32767) {
            value = -32767;
        }
        return (short) ((long) value * amplitude / 32767);
    }

    private static class Sequence {
        int[] values = new int[SEQ_CAP];
        int length;
    }

    private Sequence periodSequence(Sonic stream, short[] samples,
                                    int frames) throws Exception {
        Sequence result = new Sequence();
        int position = 0;
        int channels = stream.getNumChannels();
        while (position + maxRequired(stream) <= frames
               && result.length < SEQ_CAP) {
            short[] window = new short[maxRequired(stream) * channels];
            System.arraycopy(samples, position * channels, window, 0,
                window.length);
            result.values[result.length++] =
                callFindPitchPeriod(stream, window, true);
            position += result.values[result.length - 1];
        }
        return result;
    }

    private void printSequence(String name, int quality, Sequence seq) {
        StringBuilder builder = new StringBuilder();
        builder.append("XL HISTORY name=").append(name)
               .append(" quality=").append(quality).append(" seq=");
        for (int i = 0; i < seq.length; i++) {
            builder.append(i == 0 ? "" : ",").append(seq.values[i]);
        }
        builder.append(" tag=JAVA");
        System.out.println(builder);
    }

    private void testCostTableAgainstReference() throws Exception {
        short[] data = new short[TEST_WINDOW * 2];
        Sonic stream = new Sonic(TEST_SAMPLE_RATE, 2);
        stream.setQuality(1);

        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < TEST_WINDOW; i++) {
            short v = probeSample(30000);
            data[2 * i] = v;
            data[2 * i + 1] = (short) -v;
        }
        for (int p = minPeriod(stream); p <= maxPeriod(stream); p++) {
            long expected = referencePeriodDiff(data, 2, p);
            long actual = callFindPeriodDiff(stream, data, p);
            casesRun++;
            if (expected != actual) {
                fail("antiphase cost mismatch at p=%d expected=%d actual=%d",
                    p, expected, actual);
                return;
            }
        }

        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < TEST_WINDOW; i++) {
            data[2 * i] = 0;
            data[2 * i + 1] = probeSample(30000);
        }
        for (int p = minPeriod(stream); p <= maxPeriod(stream); p++) {
            long expected = referencePeriodDiff(data, 2, p);
            long actual = callFindPeriodDiff(stream, data, p);
            casesRun++;
            if (expected != actual) {
                fail("silent-first cost mismatch at p=%d expected=%d actual=%d",
                    p, expected, actual);
                return;
            }
        }

        short[] six = new short[TEST_WINDOW * 6];
        stream = new Sonic(TEST_SAMPLE_RATE, 6);
        stream.setQuality(1);
        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < TEST_WINDOW; i++) {
            short v = probeSample(20000);
            for (int c = 0; c < 5; c++) {
                six[6 * i + c] = (c & 1) != 0 ? (short) -v : v;
            }
            six[6 * i + 5] = 0;
        }
        for (int p = minPeriod(stream); p <= maxPeriod(stream); p++) {
            long expected = referencePeriodDiff(six, 6, p);
            long actual = callFindPeriodDiff(stream, six, p);
            casesRun++;
            if (expected != actual) {
                fail("six-channel cost mismatch at p=%d expected=%d actual=%d",
                    p, expected, actual);
                return;
            }
        }
    }

    private void testFirstChannelSilent() throws Exception {
        short[] mono = new short[TEST_WINDOW];
        short[] stereo = new short[TEST_WINDOW * 2];
        short[] three = new short[TEST_WINDOW * 3];

        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < TEST_WINDOW; i++) {
            mono[i] = probeSample(30000);
        }
        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < TEST_WINDOW; i++) {
            short v = probeSample(30000);
            stereo[2 * i] = 0;
            stereo[2 * i + 1] = v;
            three[3 * i] = v;
            three[3 * i + 1] = 0;
            three[3 * i + 2] = (short) -v;
        }

        Sonic monoStream = new Sonic(TEST_SAMPLE_RATE, 1);
        Sonic stereoStream = new Sonic(TEST_SAMPLE_RATE, 2);
        Sonic threeStream = new Sonic(TEST_SAMPLE_RATE, 3);
        monoStream.setQuality(1);
        stereoStream.setQuality(1);
        threeStream.setQuality(1);

        int monoPeriod = callFindPitchPeriod(monoStream, mono, true);
        int stereoPeriod = callFindPitchPeriod(stereoStream, stereo, true);
        int threePeriod = callFindPitchPeriod(threeStream, three, true);
        int silencePeriod = minPeriod(stereoStream);

        check(monoPeriod != silencePeriod,
            "mono probe period %d equals the silence minimum %d",
            monoPeriod, silencePeriod);
        check(stereoPeriod == monoPeriod,
            "silent-first stereo picked %d, mono picked %d",
            stereoPeriod, monoPeriod);
        check(threePeriod == monoPeriod,
            "silent-middle 3-channel picked %d, mono picked %d",
            threePeriod, monoPeriod);
        check(referenceBestPeriod(stereo, 2, minPeriod(stereoStream),
                                  maxPeriod(stereoStream)) == stereoPeriod,
            "silent-first stereo disagrees with reference");

        short[] silence = new short[TEST_WINDOW * 2];
        short[] voiced = new short[TEST_WINDOW * 2];
        Sonic poisoned = new Sonic(TEST_SAMPLE_RATE, 2);
        poisoned.setQuality(1);
        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < TEST_WINDOW; i++) {
            short v = probeSample(30000);
            voiced[2 * i] = 0;
            voiced[2 * i + 1] = v;
        }
        callFindPitchPeriod(poisoned, silence, true);
        callFindPitchPeriod(poisoned, voiced, true);
        Sonic fresh = new Sonic(TEST_SAMPLE_RATE, 2);
        fresh.setQuality(1);
        callFindPitchPeriod(fresh, voiced, true);
        check(prevPeriod(poisoned) == prevPeriod(fresh),
            "silence window poisoned next period: %d vs fresh %d",
            prevPeriod(poisoned), prevPeriod(fresh));
    }

    private void testChannelsPreferDifferentPeriods() throws Exception {
        int loudPeriod = 150;
        int quietPeriod = 233;
        int loudAmp = 15000;
        int quietAmp = 9000;
        short[] loudPattern = new short[loudPeriod];
        short[] quietPattern = new short[quietPeriod];
        short[] data = new short[TEST_WINDOW * 3];
        short[] firstOnly = new short[TEST_WINDOW];
        short[] secondOnly = new short[TEST_WINDOW];

        lcgReset();
        for (int i = 0; i < loudPeriod; i++) {
            loudPattern[i] = lcgSample(loudAmp);
        }
        lcgReset();
        for (int i = 0; i < quietPeriod; i++) {
            quietPattern[i] = lcgSample(quietAmp);
        }
        for (int i = 0; i < TEST_WINDOW; i++) {
            short loudValue = loudPattern[i % loudPeriod];
            short quietValue = quietPattern[i % quietPeriod];
            data[3 * i] = loudValue;
            data[3 * i + 1] = quietValue;
            data[3 * i + 2] = quietValue;
            firstOnly[i] = loudValue;
            secondOnly[i] = quietValue;
        }

        Sonic stream = new Sonic(TEST_SAMPLE_RATE, 3);
        stream.setQuality(1);
        int firstChannel = referenceBestPeriod(firstOnly, 1,
            minPeriod(stream), maxPeriod(stream));
        int secondChannel = referenceBestPeriod(secondOnly, 1,
            minPeriod(stream), maxPeriod(stream));
        check(firstChannel == loudPeriod,
            "loud channel best period %d, expected %d",
            firstChannel, loudPeriod);
        check(secondChannel == quietPeriod,
            "quiet channel best period %d, expected %d",
            secondChannel, quietPeriod);
        check(firstChannel != secondChannel,
            "per-channel best periods do not differ: both %d", firstChannel);

        int reference = referenceBestPeriod(data, 3, minPeriod(stream),
            maxPeriod(stream));
        int combined = callFindPitchPeriod(stream, data, true);
        check(combined == reference,
            "combined period %d differs from reference %d",
            combined, reference);
        check(combined != firstChannel,
            "combined period %d equals first/loudest-channel-only answer %d",
            combined, firstChannel);
        System.out.printf(
            "XL CANDIDATE name=diffperiod period=%d firstOnly=%d secondOnly=%d%n",
            combined, firstChannel, secondChannel);

        for (int i = 0; i < TEST_WINDOW; i++) {
            short a = data[3 * i], b = data[3 * i + 1], c = data[3 * i + 2];
            data[3 * i] = c;
            data[3 * i + 1] = a;
            data[3 * i + 2] = b;
        }
        stream = new Sonic(TEST_SAMPLE_RATE, 3);
        stream.setQuality(1);
        reference = referenceBestPeriod(data, 3, minPeriod(stream),
            maxPeriod(stream));
        combined = callFindPitchPeriod(stream, data, true);
        check(combined == reference,
            "permuted combined %d differs from reference %d",
            combined, reference);
        check(combined != loudPeriod,
            "permuted combined period %d equals loudest-channel answer",
            combined);
    }

    private void testFullScaleArithmetic() throws Exception {
        short[] data = new short[TEST_WINDOW * 2];
        short[] mono = new short[TEST_WINDOW];
        for (int i = 0; i < TEST_WINDOW; i++) {
            short a = (i & 1) != 0 ? Short.MAX_VALUE : Short.MIN_VALUE;
            short b = (i & 1) != 0 ? Short.MIN_VALUE : Short.MAX_VALUE;
            data[2 * i] = a;
            data[2 * i + 1] = b;
        }
        Sonic stream = new Sonic(TEST_SAMPLE_RATE, 2);
        for (int p = minPeriod(stream); p <= maxPeriod(stream); p++) {
            long expected = referencePeriodDiff(data, 2, p);
            long actual = callFindPeriodDiff(stream, data, p);
            casesRun++;
            if (expected != actual) {
                fail("full-scale cost mismatch at p=%d expected=%d actual=%d",
                    p, expected, actual);
                return;
            }
        }

        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < TEST_WINDOW; i++) {
            mono[i] = probeSample(32767);
        }
        stream = new Sonic(TEST_SAMPLE_RATE, 1);
        stream.setQuality(1);
        int reference = referenceBestPeriod(mono, 1, minPeriod(stream),
            maxPeriod(stream));
        int periodQ1 = callFindPitchPeriod(stream, mono, true);

        Sonic q0 = new Sonic(TEST_SAMPLE_RATE, 1);
        q0.setQuality(0);
        int periodQ0 = callFindPitchPeriod(q0, mono, true);

        check(periodQ1 == reference,
            "full-scale q1 period %d differs from reference %d",
            periodQ1, reference);
        check(periodQ0 == periodQ1,
            "full-scale q0 period %d differs from q1 period %d",
            periodQ0, periodQ1);
        System.out.printf("XL CANDIDATE name=fullscale period=%d%n", periodQ1);
    }

    private void testSilence() throws Exception {
        int[] channelCounts = {1, 2, 6};
        for (int channels : channelCounts) {
            short[] silence = new short[TEST_WINDOW * channels];
            Sonic stream = new Sonic(TEST_SAMPLE_RATE, channels);
            stream.setQuality(1);
            int period = callFindPitchPeriodInRange(stream, silence,
                minPeriod(stream), maxPeriod(stream));
            check(period >= minPeriod(stream) && period <= maxPeriod(stream),
                "silence period %d out of range %d..%d for %d channels",
                period, minPeriod(stream), maxPeriod(stream), channels);
            Field minDiffField = Sonic.class.getDeclaredField("minDiff");
            minDiffField.setAccessible(true);
            Field maxDiffField = Sonic.class.getDeclaredField("maxDiff");
            maxDiffField.setAccessible(true);
            check(minDiffField.getInt(stream) == 0
                  && maxDiffField.getInt(stream) == 0,
                "silence diffs not zero: min=%d max=%d",
                minDiffField.getInt(stream), maxDiffField.getInt(stream));
        }
    }

    private void testRateChannelGrid() throws Exception {
        int[] rates = {8000, 16000, 48000};
        int[] channelCounts = {1, 2, 6};
        short[] mono = new short[TEST_WINDOW];
        short[] data = new short[TEST_WINDOW * MAX_CHANNELS];

        for (int rate : rates) {
            probeReset(220, rate);
            for (int i = 0; i < TEST_WINDOW; i++) {
                mono[i] = probeSample(20000);
            }
            for (int channels : channelCounts) {
                for (int quality = 0; quality <= 1; quality++) {
                    Sequence referenceSeq = null;
                    for (int variant = 0; variant < 4; variant++) {
                        Sonic stream = new Sonic(rate, channels);
                        stream.setQuality(quality);
                        for (int i = 0; i < TEST_WINDOW; i++) {
                            for (int ch = 0; ch < channels; ch++) {
                                short value;
                                switch (variant) {
                                    case 0:
                                        value = mono[i];
                                        break;
                                    case 1:
                                        value = (ch & 1) != 0
                                            ? (short) -mono[i] : mono[i];
                                        break;
                                    case 2:
                                        value = mono[i];
                                        break;
                                    default:
                                        value = (ch < 2)
                                            ? ((ch & 1) != 0
                                                ? (short) -mono[i] : mono[i])
                                            : 0;
                                        break;
                                }
                                data[i * channels + ch] = value;
                            }
                            if (variant == 2 && channels > 1) {
                                short saved = data[i * channels];
                                for (int ch = 0; ch < channels - 1; ch++) {
                                    data[i * channels + ch] =
                                        data[i * channels + ch + 1];
                                }
                                data[i * channels + channels - 1] = saved;
                            }
                        }
                        Sequence seq = periodSequence(stream, data,
                            TEST_WINDOW);
                        check(seq.length > 0,
                            "no periods generated rate=%d ch=%d q=%d v=%d",
                            rate, channels, quality, variant);
                        for (int k = 0; k < seq.length; k++) {
                            check(seq.values[k] >= minPeriod(stream)
                                  && seq.values[k] <= maxPeriod(stream),
                                "period %d out of range rate=%d ch=%d q=%d v=%d",
                                seq.values[k], rate, channels, quality,
                                variant);
                        }
                        if (referenceSeq == null) {
                            referenceSeq = seq;
                        } else {
                            check(seq.length == referenceSeq.length,
                                "sequence length differs rate=%d ch=%d q=%d v=%d",
                                rate, channels, quality, variant);
                            for (int k = 0; k < seq.length; k++) {
                                check(seq.values[k] == referenceSeq.values[k],
                                    "sequence differs rate=%d ch=%d q=%d v=%d at %d",
                                    rate, channels, quality, variant, k);
                            }
                        }
                        if (variant == 0 || variant == 1) {
                            StringBuilder builder = new StringBuilder();
                            builder.append("XL GRID rate=").append(rate)
                                   .append(" ch=").append(channels)
                                   .append(" q=").append(quality)
                                   .append(" variant=").append(variant)
                                   .append(" seq=");
                            for (int k = 0; k < seq.length; k++) {
                                builder.append(k == 0 ? "" : ",")
                                       .append(seq.values[k]);
                            }
                            System.out.println(builder);
                        }
                    }
                }
            }
        }
    }

    private void testCrossLanguageVectors() throws Exception {
        String[] names = {"probe", "silentfirst"};
        short[] data = new short[TEST_WINDOW * 2];
        for (int v = 0; v < 2; v++) {
            probeReset(220, TEST_SAMPLE_RATE);
            for (int i = 0; i < TEST_WINDOW; i++) {
                short value = probeSample(30000);
                if (v == 0) {
                    data[2 * i] = value;
                    data[2 * i + 1] = (short) -value;
                } else {
                    data[2 * i] = 0;
                    data[2 * i + 1] = value;
                }
            }
            for (int quality = 0; quality <= 1; quality++) {
                Sonic stream = new Sonic(TEST_SAMPLE_RATE, 2);
                stream.setQuality(quality);
                Sequence seq = periodSequence(stream, data, TEST_WINDOW);
                printSequence(names[v], quality, seq);
                if (quality == 1) {
                    int candidate = callFindPitchPeriodInRange(stream, data,
                        minPeriod(stream), maxPeriod(stream));
                    System.out.printf("XL CANDIDATE name=%s period=%d%n",
                        names[v], candidate);
                }
            }
        }
    }

    private void testReconfigureAndReuse() throws Exception {
        short[] data = new short[TEST_WINDOW * MAX_CHANNELS];
        short[] silence = new short[2048 * MAX_CHANNELS];

        Sonic stream = new Sonic(48000, 2);
        probeReset(220, 48000);
        for (int i = 0; i < TEST_WINDOW; i++) {
            short v = probeSample(20000);
            data[2 * i] = v;
            data[2 * i + 1] = (short) -v;
        }
        stream.setQuality(1);
        callFindPitchPeriod(stream, data, true);

        stream.setSampleRate(16000);
        stream.setNumChannels(6);
        check(stream.getSampleRate() == 16000, "sample rate not reset");
        check(stream.getNumChannels() == 6, "channel count not reset");
        stream.writeShortToStream(silence, 2048);
        stream.flushStream();
        check(stream.samplesAvailable() >= 0, "bad available count");

        stream.setSampleRate(8000);
        stream.setNumChannels(1);
        probeReset(220, 8000);
        for (int i = 0; i < TEST_WINDOW; i++) {
            data[i] = probeSample(20000);
        }
        stream.setQuality(1);
        Sequence seqA = periodSequence(stream, data, TEST_WINDOW);
        Sonic fresh = new Sonic(8000, 1);
        fresh.setQuality(1);
        Sequence seqB = periodSequence(fresh, data, TEST_WINDOW);
        check(seqA.length == seqB.length,
            "reused stream sequence length %d vs fresh %d",
            seqA.length, seqB.length);
        for (int k = 0; k < seqA.length; k++) {
            check(seqA.values[k] == seqB.values[k],
                "reused stream period %d vs fresh %d at %d",
                seqA.values[k], seqB.values[k], k);
        }

        for (int channels = 1; channels <= 6; channels++) {
            Sonic cycled = new Sonic(48000, channels);
            cycled.setNumChannels(channels);
            cycled.setSampleRate(48000);
            short[] channelSilence = new short[2048 * channels];
            cycled.writeShortToStream(channelSilence, 2048);
            cycled.flushStream();
        }
    }

    private static class PcmBuffer {
        short[] data = new short[4096];
        int frames;
        int channels;

        void ensure(int extraFrames) {
            if (frames + extraFrames <= data.length / channels) {
                return;
            }
            int capacity = data.length;
            while (frames + extraFrames > capacity / channels) {
                capacity *= 2;
            }
            short[] grown = new short[capacity];
            System.arraycopy(data, 0, grown, 0,
                frames * channels);
            data = grown;
        }
    }

    private static void drain(Sonic stream, PcmBuffer buffer) {
        short[] chunk = new short[2048 * MAX_CHANNELS];
        while (true) {
            int read = stream.readShortFromStream(chunk, 2048);
            if (read <= 0) {
                return;
            }
            buffer.ensure(read);
            System.arraycopy(chunk, 0, buffer.data,
                buffer.frames * buffer.channels, read * buffer.channels);
            buffer.frames += read;
        }
    }

    private PcmBuffer runPublicStream(int channels, float speed,
                                      short[] input, int inputFrames,
                                      boolean alternatingChunks) {
        Sonic stream = new Sonic(TEST_SAMPLE_RATE, channels);
        int[] chunkSizes = {1, 7, 113};
        int chunkIndex = 0;
        int position = 0;
        PcmBuffer output = new PcmBuffer();
        output.channels = channels;

        stream.setSpeed(speed);
        stream.setPitch(1.0f);
        stream.setRate(1.0f);
        while (position < inputFrames) {
            int count;
            if (alternatingChunks) {
                count = chunkSizes[chunkIndex++ % 3];
                if (count > inputFrames - position) {
                    count = inputFrames - position;
                }
            } else {
                count = inputFrames - position;
            }
            short[] slice = new short[count * channels];
            System.arraycopy(input, position * channels, slice, 0,
                slice.length);
            stream.writeShortToStream(slice, count);
            drain(stream, output);
            position += count;
        }
        stream.flushStream();
        drain(stream, output);
        return output;
    }

    private static int dominantFrequency(PcmBuffer buffer, int channel) {
        int start = buffer.frames / 5;
        int end = buffer.frames - buffer.frames / 5;
        int bestLag = 0;
        double bestCorrelation = -1.0;
        for (int lag = 180; lag <= 260; lag++) {
            double correlation = 0.0;
            for (int n = start; n + lag < end; n++) {
                correlation += (double) buffer.data[n * buffer.channels
                                                    + channel]
                    * buffer.data[(n + lag) * buffer.channels + channel];
            }
            if (correlation > bestCorrelation) {
                bestCorrelation = correlation;
                bestLag = lag;
            }
        }
        return (int) (TEST_SAMPLE_RATE / (double) bestLag + 0.5);
    }

    private static class ChannelXform {
        final int refChannel;
        final int sign;

        ChannelXform(int refChannel, int sign) {
            this.refChannel = refChannel;
            this.sign = sign;
        }
    }

    private static int transformedMaxDelta(PcmBuffer ref, PcmBuffer got,
                                           ChannelXform[] xform) {
        int frames = Math.min(ref.frames, got.frames);
        int worst = 0;
        for (int frame = 0; frame < frames; frame++) {
            for (int ch = 0; ch < got.channels; ch++) {
                int expected = 0;
                int actual = got.data[frame * got.channels + ch];
                if (xform[ch].refChannel >= 0) {
                    expected = xform[ch].sign
                        * ref.data[frame * ref.channels
                                   + xform[ch].refChannel];
                }
                int delta = actual - expected;
                if (delta < 0) {
                    delta = -delta;
                }
                if (delta > worst) {
                    worst = delta;
                }
            }
        }
        return worst;
    }

    private void testPublicStreaming() throws Exception {
        float[] speeds = {0.7f, 1.6f};
        int inputFrames = 8192;
        short[] mono = new short[inputFrames];
        short[] inPhase = new short[inputFrames * 2];
        short[] antiPhase = new short[inputFrames * 2];
        short[] swapped = new short[inputFrames * 2];
        short[] sixChannel = new short[inputFrames * 6];

        probeReset(220, TEST_SAMPLE_RATE);
        for (int i = 0; i < inputFrames; i++) {
            mono[i] = probeSample(28000);
        }
        for (int i = 0; i < inputFrames; i++) {
            inPhase[2 * i] = mono[i];
            inPhase[2 * i + 1] = mono[i];
            antiPhase[2 * i] = mono[i];
            antiPhase[2 * i + 1] = (short) -mono[i];
            swapped[2 * i] = (short) -mono[i];
            swapped[2 * i + 1] = mono[i];
            sixChannel[6 * i] = mono[i];
            sixChannel[6 * i + 1] = (short) -mono[i];
            sixChannel[6 * i + 2] = 0;
            sixChannel[6 * i + 3] = 0;
            sixChannel[6 * i + 4] = 0;
            sixChannel[6 * i + 5] = 0;
        }

        ChannelXform[] identity = {
            new ChannelXform(0, 1), new ChannelXform(1, 1)};
        ChannelXform[] antiXform = {
            new ChannelXform(0, 1), new ChannelXform(1, -1)};
        ChannelXform[] swapXform = {
            new ChannelXform(0, -1), new ChannelXform(1, 1)};
        ChannelXform[] sixXform = {
            new ChannelXform(0, 1), new ChannelXform(1, -1),
            new ChannelXform(-1, 0), new ChannelXform(-1, 0),
            new ChannelXform(-1, 0), new ChannelXform(-1, 0)};

        for (float speed : speeds) {
            for (int chunkMode = 0; chunkMode < 2; chunkMode++) {
                boolean chunks = chunkMode != 0;
                PcmBuffer outIn = runPublicStream(2, speed, inPhase,
                    inputFrames, chunks);
                PcmBuffer outAnti = runPublicStream(2, speed, antiPhase,
                    inputFrames, chunks);
                PcmBuffer outSwap = runPublicStream(2, speed, swapped,
                    inputFrames, chunks);
                PcmBuffer outSix = runPublicStream(6, speed, sixChannel,
                    inputFrames, chunks);

                check(outIn.frames > 1000, "degenerate output length %d",
                    outIn.frames);
                check(outIn.frames == outAnti.frames,
                    "antiphase length %d vs in-phase %d (speed=%.2f chunks=%d)",
                    outAnti.frames, outIn.frames, speed, chunkMode);
                check(outIn.frames == outSwap.frames,
                    "swapped length %d vs in-phase %d (speed=%.2f chunks=%d)",
                    outSwap.frames, outIn.frames, speed, chunkMode);
                check(outSix.frames == outIn.frames,
                    "6-channel length %d vs stereo %d (speed=%.2f chunks=%d)",
                    outSix.frames, outIn.frames, speed, chunkMode);
                check(transformedMaxDelta(outIn, outIn, identity) == 0,
                    "identity transform comparator broken");
                int antiDelta = transformedMaxDelta(outIn, outAnti, antiXform);
                int swapDelta = transformedMaxDelta(outIn, outSwap, swapXform);
                int sixDelta = transformedMaxDelta(outIn, outSix, sixXform);
                check(antiDelta <= 2,
                    "antiphase sample delta %d exceeds 2 (speed=%.2f chunks=%d)",
                    antiDelta, speed, chunkMode);
                check(swapDelta <= 2,
                    "swapped sample delta %d exceeds 2 (speed=%.2f chunks=%d)",
                    swapDelta, speed, chunkMode);
                check(sixDelta <= 2,
                    "6-channel sample delta %d exceeds 2 (speed=%.2f chunks=%d)",
                    sixDelta, speed, chunkMode);
                int frequency = dominantFrequency(outAnti, 0);
                check(Math.abs(frequency - 220) <= 3,
                    "antiphase dominant frequency %d not within 3 Hz of 220",
                    frequency);
                frequency = dominantFrequency(outAnti, 1);
                check(Math.abs(frequency - 220) <= 3,
                    "antiphase channel 1 frequency %d not within 3 Hz of 220",
                    frequency);
                System.out.printf(
                    "XL STREAM impl=JAVA speed=%.2f chunks=%d frames=%d maxdelta=%d%n",
                    speed, chunkMode, outIn.frames, antiDelta);
            }
        }

        for (int chunkMode = 0; chunkMode < 2; chunkMode++) {
            PcmBuffer out = runPublicStream(2, 1.0f, antiPhase, inputFrames,
                chunkMode != 0);
            check(out.frames == inputFrames, "passthrough length %d vs %d",
                out.frames, inputFrames);
            PcmBuffer input = new PcmBuffer();
            input.channels = 2;
            input.frames = inputFrames;
            input.data = antiPhase;
            check(transformedMaxDelta(input, out, identity) == 0,
                "passthrough modified samples (chunks=%d)", chunkMode);
        }
    }

    private void runAll() throws Exception {
        testCostTableAgainstReference();
        testFirstChannelSilent();
        testChannelsPreferDifferentPeriods();
        testFullScaleArithmetic();
        testSilence();
        testRateChannelGrid();
        testCrossLanguageVectors();
        testReconfigureAndReuse();
        testPublicStreaming();
    }

    public static void main(String[] args) throws Exception {
        MultichannelPeriod harness = new MultichannelPeriod();
        try {
            harness.runAll();
        } catch (TestFailure expected) {
            // Counted and reported at the point of failure.
        }
        System.out.printf("JAVA_SUMMARY cases=%d failures=%d%n",
            casesRun, casesFailed);
        if (casesRun == 0 || casesFailed != 0) {
            System.exit(1);
        }
    }
}
