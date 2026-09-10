#!/bin/bash
# ============================================================
#  setup-android-toolchain.sh —— 在 Termux(aarch64) 一键准备 APK 打包工具链
#
#  做的事:
#    1. 安装 termux 软件包: openjdk-17 clang zip qemu-user-x86-64
#    2. 下载 Google build-tools r34 (含 aapt2/zipalign/d8/apksigner)
#    3. 下载 Android platform-34 (android.jar)
#    4. 构造仅含 glibc 的 x86_64 根 (~/x64root), 供 qemu 运行 x86_64 工具
#
#  之后即可用 build.sh 打包。需联网(能访问 dl.google.com / deb.debian.org)。
# ============================================================
set -e
SDK="$HOME/android-sdk"
X64ROOT="$HOME/x64root"
BTVER="android-14"      # build-tools r34 解压后的目录名
mkdir -p "$SDK" "$X64ROOT"

echo "=== [1/4] 安装 termux 软件包 ==="
pkg install -y openjdk-17 clang zip qemu-user-x86-64 wget 2>/dev/null || \
  apt-get install -y openjdk-17 clang zip qemu-user-x86-64 wget

echo "=== [2/4] 下载 build-tools r34 ==="
if [ ! -d "$SDK/$BTVER" ]; then
  ( cd "$SDK" && \
    curl -L -o bt.zip "https://dl.google.com/android/repository/build-tools_r34-linux.zip" && \
    unzip -q bt.zip && rm -f bt.zip )
fi
ls "$SDK/$BTVER/aapt2" >/dev/null && echo "  build-tools OK"

echo "=== [3/4] 下载 platform-34 (android.jar) ==="
if [ ! -f "$SDK/platforms/android-34/android.jar" ]; then
  mkdir -p "$SDK/platforms"
  ( cd "$SDK" && \
    curl -L -o pf.zip "https://dl.google.com/android/repository/platform-34-ext7_r02.zip" && \
    unzip -q pf.zip && rm -f pf.zip && mv android-34 platforms/ )
fi
ls "$SDK/platforms/android-34/android.jar" >/dev/null && echo "  android.jar OK"

echo "=== [4/4] 构造 x86_64 glibc 根 (~/x64root) ==="
MIR="https://deb.debian.org/debian"
IDX="$SDK/Packages.xz"
curl -sL "$MIR/dists/bookworm/main/binary-amd64/Packages.xz" -o "$IDX"
get_deb() {
  local name="$1"
  local fn
  fn="$(xz -dc "$IDX" | awk -v p="$name" '$0=="Package: "p{w=1} w && /^Filename: /{print $2; exit}')"
  [ -z "$fn" ] && { echo "  !! 未找到 $name"; return 1; }
  ( cd "$SDK" && curl -sL "$MIR/$fn" -o "pkg.deb" && dpkg-deb -x pkg.deb "$X64ROOT/" && rm -f pkg.deb )
  echo "  + $name"
}
for p in libc6 libgcc-s1 libstdc++6 zlib1g; do get_deb "$p"; done
rm -f "$IDX"
# 修正 /lib64 loader 相对链接
ln -sf ../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$X64ROOT/lib64/ld-linux-x86-64.so.2" 2>/dev/null || true

echo ""
echo "✅ 工具链就绪。测试:"
X64ROOT="$X64ROOT" bash "$(dirname "$0")/qrun" aapt2 version
