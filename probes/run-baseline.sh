set -eu
build=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$build"' EXIT
mkdir "$build/probes"
cp index.js package.json "$build/"
cp probes/baseline.mjs "$build/probes/"
npm --cache "$build/npm-cache" --prefix "$build" install --ignore-scripts --omit=dev --no-save --package-lock=false mimic-response@4.0.0
node "$build/probes/baseline.mjs"
