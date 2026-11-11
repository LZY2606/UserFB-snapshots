#!/bin/sh
# Runs the sphere-light target test group. Fails if the group is missing.
set -e
cd "$(dirname "$0")/.."

out=$(GO111MODULE=off go test ./pt -run 'TestSphereLightTarget' -v -count=1 2>&1) || {
    echo "$out"
    exit 1
}
echo "$out"
if ! echo "$out" | grep -q 'RUN   TestSphereLightTarget'; then
    echo "error: TestSphereLightTarget test group not found" >&2
    exit 1
fi
