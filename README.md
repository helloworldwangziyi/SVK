# estarx_vulkan_minimal

从 `estarx_vulkan` 提取的移动端种子工程：Android、鸿蒙和 iOS 宿主只提供
surface 与帧节奏，具体 App 业务全部由平台无关的 C++ 代码实现。

## 项目结构与依赖边界

```
core/                              # C++ SDK：公共接口、绘制与渲染
├─ src/app.h                       # IApp：平台壳与 App 之间的通用接口
├─ src/render_platform.h           # IPlatform：Vulkan surface 平台抽象
├─ src/raster_surface.h            # IRasterSurface：渲染接口
├─ src/vulkan_renderer.*
├─ src/gpu/
└─ src/ui/

platform/                          # 各端平台适配源码（不含工程文件）
├─ android/
│  ├─ cpp/                         # JNI、Vulkan 平台适配
│  └─ java/                        # NativeBridge、VulkanSurfaceView
├─ hongmeng/
│  └─ cpp/                         # NAPI + HongmengPlatform
└─ ios/
   ├─ ios_shell_bridge.*           # C 桥 + CAMetal + IosPlatform
   └─ ios_vulkan_platform.mm

samples/                           # 每个子目录是一个完整 App（业务 + 三端工程）
└─ triangle/
   ├─ app.cpp                      # createApp：装配 TriangleApp
   ├─ triangle.h                   # C++ 三角形业务
   ├─ app.json                     # App 元数据清单（仅说明用，不驱动构建）
   ├─ android/                     # Android Gradle 工程
   ├─ ios/                         # iOS Xcode 工程
   └─ hongmeng/                    # 鸿蒙 Hvigor 工程
```

依赖方向保持为 **App → core ← platform**。平台桥接层只调用 `evk::IApp`，
不得包含具体 App 头文件。各端壳中的 Java、ArkTS、Objective-C 仅承担系统要求的
壳装配，不写 App 业务。项目只面向 Android、鸿蒙和 iOS。

目录职责和各端工程的组织方式，详见
[docs/project-structure.md](docs/project-structure.md)。

每个 App 自带一套三端原生工程，直接入库、直接维护。新增 App 时复制
`samples/triangle/` 整套结构，替换 C++ 业务，并在三端工程各自修改名称、
包名和版本号。工程按相对路径引用 `core/`、`platform/` 和 App 源码，
移动仓库位置不影响构建。

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

各端工程就是常规原生工程，直接用对应 IDE 或命令行构建，没有生成步骤。

### Android

用 Android Studio 打开 `samples/triangle/android/`，或命令行：

```bash
cd samples/triangle/android
export JAVA_HOME="$(/usr/libexec/java_home -v 17)"   # macOS 选择 JDK 17
./gradlew assembleDebug
```

需要 Android SDK、NDK 和 JDK 17+。SDK 不能自动发现时，在
`samples/triangle/android/local.properties` 配置本机 `sdk.dir`（该文件不入库）。

APK 输出到：

```text
samples/triangle/android/app/build/outputs/apk/debug/app-debug.apk
```

### iOS

首次构建前拉取 MoltenVK，并确保本机有 CMake 与 glslc：

```bash
./third_party/moltenvk/fetch.sh
brew install cmake shaderc
```

用 Xcode 打开 `samples/triangle/ios/EVKApp.xcodeproj`，或命令行做无签名
编译验证：

```bash
xcodebuild \
  -project samples/triangle/ios/EVKApp.xcodeproj \
  -scheme EVKApp -configuration Debug \
  -sdk iphoneos -destination generic/platform=iOS \
  CODE_SIGNING_ALLOWED=NO build
```

工程按相对路径（`$(SRCROOT)/../../..`）引用 `core/`、`platform/ios` 和
App 源码，不复制这些源码。真机运行请在 Xcode 中选择 Team。

### 鸿蒙

用 DevEco Studio 打开 `samples/triangle/hongmeng/`，在
`File > Project Structure > Signing Configs` 配置签名后构建运行。

命令行也可以打出未签名 HAP（使用 DevEco 自带的 hvigor、SDK 与 JDK）：

```bash
cd samples/triangle/hongmeng
export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
export JAVA_HOME=/Applications/DevEco-Studio.app/Contents/jbr/Contents/Home
/Applications/DevEco-Studio.app/Contents/tools/hvigor/bin/hvigorw assembleHap --no-daemon
```

未签名 HAP 输出到：

```text
samples/triangle/hongmeng/entry/build/default/outputs/default/
```

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
