set -euo pipefail
cd "$(dirname "$0")/.."
node task_support/baseline.cjs
node task_support/old-tests.cjs
