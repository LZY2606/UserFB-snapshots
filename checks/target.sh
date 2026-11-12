#!/bin/bash
set -euo pipefail

# Regression driver for RKSimulationBank. Builds and runs the bank tests in
# two configurations (native backends, then MOOG_BANK_FORCE_SCALAR), preserves
# verifiable NEON kernel evidence under checks/evidence, and finally runs the
# original single-voice RKSimulationMoog baseline.
#
# A repo-local build directory is used on purpose: it is writable in sandboxed
# shells where mktemp(1) cannot create its default user-temp directory, and
# build/ is already listed in .gitignore.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

build="build/target-checks"
mkdir -p "$build"
trap 'rm -rf "$build"' EXIT

evidence="checks/evidence"
mkdir -p "$evidence"

common=(-std=c++17 -O2 -ffp-contract=off -I. -Ichecks -Isrc)

# Record the machine's SIMD capabilities and the toolchain in use, so the
# disassembly claim can be checked against the host that produced it.
{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "uname: $(uname -a)"
    echo "arch: $(uname -m)"
    echo "compiler: $(c++ --version | head -1)"
    echo "compile: c++ ${common[*]} tests/target_regression.cpp checks/target_main.cpp"
    if [ "$(uname -s)" = "Darwin" ]; then
        if sysctl hw.optional.AdvSIMD 2>/dev/null; then
            :
        else
            # Some sandboxed kernels hide the optional OID namespace.
            echo "hw.optional.AdvSIMD: OID hidden; AdvSIMD is mandatory on AArch64 and is verified by the execution probe in uf_native"
        fi
        sysctl machdep.cpu.brand_string 2>/dev/null || true
    else
        if [ -r /proc/cpuinfo ]; then
            grep -E '^(processor|Features|model name)' /proc/cpuinfo | head -4
        fi
    fi
} > "$evidence/native_environment.txt"

# --- Native build (NEON on AArch64, scalar elsewhere) ----------------------
c++ "${common[@]}" tests/target_regression.cpp checks/target_main.cpp -o "$build/target"

# Disassemble the test binary with the same optimisation level it uses. The
# bulky full dump stays in the (gitignored) build directory; only the extracted
# NEON kernel, which the test asserts on, is kept under checks/evidence.
if [ "$(uname -s)" = "Darwin" ]; then
    otool -tV "$build/target" > "$build/full_disasm.txt"
else
    objdump -d "$build/target" > "$build/full_disasm.txt"
fi

if grep -q 'process_neon' "$build/full_disasm.txt"; then
    # Capture from the process_neon function definition label (otool prints a
    # colon-suffixed symbol label at column zero) up to the next symbol label.
    awk '
        /process_neonEPKPfj:$/ { capture = 1 }
        capture { print }
        capture && /^[A-Za-z_].*:$/ && !/process_neonEPKPfj:$/ { exit }
    ' "$build/full_disasm.txt" > "$evidence/neon_disasm.txt"
    # Keep the full disassembly if the symbol-specific extraction did not
    # work for this toolchain's output format.
    if ! grep -q '\.2d' "$evidence/neon_disasm.txt"; then
        cp "$build/full_disasm.txt" "$evidence/neon_disasm.txt"
    fi
else
    # Scalar-only target: record that no NEON symbol exists instead of
    # fabricating one; uf_native checks the file accordingly on such hosts.
    cp "$build/full_disasm.txt" "$evidence/neon_disasm.txt"
fi

"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
test "$(grep -Ec '^GROUP numeric assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP voices assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP native assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Fxc TARGET_GROUPS_OK "$build/result")" = 1

# --- Forced scalar build: no arm_neon.h, explicit Neon rejected ------------
c++ "${common[@]}" -DMOOG_BANK_FORCE_SCALAR=1 tests/target_regression.cpp checks/target_main.cpp -o "$build/scalar"
"$build/scalar" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
test "$(grep -Ec '^GROUP numeric assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP voices assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Ec '^GROUP native assertions=[1-9][0-9]* PASS$' "$build/result")" = 1
test "$(grep -Fxc TARGET_GROUPS_OK "$build/result")" = 1

# --- Original single-voice baseline, unchanged -----------------------------
bash checks/baseline.sh
