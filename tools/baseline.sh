set -eu
export NODE_PATH="$PWD/tools/node_modules"
node tools/baseline.cjs
node tools/engine-baseline.cjs
node --test tests/mse-flush.test.cjs
