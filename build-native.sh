#!/bin/bash
# ============================================================
#  build-native.sh —— 交叉编译 native GL 渲染库 (arm64-v8a)
#  产物: lib/arm64-v8a/libnativegl.so
#
#  关键: 必须加 -lGLESv2 (配合 prebuilt/syslib/libGLESv2.so 链接 stub),
#        使 so 记录 DT_NEEDED=libGLESv2.so, 否则运行期 GL 符号无法解析,
#        dlopen 报 UnsatisfiedLinkError: cannot locate symbol 'glGetUniformLocation'。
# ============================================================
set -e
P="$(cd "$(dirname "$0")" && pwd)"
cd "$P"

TARGET=aarch64-linux-android
ABI=arm64-v8a
CC="${CC:-clang}"

mkdir -p "lib/$ABI"

echo "==> 编译 libnativegl.so ($ABI)"
"$CC" -target "$TARGET" -std=c99 -O2 -fPIC -shared \
    -Iprebuilt/include \
    -I"${PREFIX}/include" \
    jni/jni_gl.c \
    -o "lib/$ABI/libnativegl.so" \
    -Lprebuilt/syslib -lGLESv2 \
    -llog

echo "==> 校验产物"
"${PREFIX}/bin/llvm-readelf" -d "lib/$ABI/libnativegl.so" | grep NEEDED
echo "    OK: lib/$ABI/libnativegl.so"
