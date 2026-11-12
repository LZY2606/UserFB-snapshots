set -euo pipefail
build="$(mktemp -d)"
trap 'rm -r "$build"' EXIT
printf '#define SPATIALAUDIO_SUPPORTS_SOFA 0\n#define SPATIALAUDIO_SUPPORTS_MIT_HRTF 0\n' > "$build/SpatialaudioConfig.h"
extra=
if [[ -f source/AmbisonicMovingEncoder.cpp ]]; then extra=source/AmbisonicMovingEncoder.cpp; fi
c++ -O2 -std=c++14 -DSPATIALAUDIO_STATIC -I"$build" -Iinclude checks/target.cpp source/{AmbisonicEncoder,AmbisonicSource,AmbisonicBase,AmbisonicCommons,AmbisonicEncoderDist,BFormat,GainInterp}.cpp ${extra} -o "$build/target"
for group in arrival direction doppler stream; do
  "$build/target" "$group" > "$build/out"
  cat "$build/out"
  awk -v g="$group" '$1=="PASS" && $2==g && $3 ~ /^checks=[1-9][0-9]*$/ {n++} END {exit n!=1}' "$build/out"
done
bash checks/build.sh
