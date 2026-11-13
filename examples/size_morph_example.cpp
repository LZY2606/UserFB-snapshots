// Continuous room-size morphing example for MVerb.
//
// This is plain C++ core usage: no VST SDK or editor is involved. Build with:
//   c++ -std=c++17 -O2 -I. examples/size_morph_example.cpp -o size_morph_example
//
// The pattern mirrors how a host would drive size automation: parameter
// changes arrive as (normalized value, transition time in samples) commands
// between audio blocks, while every block is processed in place.

#include "MVerb.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

int main()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 256;
    constexpr int numBlocks = 200;

    // The fixed-size delay buffers make MVerb large; keep it off the stack.
    auto owner = std::make_unique<MVerb<double>>();
    MVerb<double> &verb = *owner;
    verb.setSampleRate(static_cast<double>(sampleRate)); // also performs reset()
    verb.setParameter(MVerb<double>::DENSITY, 0.5);
    verb.setParameter(MVerb<double>::DECAY, 0.8);
    verb.setParameter(MVerb<double>::PREDELAY, 0.0);
    verb.setParameter(MVerb<double>::SIZE, 0.6);
    verb.setParameter(MVerb<double>::MIX, 0.3);
    verb.setParameter(MVerb<double>::EARLYMIX, 0.75);

    // The mode may only be enabled after a reset and before the first
    // non-empty process call. It starts at the current SIZE (0.6 -> the room
    // size used at frame 0).
    if(!verb.enableSizeMorph(true)) {
        std::fprintf(stderr, "failed to enable size morphing\n");
        return 1;
    }

    std::vector<double> buffer(2 * blockSize);
    double *channels[2] = {buffer.data(), buffer.data() + blockSize};

    for(int block = 0; block < numBlocks; ++block) {
        // Host automation scheduled for this block boundary. None of these
        // calls clears or moves the delay histories: sound already travelling
        // through the tanks keeps decaying while the propagation lengths move.
        if(block == 10) {
            // Grow to normalized size 0.9 over one second of output.
            verb.morphSize(0.9, static_cast<unsigned>(sampleRate));
        } else if(block == 40) {
            // A new command while the previous ramp is still running restarts
            // from the size the next frame would have used, not from the
            // original command's starting point.
            verb.morphSize(0.2, static_cast<unsigned>(sampleRate / 2));
        } else if(block == 90) {
            // frames == 0 applies the target size to the very next frame.
            verb.morphSize(0.5, 0);
        }

        // Invalid commands are rejected without touching audio or the running
        // trajectory (shown once).
        if(block == 5 && !verb.morphSize(1.5, 100))
            std::fprintf(stderr, "block %d: out-of-range command rejected\n", block);

        for(int i = 0; i < blockSize; ++i) {
            channels[0][i] = 0.2 * std::sin(0.013 * (block * blockSize + i));
            channels[1][i] = 0.15 * std::sin(0.017 * (block * blockSize + i));
        }
        // Fully in-place processing is supported by the same call.
        verb.process(channels, channels, blockSize);
    }

    // Zero-length blocks never advance the size trajectory or the audio.
    verb.process(channels, channels, 0);

    // reset() drops the histories and any unfinished ramp but keeps morph
    // mode enabled; the next process starts again at the most recent target.
    verb.reset();

    std::printf("size morph example finished\n");
    return 0;
}
