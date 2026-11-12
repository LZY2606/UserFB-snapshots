#!/bin/sh
# Local sandbox helper only - not part of the shipped regression contract.
#
# On this macOS seatbelt sandbox, /usr/bin/mktemp's default per-user temp directory is
# not writable and the BSD mktemp binary ignores TMPDIR when no template is given.
# This prepends a TMPDIR-aware mktemp shim so the unmodified regress scripts run here.
# On a normal machine the shim is unnecessary: call `sh regress/check-target.sh` directly.
set -eu

wrap="${TMPDIR:-/tmp}/rdfa-binwrap"
mkdir -p "$wrap"
cat > "$wrap/mktemp" <<'SHIM'
#!/bin/sh
dir=0
if [ "$1" = "-d" ]; then
  dir=1
  shift
fi
if [ "$#" -eq 0 ]; then
  set -- "${TMPDIR:-/tmp}/mktemp.XXXXXX"
fi
if [ "$dir" -eq 1 ]; then
  exec /usr/bin/mktemp -d "$1"
fi
exec /usr/bin/mktemp "$1"
SHIM
chmod +x "$wrap/mktemp"

PATH="$wrap:$PATH"
export PATH
exec "$@"
