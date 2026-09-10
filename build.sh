#!/bin/bash
# ============================================================
#  build.sh —— 在 Termux (aarch64) 手机上手动打包 Android APK
#              (GLSurfaceView + native OpenGL ES2, arm64-v8a)
#
#  流程: build-native.sh (clang) -> aapt2 -> javac -> d8 -> zip
#        -> zipalign -> apksigner
#
#  依赖(见 README.md "环境准备"): 
#    - 已安装 openjdk-17 / clang / zip / qemu-user-x86-64
#    - ~/android-sdk/android-14   (build-tools, 含 aapt2/zipalign/d8/apksigner)
#    - ~/android-sdk/platforms/android-34/android.jar  (平台库)
#    - ~/android-sdk/platforms/android-34/android.jar  (平台库)
#    - tools/qrun                 (用 qemu 跑 x86_64 版 aapt2/zipalign 的封装, 工程自带)
# ============================================================
set -e
P="$(cd "$(dirname "$0")" && pwd)"
cd "$P"

# ---------------- 工具路径(按需修改) ----------------
QRUN="$P/tools/qrun"                    # 用 qemu 跑 x86_64 版 aapt2/zipalign
AAPT="$QRUN aapt2"
ZIPALIGN="$QRUN zipalign"

# build-tools 目录(自动探测, 可用 BT_DIR 覆盖)
BTDIR="${BT_DIR:-}"
if [ -z "$BTDIR" ]; then
    for d in "$HOME/android-sdk/android-14" "$HOME/android-sdk/build-tools"/* "$HOME/android-sdk"/*build-tools*; do
        [ -x "$d/aapt2" ] && { BTDIR="$d"; break; }
    done
fi
BTDIR="${BTDIR:-$HOME/android-sdk/android-14}"

PLATFORM="$HOME/android-sdk/platforms/android-34/android.jar"
D8="java -cp $BTDIR/lib/d8.jar com.android.tools.r8.D8"
APKSIGNER="java -jar $BTDIR/lib/apksigner.jar"

MIN_SDK=24
TARGET_SDK=34
ABI=arm64-v8a
OUT=out
mkdir -p "$OUT/build" "$OUT/apk"

# ---------------- 0) native ----------------
[ -f "lib/$ABI/libnativegl.so" ] || bash build-native.sh

# ---------------- 1) aapt2 compile ----------------
echo "=== [1/7] aapt2 compile resources ==="
$AAPT compile --dir res -o "$OUT/build/res.zip"

# ---------------- 2) aapt2 link ----------------
echo "=== [2/7] aapt2 link ==="
$AAPT link \
    -o "$OUT/build/app.raw.apk" \
    -I "$PLATFORM" \
    --manifest AndroidManifest.xml \
    --min-sdk-version "$MIN_SDK" \
    --target-sdk-version "$TARGET_SDK" \
    $( [ -d assets ] && echo "-A assets" ) \
    "$OUT/build/res.zip"

# ---------------- 3) javac ----------------
echo "=== [3/7] javac ==="
rm -rf "$OUT/build/classes"; mkdir -p "$OUT/build/classes"
javac -classpath "$PLATFORM" -d "$OUT/build/classes" $(find src -name '*.java')

# ---------------- 4) d8 -> classes.dex ----------------
echo "=== [4/7] d8 ==="
(cd "$OUT/build" && $D8 --release --lib "$PLATFORM" --min-api "$MIN_SDK" \
    --output . $(find classes -name '*.class'))
ls -la "$OUT/build/classes.dex"

# ---------------- 5) 组装 APK (dex + native lib) ----------------
echo "=== [5/7] assemble APK ==="
APK="$OUT/apk/app-unsigned.apk"
STAGE=apkstage
rm -rf "$STAGE" "$APK"; mkdir -p "$STAGE/lib/$ABI"
cp "$OUT/build/classes.dex" "$STAGE/classes.dex"
cp "lib/$ABI/"*.so "$STAGE/lib/$ABI/"
cp -f "$OUT/build/app.raw.apk" "$APK"
(cd "$STAGE" && zip -q -X -r "$P/$APK" classes.dex lib)

# ---------------- 6) zipalign ----------------
echo "=== [6/7] zipalign ==="
$ZIPALIGN -f 4 "$APK" "$OUT/apk/app-aligned.apk"
mv -f "$OUT/apk/app-aligned.apk" "$APK"

# ---------------- 7) apksigner ----------------
echo "=== [7/7] apksigner ==="
KS="$OUT/debug.keystore"
[ -f "$KS" ] || keytool -genkeypair -v -keystore "$KS" -storepass android \
    -keypass android -alias debug -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android, OU=Demo, O=Demo, L=City, S=State, C=US" >/dev/null 2>&1
$APKSIGNER sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/apk/ndkgles-demo.apk" "$APK"
$APKSIGNER verify "$OUT/apk/ndkgles-demo.apk"

echo ""
echo "✅ 生成成功: $P/$OUT/apk/ndkgles-demo.apk"
ls -la "$OUT/apk/ndkgles-demo.apk"
