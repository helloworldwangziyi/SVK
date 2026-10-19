# 项目目录与 App 宿主机制

本文以 `triangle` 为例，说明 `samples/`、`platform/*/host/`、生成工程和最终产物之间的关系。

## 先回答两个常见问题

### `samples/android/` 是做什么的？

它是**旧项目结构遗留下来的目录，已在本次整理中清理，当前没有构建用途**。

旧结构曾把 Android 工程放在 `samples/android/`。调整为“共享宿主 + App 配置”后，Android 工程改由生成器创建到 `build/apps/<app>/android/`。清理前，该目录只剩 Android Studio 写入的 `.idea/` 配置，不是 Gradle 工程，也不会被当前构建流程使用。

因此：

- 不要在 Android Studio 中打开或重新建立 `samples/android/`。
- 不要在这里新增 Android 源码或 Gradle 配置。
- 如果 IDE 的“最近项目”仍显示这个旧路径，应移除该历史记录。

### `host/` 是做什么的？

`host` 意为“宿主”。`platform/<platform>/host/` 是某一平台的**共享原生工程模板**，负责提供系统要求的应用入口和窗口环境，但不实现具体 App 业务。

三个宿主模板分别是：

| 目录 | 模板类型 | 主要职责 |
|---|---|---|
| `platform/android/host/` | Android Gradle 工程 | Activity、SurfaceView 布局、Manifest、JNI 库装配 |
| `platform/ios/host/` | iOS Xcode 工程 | AppDelegate、ViewController、Metal 视图、Info.plist |
| `platform/hongmeng/host/` | 鸿蒙 Hvigor 工程 | EntryAbility、页面、Native 接口和模块配置 |

每端只保留一份宿主模板，所有 App 共用，不再为每个 App 复制一套原生工程。

## 整体目录职责

```text
SVK/
├─ app                              # 统一的生成、构建和 IDE 打开入口
├─ cmake/generate_app.cmake         # App 工程生成器
│
├─ core/                            # 平台无关的 C++ SDK
│  └─ src/                          # IApp、渲染、布局与绘制接口
│
├─ platform/                        # 平台能力和共享原生宿主
│  ├─ android/
│  │  ├─ cpp/                       # Android JNI 与 Vulkan 平台适配
│  │  ├─ java/                      # 共享 NativeBridge、VulkanSurfaceView
│  │  └─ host/                      # 共享 Android 工程模板
│  ├─ ios/
│  │  ├─ ios_shell_bridge.*         # iOS 系统桥接与平台适配
│  │  └─ host/                      # 共享 iOS 工程模板
│  └─ hongmeng/
│     ├─ cpp/                       # 鸿蒙 NAPI 与 Vulkan 平台适配
│     └─ host/                      # 共享鸿蒙工程模板
│
├─ samples/                         # App 业务，每个子目录对应一个 App
│  └─ triangle/
│     ├─ app.json                   # App 元数据和源文件清单
│     ├─ app.cpp                    # 实现 evk::createApp()
│     └─ triangle.h                 # 三角形业务代码
│
├─ build/                           # 生成工程和编译中间文件，不入库
└─ dist/                            # 最终 APK、.app 等产物，不入库
```

核心边界是：

- `core/`：不知道具体 App，也不依赖 Android、iOS 或鸿蒙的界面 API。
- `platform/`：实现平台能力，但不包含某个 App 的业务。
- `samples/<app>/`：只包含 App 的 C++ 业务和声明式配置。
- `build/`：由上述内容派生，不作为手工维护的源码目录。

## `samples/<app>/` 为什么不再有安卓和 iOS 工程？

如果每个 App 都保存三份原生工程，新增一个 App 就要复制 Android、iOS、鸿蒙三套目录。修改一个公共 Activity、ViewController 或构建参数时，还要同步修改所有副本。

当前结构改为：

```text
一个 App 的 C++ 源码和 app.json
                +
对应平台的共享 host 模板
                +
core 与平台适配代码
                ↓
        生成该 App 的 IDE 工程
```

这样新增 App 只需要新增 `samples/<app>/`，不需要复制原生工程。公共宿主有问题时，也只修改对应的 `platform/*/host/` 模板。

## `app.json` 负责什么？

以 `samples/triangle/app.json` 为例：

| 字段 | 用途 |
|---|---|
| `name` | 工程名和构建产品名，例如 `Triangle` |
| `displayName` | 设备上显示的应用名称 |
| `identifier` | Android applicationId、iOS Bundle ID、鸿蒙 bundleName |
| `versionName` | 对外版本号 |
| `versionCode` | 整数构建版本号 |
| `sources` | 当前 App 的 `.cpp` 文件列表，路径相对于该 App 目录 |

App 还需要在源文件中实现 `evk::createApp()`，返回自己的 `evk::IApp` 实现。平台宿主只依赖这个公共接口，不包含 `triangle.h` 之类的具体 App 头文件。

## 生成器和 `build/` 的关系

执行：

```bash
./app generate triangle android
```

生成器会读取 `samples/triangle/app.json`，然后：

1. 将 `platform/android/host/` 复制到 `build/apps/triangle/android/`。
2. 替换模板中的名称、包名、版本号等占位符。
3. 写入当前 App 的 C++ 源文件列表和共享源码路径。
4. 排除 `.idea`、`.gradle`、旧 `build/` 等本机缓存。

iOS 和鸿蒙遵循相同规则，只是模板和生成路径不同。

生成工程会引用仓库中 `core/`、`platform/` 和 `samples/<app>/` 的实际源码路径。因此：

- 不要直接修改 `build/` 中的生成文件，重新生成后应以外层源码和模板为准。
- 不要只把生成工程复制到其他目录后继续开发，它并不是完整源码仓库。
- 移动或删除 `core/`、`platform/`、`samples/` 中的源码会破坏已有生成工程的引用。
- 修改宿主模板后，需要重新执行 `build`、`generate` 或 `open` 来更新生成工程。

`platform/android/host/` 中如果看到 `.idea/`、`.gradle/`、`build/` 或 `local.properties`，它们属于本机 IDE、构建缓存或 SDK 配置，不是需要手工维护的跨 App 业务代码。

## 宿主运行时如何调用 App？

以 Android 为例，调用链是：

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

宿主主要负责：

- 创建系统要求的 Activity、ViewController 或 EntryAbility。
- 获取绘制 Surface，并转发尺寸和生命周期变化。
- 驱动帧回调。
- 将系统事件翻译为平台无关的 C++ 接口调用。

宿主不应负责：

- 三角形、按钮、页面跳转等具体 App 业务。
- 某个 App 的颜色、文案和页面数据。
- 在 Java、Objective-C 或 ArkTS 中分别复制一套业务逻辑。

## 应该在哪里修改代码？

| 需求 | 修改位置 |
|---|---|
| 新增一个 App | `samples/<app>/` |
| 修改 App 页面、绘制和业务逻辑 | `samples/<app>/` 中的 C++ 文件 |
| 修改 App 名称、包名或版本 | `samples/<app>/app.json` |
| 修改公共渲染、布局和绘制能力 | `core/` |
| 增加 Android 系统能力 | `platform/android/cpp/` 或 `java/` |
| 修改所有 Android App 共用的宿主装配 | `platform/android/host/` |
| 修改 iOS 或鸿蒙公共宿主 | 对应平台的 `host/` |
| 修复生成工程中的临时差异 | 优先修改模板或生成器，再重新生成 |

## 正确的构建和 IDE 打开方式

```bash
./app build triangle android       # 生成并构建 Android，收集 APK 到 dist/
./app build triangle ios           # 生成并构建 iOS，收集 .app 到 dist/
./app generate triangle hongmeng   # 只生成鸿蒙工程

./app open triangle android        # 用 Android Studio 打开生成工程
./app open triangle ios            # 用 Xcode 打开生成工程
./app open triangle hongmeng       # 用 DevEco Studio 打开生成工程
```

如果需要手动在 IDE 中选择目录，应选择：

| 平台 | 应打开的目录 |
|---|---|
| Android | `build/apps/triangle/android/` |
| iOS | `build/apps/triangle/ios/EVKApp.xcodeproj` |
| 鸿蒙 | `build/apps/triangle/hongmeng/` |

仓库根目录、`samples/` 和 `platform/*/host/` 都不是当前 App 的生成工程入口。日常开发优先使用 `./app open`，避免打开旧目录或模板目录。

## 推荐阅读顺序

如果希望顺着代码理解当前架构，可以按以下顺序阅读：

1. `samples/triangle/app.json`：了解 App 如何描述自己。
2. `samples/triangle/app.cpp` 和 `triangle.h`：了解业务如何实现 `IApp`。
3. `core/src/app.h`：了解 App 与宿主之间的公共协议。
4. `platform/android/host/app/src/main/java/`：了解 Android 宿主入口。
5. `platform/android/java/` 和 `platform/android/cpp/`：了解 Surface、JNI 与 C++ 的连接。
6. `cmake/generate_app.cmake`：了解共享模板如何变成某个 App 的工程。
7. 根目录 `app`：了解生成、构建、打开 IDE 和收集产物的完整流程。
