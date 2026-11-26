#!/usr/bin/env bash
# Regenerate src/minphase_coeffs.h using the dependency-free C generator.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$script_dir/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/minphase-gen.XXXXXX")
trap 'rm -rf "$work"' EXIT

cc -std=c99 -O2 -Wall -Wextra \
	"$root/src/minphase_gen.c" -lm \
	-o "$work/minphase_gen"
"$work/minphase_gen" "$root/src/minphase_coeffs.h"

printf 'regenerated %s\n' "${root}/src/minphase_coeffs.h"
