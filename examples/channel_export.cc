// ----------------------------------------------------------------------------
//
//  Copyright (C) 2006-2020 Fons Adriaensen <fons@linuxaudio.org>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------

// Example: exporting a multichannel audio track while folding known
// inter-channel time differences into the sample rate conversion.
//
// The time difference between the channels is known up front. Instead of
// first applying a fractional delay to each channel and then resampling the
// result, we let the resampler read each channel at its own source position.
// No audio device is involved: this reads an interleaved floating point
// stream from a file and writes the resampled interleaved stream to another.
//
// Usage: channel_export <in.f32> <out.f32> <in_rate> <out_rate> <channels>
//                       <max_shift> <offset_0> ... <offset_{channels-1}>
//
// Offsets are expressed in input frames and must lie in [-max_shift,
// max_shift]. The input and output files hold interleaved float32 frames
// in host byte order.

#include <zita-resampler/vresampler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>


static const unsigned BLOCK = 4096;


int main (int argc, char *argv [])
{
    if (argc < 8)
    {
        std::fprintf (stderr,
            "usage: %s <in.f32> <out.f32> <in_rate> <out_rate> <channels> "
            "<max_shift> <offset_0> ... <offset_{channels-1}>\n", argv [0]);
        return 1;
    }

    const char *in_name  = argv [1];
    const char *out_name = argv [2];
    double in_rate  = std::atof (argv [3]);
    double out_rate = std::atof (argv [4]);
    unsigned nchan  = (unsigned) std::atoi (argv [5]);
    unsigned max_shift = (unsigned) std::atoi (argv [6]);

    if (!nchan || (unsigned) argc != 7 + nchan) return 1;

    std::vector<double> offsets (nchan);
    for (unsigned c = 0; c < nchan; ++c) offsets [c] = std::atof (argv [7 + c]);

    std::FILE *in = std::fopen (in_name, "rb");
    if (!in) { std::perror (in_name); return 1; }
    std::FILE *out = std::fopen (out_name, "wb");
    if (!out) { std::perror (out_name); std::fclose (in); return 1; }

    // 1. Configure the variable-ratio resampler for the requested rates.
    VResampler resampler;
    if (resampler.setup (out_rate / in_rate, nchan, 48)) { std::perror ("setup"); return 1; }

    // 2. Enable per-channel offsets. This allocates, so it must happen before
    //    any input is committed.
    if (resampler.prepare_channel_alignment (max_shift)) { std::perror ("prepare"); return 1; }
    if (resampler.set_channel_offsets (offsets.data (), nchan)) { std::perror ("offsets"); return 1; }

    // 3. Pre-fill half the filter length minus one zero frames so the nominal
    //    source position aligns with the first real input sample. Output is
    //    discarded while pre-filling.
    unsigned prefix = (unsigned) resampler.inpsize () / 2 - 1;
    std::vector<float> silence ((size_t) prefix * nchan, 0.0f);
    resampler.inp_count = prefix;
    resampler.inp_data  = silence.data ();
    resampler.out_count = prefix;
    resampler.out_data  = 0;
    resampler.process ();

    // 4. Stream the file through, then feed a zero tail so the full filter
    //    response of the last samples is emitted.
    std::vector<float> chunk ((size_t) BLOCK * nchan);
    std::vector<float> output ((size_t) BLOCK * nchan);
    unsigned chunk_frames = 0;
    bool tail = false;
    unsigned tail_left = 0;

    for (;;)
    {
        if (!tail && chunk_frames == 0)
        {
            size_t got = std::fread (chunk.data (), sizeof (float), chunk.size (), in);
            chunk_frames = (unsigned) (got / nchan);
            if (!chunk_frames)
            {
                tail = true;
                tail_left = (unsigned) resampler.inpsize ();
            }
        }
        if (tail && tail_left == 0) break;

        unsigned offer = tail ? tail_left : chunk_frames;
        resampler.inp_count = offer;
        resampler.inp_data  = tail ? 0 : chunk.data ();   // NULL input = silence
        resampler.out_count = BLOCK;
        resampler.out_data  = output.data ();
        if (resampler.process ()) break;

        unsigned used = offer - resampler.inp_count;
        unsigned made = BLOCK - resampler.out_count;
        if (made)
            std::fwrite (output.data (), sizeof (float), (size_t) made * nchan, out);

        if (tail) tail_left -= used;
        else
        {
            chunk_frames -= used;
            if (chunk_frames)
                std::memmove (chunk.data (), chunk.data () + (size_t) used * nchan,
                              (size_t) chunk_frames * nchan * sizeof (float));
        }
        if (!used && !made) break;
    }

    std::fclose (out);
    std::fclose (in);
    return 0;
}
