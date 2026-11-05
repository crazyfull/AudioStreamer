#!/usr/bin/env bash
# Builds webrtc-audio-processing 1.3 (+ abseil) as ONE static library for an Android ABI with the NDK (meson cross build):
#     <out>/lib/libwebrtc_apm.a     the library and every abseil lib it needs, merged
#     <out>/include/                webrtc headers (their own folder layout) + absl/
# like libs/webrtc/build_win.bat does for Windows. build-demo/android/CMakeLists.txt links it (-DWEBRTC_ROOT=<out>).
#
# usage:  ANDROID_NDK_ROOT=/path/to/ndk libs/webrtc/build_android.sh <webrtc-audio-processing-1.3 source dir> <abi> <out dir>
#         abi: arm64-v8a | armeabi-v7a | x86_64 | x86          optional: ANDROID_API (default 28)
# Needs: meson + ninja (pip install meson ninja), internet (meson downloads abseil), a Linux host.
set -euo pipefail

SRC="$(realpath "${1:?webrtc-audio-processing source dir}")"
ABI="${2:?abi}"
OUT="$(realpath -m "${3:?output dir}")"
API="${ANDROID_API:-28}"
NDK="${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

[ -f "$SRC/meson.build" ] || { echo "[error] $SRC/meson.build not found"; exit 1; }
[ -d "$TC" ] || { echo "[error] NDK toolchain not found: $TC"; exit 1; }

case "$ABI" in
    arm64-v8a)   TRIPLE=aarch64-linux-android;    CCPREFIX=aarch64-linux-android;    FAMILY=aarch64; CPU=aarch64 ;;
    armeabi-v7a) TRIPLE=arm-linux-androideabi;    CCPREFIX=armv7a-linux-androideabi; FAMILY=arm;     CPU=armv7 ;;
    x86_64)      TRIPLE=x86_64-linux-android;     CCPREFIX=x86_64-linux-android;     FAMILY=x86_64;  CPU=x86_64 ;;
    x86)         TRIPLE=i686-linux-android;       CCPREFIX=i686-linux-android;       FAMILY=x86;     CPU=i686 ;;
    *) echo "[error] unknown abi: $ABI"; exit 1 ;;
esac

WORK="$OUT/_build"
rm -rf "$WORK"; mkdir -p "$WORK" "$OUT/lib" "$OUT/include"

cat > "$WORK/cross.ini" <<INI
[binaries]
c = '$TC/bin/${CCPREFIX}${API}-clang'
cpp = '$TC/bin/${CCPREFIX}${API}-clang++'
ar = '$TC/bin/llvm-ar'
ranlib = '$TC/bin/llvm-ranlib'
strip = '$TC/bin/llvm-strip'

[host_machine]
system = 'android'
cpu_family = '$FAMILY'
cpu = '$CPU'
endian = 'little'
INI

# 32 bit ARM: the library's NEON code wants run-time CPU detection that is not available here; the plain C path is used there.
EXTRA=""
[ "$FAMILY" = "arm" ] && EXTRA="-Dneon=no"

setup() { meson setup "$WORK/b" "$SRC" --cross-file "$WORK/cross.ini" --buildtype=release --default-library=static "$@"; }
if ! setup $EXTRA; then
    echo "[warn] meson setup failed with '$EXTRA', trying without"
    rm -rf "$WORK/b"
    setup
fi
meson compile -C "$WORK/b"

# one archive: the main library + every abseil library (static libs of meson subprojects are not installed, so take them from the build tree)
MAIN="$(find "$WORK/b" -name 'libwebrtc-audio-processing-1.a' | head -n1)"
[ -n "$MAIN" ] || { echo "[error] libwebrtc-audio-processing-1.a not built"; find "$WORK/b" -name '*.a'; exit 1; }
{
    echo "create $OUT/lib/libwebrtc_apm.a"
    echo "addlib $MAIN"
    find "$WORK/b/subprojects" -name '*.a' | sort | while read -r f; do echo "addlib $f"; done
    echo "save"
    echo "end"
} > "$WORK/merge.mri"
"$TC/bin/llvm-ar" -M < "$WORK/merge.mri"
"$TC/bin/llvm-ranlib" "$OUT/lib/libwebrtc_apm.a"

# headers: webrtc ones keep their folder layout (modules/audio_processing/include/...), abseil under absl/
(cd "$SRC/webrtc" && find . -name '*.h' -print0 | xargs -0 -I{} install -D -m 0644 {} "$OUT/include/{}")
ABSL="$(find "$SRC/subprojects" -maxdepth 1 -type d -name 'abseil-cpp-*' | head -n1)"
[ -n "$ABSL" ] || { echo "[error] abseil sources not found under $SRC/subprojects"; exit 1; }
(cd "$ABSL" && find absl \( -name '*.h' -o -name '*.inc' \) -print0 | xargs -0 -I{} install -D -m 0644 {} "$OUT/include/{}")

rm -rf "$WORK"
echo "Done: $OUT"
ls -l "$OUT/lib"
