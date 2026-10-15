# estarx_vulkan_minimal

从 `estarx_vulkan` 提取的**最原始状态**：各端（Android / 鸿蒙 / iOS / 桌面）只提供
surface 并驱动帧节奏，引擎在屏幕上绘制一个旋转的三色渐变三角形。

这是一个"种子"项目——保留了母项目的引擎架构与代码风格，去掉了 UI 框架层
（视图树/布局/文字/动画/平台通道/持久化/网络），只留：

```
引擎核心（core/）            平台薄壳（platform/）          示例内容（samples/）
├─ render_platform.h         ├─ android/  JNI + Surface    ├─ triangle.h（共享）
├─ raster_surface.h          ├─ hongmeng/ NAPI + XComponent ├─ android/  Gradle 工程
├─ vulkan_renderer.*         ├─ ios/      C 桥 + CAMetal   ├─ hongmeng/ Hvigor 工程
├─ gpu/vulkan_context.*      └─ desktop/  Win32 验证壳      └─ ios/      App 源
├─ gpu/swapchain.*                                          （Xcode 工程在 Mac 上建）
├─ gpu/ui_pipeline.*
├─ gpu/texture_cache.*
└─ ui/canvas.*
```

## 架构（与母项目一致的分层）

- **`evk::IPlatform`**（`core/src/render_platform.h`）：平台抽象接口。
  各端实现它：创建 `VkSurfaceKHR`、查询绘制区尺寸。MoltenVK（iOS）的
  额外要求也经它的可选钩子声明。
- **`evk::IRasterSurface`**（`core/src/raster_surface.h`）：渲染器接口。
  平台壳只依赖它，不感知 Vulkan。
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

## 各端构建 / 运行

### 桌面（本机验证，Windows）

需要 CMake + MSVC + Vulkan SDK（提供 glslc 与 loader）：

```bash
cmake -B build/desktop -G "Visual Studio 17 2022" -A x64
cmake --build build/desktop --config Debug
./build/desktop/Debug/evk_triangle_desktop.exe
```

弹出一个 480×800 窗口：深蓝灰背景 + 旋转三色渐变三角形。

### Android（手机）

需要 Android SDK + NDK + JDK 17+。`samples/android/local.properties`
里的 `sdk.dir` 改成你的 SDK 路径，然后：

```bash
cd samples/android
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

Java 壳：`VulkanSurfaceView`（SurfaceView + Choreographer 帧驱动）；
Native 壳：`platform/android/cpp/`（JNI 桥 + AndroidPlatform）。

### 鸿蒙（手机/平板）

用 DevEco Studio 打开 `samples/hongmeng/`，
`File > Project Structure > Signing Configs` 自动签名后运行。
ArkTS 壳：`VulkanShell.ets`（XComponent SURFACE + DisplaySync 帧驱动）；
Native 壳：`platform/hongmeng/cpp/`（NAPI 桥 + HongmengPlatform）。

### iOS（手机）

在 Mac 上用 Xcode 新建 iOS App 工程（界面选 Storyboard 都不要，纯代码），
把以下文件加进去（Compile Sources / Copy 按类型自动识别）：

- 引擎：`core/src/**`（6 个 .cpp）
- 平台：`platform/ios/ios_vulkan_platform.mm`、`ios_shell_bridge.mm`
- 示例：`samples/ios/EstarxTriangle/*`、`samples/triangle.h`
- Header Search Paths：`core/src`、项目根目录、`third_party/glm`、
  `third_party/spdlog/include`
- 链接 `libMoltenVK.a`（`third_party/moltenvk` 的 `fetch.sh` 下载，
  或从 MoltenVK 发行版取）+ `QuartzCore.framework`

壳：`EVKMetalView`（layerClass → CAMetalLayer）+ `CADisplayLink` 帧驱动。

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
