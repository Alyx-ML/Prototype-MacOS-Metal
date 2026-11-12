#!/bin/sh
# Build and run the runtime unit tests (tests/test_*.c). Each test program
# links tests/harness.c, all runtime C and Objective-C objects except the
# generated import table (it refers to the import thunks, and some imports
# are not implemented yet), and the llasm objects calls.o (guest calls
# from C) and com_thunks.o (COM method thunks). The port directory can
# contain spaces, so the paths are passed as separate arguments.
set -eu
PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CC=/opt/homebrew/opt/llvm/bin/clang

cc() {
    "$CC" -target arm64-apple-macosx14.0.0 -std=c2x -O2 -g \
        -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror \
        -DPTROFS_64BIT -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" "$@"
}

mkdir -p "$PORT_DIR/build/rt" "$PORT_DIR/build/tests"
rm -f "$PORT_DIR"/build/rt/*.o
for src in "$PORT_DIR"/runtime/src/*.c; do
    name="$(basename "$src" .c)"
    [ "$name" = rt_import_table ] && continue
    cc -c "$src" -o "$PORT_DIR/build/rt/$name.o"
done
for src in "$PORT_DIR"/runtime/src/*.m; do
    name="$(basename "$src" .m)"
    "$CC" -target arm64-apple-macosx14.0.0 -fobjc-arc -O2 -g \
        -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror \
        -DPTROFS_64BIT -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" \
        -c "$src" -o "$PORT_DIR/build/rt/$name.o"
done
sh "$PORT_DIR/tools/build_llasm_rt.sh" calls com_thunks

status=0
for t in "$PORT_DIR"/tests/test_*.c; do
    name="$(basename "$t" .c)"
    set --
    for o in "$PORT_DIR"/build/rt/*.o; do
        [ "$(basename "$o")" = rt_import_table.o ] && continue
        set -- "$@" "$o"
    done
    cc -I"$PORT_DIR/tests" "$t" "$PORT_DIR/tests/harness.c" "$@" \
        -framework Security -framework CoreFoundation -framework Foundation -framework Metal -framework Cocoa -framework QuartzCore -o "$PORT_DIR/build/tests/$name"
    echo "== $name"
    "$PORT_DIR/build/tests/$name" || status=1
done
exit $status
