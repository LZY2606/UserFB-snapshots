set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
printf '#define SPATIALAUDIO_SUPPORTS_SOFA 0\n#define SPATIALAUDIO_SUPPORTS_MIT_HRTF 0\n' > "$build/SpatialaudioConfig.h"
extra=
if [[ -f source/AmbisonicMovingEncoder.cpp ]]; then extra=source/AmbisonicMovingEncoder.cpp; fi
c++ -O2 -std=c++14 -DSPATIALAUDIO_STATIC -I"$build" -Iinclude checks/baseline.cpp source/{AmbisonicEncoder,AmbisonicSource,AmbisonicBase,AmbisonicCommons,AmbisonicEncoderDist,BFormat,GainInterp}.cpp ${extra} -o "$build/baseline"
"$build/baseline"
