#!/usr/bin/env bash
#
# checks/mutations.sh -- mutation testing for the Butterworth numeric path.
#
# Two independent broken copies of the production sources are built in
# temporary directories:
#
#   prewarp       LowPassTransform prewarp  f = tan(pi*fc)  replaced by
#                 the un-warped linear term f = pi*fc.
#
#   normalization Cascade::setLayout final gain normalization multiplied
#                 by an extra 0.97.
#
# Every copy must still compile and must produce finite, non-empty stereo
# PCM through the public SimpleFilter::process entry point; the mutation
# itself is then required to be caught by the independent frequency
# response assertions in checks/target.cpp.  A compile failure, crash,
# empty/non-finite output or an undetected mutation is a fatal error.
# The working tree is only read; all edits happen in temporary copies.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sources=(Butterworth Biquad Cascade PoleFilter)

status=0

# Verify the tracked production sources are byte-identical at the end.
git -C "$root" diff --quiet -- shared/DSPFilters || {
  echo "DIAG working tree was already dirty; aborting" >&2
  exit 2
}

run_mutation ()
{
  local name="$1"
  local edit_file="$2"
  local pattern="$3"
  local replacement="$4"
  local marker="$5"

  local work
  work="$(mktemp -d)"
  trap 'rm -rf "$work"' RETURN

  cp -R "$root/shared/DSPFilters" "$work/dsp"

  local target="$work/dsp/source/$edit_file"

  # Refuse to run if the expected production text is not present, since a
  # silently failed edit would otherwise masquerade as an undetected case.
  grep -F -q "$pattern" "$target" || {
    echo "DIAG $name: mutation anchor not found in $edit_file" >&2
    status=1
    return
  }

  # Literal-string replacement (fixed strings, no regex metacharacters).
  DSPF_PATTERN="$pattern" DSPF_REPLACEMENT="$replacement" \
    perl -0777 -pi -e 's/\Q$ENV{DSPF_PATTERN}\E/$ENV{DSPF_REPLACEMENT}/' "$target"

  grep -F -q "$marker" "$target" || {
    echo "DIAG $name: edited marker missing" >&2
    status=1
    return
  }
  if grep -F -q "$pattern" "$target"; then
    echo "DIAG $name: original text still present after edit" >&2
    status=1
    return
  fi

  local mut_src=()
  for s in "${sources[@]}"; do mut_src+=("$work/dsp/source/$s.cpp"); done

  # PCM smoke driver: exercises the public stereo process() path and proves
  # the mutated build runs and yields finite, non-empty output.
  cat > "$work/mut_driver.cpp" <<'CPP'
#include "DspFilters/Dsp.h"
#include <cmath>
#include <cstdio>
int main ()
{
  const double sr = 48000.0;
  Dsp::SimpleFilter<Dsp::Butterworth::LowPass<9>, 2> f;
  f.setup (5, sr, sr * 0.31);   // high normalized cutoff magnifies prewarp error
  f.reset ();
  enum { N = 4096 };
  static double a[N], b[N];
  for (int i = 0; i < N; ++i)
  {
    a[i] = 0.5 * std::sin (i * 0.37);
    b[i] = 0.3 * std::cos (i * 0.11 + 0.4);
  }
  double* p[2] = { a, b };
  f.process (N, p);
  double ea = 0, eb = 0;
  for (int i = 0; i < N; ++i)
  {
    if (!std::isfinite (a[i]) || !std::isfinite (b[i]))
    {
      std::printf ("MUT_PCM_BAD\n");
      return 2;
    }
    ea += a[i] * a[i];
    eb += b[i] * b[i];
  }
  if (!(ea > 1e-12 && eb > 1e-12))
  {
    std::printf ("MUT_PCM_EMPTY\n");
    return 3;
  }
  std::printf ("MUT_PCM_OK energy=%.9e,%.9e\n", ea, eb);
  return 0;
}
CPP

  local build_ok=0
  local numeric=0

  if c++ -O2 -std=c++11 -DNDEBUG -I"$work/dsp/include" \
        "$work/mut_driver.cpp" "${mut_src[@]}" -o "$work/mut_driver" \
        && c++ -O2 -std=c++11 -DNDEBUG -I"$work/dsp/include" \
        "$root/checks/target.cpp" "${mut_src[@]}" -o "$work/target_mut"; then
    build_ok=0
  else
    build_ok=1
  fi

  if [ "$build_ok" -eq 0 ]; then
    local pcm_out
    if pcm_out="$("$work/mut_driver")" && [ "${pcm_out%% *}" = "MUT_PCM_OK" ]; then
      : # finite, non-empty PCM confirmed
    else
      echo "DIAG $name: mutated PCM run invalid: ${pcm_out:-no output}" >&2
      echo "DETECTED $name build=0 numeric=0"
      status=1
      return
    fi

    # The independent complex-response reference must reject the mutation.
    # A genuine assertion failure prints FAIL and exits non-zero.
    local map_out
    set +e
    map_out="$("$work/target_mut" mapping 2>&1)"
    local map_rc=$?
    set -e
    # Here-string rather than a pipe: with pipefail a large piped output
    # plus grep -q's early exit would raise SIGPIPE (141).
    if [ "$map_rc" -ne 0 ] && grep -q "FAIL" <<< "$map_out"; then
      numeric=1
    fi
  fi

  if [ "$build_ok" -eq 0 ] && [ "$numeric" -eq 1 ]; then
    echo "DETECTED $name build=0 numeric=1"
  else
    echo "DIAG $name: build_ok=$build_ok numeric=$numeric" >&2
    echo "DETECTED $name build=$build_ok numeric=$numeric"
    status=1
  fi
}

run_mutation "prewarp" "PoleFilter.cpp" \
  "f = tan (doublePi * fc);" \
  "f = doublePi * fc;" \
  "f = doublePi * fc;"

run_mutation "normalization" "Cascade.cpp" \
  "(2 * doublePi))));" \
  "(2 * doublePi))) * 0.97);" \
  "* 0.97"

if ! git -C "$root" diff --quiet -- shared/DSPFilters; then
  echo "DIAG production sources were modified" >&2
  exit 2
fi

exit "$status"
