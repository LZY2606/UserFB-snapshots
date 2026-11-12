#!/bin/sh
set -eu
if [ -n "${CHECK_NODE:-}" ]; then
  runtime="$CHECK_NODE"
elif [ -x /opt/homebrew/opt/node@22/bin/node ]; then
  runtime=/opt/homebrew/opt/node@22/bin/node
else
  runtime=node
fi
version="$("$runtime" --version)"
if [ "$version" != v22.23.2 ]; then
  echo "Node 22.23.2 required; select an installed runtime with CHECK_NODE" >&2
  exit 1
fi
exec "$runtime" "$@"
