#!/bin/sh
# Build the static recompiler tools: build/srw/SRW and build/llasm/llasm.
#
# Prerequisites:
#   - clang (Xcode or the Xcode Command Line Tools).
#   - ldc2, the LLVM D compiler: the osx-arm64 archive of
#     https://github.com/ldc-developers/ldc/releases, with its bin folder in
#     PATH.
#
# Usage: sh tools/build_toolchain.sh [output directory]
# The default output directory is build.
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$PORT_DIR/build}"
# The release ships SRW and llasm in the converter, so they must run on the
# minimum macOS version of the release (MIN_VERSION in tools/package_release.sh).
MIN_MACOS=27.2
export MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS"
SRW_SRC="$PORT_DIR/third_party/SR/SRW"
UDIS="$SRW_SRC/udis86-1.7.2"

command -v clang > /dev/null || { echo "error: clang was not found; install the Xcode Command Line Tools" >&2; exit 1; }
command -v ldc2 > /dev/null || { echo "error: ldc2 was not found; put the bin folder of LDC (https://github.com/ldc-developers/ldc/releases) in PATH" >&2; exit 1; }

mkdir -p "$OUT/srw/compat" "$OUT/llasm"
# SRW includes <malloc.h>, which macOS does not have. malloc and free are
# declared in <stdlib.h>.
echo '#include <stdlib.h>' > "$OUT/srw/compat/malloc.h"

for src in "$SRW_SRC"/*.c "$UDIS"/libudis86/*.c; do
    clang -O2 -Wall -mmacosx-version-min="$MIN_MACOS" -DHAVE_STRING_H=1 -I "$OUT/srw/compat" -I "$SRW_SRC" -I "$UDIS" \
        -c "$src" -o "$OUT/srw/$(basename "$src" .c).o"
done
clang++ -O2 -Wall -mmacosx-version-min="$MIN_MACOS" -I "$OUT/srw/compat" -I "$SRW_SRC" -I "$UDIS" -c "$SRW_SRC/container_helpers.cpp" -o "$OUT/srw/container_helpers.o"
clang++ -mmacosx-version-min="$MIN_MACOS" -o "$OUT/srw/SRW" "$OUT"/srw/*.o

ldc2 -O -release -mtriple=arm64-apple-macos"$MIN_MACOS" -of="$OUT/llasm/llasm" -od="$OUT/llasm" "$PORT_DIR/third_party/SR/llasm/llasm.d"
echo "done: $OUT/srw/SRW $OUT/llasm/llasm"
