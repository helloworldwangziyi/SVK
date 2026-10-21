# 启动与帧渲染时序（三端对照）

本文用时序图说明这套种子工程"从进程启动到三角形的每一帧上屏"的完整链路，
配合 [engine-core.md](engine-core.md)（引擎内部实现清单）和
[project-structure.md](project-structure.md)（目录与工程组织）阅读。

设计前提回顾：

- 依赖方向 **App → core ← platform**，三端桥接层刻意同构。
- 全部壳回调与引擎调用都在 **UI 线程** 顺序执行，引擎内部无锁。
- 生命周期只有两种语义：**保活挂起/热恢复**（surface 级资源）
  和 **真正销毁**（全量释放）。

## 1. 运行时总览

```mermaid
flowchart TB
    subgraph shell["平台薄壳（只做壳装配，无业务）"]
        A1["Android<br/>VulkanSurfaceView + Choreographer"]
        A2["iOS<br/>SVKMetalView + CADisplayLink"]
        A3["鸿蒙<br/>XComponent + DisplaySync"]
    end

    subgraph bridge["桥接层（解包参数 → 转发 core）"]
        B1["JNI<br/>android_jni_bridge.cpp"]
        B2["C 桥<br/>ios_shell_bridge.mm"]
        B3["NAPI<br/>hongmeng_napi_bridge.cpp"]
    end

    subgraph core["core（平台无关，svk_core）"]
        C1["IApp.onFrame<br/>App 写顶点"]
        C2["ui::Canvas<br/>CPU 顶点流 + Batch"]
        C3["Renderer (IRasterSurface)<br/>acquire → 上传 → 录制 → 提交 → present"]
        C4["gpu 子模块<br/>VulkanContext / Swapchain / UiPipeline / TextureCache"]
    end

    subgraph platformImpl["IPlatform 实现（surface 创建 + 尺寸查询）"]
        P1["AndroidPlatform<br/>vkCreateAndroidSurfaceKHR"]
        P2["IosPlatform<br/>vkCreateMetalSurfaceEXT（MoltenVK）"]
        P3["HongmengPlatform<br/>vkCreateSurfaceOHOS"]
    end

    A1 --> B1
    A2 --> B2
    A3 --> B3
    B1 --> C1
    B2 --> C1
    B3 --> C1
    C1 --> C2 --> C3 --> C4
    B1 -.-> P1
    B2 -.-> P2
    B3 -.-> P3
    P1 -.-> C3
    P2 -.-> C3
    P3 -.-> C3
```

## 2. 三端入口接线对照表

| 环节 | Android | iOS | 鸿蒙 |
|---|---|---|---|
| 进程入口 | `MainActivity.onCreate` | `main.m → UIApplicationMain` | `EntryAbility.onCreate` |
| 日志目录注入 | `nativeSetLogDirectory(filesDir/logs)` | `svkIosInit` 内自取 `Application Support/logs` | `OH_AbilityRuntime_ApplicationContextGetFilesDir()` |
| surface 载体 | `SurfaceView`（SurfaceHolder.Callback） | `CAMetalLayer`（SVKMetalView 的 backing layer） | `XComponent(SURFACE)` 的 `OHNativeWindow` |
| 引擎启动触发 | `surfaceCreated` → `nativeInit` | `viewDidAppear` → `svkIosInit` | `OnSurfaceCreatedCB`（XComponent 回调） |
| VSync 通道 | `Choreographer.postFrameCallback` | `CADisplayLink`（common runloop modes） | ArkTS `displaySync.on('frame')` |
| 帧回调入口 | `nativeBeginFrame(frameTimeNanos)` | `svkIosBeginFrame(nanos)`（steady_clock） | `beginFrame(intervalInfo.timestamp)` |
| 尺寸变化 | `surfaceChanged` → `nativeResize` | `layoutSubviews` → `svkIosResize` | `OnSurfaceChangedCB` |
| 保活挂起 | `surfaceDestroyed` → `nativeDestroy` | `viewWillDisappear` → `svkIosSuspend` | `OnSurfaceDestroyedCB` |
| 真正销毁 | `Activity.onDestroy` → `nativeShutdown` | `ViewController.dealloc` → `svkIosShutdown` | `EntryAbility.onDestroy` → `onShutdown()` |
| native 库加载 | `System.loadLibrary("estarx_vulkan")` | 静态链接进 App 二进制 | `dlopen` 后构造函数自动 `napi_module_register` |

## 3. 冷启动时序

三端差异只在"壳怎么把 surface 和 VSync 接进来"；一旦进入桥接层，
后续走的是同一条共享主干（见 3.4）。

### 3.1 Android

```mermaid
sequenceDiagram
    autonumber
    participant Sys as 系统（AMS/WMS）
    participant Act as MainActivity
    participant View as VulkanSurfaceView
    participant JNI as JNI 桥<br/>android_jni_bridge
    participant Core as core 引擎

    Sys->>Act: onCreate()
    Act->>JNI: nativeSetLogDirectory(filesDir/logs)
    JNI->>JNI: ensureLogger：Logcat sink + 文件 sink
    Act->>View: setContentView(activity_main)
    View->>Sys: getHolder().addCallback(this)
    Sys-->>View: surfaceCreated(holder)
    View->>JNI: nativeInit(holder.getSurface())
    Note over JNI,Core: 走共享主干（3.4），完成后引擎就绪
    Sys-->>View: surfaceChanged(format, w, h)
    View->>JNI: nativeResize(w, h)
    JNI->>Core: renderer->setSize(w, h)
    View->>Sys: Choreographer.postFrameCallback
    Note over View,Sys: 此后每个 VSync 触发一帧（见第 4 节）
```

### 3.2 iOS

```mermaid
sequenceDiagram
    autonumber
    participant Sys as UIKit
    participant Del as AppDelegate
    participant VC as ViewController
    participant View as SVKMetalView<br/>（CAMetalLayer）
    participant CBridge as C 桥<br/>ios_shell_bridge
    participant Core as core 引擎

    Sys->>Del: didFinishLaunchingWithOptions
    Del->>VC: window.rootViewController = ViewController
    VC->>View: loadView：backing layer = CAMetalLayer
    VC->>Sys: viewDidLoad：CADisplayLink 挂 mainRunLoop(commonModes)
    Sys-->>View: layoutSubviews
    View->>View: drawableSize = bounds × nativeScale
    View->>CBridge: svkIosResize(w, h)
    Sys-->>VC: viewDidAppear
    VC->>CBridge: svkIosInit(CAMetalLayer)
    Note over CBridge,Core: 走共享主干（3.4），surface 由<br/>vkCreateMetalSurfaceEXT 经 MoltenVK 创建
    Sys-->>VC: onDisplayLink（每个 VSync）
    Note over VC: 取 steady_clock 单调秒数，与另两端语义一致
```

### 3.3 鸿蒙

```mermaid
sequenceDiagram
    autonumber
    participant Sys as 系统（Ability/ArkUI）
    participant Ab as EntryAbility
    participant Shell as VulkanShell.ets<br/>（XComponent）
    participant SO as libestarx_vulkan.so
    participant NAPI as NAPI 桥<br/>hongmeng_napi_bridge
    participant Core as core 引擎

    Sys->>Ab: onCreate()
    Sys->>Ab: onWindowStageCreate(windowStage)
    Ab->>Sys: setWindowLayoutFullScreen(true)
    Ab->>Sys: loadContent('pages/Index')
    Sys->>Shell: 构建 XComponent(SURFACE, libraryname='estarx_vulkan')
    Sys->>SO: dlopen
    SO->>SO: 构造函数 RegisterEstarxVulkanModule<br/>→ napi_module_register
    SO->>NAPI: Init(env, exports)：挂 beginFrame/onShutdown
    NAPI->>Sys: 解包 __NATIVE_XCOMPONENT_OBJ__<br/>注册 XComponent 四回调
    Sys-->>Shell: onLoad(context = exports)
    Shell->>Shell: g_bridge = context；startFrames()
    Shell->>Sys: displaySync.create().on('frame').start()
    Sys-->>NAPI: OnSurfaceCreatedCB(component, window)
    NAPI->>NAPI: OH_NativeXComponent_GetXComponentSize
    Note over NAPI,Core: 走共享主干（3.4），surface 由<br/>vkCreateSurfaceOHOS 创建
    Sys-->>Shell: 每个 VSync：'frame' 回调
    Shell->>NAPI: beginFrame(intervalInfo.timestamp)
```

注：鸿蒙刻意用 ArkTS `DisplaySync` 而非 `OH_NativeVSync`——后者回调在
独立 vsync 线程，会破坏 core 的无锁单线程假设。模块句柄刻意经
`onLoad(context)` 获取而不手动 `import`：手动 import 会让模块先于
XComponent 加载，`Init` 就解包不到 XComponent 实例对象。

### 3.4 共享主干：桥接层 → 引擎（三端一致）

```mermaid
sequenceDiagram
    autonumber
    participant Bridge as 桥接层（JNI/C/NAPI）
    participant Plat as IPlatform 实现
    participant R as Renderer<br/>（IRasterSurface）
    participant GPU as gpu 子模块
    participant App as IApp（TriangleApp）

    Bridge->>Plat: svkCreate<平台>Platform(原生窗口)
    Note over Bridge: g_renderer 已存在 → 走热恢复（第 6 节）<br/>以下按冷启动（g_renderer == null）
    Bridge->>R: createRasterSurface(platform)
    Bridge->>R: initialize()

    R->>GPU: VulkanContext.initialize()
    GPU->>Plat: getSurfaceExtensionName()<br/>+ getRequiredInstanceExtensions()（可选钩子）
    GPU->>Plat: createVulkanSurface(instance, &surface)
    Note over GPU: instance → surface → 物理设备 → 逻辑设备/队列
    R->>GPU: TextureCache.initialize()：采样器 + Descriptor + 1×1 白纹理
    R->>GPU: Swapchain.create()：swapchain → ImageView → MSAA → RenderPass
    R->>GPU: UiPipeline.create()：唯一管线（动态 vp/scissor + 64B mvp）
    R->>GPU: Swapchain.createFramebuffers()
    R->>R: CommandPool → 双帧顶点缓冲 → 双帧 CommandBuffer → 同步对象
    R-->>Bridge: true（任一步失败 → false，桥接层反序清理可重试）

    Bridge->>App: createApp()（仅首次，g_app 为空时）
    Bridge->>Plat: getSurfaceSize(&w, &h)
    Bridge->>R: setSize(w, h)
    Note over Bridge: 记录 engine_started 日志，进入帧循环
```

## 4. 每帧渲染时序（VSync → 上屏）

```mermaid
sequenceDiagram
    autonumber
    participant VS as 系统 VSync
    participant Shell as 平台薄壳
    participant Bridge as 桥接层
    participant Plat as IPlatform 实现
    participant App as IApp
    participant Canvas as ui::Canvas
    participant R as Renderer
    participant GPU as Vulkan 驱动/GPU

    VS-->>Shell: doFrame / onDisplayLink / 'frame'
    Shell->>Bridge: beginFrame(frameTimeNanos)

    Bridge->>Plat: getSurfaceSize(&w, &h)
    Bridge->>Canvas: 栈上新建，本帧专用
    Bridge->>App: onFrame(canvas, {w, h, timeSeconds})
    App->>Canvas: drawTriangle / drawRect（像素坐标，CPU 三角化）
    Note over Canvas: 相邻且 clip 相同的图元合并为同一 Batch
    App-->>Bridge: 返回

    Bridge->>R: render(canvas)
    loop 最多 3 轮（kMaxFrameAttempts，消化尺寸/surface 变化）
        R->>R: 消费 swapchain 重建标记，必要时 vkDeviceWaitIdle 后重建
        R->>GPU: vkWaitForFences（≤16ms，超时 → 返回 Retry）
        R->>GPU: vkAcquireNextImageKHR
        alt OUT_OF_DATE
            R->>R: 立即重建 swapchain，进入下一轮
        else SUBOPTIMAL
            R->>R: 先画本帧，present 后标记重建
        end
        R->>GPU: 上传顶点到当前帧槽缓冲<br/>（HOST_VISIBLE|COHERENT，不足倍增）
        R->>R: Reset Fence（确定会提交才 Reset）+ Reset CommandBuffer
        R->>GPU: 录制：清屏 → 绑管线 → 动态 viewport<br/>→ push 64B MVP（含旋转补偿）→ 逐 Batch scissor + draw
        R->>GPU: vkQueueSubmit（acquire/render/present 间 semaphore 串联）
        R->>GPU: vkQueuePresentKHR
        Note over R: OUT_OF_DATE/SUBOPTIMAL → 标记重建，用同一 Canvas 重试
    end
    R->>R: 帧槽轮换（0 → 1 → 0，kMaxFramesInFlight = 2）
    R-->>Bridge: Rendered / Retry / Failed
```

关键点：

- **双帧在飞**：命令缓冲、Fence、顶点缓冲都是双份，CPU 最多领先 GPU 一帧，
  上传顶点不会覆盖 GPU 正在读的上一份。
- **重试语义**：`Retry` 表示"稍后再来"（等待超时/图像暂不可用/重建重试耗尽），
  壳层不做特殊处理——下一个 VSync 自然重试。空 Canvas 也会形成清屏帧。
- **旋转补偿**：App 永远用左上角原点、Y 轴向下的像素坐标；Renderer 根据
  swapchain 的 `currentTransform` 在 MVP 和 scissor 上补偿 90/180/270°。

## 5. 尺寸变化（旋转/分屏/窗口化）

```mermaid
sequenceDiagram
    participant Sys as 系统
    participant Shell as 平台薄壳
    participant Bridge as 桥接层
    participant R as Renderer

    Sys-->>Shell: surfaceChanged / layoutSubviews / OnSurfaceChangedCB
    Shell->>Bridge: resize(w, h)
    Bridge->>R: setSize(w, h)
    Note over R: 只打标记，不在回调里重建
    Note over R: 下一帧 render() 开头消费标记：<br/>vkDeviceWaitIdle → 重建 swapchain/管线/帧缓冲
```

## 6. 保活挂起与热恢复（退后台 → 回前台）

```mermaid
sequenceDiagram
    autonumber
    participant Sys as 系统
    participant Bridge as 桥接层
    participant R as Renderer
    participant Plat as IPlatform

    Note over Sys: 退后台（surface 销毁，进程保留）
    Sys-->>Bridge: surfaceDestroyed / viewWillDisappear / OnSurfaceDestroyedCB
    Bridge->>R: suspendSurface()
    R->>R: vkDeviceWaitIdle
    R->>R: 销毁 UiPipeline + Swapchain 相关 + VkSurfaceKHR
    Note over R: 保留：Instance/Device/TextureCache/<br/>CommandPool/顶点缓冲/同步对象
    Bridge->>Plat: svkDestroy<平台>Platform(g_platform)
    Note over Bridge: 释放壳侧窗口引用（ANativeWindow / 全局引用等）

    Note over Sys: 回前台（surface 重建）
    Sys-->>Bridge: surfaceCreated / viewDidAppear / OnSurfaceCreatedCB
    Bridge->>Plat: svkCreate<平台>Platform(新窗口)
    Bridge->>R: resumeSurface(newPlatform)
    R->>R: 只重建 surface → swapchain → 管线 → 帧缓冲
    alt 热恢复成功
        Bridge->>Plat: 销毁旧 platform，替换指针
        Note over Bridge: engine_resumed
    else 热恢复失败
        Bridge->>Bridge: 全量拆除 → 落到冷启动兜底重试
    end
```

挂起与恢复的真正销毁对照：Activity.onDestroy / ViewController.dealloc /
EntryAbility.onDestroy → `nativeShutdown` / `svkIosShutdown` / `onShutdown`，
做全量释放（app.reset → delete renderer → destroy platform），之后再次启动
走完整冷启动。

## 7. 涉及文件索引

| 环节 | 文件 |
|---|---|
| 公共接口 | `core/src/app.h`、`core/src/render_platform.h`、`core/src/raster_surface.h` |
| 引擎 | `core/src/vulkan_renderer.*`、`core/src/gpu/*`、`core/src/ui/canvas.*` |
| Android | `platform/android/java/.../VulkanSurfaceView.java`、`NativeBridge.java`、`platform/android/cpp/android_jni_bridge.cpp`、`android_vulkan_platform.cpp` |
| iOS | `samples/triangle/ios/SVKApp/ViewController.mm`、`platform/ios/ios_shell_bridge.mm`、`ios_vulkan_platform.mm` |
| 鸿蒙 | `samples/triangle/hongmeng/entry/src/main/ets/vulkan/VulkanShell.ets`、`entryability/EntryAbility.ets`、`platform/hongmeng/cpp/hongmeng_napi_bridge.cpp`、`hongmeng_vulkan_platform.cpp` |
| App 示例 | `samples/triangle/app.cpp`、`samples/triangle/triangle.h` |
