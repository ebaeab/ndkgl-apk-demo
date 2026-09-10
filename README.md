# NDK GLES Demo — 在 Termux(aarch64 手机) 上手动打包 Android OpenGL APK

一个**完全在安卓手机上的 Termux 里**、不使用 Android Studio / Gradle，
手动交叉编译并打包出可安装 **OpenGL ES 2.0** 应用 APK 的最小工程。

- 界面:`GLSurfaceView`(Java) + `native` OpenGL ES2 渲染(旋转的彩色三角形)
- 目标 ABI:`arm64-v8a`
- 已验证运行环境:Android 16 (API 36) 设备

---

## 1. 效果与原理

```
        Java 侧                     native 侧 (C)
  ┌──────────────────┐        ┌──────────────────────────┐
  │ MainActivity     │        │ jni_gl.c                 │
  │  └ GLSurfaceView │        │  initGL()   建 shader     │
  │     └ Renderer ──┼──JNI──▶│  setSize()  设视口        │
  │       (NativeGL) │        │  drawFrame() 画三角形     │
  └──────────────────┘        └──────────────────────────┘
```

**EGL context / surface / 渲染线程 / eglSwapBuffers 全部由框架的 `GLSurfaceView` 负责**,
native 只做最核心的 GL 绘制。这样避开了手写 EGL 在各种设备上容易黑屏的问题。

打包链(全部在手机上运行):

```
clang 编 native .so
     │
aapt2 编译/链接资源与清单 ──┐
javac 编译 Java            │
d8 生成 classes.dex        ├──▶ 组装 zip ──▶ zipalign ──▶ apksigner ──▶ 可安装 APK
native .so 放进 lib/<abi>/ ┘
```

---

## 2. 环境准备(一次性)

**前提:已安装 Termux(aarch64)。** 运行自带脚本即可自动装好一切:

```bash
bash tools/setup-android-toolchain.sh
```

它会完成:

1. `pkg install openjdk-17 clang zip qemu-user-x86-64 wget`
2. 下载 Google **build-tools r34**(含 `aapt2 / zipalign / d8 / apksigner`)
   → `~/android-sdk/android-14/`
3. 下载 **platform-34** 的 `android.jar`
   → `~/android-sdk/platforms/android-34/android.jar`
4. 构造仅含 glibc 的 **x86_64 根目录** `~/x64root/`
   (用于让 qemu 运行 Google 的 x86_64 版工具)

> 需要能访问 `dl.google.com` 与 `deb.debian.org`。

---

## 3. 构建 APK

```bash
bash build-native.sh   # (可选, build.sh 会自动调用) 编译 libnativegl.so
bash build.sh          # 一键打包出 out/apk/ndkgles-demo.apk
```

产物:

```
out/apk/ndkgles-demo.apk      # 已签名, 可安装
out/debug.keystore            # 自动生成的调试签名(口令: android)
```

安装:用文件管理器点开 APK,或从电脑 `adb install -r out/apk/ndkgles-demo.apk`。

---

## 4. 目录结构

```
ndkgl-apk-demo/
├── README.md
├── build.sh                     # 一键打包 APK
├── build-native.sh              # 只编译 native .so
├── AndroidManifest.xml
├── res/values/strings.xml       # app_name
├── src/com/example/ndkgles/
│   ├── MainActivity.java        # GLSurfaceView 宿主(含 onResume/onPause)
│   └── NativeGL.java            # native 桥接 + Renderer
├── jni/
│   └── jni_gl.c                 # native GLES2 渲染实现
├── prebuilt/
│   ├── include/                 # GLES2/GLES3/EGL/KHR 头文件(编译期需要)
│   └── syslib/libGLESv2.so      # 链接用 stub(仅占位, 不打包进 APK)
├── tools/
│   ├── qrun                     # 用 qemu 运行 x86_64 build-tools
│   └── setup-android-toolchain.sh
└── out/                         # 构建产物(自动生成)
```

---

## 5. 关键坑与说明(重要)

### 5.1 aapt2 在 arm64 上跑不了 → qemu 模拟
Google 的 build-tools **只发布 x86_64 二进制**。`aapt2` / `zipalign` 在 arm64 手机上
直接执行会失败。方案:

- 装 `qemu-user-x86-64`
- 用一个最小的 x86_64 glibc 根 `~/x64root`(由 `setup-android-toolchain.sh` 构造)
- 通过 `tools/qrun` 运行:

  ```bash
  qemu-x86_64 -L ~/x64root -E LD_LIBRARY_PATH=/lib/x86_64-linux-gnu \
              ~/android-sdk/android-14/aapt2 <args>
  ```

> `javac` / `d8` / `apksigner` 是 Java 程序,有 JDK 即可原生运行,无需 qemu。

### 5.2 native 库必须链接 `libGLESv2`
这是本项目**曾经闪退的根因**。`libnativegl.so` 里调用了 `glCreateProgram`、
`glGetUniformLocation` 等函数,若链接时不声明对 `libGLESv2.so` 的依赖,
生成的 so 里这些符号无人提供,运行期 `System.loadLibrary` 会报:

```
dlopen failed: cannot locate symbol 'glGetUniformLocation' referenced by ...libnativegl.so
```

修复:链接时加 `-lGLESv2`(配合 `prebuilt/syslib/libGLESv2.so` 这个仅占位的
链接 stub)。这样 so 会记录:

```
DT_NEEDED  libGLESv2.so   ← 运行期由系统真实库满足
```

`prebuilt/syslib/libGLESv2.so` **不打包进 APK**,仅用于链接。

### 5.3 头文件来源
`prebuilt/include/` 里的 GLES/EGL 头来自 Khronos 官方仓库
(OpenGL-Registry / EGL-Registry)。Termux 的精简 NDK sysroot 不含这些头,
故随工程内置。

### 5.4 GLSurfaceView 的生命周期
`MainActivity` 必须转发 `onResume()` / `onPause()` 给 `GLSurfaceView`,
否则切后台再回来可能不渲染。

### 5.5 覆盖安装不生效?
若改了内容但 `versionCode` 没变,系统可能不覆盖。每次更新请自行递增
`AndroidManifest.xml` 里的 `android:versionCode`。

### 5.6 签名与 targetSdk
使用 `apksigner`(v2/v3 签名),满足 Android 7+ 要求;`targetSdk=34`。
`minSdk=24`。如需在旧设备运行可下调 `MIN_SDK`。

---

## 6. 自定义

- **改渲染内容**:编辑 `jni/jni_gl.c` 的着色器 `VS`/`FS` 与顶点数据 `TRI`。
- **加其他 ABI**(如 `armeabi-v7a`):
  1. 在 `build-native.sh` 里把 `-target aarch64-linux-android` 换成
     `-target armv7-linux-androideabi -march=armv7-a -mfpu=neon`
  2. 产物放到 `lib/armeabi-v7a/`
  3. `build.sh` 会把该目录一并打包,系统按设备 ABI 选择。
- **换包名**:同步修改 `AndroidManifest.xml` 的 `package`、
  `src/.../` 目录,以及 `jni_gl.c` 里的 JNI 函数名
  (`Java_<包名下划线化>_NativeGL_*`)。

---

## 7. 常见问题排查

| 现象 | 排查方向 |
|------|---------|
| 安装失败 | 签名是否有效 `apksigner verify out/apk/ndkgles-demo.apk` |
| 打开闪退, 日志有 `UnsatisfiedLinkError ... cannot locate symbol` | 见 5.2, 确认链接带了 `-lGLESv2` |
| 黑屏但无崩溃 | 确认用的是 `GLSurfaceView` 路线;检查 `onResume/onPause` |
| `aapt2: Syntax error` / 无法执行 | aapt2 是 x86_64, 必须经 `tools/qrun`(qemu) 运行 |
| `dlopen ... libGLESv2.so` 相关 | 见 5.2 |

抓日志(需设备可读 logcat):

```bash
logcat -d | grep -E "NDKGL|libEGL|AndroidRuntime"
```
