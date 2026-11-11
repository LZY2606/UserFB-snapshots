// Standalone independent reference program.  It does not include any
// Maximilian header: the hard-sync model is rebuilt from the waveform
// specification alone in hardsync_ref.cpp.
//
// usage: hardsync_ref <bl|naive|fine> <sampleRate> <masterHz> <slaveHz>
//                     <skip> <count>
#include "hardsync_ref.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 7) {
        std::fprintf(stderr,
                     "usage: %s <bl|naive|fine> <sampleRate> <masterHz>"
                     " <slaveHz> <skip> <count>\n",
                     argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const double sampleRate = std::atof(argv[2]);
    const double masterHz = std::atof(argv[3]);
    const double slaveHz = std::atof(argv[4]);
    const long skip = std::atol(argv[5]);
    const long count = std::atol(argv[6]);
    if (count < 0 || skip < 0) {
        return 2;
    }

    std::vector<double> out(static_cast<size_t>(count));
    if (mode[0] == 'b') {
        hardsync_ref::bandlimited(sampleRate, masterHz, slaveHz,
                                  static_cast<size_t>(skip),
                                  static_cast<size_t>(count), out.data());
    } else if (mode[0] == 'n') {
        hardsync_ref::naiveHardReset(sampleRate, masterHz, slaveHz,
                                     static_cast<size_t>(skip),
                                     static_cast<size_t>(count), out.data());
    } else {
        hardsync_ref::fineAveraged(sampleRate, masterHz, slaveHz,
                                   static_cast<size_t>(skip),
                                   static_cast<size_t>(count), 128,
                                   out.data());
    }
    for (long i = 0; i < count; ++i) {
        std::printf("%.10f\n", out[static_cast<size_t>(i)]);
    }
    return 0;
}
