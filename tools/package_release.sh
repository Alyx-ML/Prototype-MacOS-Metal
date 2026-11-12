#!/bin/sh
# Make the release package build/release/Prototype-Mac-ARM64.zip.
#
# The release package holds no game code. It holds the loader, the runtime
# (as a static archive) and the converter. On the first start, the
# converter translates the game files of the player (see
# runtime/loader/rt_convert.h).
#
# Prerequisites:
#   1. tools/translate_launcher.sh and tools/translate_engine.sh completed.
#      The SRW output files in work/launcher and work/engine are the tested
#      translation; their SHA-256 values go into the manifest.
#   2. build/srw/SRW and build/llasm/llasm exist.
#   3. LLVM and lld from Homebrew (/opt/homebrew/opt/llvm, /opt/homebrew/opt/lld).
#   4. FFMPEG_PREFIX names an FFmpeg build with the LGPL license (configured
#      without --enable-gpl and without --enable-nonfree) that holds
#      lib/libavcodec and lib/libavutil and a license file (COPYING.LGPLv2.1,
#      COPYING.LGPLv3 or LICENSE.md). The Homebrew FFmpeg is a GPL build and
#      is refused.
#
# Result:
#   build/release/dist/Prototype.app
#   build/release/dist/Put game files here/
#   build/release/dist/README.txt
#   build/release/Prototype-Mac-ARM64.zip
#
# The script stops with an error if a check fails:
#   - the FFmpeg build is a GPL or nonfree build,
#   - the application holds a file with game code (.o, .ll, .llasm, .bc
#     other than the SSE helper bitcode, .exe, .dll) or a symbol of the
#     translated code,
#   - a file holds a path of the build computer (/Users/).
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_PREFIX=/opt/homebrew/opt/llvm
LLD_PREFIX=/opt/homebrew/opt/lld
LLVM="$LLVM_PREFIX/bin"
TRIPLE=arm64-apple-macosx14.0.0
OUT="$PORT_DIR/build/release"
DIST="$OUT/dist"
APP="$DIST/Prototype.app"
REC="$APP/Contents/Resources/Recompiler"
OBJ="$OUT/obj"
FRAMEWORKS="$APP/Contents/Frameworks"

die() {
    echo "error: $*" >&2
    exit 1
}

[ -n "${FFMPEG_PREFIX:-}" ] || die "set FFMPEG_PREFIX to an FFmpeg build with the LGPL license"
for f in "$FFMPEG_PREFIX/lib/libavcodec.dylib" "$FFMPEG_PREFIX/lib/libavutil.dylib" "$FFMPEG_PREFIX/include/libavcodec/avcodec.h" \
    "$PORT_DIR/build/srw/SRW" "$PORT_DIR/build/llasm/llasm" "$LLVM/opt" "$LLVM/llvm-split" "$LLVM/llvm-link" "$LLVM/clang-23" "$LLD_PREFIX/bin/lld"; do
    [ -f "$f" ] || die "$f does not exist"
done
for lib in libavcodec libavutil; do
    if grep -a -q -e "--enable-gpl" -e "--enable-nonfree" "$FFMPEG_PREFIX/lib/$lib.dylib"; then
        die "$FFMPEG_PREFIX/lib/$lib.dylib is a GPL or nonfree build; the release needs an LGPL build"
    fi
done
FFMPEG_LICENSE=""
for f in COPYING.LGPLv2.1 COPYING.LGPLv3 LICENSE.md; do
    for d in "$FFMPEG_PREFIX" "$FFMPEG_PREFIX/share/doc/ffmpeg" "$FFMPEG_PREFIX/share/ffmpeg"; do
        if [ -z "$FFMPEG_LICENSE" ] && [ -f "$d/$f" ]; then FFMPEG_LICENSE="$d/$f"; fi
    done
done
[ -n "$FFMPEG_LICENSE" ] || die "no FFmpeg license file in $FFMPEG_PREFIX"
for m in launcher engine; do
    [ -f "$PORT_DIR/work/$m/$m.llasm" ] || die "work/$m/$m.llasm does not exist; translate the game modules first"
done

rm -rf "$OUT"
mkdir -p "$OBJ" "$APP/Contents/MacOS" "$REC/bin" "$REC/lib" "$REC/include/llasm-support" "$REC/include/runtime" \
    "$REC/modules" "$FRAMEWORKS" "$APP/Contents/Resources/Licenses" "$DIST/Put game files here"

# Compile one file. The port directory can contain spaces, so the paths
# are separate quoted arguments. -ffile-prefix-map removes the build
# directory from the objects.
compile() {
    "$LLVM/clang" -target $TRIPLE -O2 -ffile-prefix-map="$PORT_DIR"=/prototype-mac-port \
        -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror -DPTROFS_64BIT \
        -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" -isystem "$FFMPEG_PREFIX/include" "$@"
}

echo "PROGRESS compile the runtime archive"
for src in "$PORT_DIR"/runtime/src/*.c; do
    compile -std=c2x -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
for src in "$PORT_DIR"/runtime/src/*.m; do
    compile -fobjc-arc -c "$src" -o "$OBJ/$(basename "$src" .m).o"
done
for src in "$PORT_DIR"/third_party/SR/SR/llasm-support/*.c; do
    name="$(basename "$src" .c)"
    [ "$name" = llasm_inout ] && continue
    compile -std=c2x -c "$src" -o "$OBJ/support_$name.o"
done
sh "$PORT_DIR/tools/build_llasm_rt.sh" thunks calls com_thunks
for name in thunks calls com_thunks; do
    cp "$PORT_DIR/build/rt/$name.o" "$OBJ/llasm_$name.o"
done
"$LLVM/llvm-ar" rcs "$REC/lib/libprototype_rt.a" "$OBJ"/*.o

echo "PROGRESS build the loader"
compile -std=c2x -c "$PORT_DIR/runtime/loader/rt_convert.c" -o "$OBJ/loader_rt_convert.o"
compile -fobjc-arc -DRT_LOADER_DEV=0 -c "$PORT_DIR/runtime/loader/rt_loader.m" -o "$OBJ/loader_rt_loader.o"
for lib in libavcodec libavutil; do
    cp -L "$FFMPEG_PREFIX/lib/$lib.dylib" "$FRAMEWORKS/$lib.dylib"
done

# Copy the libraries that a Mach-O file needs (not the system libraries)
# into a directory, and refer to them with @rpath.
# Arguments: <mach-o file> <library directory>
copy_deps() {
    own_id="$(otool -D "$1" | tail -n +2)"
    otool -L "$1" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in
            /usr/lib/* | /System/*) continue ;;
        esac
        [ "$dep" = "$own_id" ] && continue
        base="$(basename "$dep")"
        if [ ! -f "$2/$base" ]; then
            src=""
            case "$dep" in
                @rpath/* | @loader_path/* | @executable_path/*)
                    for d in "$LLVM_PREFIX/lib" "$LLD_PREFIX/lib" "$FFMPEG_PREFIX/lib"; do
                        if [ -z "$src" ] && [ -f "$d/$base" ]; then src="$d/$base"; fi
                    done
                    ;;
                *) src="$dep" ;;
            esac
            [ -n "$src" ] && [ -f "$src" ] || die "the library $dep of $1 was not found"
            cp -L "$src" "$2/$base"
            chmod u+w "$2/$base"
            install_name_tool -id "@rpath/$base" "$2/$base"
            copy_deps "$2/$base" "$2"
        fi
        install_name_tool -change "$dep" "@rpath/$base" "$1"
    done
}

# Remove each LC_RPATH entry of a file and add one entry.
# Arguments: <mach-o file> <rpath>
set_rpath() {
    otool -l "$1" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | while read -r p; do
        install_name_tool -delete_rpath "$p" "$1"
    done
    install_name_tool -add_rpath "$2" "$1"
}

for lib in libavcodec libavutil; do
    install_name_tool -id "@rpath/$lib.dylib" "$FRAMEWORKS/$lib.dylib"
    copy_deps "$FRAMEWORKS/$lib.dylib" "$FRAMEWORKS"
done
"$LLVM/clang" -target $TRIPLE -fobjc-arc "$OBJ/loader_rt_loader.o" "$OBJ/loader_rt_convert.o" \
    -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox -framework CoreAudio \
    -framework GameController -framework CoreHaptics -framework Security -framework CoreFoundation \
    -framework Foundation -framework CoreGraphics \
    "$FRAMEWORKS/libavcodec.dylib" "$FRAMEWORKS/libavutil.dylib" -Wl,-rpath,@executable_path/../Frameworks \
    -o "$APP/Contents/MacOS/Prototype"

echo "PROGRESS copy the converter tools"
cp "$PORT_DIR/build/srw/SRW" "$REC/bin/SRW"
cp "$PORT_DIR/build/llasm/llasm" "$REC/bin/llasm"
cp -L "$LLVM/opt" "$REC/bin/opt"
cp -L "$LLVM/llvm-split" "$REC/bin/llvm-split"
cp -L "$LLVM/llvm-link" "$REC/bin/llvm-link"
cp -L "$LLVM/clang-23" "$REC/bin/clang"
cp -L "$LLD_PREFIX/bin/lld" "$REC/bin/ld64.lld"
chmod u+w "$REC"/bin/*
for t in "$REC"/bin/*; do
    copy_deps "$t" "$REC/lib"
    set_rpath "$t" @loader_path/../lib
done
for l in "$REC"/lib/*.dylib; do
    set_rpath "$l" @loader_path
done
CLANG_RT="$("$LLVM/clang" -target $TRIPLE -print-file-name=lib/darwin/libclang_rt.osx.a)"
[ -f "$CLANG_RT" ] || die "libclang_rt.osx.a was not found"
cp "$CLANG_RT" "$REC/lib/libclang_rt.osx.a"
cp "$PORT_DIR"/third_party/SR/SR/llasm-support/*.llinc "$REC/include/llasm-support/"
# The SSE helpers as bitcode: the converter links them into the engine
# parts so that clang can inline them (see tools/build_sse_inline.sh).
SSE_BC="$REC/include/llasm-support/sse_inline.bc"
sh "$PORT_DIR/tools/build_sse_inline.sh" "$SSE_BC"
cp "$PORT_DIR"/runtime/llasm/*.llinc "$REC/include/runtime/"

echo "PROGRESS write the analysis files and the manifest"
SDK_VERSION="$("$LLVM/clang" -target $TRIPLE -### -c -x c /dev/null -o /dev/null 2>&1 | tr ' ' '\n' |
    sed -n 's/^"-target-sdk-version=\([0-9.]*\)"$/\1/p' | head -1)"
[ -n "$SDK_VERSION" ] || die "the SDK version was not found"
MANIFEST="$REC/manifest.txt"
{
    echo "# Prototype Mac Port converter manifest. Generated by tools/package_release.sh."
    echo "sdk $SDK_VERSION"
} > "$MANIFEST.body"
for m in launcher engine; do
    mkdir -p "$REC/modules/$m"
    for f in "$PORT_DIR/work/$m"/*.sci "$PORT_DIR/work/$m/SR.cfg" "$PORT_DIR/work/$m/extern.llinc" "$PORT_DIR/work/$m/relocations.csv"; do
        [ -f "$f" ] && cp "$f" "$REC/modules/$m/"
    done
    for f in "$PORT_DIR/work/$m/$m.llasm" "$PORT_DIR/work/$m"/seg*_code.llinc "$PORT_DIR/work/$m"/seg*_data.llinc; do
        [ -f "$f" ] || die "$f does not exist"
        echo "pin $m $(basename "$f") $(shasum -a 256 "$f" | cut -d' ' -f1)" >> "$MANIFEST.body"
    done
done

echo "PROGRESS sign the libraries and the tools"
for f in "$REC"/bin/* "$REC"/lib/*.dylib "$FRAMEWORKS"/*.dylib; do
    codesign --force --sign - "$f"
done
CONTENT="$(cd "$REC" && find . -type f ! -name manifest.txt ! -name manifest.txt.body | LC_ALL=C sort |
    while read -r f; do echo "$(shasum -a 256 "$f" | cut -d' ' -f1) $f"; done | shasum -a 256 | cut -d' ' -f1)"
{
    cat "$MANIFEST.body"
    echo "content $CONTENT"
} > "$MANIFEST"
rm "$MANIFEST.body"

echo "PROGRESS licenses"
LIC="$APP/Contents/Resources/Licenses"
cp "$LLVM_PREFIX/LICENSE.TXT" "$LIC/LLVM-LICENSE.txt"
cp "$LLD_PREFIX/LICENSE.TXT" "$LIC/LLD-LICENSE.txt"
cp /opt/homebrew/opt/z3/LICENSE.txt "$LIC/Z3-LICENSE.txt"
cp /opt/homebrew/opt/zstd/LICENSE "$LIC/ZSTD-LICENSE.txt"
cp "$FFMPEG_LICENSE" "$LIC/FFMPEG-$(basename "$FFMPEG_LICENSE")"
cp "$PORT_DIR/third_party/SR/README.md" "$LIC/SR-README.md"
for f in "$REC"/lib/*.dylib "$FRAMEWORKS"/*.dylib; do
    case "$(basename "$f")" in
        libLLVM* | libclang-cpp* | liblld* | libz3* | libzstd* | libavcodec* | libavutil*) ;;
        *) echo "warning: $(basename "$f") has no license file in Contents/Resources/Licenses; add it before release" >&2 ;;
    esac
done

# The minimum macOS version is the highest minimum version of the files.
MIN_VERSION="$(for f in "$APP/Contents/MacOS/Prototype" "$REC"/bin/* "$REC"/lib/*.dylib "$FRAMEWORKS"/*.dylib; do
    otool -l "$f" | awk '/cmd LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; b=0}'
done | awk -F. 'BEGIN{best="14.0"; split(best, B, ".")}
    { if ($1+0 > B[1]+0 || ($1+0 == B[1]+0 && $2+0 > B[2]+0)) { best=$0; split(best, B, ".") } }
    END{print best}')"

cat > "$APP/Contents/Info.plist" <<PLIST
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
    <string>$MIN_VERSION</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP"

echo "PROGRESS check the package"
# sse_inline.bc is the one permitted bitcode file. It holds only the SR
# llasm support helpers; the symbol check below includes it.
bad="$(find "$DIST" -type f \( -name '*.o' -o -name '*.ll' -o -name '*.llasm' -o -name '*.bc' -o -iname '*.exe' -o -iname '*.dll' \) ! -path "$SSE_BC" | head -5)"
[ -z "$bad" ] || die "the package holds files with game code: $bad"
# The import thunks of runtime/llasm/thunks.llasm are runtime code named
# <import symbol>_asm2c (for example _X__Q_Q2_AYAPAXI_AZ_asm2c for the
# msvcr80 operator new). The translated code defines no _asm2c symbol.
if "$LLVM/llvm-nm" --defined-only "$APP/Contents/MacOS/Prototype" "$REC/lib/libprototype_rt.a" "$SSE_BC" |
    grep -E ' (_X_|_loc_[0-9A-F]|_engine_image|_launcher_image)' | grep -q -v -E ' _X_[A-Za-z0-9_]*_asm2c$'; then
    die "the package defines a symbol of the translated code"
fi
paths="$(grep -r -l -a "/Users/" "$DIST" | head -5 || true)"
[ -z "$paths" ] || die "files hold paths of the build computer: $paths"

cat > "$DIST/README.txt" <<'README'
Prototype for Mac (Apple silicon)

1. Copy the installed Steam version of Prototype into the folder
   "Put game files here". The folder must hold prototypef.exe and
   prototypeenginef.dll.
2. Open Prototype.app.
3. On the first start, the application prepares the game for this Mac.
   This is done one time. It can take several minutes and needs 8 GB of
   free disk space for temporary files.
4. The game starts. Later starts do not prepare the game again.

The application has an ad hoc signature. If macOS does not open it, open
System Settings > Privacy & Security and click "Open Anyway".

Hold the Option key when the application starts to select a different
game folder.
README

rm -f "$OUT/Prototype-Mac-ARM64.zip"
(cd "$DIST" && ditto -c -k --sequesterRsrc . "$OUT/Prototype-Mac-ARM64.zip")
echo "PROGRESS done: $OUT/Prototype-Mac-ARM64.zip (minimum macOS $MIN_VERSION)"
