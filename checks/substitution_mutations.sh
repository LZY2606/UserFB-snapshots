#!/usr/bin/env bash
set -euo pipefail

# Mutation checks for the Itanium substitution model.
#
# Each mutation gets an independent copy of the repository.  The patch must
# match exactly once, the mutated code must build, and the shared model's
# named semantic assertion must fail normally (not through a panic, signal,
# or timeout).

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$script_dir/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/itanium-mut.XXXXXX")
trap 'chmod -R u+w "$work" 2>/dev/null || true; rm -rf "$work"' EXIT

export GOPROXY=off
export GOSUMDB=off
export GOTOOLCHAIN=local
export GOTELEMETRY=off
: "${GOCACHE:=$work/go-build}"
: "${GOMODCACHE:=$work/go-mod}"
export GOCACHE GOMODCACHE
mkdir -p "$GOCACHE" "$GOMODCACHE"

copy_repo() {
  local dst=$1
  mkdir -p "$dst"
  tar -C "$root" --exclude='./.git' -cf - . | tar -C "$dst" -xf -
}

apply_exact() {
  local file=$1 old=$2 new=$3
  OLD="$old" NEW="$new" perl -0777 -i -pe '
    my $count = () = /\Q$ENV{OLD}\E/g;
    if ($count != 1) {
      die "patch site matches=$count (wanted 1)\n";
    }
    s/\Q$ENV{OLD}\E/$ENV{NEW}/;
  ' "$file"
}

run_mutation() {
  local name=$1 subtest=$2 old=$3 new=$4
  local dst="$work/$name"
  local testbin="$work/$name.test"
  local buildlog="$work/$name.build.log"
  local runlog="$work/$name.run.log"
  local status=0

  copy_repo "$dst"

  if ! apply_exact "$dst/demangle.go" "$old" "$new" \
      >"$work/$name.patch.log" 2>&1; then
    echo "PATCH_MISMATCH $name" >&2
    sed -n '1,20p' "$work/$name.patch.log" >&2 || true
    return 1
  fi

  if ! (cd "$dst" && go test -count=1 -c -o "$testbin" .) \
      >"$buildlog" 2>&1; then
    echo "BUILD_FAILED $name" >&2
    sed -n '1,80p' "$buildlog" >&2 || true
    return 1
  fi

  set +e
  (
    cd "$dst"
    "$testbin" \
      -test.run "^TestItaniumSubstitutionModel\$/^${subtest}\$" \
      -test.v \
      -test.timeout=20s
  ) >"$runlog" 2>&1
  status=$?
  set -e

  if [[ $status -eq 0 ]]; then
    echo "MUTATION_SURVIVED $name" >&2
    sed -n '1,80p' "$runlog" >&2 || true
    return 1
  fi

  if grep -Eq 'panic:|test timed out|signal: (killed|terminated|segmentation fault)|SIG(SEGV|ABRT|KILL|TERM)' "$runlog"; then
    echo "MUTATION_UNSTABLE $name status=$status (panic/signal/timeout is not a semantic kill)" >&2
    sed -n '1,100p' "$runlog" >&2 || true
    return 1
  fi

  if [[ $status -ge 128 || $status -eq 124 ]]; then
    echo "MUTATION_SIGNAL_OR_TIMEOUT $name status=$status" >&2
    return 1
  fi

  if ! grep -Fq 'ITANIUM_MODEL_SEMANTIC_ASSERTION' "$runlog"; then
    echo "NO_SEMANTIC_ASSERTION $name status=$status" >&2
    sed -n '1,100p' "$runlog" >&2 || true
    return 1
  fi

  if ! grep -Fq -- "--- FAIL: TestItaniumSubstitutionModel/${subtest}" "$runlog"; then
    echo "NAMED_SUBTEST_DID_NOT_FAIL $name /$subtest status=$status" >&2
    sed -n '1,120p' "$runlog" >&2 || true
    return 1
  fi

  echo "KILLED $name"
}

prefix_old=$'		if c != \'S\' && (len(st.str) == 0 || st.str[0] != \'E\') {\n			st.subs.add(a)\n		}'
prefix_new=$'		if c != \'S\' && (len(st.str) == 0 || st.str[0] != \'E\') {\n			if q, ok := a.(*Qualified); !ok {\n				st.subs.add(a)\n			} else if _, ok := q.Scope.(*Name); !ok {\n				st.subs.add(a)\n			}\n		}'

index_old=$'		ret := st.subs[id]'
index_new=$'		if id > 0 {\n			id--\n		}\n		ret := st.subs[id]'

run_mutation prefix_registration prefix_chain "$prefix_old" "$prefix_new"
run_mutation subst_index candidate_order "$index_old" "$index_new"
