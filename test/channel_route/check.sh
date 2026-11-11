#!/bin/sh
# Offline check for the channel routing used by the sio_microphone example.
#
# Builds example/channel_route.c together with a test driver using the
# system C99 compiler and runs it. Fake begin/end hooks and an in-memory
# ring buffer drive the same pump logic the example callbacks use, so no
# audio backend is started and no sound card is needed.
set -e

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

cc=${CC:-cc}
build_dir="${TMPDIR:-/tmp}/channel_route_check.$$"
trap 'rm -rf "$build_dir"' 0
mkdir -p "$build_dir"

"$cc" -std=c99 -Wall -Wextra -Werror -I"$repo_root" \
    -o "$build_dir/check_channel_route" \
    "$repo_root/example/channel_route.c" \
    "$script_dir/check_channel_route.c"

"$build_dir/check_channel_route"
