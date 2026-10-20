# 向 Flutter 引擎架构靠拢路线图（C++17 + Vulkan，无 Dart）

状态：**待评审**。评审通过后按阶段拆解执行，每阶段单独交付、单独验收。

## 1. 背景与目标

`third_party/flutter-engine/` 是上游 Flutter engine 仓库 `src/flutter/` 的裸拷贝（约 70MB，BSD 协议，无 git/版本锁定，2026-10-19 入库，提交 `8db2ba1`）。其中 Dart 强耦合部分（`lib/ui/`、`runtime/`、tonic）不用；C++ 部分（`impeller/`、`flow/`、`display_list/`、`fml/`、`shell/`、`txt/`）完整可读。

**目标**：保持 `svp_core` 的 C++17 + Vulkan 技术栈与三端壳结构不变，在架构上全量对齐 Flutter 引擎的渲染管线分层，最终形态为"Flutter 引擎的 C++ 骨架"：

```text
Widget 树（C++）→ Layer 树 → DisplayList → Vulkan 渲染核 → 平台壳
```

**非目标**（明确不做）：

- 不引入 Dart VM / Dart 绑定层。
- 不整体移植 Impeller 或 flow 的源码（体量大、GN 构建、依赖深）；做法是**读源码、抄架构、用本项目代码风格重写精简版**。
- 不引入 Skia；渲染继续走自有 Vulkan 管线。
- 不切换构建系统，继续 CMake + 三端原生工程。

## 2. 现状与差距

已有骨架与 Flutter 同构，这是靠拢的基础：

| SVK 现状 | Flutter 对应物 | 状态 |
|---|---|---|
| `IPlatform`（render_platform.h） | embedder surface delegate | ✅ 雏形已在 |
| `IRasterSurface`（raster_surface.h） | `shell/gpu/gpu_surface_vulkan*` | ✅ 雏形已在 |
| 三端 bridge（platform/*/） | `shell/platform/{android,darwin,embedder}/` | ✅ 同构 |
| Vulkan 设施（gpu/、vulkan_renderer） | `impeller/renderer/backend/vulkan/` | ⚠️ 最小可用，生产级能力缺 |
| spdlog 日志封装 | — | ✅ 超预期完整 |

空白层（当前完全不存在，即本路线图的主体）：

| 缺失层 | Flutter 参考 | 说明 |
|---|---|---|
| 渲染指令流（Display List） | `display_list/` | Canvas 现在是立即模式直接收集顶点，无录制/回放 |
| 图层树 / 合成器 | `flow/layers/`、`flow/compositor_context` | 无 retained 结构，无法 diff/缓存/裁剪合成 |
| Widget / RenderObject 树 | （上游在 Dart framework；此处需自建 C++ 版） | 无组件树、无布局、无样式 |
| 帧调度器 / 动画 | `shell/common/animator`、`pipeline`、`flow/frame_timings` | 现在平台 vsync 回调直渲一帧，无帧管线 |
| 输入 / 事件分发 | `shell/platform/*/platform_view*` 的 pointer 事件链 | `IApp` 只有 `onFrame`，无任何事件 |
| 文字排版 | `txt/` | 无字体、无 shaping、无 atlas |
| 多线程调度 | `fml/task_runner`、`raster_thread_merger` | 单线程无锁契约（当前是优点，暂不动） |

## 3. 目标分层与目录落位

```text
samples/<app>/            App 业务（不变）
core/src/ui/              Widget/RenderObject 树（新建，P3）
core/src/compositor/      Layer 树 + Preroll/Paint（新建，P2）
core/src/dl/              DisplayList 指令流（新建，P1）
core/src/gpu/             Vulkan 渲染核（现有，持续加固）
core/src/frame/           帧调度器 / 动画 / 输入分发（新建，P4/P5）
platform/{android,ios,hongmeng}/  平台壳（不变，按需补事件注入）
```

依赖方向不变：`App → core ← platform`。新增各层只依赖下层，host 侧可独立测试。

## 4. 分阶段计划

### P1：Display List 指令流

- **目标**：`ui::Canvas` 从立即模式改为录制式指令流；渲染端回放翻译为 GPU 顶点流。补齐图元：圆角矩形、真实纹理/图片（替换 1×1 白纹理占位）、path（可先只支持凸多边形三角化）。
- **对照阅读**：`display_list/dl_builder.h`、`dl_canvas.h`、`dl_op_records.h`（op 划分与权重估算）。
- **落地**：`core/src/dl/`（`display_list.h/.cpp`、`ops.h`、`dl_canvas.h`）；`vulkan_renderer` 的输入从 `ui::Canvas` 换成 `DisplayList`。
- **验收**：samples/triangle 改用指令流后渲染结果不变；host 测试断言录制的 op 序列与回放输出。

### P2：Layer 树合成器

- **目标**：引入 retained 图层结构：container/clip/transform/opacity 四类图层起步，Preroll（包围盒、缓存决策）+ Paint（回放指令）两阶段遍历。
- **对照阅读**：`flow/layers/`（`container_layer`、`clip_rect_layer`、`opacity_layer`、`transform_layer`）、`compositor_context`、`diff_context`。
- **落地**：`core/src/compositor/`（`layer.h`、`layer_tree.h`、`compositor.h`）；暂不做 raster_cache，留接口。
- **验收**：嵌套 clip/transform 的用例渲染正确；preroll 包围盒、diff 区域有 host 测试。

### P3：Widget / RenderObject 树（C++ UI 框架层）

- **目标**：Flutter framework 的 C++ 精简版：Widget（不可变配置）→ Element（挂载点）→ RenderObject（layout/paint）三树分离；布局协议先只做 BoxConstraints 一套。
- **对照阅读**：上游 framework 的协议形状（无源码可读，按协议文档化设计）；渲染出口对接 P2 的 Layer 树。
- **落地**：`core/src/ui/`（`widget.h`、`element.h`、`render_object.h`、`box_layout.h`）。
- **验收**：用纯 C++ 写出一个 counter demo（文本占位 + 按钮热区），布局计算与重建（diff 更新）有 host 测试。

### P4：帧调度器与动画

- **目标**：把"vsync 回调直渲"改为"UI 侧生产 LayerTree 进 pipeline，渲染侧消费"的帧管线；动画器统一驱动 `timeSeconds`；帧各阶段耗时统计接入现有日志。
- **对照阅读**：`shell/common/animator.h`、`pipeline.h`、`vsync_waiter.h`、`flow/frame_timings`。
- **落地**：`core/src/frame/`（`frame_scheduler.h`、`pipeline.h`、`animator.h`）；三端壳从直调 `render()` 改为向调度器喂 vsync 信号。
- **验收**：动画掉帧可观测（frame timings 日志）；benchmark 场景下帧间隔方差可测。

### P5：输入与事件分发

- **目标**：`IApp` 增加事件入口；平台壳把触摸/按键事件注入 core；命中测试沿 RenderObject 树下发。
- **对照阅读**：`shell/platform/` 各端 `platform_view` 的 pointer event 链路、`embedder.h` 的 `FlutterPointerEvent`。
- **落地**：`core/src/frame/input.h`；`IPlatform` 不动，各端 bridge 增加事件翻译；P3 的 counter demo 按钮可点。
- **验收**：三端真机点击按钮计数增加；命中测试有 host 测试。

### P6：文字排版（独立大工程，排在框架稳定后）

- **目标**：字体加载、 shaping、glyph atlas、段落布局。
- **对照阅读**：`txt/src/txt/`（`paragraph`、`paragraph_builder`、`font_collection`）。
- **注意**：依赖链 HarfBuzz/ICU/FreeType 体量大，届时评估"接入现成库"还是"最小自研"，单独出文档评审。

### P7：多线程（可选，默认不做）

- **触发条件**：仅当实测出现"UI 线程被光栅阻塞"的卡顿才启动。
- **对照阅读**：`fml/task_runner`、`message_loop_task_queues`、`raster_thread_merger`。
- **原则**：core 的"单线程无锁"契约是现有优点，不提前破坏。

### 持续项：Vulkan 后端加固（机会主义，不占阶段）

遇到具体问题去 `impeller/renderer/backend/vulkan/` 查答案，按需吸收：

- 内存分配：`allocator_vk`（VMA 封装）。
- pipeline 编译卡顿：`pipeline_compile_queue_vulkan`。
- 设备兼容：`driver_info_vk` 的驱动 workaround 表。
- render pass 合并：`render_pass_builder_vk`。

## 5. 工程纪律

1. **抄架构不抄代码**：flutter-engine 目录只读；所有实现用本项目风格重写，保持 header 注释、命名、`SVP_LOG*` 日志习惯。
2. **每层 host 可测**：新层做成平台无关（参照 log.h 的 header-only 先例），在 `test/` 下配 `test_dl`、`test_compositor`、`test_widget` 等独立 target，复用现有 CHECK + ctest 模式。
3. **每阶段端到端可跑**：不允许"架构到位但 samples 跑不起来"的中间态；每个阶段收尾时三端 samples 必须正常渲染。
4. **文档同步**：阶段完成时更新 `docs/engine-core.md` 的模块清单与能力列表。

## 6. 风险与开放问题（请评审时逐条确认）

- [ ] **参考版本漂移**：flutter-engine 拷贝无版本锁定，后续如需对照升级困难。是否固定记录一个上游 commit 作为参考基线？
- [ ] **P3 的设计自由度**：Flutter framework 是 Dart 的，C++ 版三树分离没有可直接对照的源码，接口设计需先出小型 RFC（可附在本文件后追加章节）。
- [ ] **文字路线**：P6 依赖第三方库栈，与"轻依赖"传统冲突，届时单独决策。
- [ ] **MoltenVK 差异**：iOS 经 MoltenVK 跑 Vulkan，Impeller 的 vulkan 做法（如 descriptor 管理）在 MoltenVK 上有差异，P1/P2 涉及渲染改动时需真机回归。
- [ ] **人力节奏**：每阶段体量差距大（P1 小、P3/P6 大），评审时确认是否按单人节奏排期。

## 附录：参考源码索引

| 主题 | 路径（`third_party/flutter-engine/engine/src/flutter/` 下） |
|---|---|
| Display List | `display_list/dl_builder.h`、`dl_canvas.h`、`dl_op_records.h`、`dl_paint.h` |
| Layer 树 | `flow/layers/`、`flow/compositor_context.{h,cc}`、`flow/diff_context.h`、`flow/raster_cache.{h,cc}` |
| 帧编排 | `shell/common/animator.{h,cc}`、`pipeline.{h,cc}`、`vsync_waiter.{h,cc}`、`flow/frame_timings.{h,cc}` |
| 线程模型 | `fml/task_runner.{h,cc}`、`fml/message_loop_task_queues.{h,cc}`、`fml/raster_thread_merger.{h,cc}` |
| Vulkan HAL | `impeller/renderer/backend/vulkan/`（context_vk、swapchain/、allocator_vk、driver_info_vk 等） |
| Embedder 契约 | `shell/platform/embedder/embedder.h`（renderer config union、task runner 注入、pointer event） |
| 文字 | `txt/src/txt/paragraph.{h,cc}`、`paragraph_builder.{h,cc}`、`font_collection.{h,cc}` |
