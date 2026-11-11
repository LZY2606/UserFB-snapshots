set -eu
build=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$build"' EXIT
export GOCACHE="$build/cache"
go run ./probes/baseline
