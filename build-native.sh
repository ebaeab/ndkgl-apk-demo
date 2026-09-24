#!/bin/bash
# ============================================================
#  build-native.sh —— 交叉编译 日志查看器 native 库 (arm64-v8a)
#  产物: lib/arm64-v8a/liblogview.so
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

echo "==> 编译 liblogview.so ($ABI)"
"$CC" -target "$TARGET" -std=c99 -O2 -fPIC -shared \
    -Iprebuilt/include \
    -I"${PREFIX}/include" \
    jni/logview_ui.c \
    -o "lib/$ABI/liblogview.so" \
    -Lprebuilt/syslib -lGLESv2 \
    -llog

echo "==> 校验产物"
"${PREFIX}/bin/llvm-readelf" -d "lib/$ABI/liblogview.so" | grep NEEDED
echo "    OK: lib/$ABI/liblogview.so"
