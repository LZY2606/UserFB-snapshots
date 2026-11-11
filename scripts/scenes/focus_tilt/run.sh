#!/bin/sh
#
# Focus-tilt demo: a tilted checkerboard on the focus plane with one small
# object on each side of it. Renders a small headless image and, separately,
# writes the measured focus-plane error to focus_tilt_error.txt so the result
# is numeric rather than just a picture.
#
# Usage:
#   sh scripts/scenes/focus_tilt/run.sh [path/to/c-ray]
#
# If no binary is given, CRAY_BIN is used when set, then ./bin/c-ray, and as a
# last resort a fresh Release build is configured in a temporary directory.
#
set -eu

# Resolve the repository root from this script's location.
script_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$script_dir/../../.." && pwd)
cd "$root"

work=$(mktemp -d "${TMPDIR:-/tmp}/cray-focus-demo.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# 1. Build and run the numeric focus-plane verifier.
echo "Building numeric verifier"
cc -std=c99 -O1 -Iinclude -Isrc -Isrc/lib \
	"$script_dir/focus_tilt_demo.c" \
	src/lib/datatypes/camera.c src/lib/datatypes/spline.c \
	src/common/transforms.c src/common/quaternion.c src/common/cr_assert.c \
	src/lib/renderer/samplers/sampler.c src/lib/renderer/samplers/random.c \
	src/lib/renderer/samplers/halton.c src/lib/renderer/samplers/hammersley.c \
	src/lib/vendored/pcg_basic.c -lm -o "$work/focus_tilt_demo"

echo "Measuring focus-plane error"
"$work/focus_tilt_demo" | tee "$script_dir/focus_tilt_error.txt"

# 2. Locate or build a headless c-ray binary.
if [ "$#" -ge 1 ] && [ -x "$1" ]; then
	cray_bin=$1
elif [ -n "${CRAY_BIN:-}" ] && [ -x "$CRAY_BIN" ]; then
	cray_bin=$CRAY_BIN
elif [ -x "$root/bin/c-ray" ]; then
	cray_bin=$root/bin/c-ray
else
	echo "No c-ray binary supplied, configuring a fresh Release build"
	cmake -S "$root" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DNO_IPO=ON > "$work/configure.log"
	cmake --build "$work/build" --target c-ray -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" > "$work/build.log"
	cray_bin=$work/build/bin/c-ray
fi

# 3. Render the small demo image without opening a window.
echo "Rendering $script_dir/focus_tilt.json"
"$cray_bin" --no-sdl --asset-path input/ "$script_dir/focus_tilt.json" > "$work/render.log"
# The encoder suffixes the frame number, see driver/encoders.
echo "Wrote output/focus_tilt_demo_0000.png and $script_dir/focus_tilt_error.txt"
