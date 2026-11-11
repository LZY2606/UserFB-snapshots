#!/usr/bin/env bash
#
# Builds the real diStorm library entry points (including src/relocate.c) with
# the native C compiler in a temporary directory, compiles the assertion cases
# in tests/relocate_cases.c against them, and runs the seven case groups.
# No machine code under test is loaded or executed; everything is offline.
set -euo pipefail
cd "$(dirname "$0")/.."

build=$(mktemp -d "${TMPDIR:-/tmp}/relocate.XXXXXX")
trap 'rm -rf "$build"' EXIT

# The shipped library sources, in dependency order, plus the new relocator.
sources=(mnemonics textdefs prefix operands insts instructions distorm decoder relocate)
objects=()
for name in "${sources[@]}"; do
	cc -std=c99 -O2 -Wall -DSUPPORT_64BIT_OFFSET -DDISTORM_STATIC \
	   -Iinclude -c "src/$name.c" -o "$build/$name.o"
	objects+=("$build/$name.o")
done

# The test program consumes the public prototypes, so DISTORM_STATIC is not
# defined on its command line (the header would hide the declarations). The
# header enables SUPPORT_64BIT_OFFSET by default when building a consumer.
cc -std=c99 -O2 -Wall -Wno-macro-redefined \
   -Iinclude tests/relocate_cases.c "${objects[@]}" -o "$build/relocate_cases"

"$build/relocate_cases"
