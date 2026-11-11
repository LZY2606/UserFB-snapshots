set -eu
npm --prefix tools ci --ignore-scripts --no-audit --no-fund --cache "${TMPDIR:-/tmp}/media-npm-cache"
