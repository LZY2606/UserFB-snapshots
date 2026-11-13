#!/bin/bash
set -euo pipefail
R=$(pwd)
B=${CHECK_BUILD_DIR:-$(mktemp -d)}
if [[ -z ${CHECK_BUILD_DIR:-} ]]; then trap 'rm -rf "$B"' EXIT; fi
mkdir -p "$B/zlib" "$B/ff"
if [[ ! -f "$B/zlib/libz.a" ]]; then
 (cd "$B/zlib"; "$R/vendor/zlib/configure" --static; make -j4 libz.a)
fi
if [[ ! -f "$B/ff/libavcodec/libavcodec.a" ]]; then
 (cd "$B/ff"; "$R/configure" --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network --disable-x86asm --enable-zlib --enable-encoder=exr --enable-decoder=exr --extra-cflags="-I$B/zlib -I$R/vendor/zlib" --extra-ldflags="-L$B/zlib"; make -j4 libavcodec/libavcodec.a libavutil/libavutil.a)
fi
compile() {
 cc -std=c11 -I"$B/ff" -I"$R" -I"$R/support" "$1" "$B/ff/libavcodec/libavcodec.a" "$B/ff/libavutil/libavutil.a" "$B/zlib/libz.a" -lm -lpthread -o "$2"
}
compile_target() {
 cc -std=c11 -I"$B/ff" -I"$R" -I"$R/support" "$1" "$R/support/driver.c" "$B/ff/libavcodec/libavcodec.a" "$B/ff/libavutil/libavutil.a" "$B/zlib/libz.a" -lm -lpthread -o "$2"
}
