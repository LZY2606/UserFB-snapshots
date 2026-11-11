#!/bin/sh
# Mutation runner for the offline Ambisonics contract.
#
# Builds isolated source+build copies under build/ambi-regression: an
# unmodified baseline and three single-point mutations. Every copy must
# configure, compile and actually render PCM; a copy is accepted as detected
# only when the ambi_contract binary exits non-zero on a numerical assertion.
# Compile failures, missing tests, timeouts and preset exit codes never count.
#
# On success the baseline passes and all three mutations are caught:
#   DETECTED scale
#   DETECTED listener
#   DETECTED hoa
# The source working tree is left untouched.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="${ROOT}/build/ambi-regression"
LOGDIR="${WORK}/logs"
TIMEOUT_SECS=600
RUN_TIMEOUT=$(command -v timeout || command -v gtimeout || true)

# CMake configuration matching baseline/install.sh (static lib, no player).
# FETCHCONTENT_SOURCE_DIR_GOOGLETEST reuses the already-populated local gtest
# checkout so the isolated copies configure fully offline.
CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Release -DLIBTYPE=STATIC \
 -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${ROOT}/build/_deps/googletest-src \
 -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_UTILS=OFF \
 -DALSOFT_TESTS=ON -DALSOFT_BACKEND_COREAUDIO=ON -DALSOFT_BACKEND_WAVE=OFF \
 -DALSOFT_EMBED_HRTF_DATA=OFF -DALSOFT_INSTALL=OFF -DALSOFT_RTKIT=OFF \
 -DALSOFT_BACKEND_PIPEWIRE=OFF -DALSOFT_BACKEND_PULSEAUDIO=OFF \
 -DALSOFT_BACKEND_ALSA=OFF -DALSOFT_BACKEND_OSS=OFF \
 -DALSOFT_BACKEND_SOLARIS=OFF -DALSOFT_BACKEND_SNDIO=OFF \
 -DALSOFT_BACKEND_WASAPI=OFF -DALSOFT_BACKEND_DSOUND=OFF \
 -DALSOFT_BACKEND_WINMM=OFF -DALSOFT_BACKEND_JACK=OFF \
 -DALSOFT_BACKEND_OBOE=OFF -DALSOFT_BACKEND_OPENSL=OFF \
 -DALSOFT_BACKEND_PORTAUDIO=OFF -DALSOFT_BACKEND_SDL2=OFF \
 -DALSOFT_BACKEND_SDL3=OFF"

ALU=alc/alu.cpp

rm -rf "${WORK}"
mkdir -p "${LOGDIR}"

# run_with_timeout <log> <cmd...>
run_with_timeout() {
    log=$1; shift
    if [ -n "${RUN_TIMEOUT}" ]; then
        "${RUN_TIMEOUT}" "${TIMEOUT_SECS}" "$@" >"${log}" 2>&1
    else
        "$@" >"${log}" 2>&1
    fi
}

# build_copy <name>
# Configures and compiles ambi_contract in $WORK/<name>/build. Returns 0 on a
# successful build, non-zero (with the reason logged) otherwise.
build_copy() {
    name=$1
    src="${WORK}/${name}/src"
    bdir="${WORK}/${name}/build"
    log="${LOGDIR}/${name}-build.log"
    if ! run_with_timeout "${log}" cmake -S "${src}" -B "${bdir}" ${CMAKE_ARGS}; then
        echo "BUILD_FAIL ${name}: configure failed (see ${log})"
        return 1
    fi
    if ! run_with_timeout "${log}" cmake --build "${bdir}" --target ambi_contract -j 4; then
        echo "BUILD_FAIL ${name}: compile failed (see ${log})"
        return 1
    fi
    if [ ! -x "${bdir}/tests/ambi_contract" ]; then
        echo "BUILD_FAIL ${name}: ambi_contract binary missing"
        return 1
    fi
    return 0
}

# exercise <name> -> runs the rendered-PCM contract, echoes its exit code.
exercise() {
    name=$1
    bdir="${WORK}/${name}/build"
    log="${LOGDIR}/${name}-run.log"
    ALSOFT_CONF="${ROOT}/baseline/alsoft.conf" \
        run_with_timeout "${log}" "${bdir}/tests/ambi_contract"
    echo $?
}

# copy_tree <name> -> isolated source copy (build artifacts excluded).
copy_tree() {
    name=$1
    mkdir -p "${WORK}/${name}"
    if ! rsync -a --exclude 'build' --exclude 'build*/' --exclude '.git' \
            "${ROOT}/" "${WORK}/${name}/src/" >"${LOGDIR}/${name}-copy.log" 2>&1; then
        echo "FATAL: could not create isolated source copy for ${name}"
        exit 3
    fi
}

# mutate <name> <old> <new>
# Applies an in-place edit to the isolated copy's alu.cpp and fails the whole
# runner if the anchor text is not found (a broken mutation must not be
# mistaken for a detection).
mutate() {
    name=$1
    old=$2
    new=$3
    target="${WORK}/${name}/src/${ALU}"
    OLDBLOB="${old}" NEWBLOB="${new}" python3 - "$target" <<'PY'
import os, sys
path = sys.argv[1]
old, new = os.environ["OLDBLOB"], os.environ["NEWBLOB"]
with open(path, 'r') as f:
    text = f.read()
if old not in text:
    sys.stderr.write(f"mutation anchor not found in {path}\n")
    sys.exit(4)
if text.count(old) != 1:
    sys.stderr.write(f"mutation anchor not unique in {path}\n")
    sys.exit(4)
with open(path, 'w') as f:
    f.write(text.replace(old, new))
PY
    if [ $? -ne 0 ]; then
        echo "FATAL: mutation ${name} could not be applied"
        exit 4
    fi
}

# ---------------------------------------------------------------------------
# Baseline: unmodified library must pass the full contract.
# ---------------------------------------------------------------------------
copy_tree baseline
if ! build_copy baseline; then exit 3; fi
base_code=$(exercise baseline)
if [ "${base_code}" -ne 0 ]; then
    echo "FATAL: baseline failed (exit ${base_code}); see ${LOGDIR}/baseline-run.log"
    exit 3
fi
echo "baseline ambi_contract passed"

# Mutation 1: ignore the per-input-channel normalization scale in
# CalcAmbisonicPanning, so FuMa/SN3D inputs are no longer converted correctly.
SCALE_OLD='auto const scale = scales[acn] * coverage;'
SCALE_NEW='auto const scale = 1.0f /* MUTATION: ignore normalization scale */;'

# Mutation 2: stop transforming world-source orientations into the listener's
# local frame, so turning the listener no longer affects a world field.
LIST_OLD='    if(!props.HeadRelative)
    {
        N = ctxparams.Matrix * N;
        V = ctxparams.Matrix * V;
    }'
LIST_NEW='    if(!props.HeadRelative)
    {
        /* MUTATION: listener transform intentionally skipped */
    }'

# Mutation 3: cap the high-order rotator at first order, so second/third order
# components never get the full rotation.
HOA_OLD='void AmbiRotator(AmbiRotateMatrix &matrix, int const order) noexcept NONBLOCKING
{
    /* Don'\''t do anything for < 2nd order. */
    if(order < 2) return;'
HOA_NEW='void AmbiRotator(AmbiRotateMatrix &matrix, int const order) noexcept NONBLOCKING
{
    /* MUTATION: only the first order is rotated, higher orders stay identity. */
    return;
    if(order < 2) return;'

status=0
run_mutation() {
    label=$1; old=$2; new=$3
    copy_tree "${label}"
    mutate "${label}" "${old}" "${new}"
    if ! build_copy "${label}"; then
        echo "FATAL: ${label} did not compile, cannot claim detection"
        status=1
        return
    fi
    code=$(exercise "${label}")
    case ${code} in
        0)
            echo "MISSED ${label}: contract still passed despite the mutation"
            status=1 ;;
        1)
            echo "DETECTED ${label}" ;;
        *)
            echo "FATAL: ${label} exited with preset/error code ${code}, not a PCM assertion"
            status=1 ;;
    esac
}

run_mutation scale "${SCALE_OLD}" "${SCALE_NEW}"
run_mutation listener "${LIST_OLD}" "${LIST_NEW}"
run_mutation hoa "${HOA_OLD}" "${HOA_NEW}"

if [ "${status}" -eq 0 ]; then
    echo "ALL_MUTATIONS_DETECTED"
fi
exit ${status}
