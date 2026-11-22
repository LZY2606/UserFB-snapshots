////////////////////////////////////////////////////////////////////////////////
///
/// Tests for the optional source contribution tracing of SoundTouch.
///
/// Registered task case groups:
///   coefficients - independent verification of CUBIC / FIR coefficient
///                  combination and WSOLA overlap same-source merging
///   pipeline     - end-to-end row/PCM behaviour through the full pipeline
///   lifetime     - row ownership, storage bounding, clear/flush semantics
///   legacy       - old API behaviour and bit-identical PCM with/without trace
///
////////////////////////////////////////////////////////////////////////////////

#include "task_support/check.h"

#include "SoundTouch.h"
#include "TDStretch.h"
#include "RateTransposer.h"
#include "InterpolateCubic.h"
#include "AAFilter.h"
#include "FIFOSampleBuffer.h"
#include "SourceTrace.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace soundtouch;

// ---------------------------------------------------------------------------
// common helpers
// ---------------------------------------------------------------------------

// deterministic non-periodic multi-channel test signal within [-0.5, 0.5]
static float genSample(unsigned i, int c)
{
    uint32_t h = (uint32_t)(i * 2654435761u) ^ (uint32_t)((c + 1) * 40503u);
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    double noise = ((h >> 8) & 0xFFFF) / 65535.0 * 2.0 - 1.0;
    double v = 0.17 * sin(i * 0.0137 + c * 1.13)
             + 0.11 * cos(i * 0.000773 + c * 0.71)
             + 0.07 * sin(i * (i % 977) * 0.00000131 + c * 0.3)
             + 0.08 * noise;
    return (float)v;
}

static std::vector<float> genInput(unsigned frames, int channels)
{
    std::vector<float> in(frames * channels);
    for (unsigned i = 0; i < frames; i ++)
        for (int c = 0; c < channels; c ++)
            in[i * channels + c] = genSample(i, c);
    return in;
}

// reconstruct one output sample from its source row and the original PCM
static double reconstruct(const SourceRow &row, const std::vector<float> &in,
                          int channels, int c, uint64_t firstFrame)
{
    double v = 0;
    for (size_t k = 0; k < row.size(); k ++)
    {
        uint64_t idx = row[k].frame - firstFrame;
        v += row[k].weight * in[(size_t)idx * channels + c];
    }
    return v;
}

static bool rowSortedUnique(const SourceRow &row)
{
    for (size_t k = 1; k < row.size(); k ++)
        if (row[k].frame <= row[k - 1].frame) return false;
    return true;
}

// ---------------------------------------------------------------------------
// coefficients group
// ---------------------------------------------------------------------------

// cubic interpolation coefficients must appear verbatim in the rows
static int case_cubic_coefficients()
{
    const int channels = 2;
    const float rate = 1.31f;
    const int numSrc = 500;

    // cubic coefficient table (mirrors the production table)
    static const float C[16] =
    { -0.5f,  1.0f, -0.5f, 0.0f,
       1.5f, -2.5f,  0.0f, 1.0f,
      -1.5f,  2.0f,  0.5f, 0.0f,
       0.5f, -0.5f,  0.0f, 0.0f};

    std::vector<float> samples(numSrc * channels);
    for (int i = 0; i < numSrc * channels; i ++) samples[i] = genSample(i / channels, i % channels);

    // singleton source rows
    std::vector<SourceRow> inRows(numSrc);
    for (int i = 0; i < numSrc; i ++)
        inRows[i].push_back(SourceWeight{(uint64_t)(900 + i), 1.0});

    // traced transpose
    InterpolateCubic traced;
    traced.setChannels(channels);
    traced.setRate(rate);
    FIFOSampleBuffer srcBuf(channels), dstBuf(channels);
    SourceRowFifo srcRows, dstRows;
    srcBuf.putSamples(samples.data(), numSrc);
    srcRows.put(inRows.data(), numSrc);
    int produced = traced.transposeTraced(dstBuf, srcBuf, dstRows, srcRows);

    // plain transpose on a twin for bit-identical PCM comparison
    InterpolateCubic plain;
    plain.setChannels(channels);
    plain.setRate(rate);
    FIFOSampleBuffer srcBuf2(channels), dstBuf2(channels);
    srcBuf2.putSamples(samples.data(), numSrc);
    int produced2 = plain.transpose(dstBuf2, srcBuf2);

    TASK_CHECK(produced > 0);
    TASK_CHECK(produced == produced2);
    TASK_CHECK((int)dstRows.size() == produced);
    TASK_CHECK(dstBuf.numSamples() == dstBuf2.numSamples());
    TASK_CHECK(srcBuf.numSamples() == srcBuf2.numSamples());
    TASK_CHECK(srcRows.size() == srcBuf.numSamples());
    TASK_CHECK(0 == memcmp(dstBuf.ptrBegin(), dstBuf2.ptrBegin(),
                           dstBuf.numSamples() * channels * sizeof(float)));

    // independent simulation of the fract/coefficient sequence
    float fract = 0;
    int srcCount = 0;
    int srcSampleEnd = numSrc - 4;
    int out = 0;
    int consumed = 0;
    bool sawNegative = false, sawFractional = false;
    while (srcCount < srcSampleEnd)
    {
        const float x3 = 1.0f;
        const float x2 = (float)fract;
        const float x1 = x2 * x2;
        const float x0 = x1 * x2;
        float y[4];
        y[0] = C[0] * x0 + C[1] * x1 + C[2] * x2 + C[3] * x3;
        y[1] = C[4] * x0 + C[5] * x1 + C[6] * x2 + C[7] * x3;
        y[2] = C[8] * x0 + C[9] * x1 + C[10] * x2 + C[11] * x3;
        y[3] = C[12] * x0 + C[13] * x1 + C[14] * x2 + C[15] * x3;

        const SourceRow &row = dstRows[out];
        // expected terms: frames srcCount..srcCount+3 with nonzero coefficients
        std::vector<SourceWeight> expect;
        for (int t = 0; t < 4; t ++)
            if (y[t] != 0.0f)
                expect.push_back(SourceWeight{(uint64_t)(900 + srcCount + t), (double)y[t]});

        TASK_CHECK(row.size() == expect.size());
        for (size_t k = 0; k < expect.size(); k ++)
        {
            TASK_CHECK(row[k].frame == expect[k].frame);
            TASK_CHECK(row[k].weight == expect[k].weight);
            if (row[k].weight < 0) sawNegative = true;
            if (row[k].weight != 1.0 && row[k].weight != 0.0) sawFractional = true;
        }
        TASK_CHECK(rowSortedUnique(row));

        out ++;
        fract += rate;
        int whole = (int)fract;
        fract -= whole;
        srcCount += whole;
        consumed = srcCount;
    }
    TASK_CHECK(out == produced);
    TASK_CHECK(srcRows.size() == (uint)(numSrc - consumed));
    TASK_CHECK(sawNegative);        // cubic has negative lobes; weights must not be clamped
    TASK_CHECK(sawFractional);
    return 0;
}

// anti-alias FIR coefficients must appear verbatim in the rows
static int case_fir_coefficients()
{
    const uint length = 64;
    const double cutoff = 0.35;
    const int channels = 1;
    const uint numSrc = 200;

    AAFilter aa(length);
    aa.setCutoffFreq(cutoff);

    std::vector<float> samples(numSrc);
    for (uint i = 0; i < numSrc; i ++) samples[i] = genSample(i, 0);

    std::vector<SourceRow> inRows(numSrc);
    for (uint i = 0; i < numSrc; i ++)
        inRows[i].push_back(SourceWeight{(uint64_t)(5000 + i), 1.0});

    FIFOSampleBuffer srcBuf(channels), dstBuf(channels);
    SourceRowFifo srcRows, dstRows;
    srcBuf.putSamples(samples.data(), numSrc);
    srcRows.put(inRows.data(), numSrc);
    uint produced = aa.evaluateTraced(dstBuf, srcBuf, dstRows, srcRows);

    TASK_CHECK(produced == numSrc - length);
    TASK_CHECK(dstRows.size() == produced);
    TASK_CHECK(srcRows.size() == numSrc - produced);
    TASK_CHECK(srcBuf.numSamples() == numSrc - produced);

    // twin run through the plain interface: PCM must be bit-identical
    AAFilter aa2(length);
    aa2.setCutoffFreq(cutoff);
    FIFOSampleBuffer srcBuf2(channels), dstBuf2(channels);
    srcBuf2.putSamples(samples.data(), numSrc);
    uint produced2 = aa2.evaluate(dstBuf2, srcBuf2);
    TASK_CHECK(produced == produced2);
    TASK_CHECK(0 == memcmp(dstBuf.ptrBegin(), dstBuf2.ptrBegin(), produced * sizeof(float)));

    // independently recompute the Hamming-windowed sinc coefficients exactly
    // the way the production filter design does (including its scaling quirk)
    const double PI = 3.141592655357989;
    double wc = 2.0 * PI * cutoff;
    double tempCoeff = 2.0 * PI / (double)length;
    double sum = 0;
    std::vector<double> work(length);
    for (uint i = 0; i < length; i ++)
    {
        double cntTemp = (double)i - (double)(length / 2);
        double temp = cntTemp * wc;
        double h = (temp != 0) ? sin(temp) / temp : 1.0;
        double w = 0.54 + 0.46 * cos(tempCoeff * cntTemp);
        work[i] = w * h;
        sum += work[i];
    }
    double scaleCoeff = 16384.0f / sum;
    std::vector<float> coeffs(length);
    for (uint i = 0; i < length; i ++)
    {
        double temp = work[i] * scaleCoeff;
        temp += (temp >= 0) ? 0.5 : -0.5;
        coeffs[i] = (float)temp;
    }
    double dScaler = 1.0 / 16384.0;

    // every output row must be the shifted copy of the same coefficient set
    bool sawNegative = false;
    for (uint j = 0; j < produced; j ++)
    {
        const SourceRow &row = dstRows[j];
        // expected terms: frames 5000+j+i with weight coeffs[i]*dScaler, zeros dropped
        std::vector<SourceWeight> expect;
        for (uint i = 0; i < length; i ++)
        {
            double w = (double)coeffs[i] * dScaler;
            if (w != 0.0) expect.push_back(SourceWeight{(uint64_t)(5000 + j + i), w});
        }
        TASK_CHECK(row.size() == expect.size());
        for (size_t k = 0; k < expect.size(); k ++)
        {
            TASK_CHECK(row[k].frame == expect[k].frame);
            TASK_CHECK(row[k].weight == expect[k].weight);
            if (row[k].weight < 0) sawNegative = true;
        }
        TASK_CHECK(rowSortedUnique(row));
    }
    TASK_CHECK(sawNegative);    // sinc has negative taps

    // reconstruction of the filtered PCM from the rows
    std::vector<float> inVec(samples.begin(), samples.end());
    for (uint j = 0; j < produced; j ++)
    {
        double rec = reconstruct(dstRows[j], inVec, 1, 0, 5000);
        TASK_CHECK(fabs(rec - dstBuf.ptrBegin()[j]) <= 3e-5);
    }
    return 0;
}

// WSOLA overlap: same source frame arriving from the copy path and from the
// mid-buffer path must be merged into a single term with summed weights.
// Uses a constant mono signal so the best overlap position is deterministic
// (seekLength/2), and tempo 1.0 so that the mid-buffer frames of the previous
// sequence coincide exactly with the next sequence's overlap window frames.
// The expected rows are simulated independently (manual merge).
static int case_wsola_overlap_merge()
{
    const int sampleRate = 44100;
    const int sequenceMs = 82, seekWindowMs = 28, overlapMs = 8;
    const float tempo = 1.0f;
    const unsigned total = 60000;

    TDStretch *st = TDStretch::newInstance();
    st->setChannels(1);
    st->setParameters(sampleRate, sequenceMs, seekWindowMs, overlapMs);
    st->setTempo(tempo);
    st->enableTrace(true);

    // derived algorithm constants (mirror the production calculations)
    const int seekWindowLength = sampleRate * sequenceMs / 1000;    // 3616
    const int seekLength = sampleRate * seekWindowMs / 1000;        // 1234
    int ovl = sampleRate * overlapMs / 1000;                        // 352
    if (ovl < 16) ovl = 16;
    ovl -= ovl % 8;
    const int overlapLength = ovl;
    const int temp = seekWindowLength - 2 * overlapLength;          // 2912
    const float nominalSkip = tempo * (seekWindowLength - overlapLength);
    const int intskip = (int)(nominalSkip + 0.5f);
    const int sampleReq = (intskip + overlapLength > seekWindowLength ?
                           intskip + overlapLength : seekWindowLength) + seekLength;
    const int offset = seekLength / 2;  // deterministic best position for constant input

    TASK_CHECK(sampleReq > 0);

    // independent simulation state
    std::deque<SourceRow> simIn;
    std::vector<SourceRow> simMid(overlapLength);
    std::deque<SourceRow> simOut;
    float skipFract = 0;
    unsigned merges = 0;

    std::vector<float> samples(total, 0.3f);
    std::vector<float> pcmOut;
    std::vector<SourceRow> rowsOut;

    unsigned pos = 0;
    while (pos < total)
    {
        unsigned n = 997;
        if (n > total - pos) n = total - pos;

        // feed the processor: rows first, then samples
        std::vector<SourceRow> rows(n);
        for (unsigned i = 0; i < n; i ++)
            rows[i].push_back(SourceWeight{(uint64_t)(pos + i), 1.0});
        st->inputRows().put(rows.data(), n);
        st->putSamples(samples.data() + pos, n);
        for (unsigned i = 0; i < n; i ++)
            simIn.push_back(SourceRow{SourceWeight{(uint64_t)(pos + i), 1.0}});
        pos += n;

        // simulate the processing loop with a manual, independent merge
        while ((int)simIn.size() >= sampleReq)
        {
            for (int i = 0; i < overlapLength; i ++)
            {
                double cIn = (double)i / (double)overlapLength;
                double cMid = (double)(overlapLength - i) / (double)overlapLength;
                SourceRow expect;
                if (cIn != 0.0)
                    for (size_t a = 0; a < simIn[offset + i].size(); a ++)
                        expect.push_back(SourceWeight{
                            simIn[offset + i][a].frame, simIn[offset + i][a].weight * cIn});
                if (cMid != 0.0)
                    for (size_t b = 0; b < simMid[i].size(); b ++)
                    {
                        uint64_t f = simMid[i][b].frame;
                        double w = simMid[i][b].weight * cMid;
                        bool found = false;
                        for (size_t k = 0; k < expect.size(); k ++)
                            if (expect[k].frame == f)
                            {
                                expect[k].weight += w;
                                found = true;
                                merges ++;
                                break;
                            }
                        if (!found) expect.push_back(SourceWeight{f, w});
                    }
                // keep ascending order (input path frames precede mid frames here)
                for (size_t k = 1; k < expect.size(); k ++)
                    TASK_CHECK(expect[k].frame > expect[k - 1].frame);
                simOut.push_back(expect);
            }
            for (int i = 0; i < temp; i ++)
                simOut.push_back(simIn[offset + overlapLength + i]);
            for (int i = 0; i < overlapLength; i ++)
                simMid[i] = simIn[offset + temp + overlapLength + i];
            skipFract += nominalSkip;
            int ovlSkip = (int)skipFract;
            skipFract -= ovlSkip;
            for (int i = 0; i < ovlSkip; i ++) simIn.pop_front();
        }

        // compare the processor's queued rows against the simulation
        TASK_CHECK(st->outputRows().size() == simOut.size());
        uint avail = st->numSamples();
        TASK_CHECK(avail == simOut.size());
        std::vector<float> pcm(avail ? avail : 1);
        std::vector<SourceRow> got;
        uint nread = st->receiveSamples(pcm.data(), avail);
        st->outputRows().receive(got, nread);
        TASK_CHECK(nread == avail);
        TASK_CHECK(got.size() == simOut.size());
        for (size_t k = 0; k < got.size(); k ++)
        {
            TASK_CHECK(got[k].size() == simOut[k].size());
            for (size_t t = 0; t < got[k].size(); t ++)
            {
                TASK_CHECK(got[k][t].frame == simOut[k][t].frame);
                TASK_CHECK(got[k][t].weight == simOut[k][t].weight);
            }
            TASK_CHECK(rowSortedUnique(got[k]));
        }
        for (uint k = 0; k < nread; k ++) simOut.pop_front();
        pcmOut.insert(pcmOut.end(), pcm.begin(), pcm.begin() + nread);
        rowsOut.insert(rowsOut.end(), got.begin(), got.end());
    }

    delete st;

    // the test must actually have exercised same-source merging
    TASK_CHECK(merges > 10);
    TASK_CHECK(pcmOut.size() > 1000);

    // every output sample reconstructs from its row and the constant input
    for (size_t k = 0; k < rowsOut.size(); k ++)
    {
        double rec = reconstruct(rowsOut[k], samples, 1, 0, 0);
        TASK_CHECK(fabs(rec - pcmOut[k]) <= 3e-5);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// pipeline group
// ---------------------------------------------------------------------------

struct PipelineResult
{
    unsigned received;
    double energy;
    double maxErr;
};

// runs one traced pipeline configuration; verifies row structure and
// reconstruction; also runs an untraced twin and demands bit-identical PCM
static PipelineResult runPipelineConfig(int channels, uint sampleRate, float tempo,
                                        float rate, int aa, int quickseek,
                                        uint64_t firstFrame)
{
    PipelineResult res = {0, 0, 0};
    const unsigned total = 24000;
    std::vector<float> input = genInput(total, channels);

    SoundTouch st, twin;
    for (SoundTouch *s : {&st, &twin})
    {
        s->setSampleRate(sampleRate);
        s->setChannels(channels);
        s->setTempo(tempo);
        s->setRate(rate);
        s->setSetting(SETTING_USE_AA_FILTER, aa);
        s->setSetting(SETTING_USE_QUICKSEEK, quickseek);
    }
    st.enableSourceTrace(firstFrame);

    unsigned sent = 0, blocks = 0;
    std::vector<float> out(113 * channels), outTwin(113 * channels);
    std::vector<SourceRow> rows;

    auto drain = [&]()
    {
        unsigned n, n2;
        do
        {
            n = st.receiveSamples(out.data(), 113, rows);
            n2 = twin.receiveSamples(outTwin.data(), 113);
            res.received += n;
            if (n != n2) res.maxErr = 1e9;
            if (n && memcmp(out.data(), outTwin.data(), n * channels * sizeof(float)))
                res.maxErr = 1e9;
            if (rows.size() != n) res.maxErr = 1e9;
            for (unsigned i = 0; i < n; i ++)
            {
                if (!rowSortedUnique(rows[i])) res.maxErr = 1e9;
                for (int c = 0; c < channels; c ++)
                {
                    double rec = 0;
                    for (size_t k = 0; k < rows[i].size(); k ++)
                    {
                        if (rows[i][k].frame < firstFrame) { res.maxErr = 1e9; continue; }
                        uint64_t idx = rows[i][k].frame - firstFrame;
                        if (idx >= total) { res.maxErr = 1e9; continue; }
                        rec += rows[i][k].weight * input[(size_t)idx * channels + c];
                    }
                    double err = fabs(rec - out[i * channels + c]);
                    if (rows[i].empty() && fabs(out[i * channels + c]) > 1e-6)
                        res.maxErr = 1e9;   // silent rows must produce silence
                    if (err > res.maxErr) res.maxErr = err;
                    res.energy += (double)out[i * channels + c] * out[i * channels + c];
                }
            }
        } while (n > 0);
    };

    while (sent < total)
    {
        unsigned n = (blocks % 3 == 0 ? 1 : blocks % 3 == 1 ? 7 : 113);
        blocks ++;
        if (n > total - sent) n = total - sent;
        st.putSamples(input.data() + sent * channels, n);
        twin.putSamples(input.data() + sent * channels, n);
        sent += n;
        if (st.numSamples() != twin.numSamples()) res.maxErr = 1e9;
        if (blocks % 4) drain();
    }
    st.flush();
    twin.flush();
    drain();

    return res;
}

static int case_pipeline_matrix()
{
    struct Cfg { int ch; uint sr; float tempo, rate; int aa, qs; };
    const Cfg cfgs[] = {
        {1, 44100, 0.83f, 1.31f, 1, 0},
        {2, 48000, 1.31f, 0.73f, 1, 1},
        {6, 48000, 0.97f, 1.0f,  0, 0},
        {2, 44100, 1.0f,  0.5f,  1, 0},
        {1, 48000, 0.5f,  2.0f,  0, 1},
        {6, 44100, 1.77f, 1.13f, 1, 1},
    };
    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i ++)
    {
        PipelineResult r = runPipelineConfig(cfgs[i].ch, cfgs[i].sr, cfgs[i].tempo,
                                             cfgs[i].rate, cfgs[i].aa, cfgs[i].qs, 0);
        TASK_CHECK(r.maxErr <= 3e-5);
        TASK_CHECK(r.received > 5000);  // non-empty audio actually processed
        TASK_CHECK(r.energy > 1.0);     // non-silent audio actually processed
    }
    return 0;
}

// alternate the three receive variants; contributions must not shift
static int case_pipeline_mixed_reads()
{
    const int channels = 2;
    const unsigned total = 12000;
    std::vector<float> input = genInput(total, channels);

    SoundTouch st, twin;
    for (SoundTouch *s : {&st, &twin})
    {
        s->setSampleRate(48000);
        s->setChannels(channels);
        s->setTempo(0.83f);
        s->setRate(1.31f);
        s->setSetting(SETTING_USE_AA_FILTER, 1);
    }
    st.enableSourceTrace(0);

    unsigned sent = 0, mode = 0;
    double maxErr = 0, energy = 0;
    std::vector<float> pcm(97 * channels), pcm2(97 * channels);
    std::vector<SourceRow> rows;

    auto drainStep = [&]() -> int
    {
        if (st.numSamples() == 0) return 0;
        unsigned req = 97;
        switch (mode % 3)
        {
            case 0:
            {
                // plain read on both
                unsigned n = st.receiveSamples(pcm.data(), req);
                unsigned n2 = twin.receiveSamples(pcm2.data(), req);
                TASK_CHECK(n == n2);
                TASK_CHECK(0 == memcmp(pcm.data(), pcm2.data(), n * channels * sizeof(float)));
                break;
            }
            case 1:
            {
                // traced read on st, plain read on twin
                unsigned n = st.receiveSamples(pcm.data(), req, rows);
                unsigned n2 = twin.receiveSamples(pcm2.data(), req);
                TASK_CHECK(n == n2);
                TASK_CHECK(rows.size() == n);
                TASK_CHECK(0 == memcmp(pcm.data(), pcm2.data(), n * channels * sizeof(float)));
                for (unsigned i = 0; i < n; i ++)
                {
                    for (int c = 0; c < channels; c ++)
                    {
                        double rec = reconstruct(rows[i], input, channels, c, 0);
                        double err = fabs(rec - pcm[i * channels + c]);
                        if (err > maxErr) maxErr = err;
                    }
                    energy += pcm[i * channels] * pcm[i * channels];
                }
                break;
            }
            default:
            {
                // discard read on both
                unsigned n = st.receiveSamples(req);
                unsigned n2 = twin.receiveSamples(req);
                TASK_CHECK(n == n2);
                break;
            }
        }
        mode ++;
        return 0;
    };

    while (sent < total)
    {
        unsigned n = (sent % 5 == 0) ? 1 : (sent % 5 == 1 ? 7 : 113);
        if (n > total - sent) n = total - sent;
        st.putSamples(input.data() + sent * channels, n);
        twin.putSamples(input.data() + sent * channels, n);
        sent += n;
        TASK_CHECK(st.numSamples() == twin.numSamples());
        TASK_CHECK(0 == drainStep());
        TASK_CHECK(0 == drainStep());
    }
    st.flush();
    twin.flush();
    for (int k = 0; k < 40 && st.numSamples() > 0; k ++) TASK_CHECK(0 == drainStep());

    TASK_CHECK(maxErr <= 3e-5);
    TASK_CHECK(energy > 1.0);
    TASK_CHECK(st.numSamples() == twin.numSamples());
    return 0;
}

// flush behaviour: silent tail rows, no new identities from repeated flush,
// frame numbering continues correctly after flush
static int case_pipeline_flush()
{
    const int channels = 2;
    SoundTouch st;
    st.setSampleRate(48000);
    st.setChannels(channels);
    st.setTempo(1.21f);
    st.setRate(0.91f);
    st.enableSourceTrace(0);

    std::vector<float> input = genInput(4000, channels);
    std::vector<SourceRow> rows;
    std::vector<float> pcm(256 * channels);

    st.putSamples(input.data(), 1000);
    st.flush();

    unsigned total1 = 0;
    bool sawEmptyRow = false, sawNonEmpty = false;
    unsigned n;
    while ((n = st.receiveSamples(pcm.data(), 256, rows)) > 0)
    {
        TASK_CHECK(rows.size() == n);
        for (unsigned i = 0; i < n; i ++)
        {
            if (rows[i].empty())
            {
                sawEmptyRow = true;
                // purely silent rows must correspond to (near-)zero output
                for (int c = 0; c < channels; c ++)
                    TASK_CHECK(fabs(pcm[i * channels + c]) < 1e-6);
            }
            else
            {
                sawNonEmpty = true;
                for (size_t k = 0; k < rows[i].size(); k ++)
                    TASK_CHECK(rows[i][k].frame < 1000);
                double rec = reconstruct(rows[i], input, channels, 0, 0);
                TASK_CHECK(fabs(rec - pcm[i * channels]) <= 3e-5);
            }
        }
        total1 += n;
    }
    TASK_CHECK(total1 > 0);
    TASK_CHECK(sawNonEmpty);

    // repeated flush and empty reads must not produce new output or identities
    st.flush();
    n = st.receiveSamples(pcm.data(), 256, rows);
    TASK_CHECK(n == 0);
    TASK_CHECK(rows.empty());
    st.flush();
    TASK_CHECK(st.numSamples() == 0);

    // frame numbering continues where it stopped (flush zeros got no numbers)
    st.putSamples(input.data() + 1000 * channels, 500);
    st.flush();
    bool sawHigh = false;
    while ((n = st.receiveSamples(pcm.data(), 256, rows)) > 0)
    {
        for (unsigned i = 0; i < n; i ++)
            for (size_t k = 0; k < rows[i].size(); k ++)
            {
                TASK_CHECK(rows[i][k].frame >= 1000 && rows[i][k].frame < 1500);
                sawHigh = true;
            }
    }
    TASK_CHECK(sawHigh);
    (void)sawEmptyRow;
    return 0;
}

// frame numbering: firstFrame above 2^53 stays exact, adjacent frames are
// not merged; numbering overflow is rejected before writing
static int case_pipeline_frame_numbering()
{
    const int channels = 2;
    const uint64_t base = (1ULL << 53) + 7;

    // direct FIR check: 64 consecutive singleton frames above 2^53 must all
    // survive as distinct, exactly-numbered terms
    {
        AAFilter aa(64);
        aa.setCutoffFreq(0.4);
        FIFOSampleBuffer srcBuf(1), dstBuf(1);
        SourceRowFifo srcRows, dstRows;
        std::vector<float> samples(100);
        for (int i = 0; i < 100; i ++) samples[i] = genSample(i, 0);
        std::vector<SourceRow> inRows(100);
        for (int i = 0; i < 100; i ++)
            inRows[i].push_back(SourceWeight{base + (uint64_t)i, 1.0});
        srcBuf.putSamples(samples.data(), 100);
        srcRows.put(inRows.data(), 100);
        uint produced = aa.evaluateTraced(dstBuf, srcBuf, dstRows, srcRows);
        TASK_CHECK(produced == 100 - 64);
        const SourceRow &row0 = dstRows[0];
        TASK_CHECK(row0.size() == 64);      // no adjacent frames merged
        for (size_t k = 0; k < row0.size(); k ++)
            TASK_CHECK(row0[k].frame == base + k);  // exact numbers, no rounding
    }

    SoundTouch st;
    st.setSampleRate(48000);
    st.setChannels(channels);
    st.setTempo(1.0f);
    st.setRate(1.31f);      // AA filter path: FIR rows hold 64 consecutive frames
    st.enableSourceTrace(base);

    // empty input must not advance numbering
    float dummy[4] = {0, 0, 0, 0};
    st.putSamples(dummy, 0);

    const unsigned total = 12000;
    std::vector<float> input = genInput(total, channels);
    st.putSamples(input.data(), total);

    std::vector<SourceRow> rows;
    std::vector<float> pcm(128 * channels);
    bool sawAdjacentPair = false;
    uint64_t minFrame = ~0ULL, maxFrame = 0;
    double maxErr = 0;
    unsigned n;

    // drain before flush (flush clears the rate>1 transposer tail, an
    // upstream behaviour of this SoundTouch version), then flush and drain
    // the rest
    for (int round = 0; round < 2; round ++)
    {
        if (round == 1) st.flush();
        while ((n = st.receiveSamples(pcm.data(), 128, rows)) > 0)
        {
            for (unsigned i = 0; i < n; i ++)
            {
                const SourceRow &r = rows[i];
                for (size_t k = 0; k < r.size(); k ++)
                {
                    TASK_CHECK(r[k].frame >= base);
                    TASK_CHECK(r[k].frame < base + total);
                    if (r[k].frame < minFrame) minFrame = r[k].frame;
                    if (r[k].frame > maxFrame) maxFrame = r[k].frame;
                    if (k > 0 && r[k].frame == r[k - 1].frame + 1)
                        sawAdjacentPair = true; // adjacent >2^53 numbers kept distinct
                }
                TASK_CHECK(rowSortedUnique(r));
                double rec = reconstruct(r, input, channels, 0, base);
                double err = fabs(rec - pcm[i * channels]);
                if (err > maxErr) maxErr = err;
            }
        }
    }
    TASK_CHECK(minFrame >= base && minFrame < base + 2000);
    TASK_CHECK(maxFrame >= base + 5000);
    TASK_CHECK(sawAdjacentPair);
    TASK_CHECK(maxErr <= 3e-5);

    // exact numbering at the very top of the uint64 range (direct FIR check)
    {
        AAFilter aa(64);
        aa.setCutoffFreq(0.4);
        FIFOSampleBuffer srcBuf(1), dstBuf(1);
        SourceRowFifo srcRows, dstRows;
        std::vector<float> samples(100);
        for (int i = 0; i < 100; i ++) samples[i] = genSample(i, 0);
        const uint64_t top = ~0ULL;
        std::vector<SourceRow> inRows(100);
        for (int i = 0; i < 100; i ++)
            inRows[i].push_back(SourceWeight{top - 99 + (uint64_t)i, 1.0});
        srcBuf.putSamples(samples.data(), 100);
        srcRows.put(inRows.data(), 100);
        uint produced = aa.evaluateTraced(dstBuf, srcBuf, dstRows, srcRows);
        TASK_CHECK(produced == 100 - 64);
        const SourceRow &first = dstRows[0];
        const SourceRow &last = dstRows[produced - 1];
        TASK_CHECK(first.size() == 64);
        TASK_CHECK(first.front().frame == top - 99);
        TASK_CHECK(last.back().frame == top - 1);   // exact top-of-range numbers
    }

    // overflow rejection
    SoundTouch ov;
    ov.setSampleRate(48000);
    ov.setChannels(channels);
    ov.enableSourceTrace(~0ULL - 5);
    std::vector<float> in10 = genInput(10, channels);
    bool threw = false;
    try
    {
        ov.putSamples(in10.data(), 10);     // would exceed uint64 range
    }
    catch (const std::overflow_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(ov.numUnprocessedSamples() == 0);    // rejected before writing
    TASK_CHECK(ov.getTraceStorageTerms() == 0);     // no rows were written either

    // a smaller block that exactly reaches the last number must pass
    ov.putSamples(in10.data(), 6);
    TASK_CHECK(ov.getTraceStorageTerms() == 6);     // boundary write accepted

    // one more frame would overflow now
    threw = false;
    try { ov.putSamples(in10.data(), 1); }
    catch (const std::overflow_error &) { threw = true; }
    TASK_CHECK(threw);
    TASK_CHECK(ov.getTraceStorageTerms() == 6);
    return 0;
}

// enable-time rules: unsupported configs, wrong call state, locked settings
static int case_pipeline_enable_rules()
{
    std::vector<float> input = genInput(64, 2);
    float pcm[16];
    std::vector<SourceRow> rows;

    // rows read without enabling trace
    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(2);
        bool threw = false;
        try { st.receiveSamples(pcm, 4, rows); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
    }
    // enable before sample rate / channels are set
    {
        SoundTouch st;
        bool threw = false;
        try { st.enableSourceTrace(0); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
    }
    // unsupported channel count
    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(3);
        bool threw = false;
        try { st.enableSourceTrace(0); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
    }
    // unsupported sample rate
    {
        SoundTouch st;
        st.setSampleRate(96000);
        st.setChannels(2);
        bool threw = false;
        try { st.enableSourceTrace(0); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
    }
    // effective tempo out of [0.5, 2]
    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(2);
        st.setTempo(2.5f);
        bool threw = false;
        try { st.enableSourceTrace(0); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
    }
    // effective rate out of [0.5, 2]
    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(2);
        st.setRate(0.4f);
        bool threw = false;
        try { st.enableSourceTrace(0); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
    }
    // non-default anti-alias filter length
    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(2);
        st.setSetting(SETTING_AA_FILTER_LENGTH, 32);
        bool threw = false;
        try { st.enableSourceTrace(0); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
    }
    // enable after input was sent
    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(2);
        st.putSamples(input.data(), 4);
        bool threw = false;
        try { st.enableSourceTrace(0); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
    }
    // settings locked while tracing; state must stay unchanged
    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(2);
        st.setTempo(1.1f);
        st.enableSourceTrace(0);

        bool threw = false;
        try { st.setTempo(1.5f); } catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
        threw = false;
        try { st.setRate(1.5f); } catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
        threw = false;
        try { st.setPitch(1.5f); } catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
        threw = false;
        try { st.setChannels(1); } catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
        threw = false;
        try { st.setSampleRate(44100); } catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
        threw = false;
        try { st.setSetting(SETTING_USE_QUICKSEEK, 1); } catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
        threw = false;
        try { st.setSetting(SETTING_USE_AA_FILTER, 0); } catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
        // state unchanged
        TASK_CHECK(st.getSetting(SETTING_USE_QUICKSEEK) == 0);
        TASK_CHECK(st.getSetting(SETTING_USE_AA_FILTER) == 1);

        // enable after stream start is rejected even though trace is on
        st.putSamples(input.data(), 64);
        threw = false;
        try { st.enableSourceTrace(300); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);

        // clear() unlocks settings and disables tracing
        st.clear();
        TASK_CHECK(st.getTraceStorageTerms() == 0);
        st.setTempo(1.5f);      // must not throw anymore
        threw = false;
        try { st.receiveSamples(pcm, 4, rows); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
    }
    // repeated enable before stream start replaces firstFrame
    {
        std::vector<float> in8k = genInput(8000, 2);
        SoundTouch s1, s2;
        for (SoundTouch *s : {&s1, &s2})
        {
            s->setSampleRate(48000);
            s->setChannels(2);
            s->setTempo(1.1f);
        }
        s1.enableSourceTrace(100);
        s1.enableSourceTrace(200);      // must replace, not accumulate
        s2.enableSourceTrace(200);

        std::vector<SourceRow> r1, r2;
        for (SoundTouch *s : {&s1, &s2})
        {
            s->putSamples(in8k.data(), 8000);
            s->flush();
            std::vector<SourceRow> rows, *dst = (s == &s1) ? &r1 : &r2;
            unsigned n;
            while ((n = s->receiveSamples(pcm, 8, rows)) > 0)
                dst->insert(dst->end(), rows.begin(), rows.end());
        }
        TASK_CHECK(r1.size() == r2.size());
        TASK_CHECK(!r1.empty());
        uint64_t minFrame = ~0ULL;
        for (size_t i = 0; i < r1.size(); i ++)
        {
            TASK_CHECK(r1[i].size() == r2[i].size());
            for (size_t k = 0; k < r1[i].size(); k ++)
            {
                TASK_CHECK(r1[i][k].frame == r2[i][k].frame);
                TASK_CHECK(r1[i][k].weight == r2[i][k].weight);
                if (r1[i][k].frame < minFrame) minFrame = r1[i][k].frame;
            }
        }
        TASK_CHECK(minFrame >= 200);    // numbering started at the replaced value
    }
    return 0;
}

// the C++ example program: compiled and run by this test
static int case_pipeline_example_program()
{
    const char *tmp = getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    std::string bin = dir + "/source_trace_example_bin";
    std::string cmd = "c++ -std=c++17 -O2 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS "
                      "-Iinclude -Isource/SoundTouch -I.task-build "
                      "examples/source_trace_example.cpp .task-build/libsoundtouch.a "
                      "-o " + bin + " 2>&1";
    FILE *p = popen(cmd.c_str(), "r");
    TASK_CHECK(p != NULL);
    std::string out;
    char buf[256];
    while (p && fgets(buf, sizeof(buf), p)) out += buf;
    int rc = p ? pclose(p) : -1;
    TASK_CHECK(rc == 0);

    // run the example
    p = popen(bin.c_str(), "r");
    TASK_CHECK(p != NULL);
    out.clear();
    while (p && fgets(buf, sizeof(buf), p)) out += buf;
    rc = p ? pclose(p) : -1;
    TASK_CHECK(rc == 0);
    TASK_CHECK(out.find("EXAMPLE_OK") != std::string::npos);
    TASK_CHECK(out.find("EXAMPLE_FRAME 500 terms=") != std::string::npos);
    TASK_CHECK(out.find("SRC frame=") != std::string::npos);
    return 0;
}

// ---------------------------------------------------------------------------
// lifetime group
// ---------------------------------------------------------------------------

// rows read out are owned by the caller: later processing, clear() and
// destruction of the object must not affect them
static int case_lifetime_row_isolation()
{
    const int channels = 2;
    const unsigned total = 9000;
    std::vector<float> input = genInput(total, channels);

    std::vector<SourceRow> savedRows;
    std::vector<float> savedPcm;

    {
        SoundTouch st;
        st.setSampleRate(48000);
        st.setChannels(channels);
        st.setTempo(0.87f);
        st.setRate(1.19f);
        st.enableSourceTrace(42);

        st.putSamples(input.data(), total);
        std::vector<float> pcm(512 * channels);
        std::vector<SourceRow> rows;
        unsigned n = st.receiveSamples(pcm.data(), 512, rows);
        TASK_CHECK(n > 0);
        savedRows = rows;
        savedPcm.assign(pcm.begin(), pcm.begin() + n * channels);

        // caller mutates its own copy: subsequent reads must be unaffected
        if (!rows.empty() && !rows[0].empty())
        {
            rows[0][0].weight = 12345.0;
            rows[0][0].frame = 7;
        }
        std::vector<SourceRow> rows2;
        unsigned n2 = st.receiveSamples(pcm.data(), 512, rows2);
        TASK_CHECK(n2 > 0);
        bool corrupted = false;
        for (unsigned i = 0; i < n2; i ++)
            for (size_t k = 0; k < rows2[i].size(); k ++)
                if (rows2[i][k].weight == 12345.0) corrupted = true;
        TASK_CHECK(!corrupted);

        st.putSamples(input.data(), 1000);  // more processing
        st.clear();                         // clear history
        // object destroyed at scope end
    }

    // saved rows still reconstruct the saved PCM against the original input
    TASK_CHECK(!savedRows.empty());
    double maxErr = 0;
    for (size_t i = 0; i < savedRows.size(); i ++)
        for (int c = 0; c < channels; c ++)
        {
            double rec = reconstruct(savedRows[i], input, channels, c, 42);
            double err = fabs(rec - savedPcm[i * channels + c]);
            if (err > maxErr) maxErr = err;
        }
    TASK_CHECK(maxErr <= 3e-5);
    return 0;
}

// a long stream that is drained promptly must not accumulate trace history
static int case_lifetime_storage_bounded()
{
    const int channels = 2;
    SoundTouch st;
    st.setSampleRate(48000);
    st.setChannels(channels);
    st.setTempo(0.83f);
    st.setRate(1.31f);
    st.enableSourceTrace(0);

    std::vector<float> pcm(256 * channels);
    std::vector<SourceRow> rows;
    size_t firstTerms = 0, lastTerms = 0, maxTerms = 0;
    const int blocks = 700;

    for (int b = 0; b < blocks; b ++)
    {
        std::vector<float> in(113 * channels);
        for (unsigned i = 0; i < 113; i ++)
            for (int c = 0; c < channels; c ++)
                in[i * channels + c] = genSample((unsigned)(b * 113 + i), c);
        st.putSamples(in.data(), 113);

        unsigned n;
        while ((n = st.receiveSamples(pcm.data(), 256, rows)) > 0) {}

        size_t t = st.getTraceStorageTerms();
        if (b == 100) firstTerms = t;
        if (t > maxTerms) maxTerms = t;
        lastTerms = t;
    }
    TASK_CHECK(firstTerms > 0);
    // bounded by pipeline buffering, not by stream length
    TASK_CHECK(maxTerms < 4000000);
    TASK_CHECK(lastTerms <= firstTerms + firstTerms / 2 + 2000);

    st.clear();
    TASK_CHECK(st.getTraceStorageTerms() == 0);
    return 0;
}

// clear() turns tracing off and wipes history; object can be reused and
// tracing can be enabled again with a new firstFrame
static int case_lifetime_clear_reenable()
{
    const int channels = 1;
    std::vector<float> input = genInput(6000, channels);
    float pcm[128];
    std::vector<SourceRow> rows;

    SoundTouch st;
    st.setSampleRate(44100);
    st.setChannels(channels);
    st.setTempo(0.9f);
    st.enableSourceTrace(0);
    st.putSamples(input.data(), 6000);
    unsigned n = st.receiveSamples(pcm, 128, rows);
    TASK_CHECK(n > 0);
    TASK_CHECK(st.getTraceStorageTerms() > 0);

    st.clear();
    TASK_CHECK(st.getTraceStorageTerms() == 0);
    TASK_CHECK(st.numSamples() == 0);

    // old interface keeps working without tracing
    st.setTempo(1.2f);
    st.putSamples(input.data(), 2000);
    n = st.receiveSamples(pcm, 128);
    TASK_CHECK(n >= 0);
    bool threw = false;
    try { st.receiveSamples(pcm, 128, rows); }
    catch (const std::logic_error &) { threw = true; }
    TASK_CHECK(threw);

    // re-enable with a different firstFrame
    st.clear();
    st.enableSourceTrace(5000);
    st.putSamples(input.data(), 3000);
    st.flush();
    uint64_t minFrame = ~0ULL;
    bool anyRow = false;
    while ((n = st.receiveSamples(pcm, 128, rows)) > 0)
        for (unsigned i = 0; i < n; i ++)
            for (size_t k = 0; k < rows[i].size(); k ++)
            {
                anyRow = true;
                if (rows[i][k].frame < minFrame) minFrame = rows[i][k].frame;
            }
    TASK_CHECK(anyRow);
    TASK_CHECK(minFrame >= 5000 && minFrame < 5000 + 2000);
    return 0;
}

// ---------------------------------------------------------------------------
// legacy group
// ---------------------------------------------------------------------------

// the pre-existing pipeline behaviour (mirrors the baseline program)
static int case_legacy_baseline_behaviour()
{
    for (int ch : {1, 2, 6})
    for (float rate : {0.73f, 1.31f})
    {
        SoundTouch s;
        s.setSampleRate(48000);
        s.setChannels(ch);
        s.setRate(rate);
        s.setTempo(0.83f);
        s.setSetting(SETTING_USE_QUICKSEEK, 0);
        s.setSetting(SETTING_USE_AA_FILTER, 1);
        const unsigned total = 24000;
        unsigned sent = 0, received = 0;
        uint32_t rng = 17;
        std::vector<float> out(97 * ch);
        double energy = 0, error = 0;
        auto drain = [&]() -> int
        {
            unsigned n;
            while ((n = s.receiveSamples(out.data(), 97)))
            {
                received += n;
                for (unsigned i = 0; i < n; i ++)
                {
                    float x = out[i * ch];
                    TASK_CHECK(std::isfinite(x));
                    energy += x * x;
                    for (int c = 1; c < ch; c ++)
                        error = std::fmax(error, (double)std::fabs(out[i * ch + c] - x * ((c % 2) ? -1.f : 1.f)));
                }
            }
            return 0;
        };
        unsigned blocks = 0;
        while (sent < total)
        {
            unsigned n = (blocks++ % 3 == 0 ? 1 : blocks % 3 == 1 ? 7 : 113);
            if (n > total - sent) n = total - sent;
            std::vector<float> in(n * ch);
            for (unsigned i = 0; i < n; i ++)
            {
                rng = rng * 1664525u + 1013904223u;
                float x = 0.15f * std::sin((sent + i) * 0.031) + 0.10f * (float(int(rng >> 16) - 32768) / 32768.f);
                for (int c = 0; c < ch; c ++) in[i * ch + c] = x * ((c % 2) ? -1.f : 1.f);
            }
            s.putSamples(in.data(), n);
            sent += n;
            if (blocks % 4) TASK_CHECK(0 == drain());
        }
        s.flush();
        TASK_CHECK(0 == drain());
        TASK_CHECK(received >= 10000);
        TASK_CHECK(energy >= 1);
        TASK_CHECK(error <= 1e-6);
    }
    return 0;
}

// enabling trace must not change the produced PCM or lengths, bit for bit
static int case_legacy_bit_identical()
{
    const int channels = 2;
    const unsigned total = 20000;
    std::vector<float> input = genInput(total, channels);

    for (float rate : {0.73f, 1.31f})
    {
        SoundTouch a, b;
        for (SoundTouch *s : {&a, &b})
        {
            s->setSampleRate(48000);
            s->setChannels(channels);
            s->setTempo(0.83f);
            s->setRate(rate);
            s->setSetting(SETTING_USE_AA_FILTER, 1);
        }
        b.enableSourceTrace(0);

        unsigned sent = 0, blocks = 0;
        double energy = 0;
        unsigned receivedA = 0, receivedB = 0;
        std::vector<float> pa(64 * channels), pb(64 * channels);
        std::vector<SourceRow> rows;
        auto drain = [&]() -> int
        {
            unsigned na, nb;
            do
            {
                na = a.receiveSamples(pa.data(), 64);
                nb = b.receiveSamples(pb.data(), 64, rows);
                TASK_CHECK(na == nb);
                TASK_CHECK(rows.size() == nb);
                if (na)
                    TASK_CHECK(0 == memcmp(pa.data(), pb.data(), na * channels * sizeof(float)));
                receivedA += na;
                receivedB += nb;
                for (unsigned i = 0; i < na; i ++) energy += pa[i * channels] * pa[i * channels];
            } while (na > 0);
            return 0;
        };
        while (sent < total)
        {
            unsigned n = (blocks % 3 == 0 ? 1 : blocks % 3 == 1 ? 7 : 113);
            blocks ++;
            if (n > total - sent) n = total - sent;
            a.putSamples(input.data() + sent * channels, n);
            b.putSamples(input.data() + sent * channels, n);
            sent += n;
            TASK_CHECK(a.numSamples() == b.numSamples());
            TASK_CHECK(a.numUnprocessedSamples() == b.numUnprocessedSamples());
            if (blocks % 4) TASK_CHECK(0 == drain());
        }
        a.flush();
        b.flush();
        TASK_CHECK(0 == drain());
        TASK_CHECK(receivedA == receivedB);
        TASK_CHECK(receivedA > 10000);
        TASK_CHECK(energy > 1.0);   // non-empty, non-zero audio was compared
    }
    return 0;
}

// old API surface keeps working: settings roundtrip, discard reads, trimming
static int case_legacy_old_api()
{
    SoundTouch st;
    st.setSampleRate(48000);
    st.setChannels(2);
    TASK_CHECK(st.getSetting(SETTING_USE_AA_FILTER) == 1);
    st.setSetting(SETTING_USE_AA_FILTER, 0);
    TASK_CHECK(st.getSetting(SETTING_USE_AA_FILTER) == 0);
    st.setSetting(SETTING_USE_QUICKSEEK, 1);
    TASK_CHECK(st.getSetting(SETTING_USE_QUICKSEEK) == 1);
    st.setSetting(SETTING_AA_FILTER_LENGTH, 32);
    TASK_CHECK(st.getSetting(SETTING_AA_FILTER_LENGTH) == 32);
    st.setTempo(1.31f);
    st.setRate(0.87f);
    st.setPitch(1.05f);

    std::vector<float> input = genInput(8000, 2);
    st.putSamples(input.data(), 4000);
    TASK_CHECK(st.numUnprocessedSamples() > 0 || st.numSamples() > 0);
    st.putSamples(input.data() + 4000 * 2, 4000);

    // discard-read overload
    uint avail = st.numSamples();
    if (avail > 0)
    {
        uint dropped = st.receiveSamples(avail / 2);
        TASK_CHECK(dropped == avail / 2);
        TASK_CHECK(st.numSamples() == avail - dropped);
    }

    // trimming
    avail = st.numSamples();
    if (avail > 4)
    {
        uint adj = st.adjustAmountOfSamples(4);
        TASK_CHECK(adj == 4);
        TASK_CHECK(st.numSamples() == 4);
    }

    st.flush();
    float pcm[256 * 2];
    unsigned got = st.receiveSamples(pcm, 256);
    TASK_CHECK(got <= 256);
    TASK_CHECK(st.isEmpty() == (st.numSamples() == 0 ? 1 : 0));

    st.clear();
    TASK_CHECK(st.numSamples() == 0);
    TASK_CHECK(st.numUnprocessedSamples() == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// case registry
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"coefficients", "cubic_coefficients", case_cubic_coefficients},
    {"coefficients", "fir_coefficients", case_fir_coefficients},
    {"coefficients", "wsola_overlap_merge", case_wsola_overlap_merge},
    {"pipeline", "matrix", case_pipeline_matrix},
    {"pipeline", "mixed_reads", case_pipeline_mixed_reads},
    {"pipeline", "flush", case_pipeline_flush},
    {"pipeline", "frame_numbering", case_pipeline_frame_numbering},
    {"pipeline", "enable_rules", case_pipeline_enable_rules},
    {"pipeline", "example_program", case_pipeline_example_program},
    {"lifetime", "row_isolation", case_lifetime_row_isolation},
    {"lifetime", "storage_bounded", case_lifetime_storage_bounded},
    {"lifetime", "clear_reenable", case_lifetime_clear_reenable},
    {"legacy", "baseline_behaviour", case_legacy_baseline_behaviour},
    {"legacy", "bit_identical", case_legacy_bit_identical},
    {"legacy", "old_api", case_legacy_old_api},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
