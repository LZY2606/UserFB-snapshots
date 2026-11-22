////////////////////////////////////////////////////////////////////////////////
///
/// Example for the optional source contribution tracing of SoundTouch.
///
/// Processes an inverse-phase stereo signal (right channel = inverted left
/// channel) through the tempo/rate pipeline with source tracing enabled,
/// then prints the source frames and weights that were mixed into one
/// chosen output frame, and verifies that the output frame can be
/// reconstructed from the original PCM and the reported weights.
///
/// Build (from the repository root):
///   c++ -std=c++17 -Iinclude -Isource/SoundTouch -I.task-build \
///       examples/source_trace_example.cpp .task-build/libsoundtouch.a -o source_trace_example
///
////////////////////////////////////////////////////////////////////////////////

#include "SoundTouch.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace soundtouch;

int main()
{
    const uint sampleRate = 48000;
    const int channels = 2;
    const unsigned numInput = 8000;
    const unsigned probeFrame = 500;    // output frame whose source weights are printed

    SoundTouch st;
    st.setSampleRate(sampleRate);
    st.setChannels(channels);
    st.setTempo(1.13f);
    st.setRate(0.87f);
    st.enableSourceTrace(0);

    // generate an inverse-phase stereo test signal
    std::vector<float> input(numInput * channels);
    for (unsigned i = 0; i < numInput; i ++)
    {
        float v = 0.3f * (float)sin(i * 0.017) + 0.15f * (float)sin(i * 0.00071 + 1.3);
        input[i * 2 + 0] = v;
        input[i * 2 + 1] = -v;
    }

    std::vector<float> pcm;
    std::vector<SourceRow> rows;

    // feed the signal in chunks and collect traced output
    for (unsigned pos = 0; pos < numInput; )
    {
        unsigned n = 257;
        if (n > numInput - pos) n = numInput - pos;
        st.putSamples(input.data() + pos * channels, n);
        pos += n;

        float buff[512 * 2];
        std::vector<SourceRow> chunk;
        unsigned got;
        while ((got = st.receiveSamples(buff, 512, chunk)) > 0)
        {
            pcm.insert(pcm.end(), buff, buff + got * channels);
            rows.insert(rows.end(), chunk.begin(), chunk.end());
        }
    }
    st.flush();
    {
        float buff[512 * 2];
        std::vector<SourceRow> chunk;
        unsigned got;
        while ((got = st.receiveSamples(buff, 512, chunk)) > 0)
        {
            pcm.insert(pcm.end(), buff, buff + got * channels);
            rows.insert(rows.end(), chunk.begin(), chunk.end());
        }
    }

    if (probeFrame >= rows.size())
    {
        printf("EXAMPLE_FAIL not enough output frames (%zu)\n", rows.size());
        return 1;
    }

    const SourceRow &row = rows[probeFrame];
    printf("EXAMPLE_FRAME %u terms=%zu\n", probeFrame, row.size());
    for (size_t k = 0; k < row.size(); k ++)
    {
        printf("SRC frame=%llu weight=%.10g\n",
               (unsigned long long)row[k].frame, row[k].weight);
    }

    // verify: dot the row with the original PCM, per channel
    for (int c = 0; c < channels; c ++)
    {
        double recon = 0;
        for (size_t k = 0; k < row.size(); k ++)
        {
            recon += row[k].weight * input[row[k].frame * channels + c];
        }
        double err = fabs(recon - pcm[probeFrame * channels + c]);
        printf("RECON channel=%d value=%.8g error=%.3g\n", c, recon, err);
        if (err > 3e-5)
        {
            printf("EXAMPLE_FAIL reconstruction error too large\n");
            return 1;
        }
    }

    printf("EXAMPLE_OK outputs=%zu\n", rows.size());
    return 0;
}
