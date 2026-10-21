# 项目目录与各端工程

本文以 `triangle` 为例，说明 `core/`、`platform/`、`samples/` 的职责边界，
以及各端工程的组织方式。

## 整体目录职责

```text
SVK/
├─ core/                            # 平台无关的 C++ SDK
│  └─ src/                          # IApp、渲染、布局与绘制接口
│
├─ platform/                        # 平台适配源码（不含工程文件）
│  ├─ android/
│  │  ├─ cpp/                       # JNI 与 Vulkan 平台适配
│  │  └─ java/                      # NativeBridge、VulkanSurfaceView
│  ├─ ios/
│  │  ├─ ios_shell_bridge.*         # iOS 系统桥接
│  │  └─ ios_platform.mm            # 持有 CAMetalLayer*，供 Metal 后端
│  └─ hongmeng/
│     └─ cpp/                       # NAPI 与 Vulkan 平台适配
│
├─ samples/                         # 每个子目录是一个完整 App
│  └─ triangle/
│     ├─ app.cpp                    # 实现 svk::createApp()
│     ├─ triangle.h                 # 三角形业务代码
│     ├─ android/                   # Android Gradle 工程
│     ├─ ios/                       # iOS Xcode 工程
│     └─ hongmeng/                  # 鸿蒙 Hvigor 工程
│
├─ test/                            # host 侧测试（经根 CMake 的 SVK_BUILD_TESTS 开关构建）
│  └─ test_log.cpp                  # log 模块功能与性能测试
│
└─ build/                           # 根 CMake 的本机构建目录，不入库
```

核心边界是：

- `core/`：不知道具体 App，也不依赖 Android、iOS 或鸿蒙的界面 API。
- `platform/`：实现平台能力，但不包含某个 App 的业务，也不保存工程文件。
- `samples/<app>/`：一个 App 的全部——C++ 业务和三端原生工程。

## 一个 App 一套各端工程

每个 App 的三端工程直接入库、直接维护，没有模板和生成步骤：

| 工程 | 位置 | 打开方式 |
|---|---|---|
| Android | `samples/<app>/android/` | Android Studio |
| iOS | `samples/<app>/ios/SVKApp.xcodeproj` | Xcode |
| 鸿蒙 | `samples/<app>/hongmeng/` | DevEco Studio |

工程不复制仓库中的共享源码，而是按**相对路径**引用：

- Android、鸿蒙的 CMakeLists 以自身位置推导仓库根目录
  （`get_filename_component(SVK_ROOT ...)`），再引用 `core/`、
  `platform/<平台>/cpp` 和 App 源文件。
- iOS 工程以 `$(SRCROOT)/../../..` 推导仓库根目录，引用 `core/`、
  `platform/ios/` 和 App 源文件。
- Android `app/build.gradle` 以 `../../../../platform/android/java`
  引用共享 Java 组件。

因此：

- 移动整个仓库不影响构建；但**单独移动** `core/`、`platform/`、
  `samples/<app>/` 中的源码会破坏相对引用。
- `local.properties`、`.idea/`、`.gradle/`、`build/`、`.cxx/`、
  `DerivedData/`、`.hvigor/`、`oh_modules/` 是本机缓存，不入库。

## App 与壳的分工

App 在 `app.cpp` 中实现 `svk::createApp()`，返回自己的 `svk::IApp` 实现。
各端壳代码（Activity、ViewController、EntryAbility + `platform/` 桥接）
只依赖这个公共接口，不包含 `triangle.h` 之类的具体 App 头文件。

以 Android 为例，运行时调用链：

```text
MainActivity
    ↓ 创建界面
VulkanSurfaceView
    ↓ Surface 生命周期、尺寸和 VSync
NativeBridge
    ↓ JNI
android_jni_bridge.cpp
    ↓ 调用公共 C++ 接口
IApp.onFrame(...) + Renderer
    ↓
当前 App 的 C++ 业务与绘制内容
```

壳主要负责：

- 创建系统要求的 Activity、ViewController 或 EntryAbility。
- 获取绘制 Surface，并转发尺寸和生命周期变化。
- 驱动帧回调。
- 将系统事件翻译为平台无关的 C++ 接口调用。

壳不应负责具体 App 业务（三角形、按钮、页面跳转等），也不应在
Java、Objective-C 或 ArkTS 中分别复制一套业务逻辑。

## 新增一个 App

复制 `samples/triangle/` 为 `samples/<app>/`，然后：

1. 替换 `app.cpp`、`triangle.h` 为自己的业务，实现 `svk::createApp()`。
2. 在三端工程中分别修改应用名、包名和版本号：
   - Android：`android/app/build.gradle` 的 `applicationId`、
     `versionName`、`versionCode`、`resValue 'app_name'`。
   - iOS：project.pbxproj 中 target 的 `PRODUCT_BUNDLE_IDENTIFIER`、
     `PRODUCT_NAME`、`MARKETING_VERSION`、`CURRENT_PROJECT_VERSION`，
     以及 `SVKApp/Info.plist` 的显示名。
   - 鸿蒙：`AppScope/app.json5` 的 `bundleName`、`versionCode`、
     `versionName`，以及两处 `string.json` 的显示名。
3. 如果重命名了 App 源文件，同步修改两个 CMakeLists 的 `SVK_APP_SOURCES`
   和 iOS 工程的 App 分组。

## 应该在哪里修改代码？

| 需求 | 修改位置 |
|---|---|
| 修改 App 页面、绘制和业务逻辑 | `samples/<app>/` 中的 C++ 文件 |
| 修改某端应用名、包名或版本 | 该端工程自身的配置文件 |
| 修改公共渲染、布局和绘制能力 | `core/` |
| 增加 Android 系统能力 | `platform/android/cpp/` 或 `java/` |
| 修改 Triangle 的 Android 壳 | `samples/triangle/android/` |
| 修改 Triangle 的 iOS 或鸿蒙壳 | `samples/triangle/ios/` 或 `hongmeng/` |
| 新增 host 侧测试 | `test/`，经 `SVK_BUILD_TESTS` 开关接入根 CMake |

## 构建方式

```bash
# Android（需要 JDK 17+、Android SDK 与 NDK）
cd samples/triangle/android && ./gradlew assembleDebug

# iOS（Metal 着色器以内嵌 MSL 源在运行时编译，构建无需额外工具）
xcodebuild -project samples/triangle/ios/SVKApp.xcodeproj \
  -scheme SVKApp -configuration Debug \
  -sdk iphoneos -destination generic/platform=iOS \
  CODE_SIGNING_ALLOWED=NO build

# 鸿蒙（需要 DevEco Studio 及其 SDK；命令行产出未签名 HAP）
cd samples/triangle/hongmeng
export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
export JAVA_HOME=/Applications/DevEco-Studio.app/Contents/jbr/Contents/Home
/Applications/DevEco-Studio.app/Contents/tools/hvigor/bin/hvigorw assembleHap --no-daemon
# 真机运行：用 DevEco Studio 打开工程，配置签名后构建
```

仓库根目录的 `CMakeLists.txt` 只用于在桌面端构建 `svk_core` 静态库和
`test/` 测试，不参与任何 App 的构建。
