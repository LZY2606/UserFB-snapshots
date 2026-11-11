#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
command -v cc
cc --version
# The only library dependencies are the pinned in-tree stb and font fixture.
if command -v sha256sum >/dev/null 2>&1; then sha256sum -c harness/dependencies.sha256; else shasum -a 256 -c harness/dependencies.sha256; fi
