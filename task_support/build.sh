#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/objects
if [[ ! -f .task-build/xml/libxml2.a ]]; then
 cmake -S vendor/libxml2 -B .task-build/xml -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_ICU=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_CATALOG=OFF -DLIBXML2_WITH_C14N=OFF -DLIBXML2_WITH_WRITER=OFF -DLIBXML2_WITH_XINCLUDE=OFF -DLIBXML2_WITH_XPTR=OFF -DLIBXML2_WITH_XPATH=OFF -DLIBXML2_WITH_SCHEMAS=OFF -DLIBXML2_WITH_SCHEMATRON=OFF -DLIBXML2_WITH_VALID=OFF -DLIBXML2_WITH_READER=OFF >.task-build/xml-config.log 2>&1
 cmake --build .task-build/xml --target LibXml2 --parallel 4 >.task-build/xml-build.log 2>&1
fi
# All added production C files in c/ are included. Optional Raptor and Windows backends are excluded.
objects=()
for source in c/*.c; do
 case "$source" in c/parser_raptor.c|c/mingw32_utils.c) continue;; esac
 object=".task-build/objects/$(basename "${source%.c}").o"
 cc -std=c99 -D_POSIX_C_SOURCE=200809L -DHAVE_STRTOK_R=1 -DHAVE_STRINGS_H=1 -I. -Ic -Ivendor/libxml2/include -I.task-build/xml -c "$source" -o "$object"
 objects+=("$object")
done
rm -f .task-build/librdfa.a
ar rcs .task-build/librdfa.a "${objects[@]}"
