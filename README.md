# 日志查看器 — 在 Termux(aarch64 手机) 上手动打包 Android OpenGL APK

一个**完全在安卓手机的 Termux 里**、**不使用 Android Studio / Gradle**，
手动交叉编译并打包出可安装 APK 的工程。

应用本体是一个**原生 OpenGL ES 2.0 绘制的日志查看器**:界面(工具栏、日志窗口、
行号、滚动条、搜索、帮助面板)全部由 native C 代码通过 GLES2 渲染,
Java 层只提供 `GLSurfaceView` 宿主与系统能力(文件选择、输入对话框、生命周期)。

- UI / 渲染:`GLSurfaceView`(Java) + `native` OpenGL ES2(`liblogview.so`)
- 文本渲染:自带位图字体(由 FreeType 预生成,见 `tools/genfont.c`)
- 目标 ABI:`arm64-v8a`
- 已验证运行环境:Android 16 (API 36) 设备
- `minSdk=24`,`targetSdk=34`

---

## 1. 功能

| 功能 | 说明 |
|------|------|
| **打开** | 调用系统文件选择器(SAF),读取任意日志文件(最多 50MB) |
| **刷新** | 重读同一文件并跳到末尾(tail),适合查看持续写入的日志 |
| **运行解析 / 停止** | 开始 / 停止解析并显示日志内容 |
| **搜索** | 弹出输入框输入关键字,按关键字过滤显示,并提示匹配行数 |
| **帮助** | 显示操作说明面板 |
| **行号** | 日志窗口左侧显示行号(空行做了重叠修复) |
| **滚动条** | 右边缘滚动条,支持点击跳转与拖动 |
| **自动换行** | 过长行自动换行显示 |

---

## 2. 架构与原理

```
        Java 侧 (宿主/系统能力)              native 侧 (liblogview.so)
  ┌──────────────────────────┐        ┌────────────────────────────────┐
  │ LogViewActivity          │        │ logview_ui.c                   │
  │  ├ GLSurfaceView         │        │  init()      建 shader/字体纹理 │
  │  │   └ LogViewGL.Renderer┼──JNI──▶│  setSize()   设视口             │
  │  │        (liblogview)   │        │  drawFrame() 绘制整个 UI        │
  │  ├ onTouch ──touchDown/Move/Up ──▶│  命中按钮/滚动/拖动             │
  │  ├ 文件选择器 ◀──requestOpenFile─┤  触摸与绘制都在 native          │
  │  ├ 搜索输入框 ◀──requestSearchFocus┤                              │
  │  └ 读取文件字节 ─setFileContent──▶│  解析/过滤/排版/显示            │
  └──────────────────────────┘        └────────────────────────────────┘
```

- **渲染与交互全在 native**:native 负责布局、命中测试、日志解析、文本排版。
- **Java 负责 native 做不到/不方便的事**:`GLSurfaceView` 管理 EGL context/
  surface/渲染线程/swap;通过 `ContentResolver` 读文件;弹系统对话框。
- native 需要弹窗/选文件时,调用 Java 静态方法(`requestOpenFile` 等),
  由 Java 切回主线程操作。

打包链(全部在手机上运行):

```
clang 编 native .so (liblogview.so)
     │
aapt2 编译/链接资源与清单 ──┐
javac 编译 Java            │
d8 生成 classes.dex        ├──▶ 组装 zip ──▶ zipalign ──▶ apksigner ──▶ 可安装 APK
native .so 放进 lib/<abi>/ ┘
```

---

## 3. 环境准备(一次性)

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

## 4. 构建 APK

```bash
bash build-native.sh   # (可选, build.sh 会自动调用) 编译 native .so
bash build.sh          # 一键打包出 out/apk/ndkgles-demo.apk
```

产物:

```
out/apk/ndkgles-demo.apk      # 已签名, 可安装
out/debug.keystore            # 自动生成的调试签名(口令: android)
```

安装:用文件管理器点开 APK,或从电脑 `adb install -r out/apk/ndkgles-demo.apk`。

---

## 5. 目录结构

```
ndkgl-apk-demo/
├── README.md
├── build.sh                       # 一键打包 APK
├── build-native.sh                # 编译 native .so
├── AndroidManifest.xml            # 启动项 = LogViewActivity
├── res/values/strings.xml         # app_name / logview_name
├── src/com/example/ndkgles/
│   ├── LogViewActivity.java       # 日志查看器宿主(触摸/文件/对话框)
│   └── LogViewGL.java             # native 桥接(liblogview)+ Renderer
├── jni/
│   ├── logview_ui.c               # 日志查看器 native 实现(UI/解析/排版)
│   ├── ui_strings.h               # 所有 UI 文案(改字后需重生成字体)
│   └── font_bitmap.h              # 预生成位图字形(由 genfont 产出)
├── prebuilt/
│   ├── include/                   # GLES2/GLES3/EGL/KHR 头文件(编译期需要)
│   └── syslib/libGLESv2.so        # 链接用 stub(仅占位, 不打包进 APK)
├── tools/
│   ├── genfont.c                  # 位图字体生成器(主机运行, 不进 APK)
│   ├── qrun                       # 用 qemu 运行 x86_64 build-tools
│   └── setup-android-toolchain.sh
└── out/                           # 构建产物(自动生成)
```

---

## 6. 修改 UI 文案 → 必须重新生成字体

UI 里所有中文/ASCII 字符都来自预生成的位图字体 `jni/font_bitmap.h`。
**只要在 `jni/ui_strings.h` 增删了文案,就必须重新运行字体生成器**,
否则新字符会缺字形(显示为方块)。生成步骤:

```bash
pkg install freetype            # 若未安装
clang -O2 -o /tmp/genfont tools/genfont.c \
      -I$PREFIX/include/freetype2 -L$PREFIX/lib -lfreetype
/tmp/genfont > jni/font_bitmap.h
bash build.sh
```

---

## 7. 关键坑与说明(重要)

### 7.1 aapt2 在 arm64 上跑不了 → qemu 模拟
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

### 7.2 native 库必须链接 `libGLESv2`
这是本项目**曾经闪退的根因**。native 代码调用了 `glCreateProgram`、
`glGetUniformLocation` 等函数,若链接时不声明对 `libGLESv2.so` 的依赖,
生成的 so 里这些符号无人提供,运行期 `System.loadLibrary` 会报:

```
dlopen failed: cannot locate symbol 'glGetUniformLocation' referenced by ...liblogview.so
```

修复:链接时加 `-lGLESv2`(配合 `prebuilt/syslib/libGLESv2.so` 这个仅占位的
链接 stub)。这样 so 会记录:

```
DT_NEEDED  libGLESv2.so   ← 运行期由系统真实库满足
```

`prebuilt/syslib/libGLESv2.so` **不打包进 APK**,仅用于链接。

### 7.3 头文件来源
`prebuilt/include/` 里的 GLES/EGL 头来自 Khronos 官方仓库
(OpenGL-Registry / EGL-Registry)。Termux 的精简 NDK sysroot 不含这些头,
故随工程内置。

### 7.4 GLSurfaceView 的生命周期
宿主 Activity 必须转发 `onResume()` / `onPause()` 给 `GLSurfaceView`,
否则切后台再回来可能不渲染。

### 7.5 覆盖安装不生效?
若改了内容但 `versionCode` 没变,系统可能不覆盖。每次更新请自行递增
`AndroidManifest.xml` 里的 `android:versionCode`。

### 7.6 签名与 targetSdk
使用 `apksigner`(v2/v3 签名),满足 Android 7+ 要求;`targetSdk=34`,
`minSdk=24`。如需在旧设备运行可下调 `MIN_SDK`。

---

## 8. 自定义

- **改 UI 文案**:编辑 `jni/ui_strings.h`,然后按第 6 节重新生成字体。
- **改 UI 布局/绘制**:编辑 `jni/logview_ui.c`(`drawFrame` 负责整体绘制)。
- **加其他 ABI**(如 `armeabi-v7a`):
  1. 在 `build-native.sh` 里把 `-target aarch64-linux-android` 换成
     `-target armv7-linux-androideabi -march=armv7-a -mfpu=neon`
  2. 产物放到 `lib/armeabi-v7a/`
  3. `build.sh` 会把该目录一并打包,系统按设备 ABI 选择。
- **换包名**:同步修改 `AndroidManifest.xml` 的 `package`、
  `src/.../` 目录,以及所有 `jni/*.c` 里的 JNI 函数名
  (`Java_<包名下划线化>_<类名>_*`)。

---

## 9. 常见问题排查

| 现象 | 排查方向 |
|------|---------|
| 安装失败 | 签名是否有效 `apksigner verify out/apk/ndkgles-demo.apk` |
| 打开闪退, 日志有 `UnsatisfiedLinkError ... cannot locate symbol` | 见 7.2, 确认链接带了 `-lGLESv2` |
| 新加的中文显示成方块 | 忘了重新生成 `jni/font_bitmap.h`, 见第 6 节 |
| 黑屏但无崩溃 | 确认用的是 `GLSurfaceView` 路线;检查 `onResume/onPause` |
| `aapt2: Syntax error` / 无法执行 | aapt2 是 x86_64, 必须经 `tools/qrun`(qemu) 运行 |

抓日志(需设备可读 logcat):

```bash
logcat -d | grep -E "NDKGL|libEGL|AndroidRuntime"
```
