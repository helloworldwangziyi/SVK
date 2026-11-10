# SVK 架构 Roadmap：对齐 Flutter

本文档是项目唯一的架构与演进文档。方向是借鉴 Flutter 的分层、帧管线和职责边界，在现有 C++17/Vulkan 工程上渐进演进，而不是复制整个 Flutter engine。

## 1. 目标与范围

保持现有能力边界：

- 平台只支持 Android、鸿蒙，图形后端只使用 Vulkan。
- App 使用平台无关的 C++ API，最终只呈现到平台提供的 Surface。
- 平台壳只负责 Surface、生命周期、VSync、输入和系统能力适配。
- 保留轻量依赖，不引入 Dart VM、Skia，也不整体移植 Impeller 或 Flutter Framework。

目标架构：

```text
┌──────────────── Application ────────────────┐
│ App / Widget / Element / RenderObject        │  后续按需建设
└──────────────────────┬───────────────────────┘
                       │ 布局、绘制
┌──────────────────────▼───────────────────────┐
│ Canvas → DisplayList → Layer Tree            │
│                         ↓ Preroll / Paint     │
│                      Compositor              │
│                                              │
│ Shell：Engine 生命周期、帧编排、线程与任务调度 │
│                                              │
│ Vulkan Raster：资源、Swapchain、提交、Present │
└──────────────────────┬───────────────────────┘
                       │ VkSurfaceKHR / 系统事件
┌──────────────────────▼───────────────────────┐
│ Android Embedder       │ 鸿蒙 Embedder        │
│ SurfaceView / JNI      │ XComponent / NAPI    │
└──────────────────────────────────────────────┘
```

核心绘制链路：

```text
App 构建绘制内容
  → DisplayList 记录
  → Layer Tree 组织
  → Compositor 遍历
  → Vulkan 光栅化
  → Swapchain Present 到平台 Surface
```

Shell 负责协调上述过程，不负责具体 App 绘制；Embedder 负责系统事件翻译，不包含 App 业务。

## 2. 当前基础与差距

现有对应关系：

- `IPlatform`、Android JNI、鸿蒙 NAPI：对应简化的 Embedder 边界。
- `IRasterSurface`、`Renderer`、`gpu/vulkan/`：对应光栅化与 Surface 呈现。
- `ui::Canvas`：当前直接收集 CPU 顶点与裁剪批次，尚不是 DisplayList。
- `IApp::onFrame()`：当前由平台直接驱动，尚无独立 Shell 和帧管线。

主要缺口：

- 没有可记录、回放的 DisplayList。
- 没有 Layer Tree、Compositor 和统一的 Preroll/Paint 遍历。
- 没有 Widget、Element、RenderObject 及布局协议。
- 没有统一的输入分发、动画和帧调度模型。
- 所有引擎方法目前仍在同一个线程顺序调用。

## 3. 架构约束

1. 保持公共边界 `App → core ← platform`，core 不依赖平台界面 API，platform 不反向依赖具体 App。
2. core 内部职责单向拆分：绘制描述不依赖 Vulkan，Layer/Compositor 不依赖 Android 或鸿蒙，Embedder 不感知 Widget 和 App 业务。
3. Vulkan 只位于光栅化实现中；DisplayList、Layer 和布局层不得暴露 Vulkan 类型。
4. 先单线程正确运行，再依据性能数据拆分 UI/Raster 线程；不在现有公共方法中零散加锁。
5. 每阶段都保持示例可运行、现有 Surface 生命周期可用，不做一次性重写。
6. 平台无关逻辑放在 host 测试中；每阶段结束同时验证 Android、鸿蒙示例。
7. 架构决策、完成状态和后续调整统一更新本文档，不新增分散的架构文档。

## 4. 分阶段实施

### P0：稳定边界与回归基线

目标：在改变内部实现前固定现有行为。

- 明确 App、Surface、Renderer 和平台桥接的所有权及调用顺序。
- 固定 Surface 创建、尺寸变化、挂起、恢复和销毁的状态转换。
- 为 Canvas 的图元、变换和裁剪补充 host 回归测试。
- 记录三角形示例在两端的当前表现，作为后续重构基线。

完成标准：host 测试通过，两端示例构建和运行通过，行为与重构前一致。

### P1：Canvas 与 DisplayList

目标：把“App 如何描述绘制”与“Vulkan 如何执行绘制”分开。

- 引入 DisplayList Builder，将 Canvas 调用记录为可回放指令。
- 首批指令覆盖现有三角形、矩形、变换、裁剪和状态保存/恢复。
- DisplayList 记录完成后保持不可变，由光栅化阶段统一消费。
- Renderer 不再接收 App 的即时顶点写入，只负责回放和资源执行。
- 参考 Flutter DisplayList 的接口和指令组织，不复制其无关后端能力。

完成标准：host 覆盖指令记录、回放、状态恢复和裁剪；两端示例画面保持一致。

### P2：Layer Tree 与 Compositor

目标：以 Layer 组织绘制，建立 Flutter 风格的场景遍历。

- 引入 Container、Transform、Clip、Opacity 和 DisplayList Layer。
- 建立 Preroll/Paint 两阶段遍历，统一计算变换、裁剪和绘制范围。
- Compositor 负责遍历 Layer Tree，Vulkan Renderer 只执行最终绘制。
- 首版不加入 RasterCache，避免为尚无性能问题的路径增加离屏渲染。

完成标准：嵌套 transform/clip/opacity 用例正确；host 覆盖树遍历和状态隔离；两端示例通过。

### P3：Shell 与帧管线

目标：把平台直驱调用改为统一的 Engine/Shell 帧编排。

- 由 Shell 统一管理 App、Compositor、RasterSurface 和平台接口的生命周期。
- 建立 VSync → BeginFrame → 构建场景 → 光栅化 → Present 的明确流程。
- 增加按需请求帧和动画回调，避免无内容变化时持续绘制。
- 统一两端 Surface 挂起、恢复、尺寸变化时的帧状态。
- 首版仍保持单线程；只有性能分析证明需要时，才设计独立 UI/Raster TaskRunner 和帧传递管线。

完成标准：两端使用同一帧协议；生命周期切换无重复提交或失效 Surface 绘制；按需帧和动画用例通过。

### P4：统一输入与事件分发

目标：让输入像 Flutter 一样从 Embedder 进入 Engine，再由 UI 层处理。

- 定义平台无关的 PointerEvent，统一触摸、按下、移动、抬起和取消语义。
- Android、鸿蒙只转换坐标和系统事件，不实现 App 手势业务。
- Shell 负责事件注入，UI 层负责命中测试、事件分发和手势识别。
- 明确输入与帧调度、Surface 生命周期之间的顺序。

完成标准：相同输入在两端产生一致的标准事件流；命中测试、取消和生命周期中断用例通过。

### P5：声明式 UI 与布局

目标：在稳定的绘制和帧管线之上，建立简化版 Flutter Framework 分层。

- Widget：不可变配置，不直接执行 Vulkan 绘制。
- Element：挂载、更新、重建和卸载，管理 Widget 与 RenderObject 的关系。
- RenderObject：布局、绘制和命中测试，通过 Canvas 生成 DisplayList。
- 引入 BoxConstraints、父级约束和 dirty 标记，按需触发布局与重绘。
- 先实现最小组件集合，再依据真实 App 需求扩展，不追求一次性覆盖 Flutter Widget 库。

完成标准：同一声明式组件树可完成更新、布局、绘制和输入；host 覆盖重建、局部重布局及命中测试；两端示例通过。

## 5. 后续独立评审项

以下能力不进入当前默认范围，需要真实需求、依赖评估和单独设计后再排期：

- 字体加载、文字 shaping、glyph atlas 和段落布局。
- 图片解码、纹理上传和跨帧资源缓存。
- Semantics、无障碍、Platform View 和平台通道。
- RasterCache、离屏输出、像素回读及其他图形后端。
- 独立 UI/Raster 线程、多引擎和 Isolate 等复杂运行模型。

## 6. 阶段验收与参考

每个阶段必须依次满足：

1. 对应 host 测试和 `ctest` 通过。
2. Android 三角形示例构建、运行和生命周期验证通过。
3. 鸿蒙三角形示例构建、运行和生命周期验证通过。
4. 未把平台类型泄漏到 core，未把 App 业务下沉到 platform。
5. 更新本文档中的完成状态、偏差和下一阶段前置条件。

Flutter 参考源码根目录：`third_party/flutter-engine/engine/src/flutter/`。

- DisplayList：`display_list/dl_builder.h`、`display_list/dl_canvas.h`、`display_list/dl_op_records.h`。
- Layer/Compositor：`flow/layers/`、`flow/compositor_context.{h,cc}`。
- 帧编排：`shell/common/animator.{h,cc}`、`shell/common/pipeline.{h,cc}`。
- 线程模型：`fml/task_runner.{h,cc}`、`fml/raster_thread_merger.{h,cc}`。
- Vulkan 参考：`impeller/renderer/backend/vulkan/`。

参考上游的职责划分和协议设计，不按目录整体搬运代码。
