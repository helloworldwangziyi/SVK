# 多 Surface 普通联动视图架构方案

> 状态：**待审阅，未批准实施**。
>
> 本文只描述目标架构、职责边界、行为契约和实施顺序，不记录日常 Bug、构建结果或提交历史。

## 1. 目标

将当前 sample 中的多 Surface 联动能力迁移到平台层，形成一个可复用的普通 C++ 视图：

- sample 只新增 `.cpp/.h`，描述页面、URL、颜色和回调。
- Android、鸿蒙分别提供原生宿主、Surface、WebView、手势、动画和生命周期。
- 双端共享同一套 C++ 配置、状态转换和交互契约。
- 后续可以替换为自定义 Tab 控件，不改变联动视图本身。
- 不建设通用 Widget 引擎，不引入字体、复杂渲染或动态 DSL。

## 2. 可行性与主要取舍

该方向可行，因为双端主要联动路径已经通过具体实现验证，当前 C++ 示例也只负责简单的纯色绘制。

但不能只把 Java/ArkTS 文件从 sample 搬到 platform。那样只会改变代码位置，不能解决状态重复、行为分叉和新增 App 仍需原生工程的问题。

推荐方案：

> **一套 C++ 场景状态与普通视图接口 + Android/鸿蒙各自的原生宿主实现。**

统一的是：

- 页面描述。
- 当前页面和各页面滚动状态。
- 外层滚动与内部滚动的切换规则。
- 相邻页切换与跨页直接切换规则。
- 菜单动画进度。
- Surface 会话和旧回调失效规则。

保留平台差异的是：

- Android SurfaceView、WebView、MotionEvent、ValueAnimator。
- 鸿蒙 XComponent、ArkWeb、Swiper、nestedScroll、ArkUI 手势和 animateTo。
- 系统 View 的创建、层级、线程和生命周期。
- dp/vp/物理像素转换。

因此，双端可以统一 API 和外部行为，但不强行统一内部实现。

## 3. 当前实现的问题

### 3.1 联动状态分散

- Android 的 `SurfaceLinkageLayout` 同时管理布局、滚动、Tab、菜单和 WebView。
- 鸿蒙的 `Index.ets` 另维护一套页面状态、Swiper、嵌套滚动和菜单动画。

同一规则修改两次，容易再次出现平台行为差异。

### 3.2 固定 Surface 槽位不适合普通视图

当前 Native Bridge 使用固定 Surface ID 管理 Header、Native 内容和菜单，适合探针，不适合任意创建视图。普通视图必须改为实例与 generation 管理。

### 3.3 sample 仍包含原生工程

当前 sample 包含 Android Activity/Java/Gradle 和鸿蒙 Ability/ArkTS/Hvigor。若不把通用宿主移到平台层，新增 App 就不可能只添加 C++ 文件。

### 3.4 迁移不能夹带行为变化

网页内部手势、滚动接缝、菜单动画和 Surface 重建必须先有明确的基线。迁移过程中不得通过修改交互规则来降低实现难度。

## 4. 目标架构

```text
samples/<app>/
  C++ 页面描述、URL、颜色、绘制和事件回调
                    │
                    ▼
core：平台无关的普通视图协议与状态机
  LinkedViewConfig / LinkedViewState / ILinkedView
                    │
          ┌─────────┴─────────┐
          ▼                   ▼
platform/android          platform/hongmeng
  通用 Activity/ViewHost     通用 Ability/ViewHost
  SurfaceView/WebView       XComponent/ArkWeb
  JNI 适配                  NAPI 适配
  MotionEvent/动画          ArkUI 手势/动画
```

“普通视图”不是新的 Widget 树，而是一个小型、明确的生命周期与交互协议：

- 创建、布局、显示、隐藏、销毁。
- Header、若干内容页和右侧 Drawer。
- Web URL 与 Native Surface 两类内容。
- 页面选择、纵向滚动、Drawer 开关和状态查询。
- 不向 sample 暴露 WebView、ArkWeb、VkSurfaceKHR 等平台对象。

## 5. 职责边界

### 5.1 sample 只负责场景描述

sample 可以提供：

- Header 的 Native Surface 描述。
- Web 页面标题和 URL。
- Native 页面颜色或绘制回调。
- Tab 的标题、顺序和初始选中项。
- Drawer 的颜色、宽度比例和开关回调。
- 必要的业务事件处理。

sample 不提供：

- Java、ArkTS、XML、Ability 或独立原生工程。
- Surface 创建和销毁逻辑。
- WebView 创建和滚动控制。
- 手势拦截、嵌套滚动和动画执行代码。
- JNI/NAPI 或平台类型。

### 5.2 core 负责共享语义

core 可以包含以下候选类型，最终命名在实现评审时确定：

- `LinkedViewConfig`：Header、页面、Tab、Drawer 和交互配置。
- `LinkedViewState`：当前页面、各页偏移、外层偏移、页面切换和 Drawer 状态。
- `ILinkedView`：布局、选择页面、滚动、开关 Drawer、查询状态和销毁。
- `ViewHandle`：一个普通视图实例的身份，不再使用固定槽位代替实例。
- `SurfaceGeneration`：识别 Surface 重建，拒绝旧回调。

core 不依赖 Android、鸿蒙界面 API，不直接创建 Vulkan Surface 或 WebView。

### 5.3 平台层负责真实能力

Android 平台层：

- 通用 Activity 与 ViewHost。
- SurfaceView、WebView 和默认占位 Tab。
- MotionEvent 手势、滚动与动画执行。
- JNI 与 core 视图实例的映射。
- Surface 和 WebView 生命周期。

鸿蒙平台层：

- 通用 Ability 与 ViewHost。
- XComponent、ArkWeb 和默认占位 Tab。
- Swiper、nestedScroll、ArkUI 手势和动画执行。
- NAPI 与 core 视图实例的映射。
- Surface 和 ArkWeb controller 生命周期。

平台层不得出现 sample 的 URL、颜色、合约参数或具体业务名称。

## 6. 普通视图行为契约

迁移前后必须保持以下外部行为一致：

1. TopBar 固定，Header 占满 TopBar 下方的完整视口。
2. Tab 覆盖在 Header 底边，WebView 初始完全位于屏幕外。
3. 最大外层滚动距离为 `Header 高 - Tab 高`，Tab 最终停在 TopBar 下方。
4. Tab 停靠后，继续上滑只滚动当前 WebView 或 Native 内容。
5. 当前内容到顶后继续下拉，整个内容区被拉回，Header 重新露出。
6. 各页面保留自己的文档和滚动位置，不串用状态。
7. 相邻 Tab 使用同一动画进度；跨多个 Tab 直接交换首尾页面，不穿过中间页面。
8. 横向手势使用当前已确认的规则，迁移不得擅自改变。
9. Drawer 与主场景共享同一动画进度，菜单宽度为场景的 `1/4`。
10. Surface 重建后旧 generation、旧事件和旧帧不得生效。

这些规则应尽量由共享 C++ 状态机计算，平台只执行几何、滚动和动画结果。

## 7. sample 的目标形态

以下仅用于说明边界，不是已经确定的最终 API：

```cpp
LinkedViewConfig config;
config.header = NativeContent::color(Red);
config.pages = {
    WebContent{"资讯", noticeUrl},
    WebContent{"关联", relevanceUrl},
    WebContent{"排行", rankUrl},
    WebContent{"F10", f10Url},
    NativeContent{"绿色", Green}
};
config.drawer = NativeContent::color(Yellow);
```

一个 C++ 配置应能在 Android 和鸿蒙生成等价视图。新增第二个 sample 时，只允许修改这类配置和回调，不允许复制平台宿主。

## 8. 分阶段实施计划

### 阶段 0：补齐双端行为基线

先不迁移代码，使用相同操作序列验收：

- Tab 点击、Tab 条横滑、内容区横滑。
- 第一个到最后一个、最后一个回到第一个。
- 上拉、Tab 停靠、内部滚动、到顶下拉。
- 菜单打开、关闭和连续操作。
- 前后台、旋转、Surface 销毁与恢复。

尚未确认的场景必须先得到实际结果，不能直接进入抽象实现。

### 阶段 1：提取纯 C++ 状态机与接口

- 定义普通视图配置、状态、实例 Handle 和生命周期。
- 实现页面切换、滚动边界、Drawer 状态和旧 generation 失效规则。
- 为状态转换添加 host 测试。
- 本阶段不接 Java/ArkTS，不改变现有双端实现。

### 阶段 2：迁移 Android 平台宿主

- 将 `SurfaceLinkageLayout` 的通用能力迁入 `platform/android/`。
- 移除 URL、颜色和 sample 业务数据。
- JNI 从固定 Surface 槽位迁移到视图实例与 generation。
- 由 C++ 配置创建 WebView、Surface 和默认 Tab。
- 保持现有 Android 行为并完成真机回归。

### 阶段 3：迁移鸿蒙平台宿主

- 将 `Index.ets` 的通用场景迁入 `platform/hongmeng/`。
- Swiper、nestedScroll、手势和动画仅作为平台实现细节。
- NAPI 与 core 使用同一套视图实例和状态协议。
- 保持已验证的 Tab、滚动和菜单行为并完成真机回归。

### 阶段 4：收口 sample 与构建入口

- 将通用 Android Activity/Gradle 宿主移到平台维护。
- 将通用鸿蒙 Ability/Hvigor 宿主移到平台维护。
- 平台构建系统通过应用名选择需要编译的 C++ sample。
- 新 sample 不再增加 Java、ArkTS、XML、Ability 或独立原生工程。
- 新路径验证通过后，再删除 sample 中的旧平台源码。

### 阶段 5：验证普通视图复用

新增第二个纯 C++ sample，只改变：

- 页面数量。
- URL。
- Surface 颜色。
- Tab 标题。

验收时必须证明：新增文件只有 `.cpp/.h`，双端均可构建运行，不需要修改平台文件。

## 9. 自定义 Tab 的后续接入

系统 Button 不属于普通视图的核心协议。平台只提供默认占位 Tab，后续自定义控件通过一个小接口接入：

- Tab 标题或图标描述。
- 当前选中项。
- 点击与滑动事件。
- 选中状态和动画进度。

自定义 Tab 只改变展示与事件来源，不改变页面、滚动和 Drawer 的内部逻辑。

## 10. 验收标准

方案落地至少需要同时满足：

1. sample 中不再存在 Java、ArkTS 或平台专属视图代码。
2. 新增 sample 只添加 `.cpp/.h`，双端不需要修改平台文件。
3. 双端行为契约全部通过，尤其是 Tab 切换、滚动接缝和菜单动画。
4. 普通视图不再依赖固定 Surface 槽位，旧 Surface 和旧回调能够失效。
5. 平台层不包含 sample 的 URL、颜色或业务参数。
6. host 状态测试通过，Android 和鸿蒙分别完成构建与设备验证。
7. 抽象后不得出现比当前实现更差的滚动、切换或动画表现。

## 11. 范围限制与主要风险

- 第一版只支持一个 LinkedView 实例，不引入视图池或多实例复用。
- 保留当前多个 WebView 的验证场景，不提前扩展为无限页面或复杂缓存。
- 平台代码总量可能增加，但 sample 会显著简化，平台差异也更容易集中审阅。
- Android MotionEvent 与 ArkUI 手势不能保证内部实现一致，只能统一外部语义。
- 如果迁移导致联动回退，应保留平台适配实现并撤销不合适的公共化，而不是降低验收标准。

## 12. 待审阅决策点

1. 是否接受“共享 C++ 状态机 + 两端原生宿主”的整体方向。
2. 是否接受普通视图先只支持一个实例，暂不设计视图池。
3. 是否接受平台提供默认占位 Tab，后续通过小接口替换为自定义控件。
4. 是否接受将通用 Android/鸿蒙原生宿主工程也移出 sample。
5. 是否同意按“行为基线 → core 状态机 → Android → 鸿蒙 → sample 收口 → 第二个 sample 验证”的顺序实施。
