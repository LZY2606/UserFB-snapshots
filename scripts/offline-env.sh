# This repository vendors its only third-party dependency (the pure-Go
# modernc.org/sqlite driver) under ./vendor, so the normal commands work with
# no network access and no extra configuration:
#
#	go build ./...
#	go test ./... -count=1
#	go run ./cmd/server --addr 127.0.0.1:5532
#
# The script below was only needed while bootstrapping dependencies inside a
# sandbox whose egress proxy re-signs TLS and blocks the Go toolchain's access
# to the macOS keychain. scripts/mirror_mods.py documents how the dependency
# closure was fetched with curl/Python and laid out as a file:// module proxy.
#
# If you ever add a dependency in that sandbox, recreate the mirror with
# mirror_mods.py, then (with this file sourced) run `go get` and `go mod
# vendor` again.
_here="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)"
export GOSUMDB=off
export GOTOOLCHAIN=local
if [ -d "${_here}/modmirror/cache/download" ]; then
	export GOPROXY="file://${_here}/modmirror/cache/download"
	export GOFLAGS=-mod=mod
else
	# Vendored build: no proxy required at all.
	export GOPROXY=off
	export GOFLAGS=-mod=vendor
fi
unset _here
