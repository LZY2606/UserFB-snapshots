#!/bin/sh
# Full verification: typecheck + lint + the entire test suite (including the
# chunking-equivalence tests) in non-watch mode.
set -e
cd "$(dirname "$0")"

npm run lint
npx vitest run
