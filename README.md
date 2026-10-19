# estarx_vulkan_minimal

从 `estarx_vulkan` 提取的移动端种子工程：Android、鸿蒙和 iOS 宿主只提供
surface 与帧节奏，具体 App 业务全部由平台无关的 C++ 代码实现。

## 项目结构与依赖边界

```
app                                # 统一的 App 生成、构建和打开入口
cmake/generate_app.cmake           # 读取 app.json，生成各端工程
dist/                              # 最终 App 产物（构建后生成，不入库）

core/                              # C++ SDK：公共接口、绘制与渲染
├─ src/app.h                       # IApp：平台宿主与 App 之间的通用接口
├─ src/render_platform.h           # IPlatform：Vulkan surface 平台抽象
├─ src/raster_surface.h            # IRasterSurface：渲染接口
├─ src/vulkan_renderer.*
├─ src/gpu/
└─ src/ui/

platform/                          # 各端平台适配、系统能力与共享宿主
├─ android/
│  ├─ cpp/ + java/                 # JNI、Surface 和共享 Java 组件
│  └─ host/                        # 唯一的 Android Gradle 宿主模板
├─ hongmeng/
│  ├─ cpp/                         # NAPI + HongmengPlatform
│  └─ host/                        # 唯一的鸿蒙 Hvigor 宿主模板
└─ ios/
   ├─ ios_shell_bridge.*           # C 桥 + CAMetal + IosPlatform
   └─ host/                        # 唯一的 iOS Xcode 宿主模板

samples/                           # 每个子目录只描述一个独立 App
└─ triangle/
   ├─ app.json                     # 名称、包名、版本和 C++ 源文件列表
   ├─ app.cpp                      # createApp：装配 TriangleApp
   └─ triangle.h                   # C++ 三角形业务
```

依赖方向保持为 **App → core ← platform**。平台桥接层只调用 `evk::IApp`，
不得包含具体 App 头文件。共享宿主中的 Java、ArkTS、Objective-C 仅承担系统要求的
宿主装配，不写 App 业务。项目只面向 Android、鸿蒙和 iOS。

目录职责、旧 `samples/android/` 的来历以及宿主生成流程，详见
[docs/project-structure.md](docs/project-structure.md)。

新增 App 时只需建立 `samples/<app>/`，编写 `app.json` 和 C++ 源文件，并实现
`evk::createApp()`；不需要复制三端工程，也不需要修改平台层。`app.json` 中的
`identifier` 同时作为 Android applicationId、iOS Bundle ID 和鸿蒙 bundleName，
`sources` 支持列出多个 `.cpp`。各端工程生成到 `build/apps/<app>/<platform>/`，
属于可重建的派生产物，不应直接修改或提交。

## 核心架构

- **`evk::IApp`**（`core/src/app.h`）：App 公共接口；平台宿主在每帧调用
  `onFrame`，具体实现由各 App 的 `createApp()` 提供。
- **`evk::IPlatform`**（`core/src/render_platform.h`）：创建 `VkSurfaceKHR`、
  查询绘制区尺寸；MoltenVK（iOS）的额外要求也经可选钩子声明。
- **`evk::IRasterSurface`**（`core/src/raster_surface.h`）：渲染器接口，
  平台壳只依赖它，不感知 Vulkan 实现。
- **`evk::Renderer`**（`core/src/vulkan_renderer.*`）：帧编排
  acquire → 上传顶点 → 录制 → 提交 → present；含 surface 旋转补偿、
  swapchain 重建重试、保活挂起/恢复。
- **gpu/ 子模块**：`VulkanContext`（instance/设备）、`Swapchain`
  （交换链/MSAA/render pass/framebuffer）、`UiPipeline`（图形管线）、
  `TextureCache`（1x1 白纹理，纯色批次的采样对象）。
- **`ui::Canvas`**（`core/src/ui/canvas.*`）：每帧顶点流收集器
  （立即模式），`drawTriangle` / `drawRect`，clip 合批。

着色公式统一为 `顶点色 × 纹理`：纯色几何绑 1x1 白纹理，采样恒为 1，
GPU 顶点插值免费得到渐变。SPIR-V 在构建期由 `glslc` 编译并内嵌进二进制
（`cmake/embed_shaders.cmake`），运行时零文件读取。

C++ 日志会保留各端终端输出，并经有界异步队列写入应用私有日志文件，
兼顾问题复现与高频业务场景。引擎模块、资源所有权、帧流程、简化点和日志
获取方式，详见 [docs/engine-core.md](docs/engine-core.md)。

## 构建与打开 App

统一入口会根据 `samples/<app>/app.json` 重新生成对应工程：

```bash
./app build triangle android       # 生成并构建 Android APK
./app build triangle ios           # 生成并执行 iOS 无签名 Debug 构建
./app generate triangle hongmeng   # 只生成鸿蒙工程

./app open triangle android        # 生成并用 Android Studio 打开
./app open triangle ios            # 生成并用 Xcode 打开
./app open triangle hongmeng       # 生成并用 DevEco Studio 打开
```

`open` 找不到对应 IDE 时会打印生成路径。需要真机签名和运行时，使用 `open`
打开生成工程后按各端常规流程操作；直接修改生成目录会在下次生成时失去意义，
应改 `app.json`、App 源码或 `platform/*/host` 模板。

`build` 成功后会将可安装或可运行的最终产物收集到
`dist/<app>/<platform>/` 并打印完整路径；`build/` 只保存生成工程和中间产物。
两个目录都可删除并重新生成，不提交到版本库。

### Android

Android Studio 应通过 `./app open triangle android` 打开，或手动选择
`build/apps/triangle/android/`。仓库根目录和 `samples/` 都不是 Gradle 工程，
旧的 `samples/android/` 已不再使用。

需要 Android SDK、NDK 和 JDK 17+。macOS 入口会通过 `/usr/libexec/java_home`
选择 JDK 17，其他环境使用当前 `JAVA_HOME`。若 SDK 不能自动发现，可在
`platform/android/host/local.properties` 配置本机 `sdk.dir`（该文件不入库）。

APK 输出到：

```text
dist/triangle/android/triangle-debug.apk
```

### iOS

首次构建前拉取 MoltenVK，并确保本机有 CMake 与 glslc：

```bash
./third_party/moltenvk/fetch.sh
brew install cmake shaderc
```

iOS 生成工程引用 `core/src`、`platform/ios` 和当前 App 的绝对源路径，
不复制这些源码。`build` 使用真机 arm64 目标并关闭签名，仅用于编译验证；
真机运行请用 `open` 打开 Xcode 后选择 Team。无签名 `.app` 输出到：

```text
dist/triangle/ios/Triangle.app
```

### 鸿蒙

`generate` 或 `open` 会生成完整 Hvigor 工程到
`build/apps/triangle/hongmeng/`。在 DevEco Studio 的
`File > Project Structure > Signing Configs` 配置签名后构建运行。
当前统一入口不替代 DevEco 的鸿蒙构建流程，也不自动收集其构建产物。

## 与母项目的对应关系

| 母项目（完整版） | 本项目（种子版） |
|---|---|
| `Compositor` + Raster 线程 + `frame_scheduler` | 平台壳 UI 线程直接驱动每帧 |
| `ui::Canvas` 全量图元（path/圆/文字/SDF/模糊） | 仅 `drawTriangle` / `drawRect` |
| `TextureCache` + `ITextureSource` 纹理体系 | 仅 1x1 白纹理 |
| `UiPipeline` 主/SDF 两条管线 | 仅主管线（64B mvp push constant） |
| `OffscreenEffects` 背景模糊 | 无（直通渲染） |
| `app_lifecycle` 事件总线 / 平台通道 | 无（生命周期直连渲染器） |
| Android：无障碍/文本输入/安全区/原生视图 | 仅 surface 生命周期 + 帧驱动 |

## 线程模型

所有端的壳回调（surface 生命周期、VSync 帧回调）都在 UI 线程触发，
引擎全部方法也在该线程调用——无锁单线程假设。母项目后续的 Raster 线程
是把这套调用挪到专用线程，引擎代码不需要改动。
