set -euo pipefail
build_task() { local src=$1 out=$2; shift 2; "${CXX:-c++}" -std=c++17 -O2 -Dkiss_fft_scalar=double -I. -Iext/kissfft -Iext/kissfft/tools -Itask_support "$src" "$@" dsp/chromagram/*.cpp dsp/transforms/FFT.cpp maths/MathUtilities.cpp -x c++ ext/kissfft/kiss_fft.c ext/kissfft/tools/kiss_fftr.c -o "$out"; }
