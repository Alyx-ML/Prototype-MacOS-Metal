#!/bin/sh
# Build the development application build/app/Prototype.app.
#
# The application has the same structure as the release application
# (tools/package_release.sh), but the game bundle is made here from the
# translated objects of the work directory:
#   Contents/MacOS/Prototype            the loader (runtime/loader), with
#                                       RT_LOADER_DEV=1
#   Contents/Resources/Prototype.bundle the runtime and the translated code
#
# Prerequisites:
#   1. tools/translate_launcher.sh, tools/translate_engine.sh and
#      tools/compile_engine.sh completed (work/launcher/launcher.o,
#      work/engine/parts/*.o).
#   2. LLVM and FFmpeg from Homebrew (/opt/homebrew/opt/llvm,
#      /opt/homebrew/opt/ffmpeg).
#
# Steps:
#   1. Compile each runtime C and Objective-C file with -Werror.
#   2. Translate the runtime llasm files (import thunks, guest calls, COM
#      thunks) with tools/build_llasm_rt.sh.
#   3. Compile the SR llasm support helpers.
#   4. Link the runtime, the translated launcher and the translated engine
#      into the game bundle (MH_BUNDLE). The undefined symbols (system
#      libraries, frameworks, FFmpeg) are found when the loader opens the
#      bundle.
#   5. Compile and link the loader with the frameworks and FFmpeg, and sign
#      the application with an ad hoc signature.
#
# WARNING: the application contains the translated code of the game. Do not
# distribute it, upload it or commit it. Players get the runtime and the
# converter only.
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LLVM=/opt/homebrew/opt/llvm/bin
# The LGPL FFmpeg build of tools/build_ffmpeg_lgpl.sh (the same libraries
# as the release).
FFMPEG="$PORT_DIR/build/ffmpeg-lgpl"
[ -f "$FFMPEG/lib/libavcodec.dylib" ] || { echo "error: $FFMPEG has no FFmpeg build; run tools/build_ffmpeg_lgpl.sh" >&2; exit 1; }
TRIPLE=arm64-apple-macosx14.0.0
OBJ="$PORT_DIR/build/app/obj"
APP="$PORT_DIR/build/app/Prototype.app"

# Compile one file. The port directory can contain spaces, so the include
# paths are separate quoted arguments.
compile() {
    "$LLVM/clang" -target $TRIPLE -O2 -g \
        -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror -DPTROFS_64BIT \
        -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" -isystem "$FFMPEG/include" "$@"
}

if [ ! -f "$PORT_DIR/work/launcher/launcher.o" ]; then
    echo "error: work/launcher/launcher.o does not exist; translate the game modules first" >&2
    exit 1
fi
if ! ls "$PORT_DIR"/work/engine/parts/engine*.o > /dev/null 2>&1; then
    echo "error: work/engine/parts has no objects; run tools/compile_engine.sh first" >&2
    exit 1
fi

rm -rf "$OBJ"
mkdir -p "$OBJ"

echo "PROGRESS compile the runtime"
for src in "$PORT_DIR"/runtime/src/*.c; do
    name="$(basename "$src" .c)"
    compile -std=c2x -c "$src" -o "$OBJ/$name.o"
done
for src in "$PORT_DIR"/runtime/src/*.m; do
    name="$(basename "$src" .m)"
    compile -fobjc-arc -c "$src" -o "$OBJ/$name.o"
done
# Helpers that the translated code calls (x87, SSE, string instructions,
# 64-bit division, fs access): the C files of the SR llasm support. The
# port I/O helpers (llasm_inout.c) are not used: the game code has no in or
# out instruction.
for src in "$PORT_DIR"/third_party/SR/SR/llasm-support/*.c; do
    name="$(basename "$src" .c)"
    [ "$name" = llasm_inout ] && continue
    compile -std=c2x -c "$src" -o "$OBJ/support_$name.o"
done

echo "PROGRESS translate the runtime llasm files"
sh "$PORT_DIR/tools/build_llasm_rt.sh" thunks calls com_thunks
for name in thunks calls com_thunks; do
    cp "$PORT_DIR/build/rt/$name.o" "$OBJ/llasm_$name.o"
done

echo "PROGRESS link the game bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
set --
for o in "$OBJ"/*.o "$PORT_DIR/work/launcher/launcher.o" "$PORT_DIR"/work/engine/parts/engine*.o; do
    set -- "$@" "$o"
done
"$LLVM/clang" -target $TRIPLE -bundle -undefined dynamic_lookup "$@" -o "$APP/Contents/Resources/Prototype.bundle"

echo "PROGRESS build the loader"
LOBJ="$PORT_DIR/build/app/loader"
rm -rf "$LOBJ"
mkdir -p "$LOBJ"
compile -std=c2x -c "$PORT_DIR/runtime/loader/rt_convert.c" -o "$LOBJ/rt_convert.o"
compile -fobjc-arc -DRT_LOADER_DEV=1 -c "$PORT_DIR/runtime/loader/rt_loader.m" -o "$LOBJ/rt_loader.o"
"$LLVM/clang" -target $TRIPLE -fobjc-arc "$LOBJ/rt_loader.o" "$LOBJ/rt_convert.o" \
    -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox -framework CoreAudio \
    -framework GameController -framework CoreHaptics -framework Security -framework CoreFoundation \
    -framework Foundation -framework CoreGraphics \
    -L"$FFMPEG/lib" -lavcodec -lavutil -Wl,-rpath,"$FFMPEG/lib" \
    -o "$APP/Contents/MacOS/Prototype"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>Prototype</string>
    <key>CFBundleIdentifier</key>
    <string>local.prototype-mac-port</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>Prototype</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.action-games</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST

codesign --force --sign - "$APP"
echo "PROGRESS done: $APP"
