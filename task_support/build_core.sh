#!/bin/bash
set -euo pipefail
mkdir -p .task-build
# Preserve current production bodies. Only file loading/unloading and its
# optional OpenSSL initialization/cleanup are excluded; no API is stubbed.
# Tests own an in-memory file buffer and call pe_parse/pe_exports themselves.
awk '
/^#include <openssl\// {next}
/^pe_err_e pe_load_file\(/ {skip=1}
/^pe_err_e pe_parse\(/ {skip=0}
!skip {print}
' lib/libpe/pe.c > .task-build/pe_core.c
cc -std=c11 -D_DEFAULT_SOURCE -I. -Ilib/libpe/include -c .task-build/pe_core.c -o .task-build/pe_core.o
cc -std=c11 -D_DEFAULT_SOURCE -I. -Ilib/libpe/include -c lib/libpe/exports.c -o .task-build/exports.o
cc -std=c11 -I. -Ilib/libpe/include -c lib/libpe/error.c -o .task-build/error.o
