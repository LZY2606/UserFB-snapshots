#!/bin/bash
# Builds the crate into an isolated target directory, explicitly runs the exception
# diagnostics contract test, then executes the legacy exception tests from baseline/.
set -euo pipefail

cd "$(dirname "$0")/.."

export CARGO_HOME="$PWD/.cargo-home"

# Some sandboxes forbid the no-template `mktemp -d` default location (macOS resolves it through
# confstr, not $TMPDIR). Put a tiny mktemp shim first on PATH that redirects bare `mktemp -d`
# into $TMPDIR while delegating every other invocation unchanged. The shim lives in $TMPDIR and
# baseline/ itself is untouched.
real_mktemp="$(command -v mktemp)"
shim_dir="$("$real_mktemp" -d "${TMPDIR:-/tmp}/exception-diags.XXXXXX")"
trap 'rm -rf "$shim_dir" "${build:-}"' EXIT
cat > "$shim_dir/mktemp" <<EOF
#!/bin/bash
if [ "\$#" -eq 1 ] && [ "\$1" = "-d" ]; then
    exec "$real_mktemp" -d "\${TMPDIR:-/tmp}/tmp.XXXXXXXXXX"
fi
exec "$real_mktemp" "\$@"
EOF
chmod +x "$shim_dir/mktemp"
export PATH="$shim_dir:$PATH"

build="$(mktemp -d)"
export CARGO_TARGET_DIR="$build"

# Fresh build so the test run below cannot silently reuse a stale artifact.
cargo build --locked --tests --quiet

# Run the single diagnostics test explicitly and confirm it really passed (not just "filtered
# out") by requiring its name in the output plus an overall ok result.
log="$build/diagnostics.log"
cargo test --locked --test exception_diagnostics diagnostics_contract -- --nocapture --exact 2>&1 | tee "$log"
grep -Eq 'test diagnostics_contract \.\.\. ok' "$log"
grep -Eq 'test result: ok\. 1 passed' "$log"

bash baseline/run_old.sh

echo EXCEPTION_DIAGNOSTICS_OK
