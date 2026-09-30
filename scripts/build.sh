#!/bin/sh
# Build vwinput (virtual Wayland pointer/keyboard injector) into ../bin.
set -eu

cd "$(dirname "$0")"

SC="${SC:-wayland-scanner}"
CC="${CC:-cc}"
OUT="${OUT:-../bin/vwinput}"
BUILD="build"

command -v "$SC" >/dev/null 2>&1 || { echo "build.sh: $SC not found (package: wayland)" >&2; exit 1; }
command -v "$CC" >/dev/null 2>&1 || { echo "build.sh: no C compiler" >&2; exit 1; }

mkdir -p "$BUILD" "$(dirname "$OUT")"

gen() {
    xml="$1"
    base="$(basename "$xml" .xml)"
    "$SC" client-header "$xml" "$BUILD/$base-client-protocol.h"
    # public-code: interface symbols must be externally visible, vwinput.c
    # references them through the generated header.
    if "$SC" public-code "$xml" "$BUILD/$base-code.c" 2>/dev/null; then
        :
    else
        "$SC" code "$xml" "$BUILD/$base-code.c"
    fi
}

gen protocol/virtual-keyboard-unstable-v1.xml
gen protocol/wlr-virtual-pointer-unstable-v1.xml

"$CC" -O2 -Wall -Wextra -o "$OUT" vwinput.c "$BUILD"/*-code.c -I"$BUILD" \
    -lwayland-client -lxkbcommon -lm

echo "built $OUT"
