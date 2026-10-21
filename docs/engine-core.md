# 引擎层（core）代码汇总与审阅

代码快照：当前工作区（基于 `6b5bbbd`，含后续日志增强）。

本文汇总当前引擎层的实际代码，范围以 `core/` 为主，并包含与它直接相关的 `CMakeLists.txt`、`cmake/embed_shaders.cmake` 和 `shaders/`。平台目录只在说明接口边界和日志注入时涉及。

文末单独列出当前简化点、异步文件日志策略和后续完善方向。

## 1. 当前定位

当前引擎是一个 **C++17、Vulkan、立即模式 2D 几何渲染核心**，构建为静态库 `svk_core`。

它已经具备：

- App 与平台宿主之间的公共接口。
- Vulkan instance、surface、设备、队列和交换链创建。
- 双帧在飞的命令提交与同步。
- 三角形和矩形的 CPU 顶点收集、按裁剪区域合批。
- 顶点色 × 纹理的统一着色，以及 1×1 白纹理。
- 透明混合、动态 viewport/scissor、MSAA。
- 尺寸变化、旋转补偿、交换链重建与有限次重试。
- Surface 挂起和恢复。
- 基于 spdlog 的统一日志宏和平台 sink 注入。

它目前不是完整的 UI 引擎，尚不具备：

- 布局系统、组件树和样式系统。
- 文本、字体 atlas、path、圆角、图片和真实纹理缓存。
- 输入、屏幕变化、生命周期等通用 App 事件分发。
- 独立 Raster 线程、帧调度器和异步任务系统。
- 自动化测试体系。

## 2. 文件与模块清单

| 文件 | 当前职责 |
|---|---|
| `core/src/app.h` | `IApp`、`AppFrameContext`、`createApp()`，连接 App 业务与宿主 |
| `core/src/render_platform.h` | `IPlatform`，抽象原生 Surface 和平台所需 Vulkan 扩展 |
| `core/src/raster_surface.h` | `IRasterSurface`，隐藏 Vulkan 渲染器实现 |
| `core/src/vulkan_renderer.h/.cpp` | 初始化、资源编排、帧提交、重建、挂起恢复、顶点和同步设施 |
| `core/src/gpu/vulkan_context.h/.cpp` | VkInstance、VkSurfaceKHR、物理/逻辑设备、队列和调试回调 |
| `core/src/gpu/swapchain.h/.cpp` | Swapchain、ImageView、MSAA、RenderPass、Framebuffer 和尺寸状态 |
| `core/src/gpu/ui_pipeline.h/.cpp` | 唯一一条 UI GraphicsPipeline 及其 Layout |
| `core/src/gpu/texture_cache.h/.cpp` | 采样器、Descriptor 和 1×1 白纹理；目前不是通用纹理缓存 |
| `core/src/ui/canvas.h/.cpp` | CPU 侧顶点流和 Batch 收集，支持三角形、矩形 |
| `core/src/log.h` | spdlog 初始化、统一消息前缀和 `SVK_LOG*` 宏 |
| `shaders/ui_vertex.vert` | 像素坐标经 MVP 转到 NDC，传递颜色和 UV |
| `shaders/ui_fragment.frag` | 输出 `顶点色 × 纹理采样` |
| `CMakeLists.txt` | 定义 `svk_core`、依赖和 Shader 构建 |
| `cmake/embed_shaders.cmake` | GLSL → SPIR-V → C++ `uint32_t` 数组 |

## 3. 依赖方向与对象关系

```text
platform 宿主
  ├─ 持有并调用 svk::IApp
  ├─ 提供 svk::IPlatform 实现
  └─ 通过 IRasterSurface 持有 Renderer

App（samples/<app>/）
  └─ IApp.onFrame(...) 向 ui::Canvas 写入本帧几何

Renderer
  ├─ VulkanContext context_
  ├─ TextureCache textureCache_
  ├─ Swapchain swapchain_
  ├─ UiPipeline uiPipeline_
  ├─ CommandPool / CommandBuffers
  ├─ 双帧 Semaphore / Fence
  └─ 双帧动态 VertexBuffer
```

依赖原则：

- App 只调用 `core` 的公共 C++ 接口。
- 平台桥接层只依赖 `IApp`、`IPlatform`、`IRasterSurface` 和 `Canvas`。
- `Renderer` 不直接调用 Android、iOS 或鸿蒙 API。
- `IPlatform` 是由平台实现、被引擎非拥有引用的指针；恢复 Surface 时可以替换。
- GPU 子模块以值成员方式由 `Renderer` 持有，声明顺序保证 `VulkanContext` 最先构造。

## 4. 三个公共边界

### 4.1 `IApp`：App 业务边界

`core/src/app.h` 当前只有两个核心声明：

```cpp
struct AppFrameContext {
    uint32_t width;
    uint32_t height;
    double timeSeconds;
};

class IApp {
public:
    virtual void onFrame(ui::Canvas&, const AppFrameContext&) = 0;
};
```

每个 App 通过 `svk::createApp()` 返回自己的实现。当前接口只有帧回调，没有输入、生命周期、系统路由或屏幕变化事件。

### 4.2 `IPlatform`：原生平台边界

必须实现：

- `getSurfaceExtensionName()`：平台 Surface 扩展。
- `createVulkanSurface()`：从原生窗口创建 `VkSurfaceKHR`。
- `getSurfaceSize()`：返回像素尺寸。

可选钩子：

- `getInstanceCreateFlags()`。
- `getRequiredInstanceExtensions()`。
- `getRequiredDeviceExtensions()`。

这些可选钩子目前主要服务 iOS MoltenVK 的 portability 要求。

### 4.3 `IRasterSurface`：渲染后端边界

公开方法：

- `initialize()`。
- `setSize()`。
- `render()`。
- `suspendSurface()`。
- `resumeSurface()`。

`createRasterSurface()` 当前固定返回 Vulkan `Renderer`。`RenderResult` 的含义：

| 结果 | 当前含义 |
|---|---|
| `Rendered` | 本帧提交和 present 请求已接受，不等于物理屏幕已经显示 |
| `Retry` | 等待超时、图像暂不可用或重建重试耗尽，应稍后再试 |
| `Failed` | 初始化后的帧流程遇到不可继续处理的错误 |

## 5. 初始化、资源所有权与销毁

### 5.1 初始化顺序

`Renderer::initialize()` 严格按依赖关系执行：

1. `VulkanContext.initialize()`：instance → surface → 物理设备 → 逻辑设备。
2. `TextureCache.initialize()`：先建立 Descriptor 布局和白纹理。
3. `Swapchain.create()`：swapchain → image view → MSAA → render pass。
4. `UiPipeline.create()`：依赖 render pass、采样数和 Descriptor 布局。
5. `Swapchain.createFramebuffers()`。
6. 创建命令池、双帧顶点缓冲、命令缓冲和同步对象。
7. 清除初始化阶段多余的重建标记。

任何一步失败都返回 `false`，由平台桥接层销毁 Renderer 并清理平台对象。

### 5.2 资源归属

| 所有者 | 管理的资源 |
|---|---|
| `VulkanContext` | Instance、DebugMessenger、Surface、Device；队列句柄只读取不单独销毁 |
| `TextureCache` | Sampler、DescriptorPool/Layout、白纹理 Image/Memory/View；DescriptorSet 随池释放 |
| `Swapchain` | Swapchain、ImageViews、Framebuffers、RenderPass、MSAA Image/Memory/View |
| `UiPipeline` | Pipeline 和 PipelineLayout |
| `Renderer` | 上述模块，以及 CommandPool、同步对象和动态顶点缓冲 |
| `Canvas` | 纯 CPU 的 `vertices_` 和 `batches_`，不持有 GPU 资源 |

Swapchain 图像本体由 Swapchain 管理，代码销毁的是 ImageView，不会单独 `vkDestroyImage` 这些呈现图像。

### 5.3 销毁顺序

`Renderer::shutdown()`：

1. `vkDeviceWaitIdle()`，等待在飞工作结束。
2. 销毁 Fence、Semaphore 和顶点缓冲。
3. 销毁 CommandPool，连带释放 CommandBuffer。
4. 清理 TextureCache、UiPipeline、Swapchain。
5. `VulkanContext.shutdown()`：Device → DebugMessenger → Surface → Instance。
6. 清空句柄容器。

主要模块的清理函数都按幂等目标编写，析构函数会再次调用 `shutdown()` 作为兜底。

## 6. 每帧执行流程

### 6.1 关键常量

| 常量 | 当前值 | 用途 |
|---|---:|---|
| `kMaxFramesInFlight` | 2 | 两套命令、同步和顶点资源，限制 CPU 领先 GPU 的帧数 |
| `kMaxFrameAttempts` | 3 | 单次 `render()` 内最多重建/重画三次 |
| `waitNanos` | 16,000,000 ns | Fence 和 Acquire 的单次等待上限，约 16 ms |
| `kInitialVertexCapacity` | 8192 顶点 | 每个帧槽的初始顶点容量，不足时倍增 |
| `sizeof(UiVertex)` | 32 字节 | 位置 8 + 颜色 16 + UV 8 |

两个初始顶点缓冲各占 256 KiB，不含后续扩容。

### 6.2 `render()` 主流程

每次调用最多执行三轮，以处理 Surface 正在变化的情况：

1. 检查并消费 Swapchain 重建标记，必要时先等待设备空闲并重建。
2. 比较平台请求的视觉尺寸与 Swapchain 实际尺寸，不匹配则继续重建。
3. 等待当前帧槽 Fence；超时返回 `Retry`。
4. Acquire 下一张 Swapchain 图像。
   - `TIMEOUT/NOT_READY`：返回 `Retry`。
   - `OUT_OF_DATE`：立即重建并重试。
   - `SUBOPTIMAL`：先继续本帧，present 后再重建。
5. 将 Canvas 顶点上传到当前帧槽的 HOST_VISIBLE、HOST_COHERENT 缓冲。
6. Reset Fence 和 CommandBuffer，录制整帧命令。
7. 提交图形队列，并设置 Acquire/Render/Present 之间的 Semaphore。
8. Present；遇到 `OUT_OF_DATE/SUBOPTIMAL` 则标记重建并用同一个 Canvas 重试。
9. 成功后轮换帧槽 `0 → 1 → 0`。

Fence 只在确定会提交前 Reset，避免提前 Reset 后中途返回导致后续永久等待。

### 6.3 CommandBuffer 录制内容

`recordCommandBuffer()` 依次完成：

1. 开始 CommandBuffer。
2. 在 RenderPass 外执行白纹理的首次布局转换和清除上传。
3. 根据 Swapchain 尺寸和 SurfaceTransform 计算视觉宽高。
4. 创建左上角原点、Y 轴向下的正交投影，并补偿 90/180/270 度呈现旋转。
5. 以 `(0.06, 0.06, 0.09, 1.0)` 清屏并启动 RenderPass。
6. 绑定 UI Pipeline，设置动态 Viewport、顶点缓冲和 64 字节 MVP PushConstant。
7. 逐 Batch 求裁剪交集、旋转裁剪区域、设置 Scissor、绑定白纹理并 Draw。
8. 结束 RenderPass 和 CommandBuffer。

空 Canvas 也会形成清屏帧，只是没有顶点上传和 Draw。

## 7. 各子模块的实际行为

### 7.1 `ui::Canvas`

数据结构：

- `Color`：0～1 的 RGBA 浮点分量。
- `Rect`：像素坐标矩形，左上角原点。
- `UiVertex`：位置、颜色、UV 的交错布局。
- `Batch`：共享同一个 Clip 的连续顶点区间。

当前图元：

- `drawTriangle()`：3 个顶点，可为每个顶点指定不同颜色。
- `drawRect()`：2 个三角形、6 个顶点，没有索引缓冲。

合批规则只有一条：**相邻绘制且 Clip 四个浮点值完全相等**。Clip 改变就新建 Batch；不相邻的相同 Clip 不会重新合并。Batch 目前不记录纹理键，因为渲染端始终绑定白纹理。

### 7.2 `VulkanContext`

- 创建 Instance 时加入通用 Surface 扩展和 `IPlatform` 提供的平台扩展。
- 可选启用 `VK_EXT_debug_utils`。
- 只有定义 `SVK_ENABLE_VALIDATION` 且系统存在 `VK_LAYER_KHRONOS_validation` 时才启用校验层。
- 校验回调只订阅 Warning/Error，并转发到 `SVK_LOG*`。
- 选择第一个同时满足 Graphics、Present 和 Swapchain 扩展的物理设备。
- Graphics/Present 队列族相同时只创建一个队列，不同则分别创建。
- 提供显存类型查找和 SPIR-V ShaderModule 创建。

`findMemoryType()` 当前找不到匹配类型时返回 `0`，这是明确保留的教学版简化，不是可靠的错误处理。

### 7.3 `Swapchain`

- 优先选择 `B8G8R8A8_UNORM + SRGB_NONLINEAR`，否则使用列表第一项。
- 优先 Mailbox PresentMode，否则使用 FIFO。
- 图像数量通常为 `minImageCount + 1`，并受 `maxImageCount` 限制。
- Graphics/Present 队列族不同则使用 `CONCURRENT` 图像共享，否则 `EXCLUSIVE`。
- 默认不透明；构造参数可请求透明 Surface，但当前 Renderer 使用默认不透明模式。
- 记录 `currentTransform`，供 Renderer 做投影和裁剪旋转补偿。
- `setSize()` 和 `requestRebuild()` 只记录状态，真正重建延迟到 `render()`。
- 重建前会 `vkDeviceWaitIdle()`，并将 Pipeline、RenderPass、Framebuffer 等一起重建。

MSAA 的实际选择顺序是 **2x → 4x → 1x**。启用后优先使用 `TRANSIENT + LAZILY_ALLOCATED` 的片上附件；不支持时退回普通 DeviceLocal 显存，创建失败再降级为单采样。

### 7.4 `UiPipeline`

唯一管线的主要配置：

- TriangleList，无索引、无剔除、无深度附件。
- SRC_ALPHA 源色混合和 ONE 源 Alpha 混合。
- 动态 Viewport 和 Scissor。
- Set 0 Binding 0：CombinedImageSampler，Fragment 阶段可见。
- PushConstant：64 字节 `mat4 mvp`，Vertex 阶段可见。

顶点输入：

| Location | 内容 | 格式 | Offset |
|---|---|---|---:|
| 0 | 像素位置 | `R32G32_SFLOAT` | 0 |
| 1 | RGBA 颜色 | `R32G32B32A32_SFLOAT` | 8 |
| 2 | UV | `R32G32_SFLOAT` | 24 |

ShaderModule 在 Pipeline 创建完成后立即销毁。

### 7.5 `TextureCache`

当前只创建：

- 一个线性过滤、ClampToEdge 的 Sampler。
- 一个 DescriptorSetLayout、一个 DescriptorPool 和一个 DescriptorSet。
- 一张 RGBA8、1×1 白纹理及其 ImageView。

白纹理首次使用时，通过 CommandBuffer 完成：

```text
UNDEFINED → TRANSFER_DST_OPTIMAL
          → vkCmdClearColorImage(白色)
          → SHADER_READ_ONLY_OPTIMAL
```

它不是通用纹理缓存，没有外部图片、脏上传、缓存键或淘汰策略。

## 8. Shader 与构建

根 `CMakeLists.txt` 将以下实现编入 `svk_core`：

- Renderer。
- 四个 GPU 模块。
- Canvas。

公开 Include 路径包含 `core/src`、GLM 和 spdlog；生成的 Shader 头目录为 Renderer 私有依赖。非 Android 构建通过 `find_package(Vulkan)` 链接 Vulkan，Android 使用 NDK 头和平台宿主提供的链接环境。

Shader 在 CMake 配置阶段处理：

1. `glslc` 编译 Vertex 和 Fragment GLSL。
2. `embed_shaders.cmake` 将 SPIR-V 按小端字节序转成 `uint32_t` 数组。
3. 生成 `assets/ui_shaders.h`。
4. 删除临时 `.spv` 文件。

运行时只读取内嵌数组，不读取 Shader 源文件或 `.spv` 文件。

## 9. 生命周期与线程模型

当前契约是：**同一个 Renderer 的所有公共方法在同一个线程顺序调用，无内部锁。** 当前三端宿主都直接由 UI/主线程驱动；未来如果迁移到 Raster 线程，需要由上层统一迁移整条调用链。

Surface 挂起时：

- 等待 Device 空闲。
- 销毁 UiPipeline、Swapchain 相关资源和 VkSurfaceKHR。
- 保留 Instance、Device、TextureCache、CommandPool、顶点缓冲和同步对象。

Surface 恢复时：

- 替换 `IPlatform` 指针。
- 重建 Surface、Swapchain、UiPipeline 和 Framebuffer。
- 不重建设备级和执行级资源。

尺寸重建、挂起和销毁都使用同步 `vkDeviceWaitIdle()`，实现简单，但会造成调用线程停顿。

部分注释仍保留母项目的 Compositor、Raster 或按需渲染术语，与当前 UI 线程 + 持续帧回调的宿主模型不完全一致，后续应统一清理。

## 10. 当前日志实现

### 10.1 公共 API

`core/src/log.h` 提供：

- `svk::log::init(tag)`：创建或复用 stdout logger。
- `svk::log::init(logger)`：注入平台构造的终端 logger。
- `LogFileConfig`：配置文件目录、文件名前缀、终端/文件级别、自动 Flush 级别，以及异步队列大小、线程数和溢出策略。
- `svk::log::initWithFile(...)`：保留平台终端 sink，追加 `basic_file_sink_mt`，并默认放入异步 logger。
- `hasFileSink()`、`currentLogFilePath()`：查询文件日志状态和准确路径。
- `asyncQueuedCount()`、`asyncOverrunCount()`、`asyncDroppedCount()`：查询异步队列状态。
- `flush()`、`shutdown()`：生命周期收尾封装，不跨 JNI/NAPI 抛异常。
- `SVK_LOGV/D/I/W/E`：Trace、Debug、Info、Warn、Error 五级宏（引擎/业务通道）。
- `SVK_ALOGV/D/I/W/E`：审计通道宏（`svk::log::writeAudit`），登录、下单、成交等
  不可丢事件写入独立文件、同步直写，不经过异步队列，也不提供限速变体。
- `currentAuditLogFilePath()`：审计通道的独立日志文件路径。
- `SVK_LOG*_ONCE`：同一调用点只写一次；`SVK_LOG*_THROTTLE(feature, intervalMs, ...)`：
  同一调用点在时间窗内只放行第一条；`SVK_LOG*_THROTTLE_KEY(feature, key, intervalMs, ...)`：
  按 key（如合约代码）各自限速。限速压制的条数在下一次放行时以 `suppressed=N` 附在正文末尾。

消息正文仍统一为：

```text
[svk][<feature>][<event> key=value ...]
```

### 10.2 异步队列 + 多 sink 结构

```text
业务线程 / UI 线程
    ↓ SVK_LOG*
svk::log::write（格式化正文并复制 log_msg）
    ↓
有界异步队列（默认 8192 条）
    ↓
后台日志线程（默认 1 个）
    ├─ 平台终端 sink：Logcat / HiLog / stdout
    └─ 完整文件 sink：basic_file_sink_mt
```

队列满时默认使用 `overrun_oldest`：覆盖最旧消息，不阻塞行情等业务线程，并保留最新日志。每次 Flush 会检查溢出计数；发生覆盖时补写一条 `async_queue_overrun count=...`，便于发现日志生产长期超过消费能力。

终端 sink 使用简洁的 `%v`，只输出消息正文。文件 sink 使用完整格式：

```text
%Y-%m-%d %H:%M:%S.%e [%l] [thread %t] %v
```

文件中的 `%t` 仍表示调用 `SVK_LOG*` 的生产线程，而不是后台日志线程。

默认终端和文件级别均为 Debug，因此 Trace 仍不会输出；logger 级别取两者中更详细的一级，后续可以通过 `LogFileConfig` 单独打开某一侧。Info 及以上会在后台线程自动 Flush；Error 在入队后还会让调用线程等待一次 Flush，尽量保证关键错误先于 Crash 落盘。Surface 挂起和引擎关闭时的 `svk::log::flush()` 会等待队列排空。

异步线程池是进程级共享的，第一次 `initWithFile()` 中的队列大小和线程数生效；后续调用复用同一个线程池。

### 10.3 三端实际输出位置

| 环境 | 终端输出 | 文件目录 | 初始化方式 |
|---|---|---|---|
| Android | Logcat | `getFilesDir()/logs/` | `MainActivity.onCreate()` 在 Surface 创建前经 JNI 传入 |
| iOS | stdout（Xcode 控制台） | `Application Support/logs/` | `svkIosInit()` 创建命名 stdout logger 并追加文件 |
| 鸿蒙 | HiLog | `filesDir/logs/` | 经 `OH_AbilityRuntime_ApplicationContextGetFilesDir()` 获取 |
| 默认/测试 | stdout | 无，除非显式调用 `initWithFile()` | `svk::log::init(tag)` |

文件名为：

```text
estarx-vulkan-YYYYMMDD-HHMMSS-<毫秒>.log      # 主通道
audit-YYYYMMDD-HHMMSS-<毫秒>.log              # 审计通道（独立文件）
```

每次进程冷启动创建一个新文件。同一进程内 Surface 重建、Activity/View 恢复或重复初始化会复用现有文件，不会重复创建。文件 sink 由 spdlog 自动创建父目录；路径不可写或创建失败时自动回退为仅终端输出，并保留渲染初始化流程。

单文件超过 `LogFileConfig::maxFileSizeBytes`（默认 10MB）后轮转为
`name.N.log`，同次启动最多保留 `maxRotatedFiles` 份（默认 5）。
`initWithFile` 创建新文件前会清理目录：同前缀且超过 `maxFileAgeDays`
（默认 7 天，审计 30 天）的文件直接删除；同前缀总量超过 `maxTotalBytes`
（默认 200MB，审计 50MB）时从最老删起，清理结果以一条 `log_pruned` 日志记录。

三端初始化成功后会写一条启动日志，其中包含真实文件路径：

```text
[svk][log][file_started platform=<platform> path=...]
```

### 10.4 当前覆盖范围

文件会包含：

- Core 引擎各模块的 `SVK_LOG*`。
- Android JNI、iOS C 桥、鸿蒙 NAPI 桥接日志。
- App 业务中主动使用 `SVK_LOG*` 的日志。
- 启用 Vulkan Validation 时的校验消息。

文件不会自动包含：

- Java、Objective-C、ArkTS 中未经过 `SVK_LOG*` 的原生日志。
- 系统日志、GPU 驱动内部日志或其他第三方库自己的输出。

### 10.5 初始化与生命周期日志事件

冷启动时，顶层事件按以下顺序出现：

```text
[svk][renderer][initialize_start]
[svk][renderer][initialize_stage_start stage=vulkan_context]
[svk][vulkan][instance_create_start]
[svk][vulkan][surface_create_start]
[svk][vulkan][physical_device_select_start]
[svk][vulkan][logical_device_create_start]
[svk][vulkan][initialize_complete]
[svk][renderer][initialize_stage_done stage=vulkan_context]
[svk][renderer][initialize_stage_start stage=texture_cache]
[svk][renderer][initialize_stage_done stage=texture_cache]
[svk][renderer][initialize_stage_start stage=swapchain]
[svk][renderer][initialize_stage_done stage=swapchain]
[svk][renderer][initialize_stage_start stage=ui_pipeline]
[svk][renderer][initialize_stage_done stage=ui_pipeline]
[svk][renderer][initialize_stage_start stage=framebuffers]
[svk][renderer][initialize_stage_done stage=framebuffers]
[svk][renderer][initialize_stage_start stage=command_pool]
[svk][renderer][initialize_stage_start stage=vertex_buffers]
[svk][renderer][initialize_stage_start stage=command_buffers]
[svk][renderer][initialize_stage_start stage=sync_objects]
[svk][renderer][initialize_complete duration_ms=...]
```

子模块还会记录关键资源参数：实例扩展、物理设备名称和队列族、白纹理显存、Surface 能力、Swapchain 尺寸/格式/PresentMode、MSAA 采样和显存、RenderPass 附件、Shader 字节数、顶点缓冲容量以及同步对象数量。

生命周期重建也有对应事件：

- `swapchain_recreate_start / stage_done / complete`
- `surface_suspend_start / surface_suspended`
- `surface_resume_start / stage_done / surface_resumed`

当前 Android 宿主在 Renderer 初始化完成后才调用 `setSize()`，因此启动日志中可能在 `engine_started` 后看到一次额外的 `swapchain_recreate_start`。这不是初始化失败，而是现有启动顺序带来的重复建链，后续可通过“先传尺寸再初始化”优化掉。

### 10.6 高频日志使用边界

异步队列把终端与文件 I/O 移出业务线程，但不会消除所有成本：消息正文格式化、`log_msg` 复制和队列入队仍发生在调用线程。行情推送这类高频来源仍应做采样、聚合、节流或只记录异常与关键快照，不能把全量业务流量无差别写入日志。

推荐边界：

- 常规逐笔行情不直接逐条写 Info；需要追踪时使用 Debug，并按序号、时间窗口或变化量采样。
- 队列只作为瞬时削峰，长期写满会按 `overrun_oldest` 丢弃旧日志，文件不再是完整流水。
- `asyncDroppedCount()` 或 Flush 时的 `async_queue_overrun` 应作为日志系统自身告警处理。
- Error 会等待 Flush，但 Warn 及以下在 Crash 瞬间仍可能留在队列中。

### 10.7 日志文件过大怎么处理

异步队列限制的是内存削峰，不限制磁盘增长。当前每个进程冷启动都会新建完整文件，长时间运行或高频日志持续开启时，文件仍然可能变得很大，应把“完整复现”和“长期运行”分开处理：

- **调试/问题复现模式**：保留当前按会话创建完整文件的方式，问题发生后尽快导出；导出后再清理。
- **高频/Release 模式**：改用有上限的滚动文件，建议首版按 **16 MiB × 5 个文件** 估算，总上限约 80 MiB；具体值再按目标设备存储和真实日志速率调整。
- **会话保留**：如果继续使用带时间戳的会话文件，仅滚动当前文件还不够，还必须限制保留的会话数量，否则多次启动仍会持续累积。
- **内容治理**：滚动不能替代采样。行情全量推送不应依赖文本日志长期保存，应记录关键快照、异常和聚合指标；完整业务流水应使用专门的数据文件或上报通道。

后续实现建议使用 spdlog 的 `rotating_file_sink_mt`，并在 `LogFileConfig` 中增加单文件大小、文件数量、会话保留数量和 Debug/Release 策略。验收时应用小尺寸配置实际触发滚动，确认旧文件被删除、当前写入继续、异步 Flush 不丢失滚动后的记录。

如果设备上已经出现过大的日志，先通过前面各平台的导出方式取回需要分析的文件，再删除对应日志目录中的旧文件；不要通过清空整个 App 数据来处理。

### 10.8 仍未做的日志能力

- 没有滚动文件、大小上限、保留数量和自动清理。
- 没有源码文件、行号和函数名。
- 没有日志上传、压缩或问题单自动附带。
- Crash 瞬间仍可能丢失尚未进入后台线程的队列尾部；Error 会等待 Flush，挂起和关闭路径会显式排空队列。

## 11. 审阅时需要关注的简化点

以下不是新增功能承诺，而是当前代码中已经存在、后续适合优先完善的基础问题：

1. **日志运维**：缺少滚动、大小控制、保留策略和自动化测试。
2. **显存选择**：`findMemoryType()` 失败返回 0，可能掩盖资源配置错误。
3. **Vulkan 错误处理**：部分调用没有检查返回值，设备能力检查也只覆盖最低要求。
4. **失败路径一致性**：初始化中途失败、Shader/Pipeline 创建失败和资源重建失败的清理路径需要专项核查。
5. **同步开销**：重建和挂起使用全设备等待，后续有性能需求时再评估更细粒度同步。
6. **注释准确性**：MSAA 选择顺序、Compositor/Raster、按需渲染等部分注释与实际代码存在历史遗留差异。
7. **测试缺口**：当前未见针对 Canvas、资源状态机、Swapchain 重建和日志的自动化测试。
8. **能力边界**：TextureCache、Canvas 和 IApp 都是最小接口，不应按完整 UI 引擎理解。

## 12. 如何获取完整日志文件

优先从启动日志中的 `file_started` 记录获取准确路径，不手写猜测容器路径。

### Android

Debug 包可用 `run-as` 查看应用私有目录：

```bash
adb shell run-as <applicationId> ls files/logs
adb shell run-as <applicationId> cat files/logs/<日志文件名>
```

例如 Triangle 当前 applicationId 是 `com.estarx.triangle`。Release、系统限制或设备厂商策略可能要求改用 `adb bugreport` 或应用内导出。

### iOS

- 真机：在 Xcode 的 Devices 窗口下载 App Container，然后查看 `Application Support/logs/`。
- 模拟器：直接进入当前模拟器 App 数据容器的 `Application Support/logs/`。

### 鸿蒙

启动时通过 HiLog 中的 `file_started` 获取实际路径。随后可按当前 DevEco/HDC 环境查看应用 `filesDir/logs/` 下的对应文件。

### 反馈问题时建议附带

- 完整 `.log` 文件，而不是只复制最后几行。
- 设备型号、系统版本、App 版本和构建类型。
- 复现步骤、发生时间和是否经过旋转、退后台或锁屏。

## 13. 建议的下一步顺序

1. 在三端真机上启动一次，确认 `file_started`、挂起恢复和关闭日志均落盘。
2. 实现滚动文件和会话保留，首版按 16 MiB × 5 的磁盘上限验证并调整。
3. 补齐日志及 Canvas 等纯 CPU 模块的基础测试。
4. 统一显存查找、Vulkan 返回值和失败路径的错误处理。
5. 清理过时注释，再继续扩展布局、事件和更多绘制能力。
