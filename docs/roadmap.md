# SVK 自有跨端 App 能力 Roadmap

> 状态：**当前评审方向，待按阶段审阅，不构成提前实施授权**。
>
> 本文件以当前讨论的“最小持久 ViewTree、可复用自绘控件、独立导航栈、可复用 SceneHost、
> 平台组合子树、数据驱动 dirty”为准。
> 此前版本与本版不一致时，以本版为后续评审方向；当前联动 Demo 的实现和验收仍以
> `linked-view-architecture.md` 为基线，不因本规划提前重写。

## 1. 产品目标与第一条完整链路

SVK 的目标不是为 Android、鸿蒙分别维护两套页面，也不是复制 Flutter 的源码和三棵树，而是：

- 使用 C++ 描述页面结构、数据状态、业务意图、Native 绘制和事件回调。
- 使用一棵最小持久 ViewTree 表达布局父子、兄弟顺序、绘制、命中和 dirty。
- 在 ViewTree 上逐步提供可复用的 Button、Dropdown、Table 等 Native 自绘控件；控件只通过
  Canvas 绘制，输入和手势归 View，Surface 不监听输入或手势事件。
- 使用独立的 Route 栈表达应用导航；导航父子不冒充布局父子。
- 普通 UI 由现有 Vulkan 路径绘制，WebView、LinkPage 等系统能力由平台宿主接入。
- Surface 生命周期、系统手势、联动执行和转场动画优先交还平台。
- 数据与 UI 分离，变化只使相关节点失效，并在下一帧统一布局、绘制和平台更新。
- 全局环境变化、定向输入、组件语义事件和数据流事件分别处理，不使用无差别 EventBus。
- HTTP 请求和 TCP 长连接都由数据层拥有，不让网络回调直接操作 View、Canvas 或 Vulkan。

第一条完整产品链路为：

```text
启动 App
  → 首页（Native 自绘，底部导航）
  → 切换底部导航
  → 点击首页条目，平台转场进入详情页
  → 点击“多 Surface 联动”，进入当前 Linked Demo
  → 点击 Native 自绘按钮，以浮层形式打开 WebView
  → 关闭浮层、返回详情、返回首页，原页面状态仍正确
```

首页随后接入真实 HTTP 数据。全局环境至少覆盖暗黑/亮色、中英文、折叠屏状态和关爱模式。首个期货看盘样例
后续使用项目自建的模拟行情验证 TCP 长连接和页面行为。网络慢、断线重连、环境变化、页面离开或 App
进入后台时，UI 仍应可响应，且旧回调不得写入新页面。

第一版仍是可验证的最小 App 能力，不提前承诺字体体系、复杂图片、完整商业首页或通用 UI 引擎。

## 2. 当前基础与目标差异

当前工程已有：

- `IApp.onFrame` 和 `ui::Canvas`：生成最小 Native 绘制内容。
- `IRasterSurface`：沿用 Vulkan Swapchain Present，不开放绕过现有路径的输出。
- `IPlatformSurface`、`IWebSurface`：控制系统视图和 Web 导航。
- `ILinkedLayout`、`LinkedSceneSpec`：描述当前 Header、页面列表和右侧菜单组合。
- `LinkedPageState`：处理当前联动布局内部的分页决策和 generation。
- Android、鸿蒙宿主：创建系统对象并执行手势、滚动和动画。

这些仍是单个场景及成员能力，尚不是应用级 ViewTree、导航和数据运行框架。主要缺口为：

| 缺口 | 当前不能直接承担的事情 |
| --- | --- |
| 持久视图结构 | 没有稳定 ViewId、通用布局父子、兄弟顺序和节点 dirty |
| 应用导航 | 没有独立 Route 栈、返回规则和跨页面状态保留 |
| 普通 UI 布局 | 没有约束布局、布局传播和最小节点复用 |
| 可复用自绘控件 | 没有统一的控件属性、交互状态、语义回调和 Button/Dropdown/Table 实现 |
| 自绘输入 | 没有公共 Pointer 输入、命中区域和整段输入会话 |
| 平台组合边界 | 没有把 LinkPage 明确视为平台接管交互的子树 |
| 场景组合与复用 | 当前 SurfaceHost 只承载固定联动场景，没有按 Route 管理 SceneHost、换绑代次和有界复用 |
| 帧调度 | 没有将数据、布局、绘制和平台几何合并为按需帧更新 |
| 全局环境 | 没有主题、语言、折叠状态和关爱模式的统一快照与订阅 |
| 应用转场 | 当前动画只服务固定联动关系，不能承担 Route 进退场 |
| Overlay | 没有统一遮罩、层级、返回优先级和 Web 弹层所有权 |
| HTTP 数据 | 没有请求会话、取消、缓存和迟到结果处理 |
| TCP 长连接 | 没有字节流分帧、连接代次、订阅、背压、乱序和重连恢复 |

后续实现必须保留当前联动 Demo 已验证行为，不能先建设一套庞大框架再寻找使用场景。

## 3. 总体分层与依赖方向

```text
samples/<app>/
  页面构建、数据模型、业务控制器、事件回调、导航意图、数据订阅
                         │
                         ▼
C++ core
  持久 ViewTree / 约束布局 / Canvas / Pointer 命中与输入会话
  可复用 Native 控件 / 控件语义回调
  Dirty 与按需帧请求 / 独立导航状态 / 全局环境快照与订阅
  类型明确的 HTTP、TCP、系统事件和平台内容协议
                         │
             同一 frameId + revision 的一致更新
                         │
                         ▼
Android / 鸿蒙平台适配
  系统输入转换与投递 / ViewId → 系统对象映射 / SceneHost 与 Surface 生命周期
  Web / LinkPage / 嵌套滚动与手势 / 场景根容器转场 / VSync / 系统环境事件 / 异步网络
                         │
                         ▼
             现有 Canvas → IRasterSurface → Vulkan
```

依赖方向保持 `App → core ← platform`：

- core 不依赖 Java View、ArkWeb controller、XComponent、ANativeWindow、OHNativeWindow 或 Vulkan 类型。
- 平台不包含具体 App 页面、业务 URL、固定页面数量或行情业务规则。
- core 的“绘制”只生成 `ui::Canvas` 内容，不直接调用 Vulkan，不另建 DisplayList 或 Layer Tree。
- Native 控件不持有或调用 Surface；系统原始输入只经平台输入适配器转换并投递到 ViewTree，
  Surface 接口不接收输入、手势或业务事件回调。
- 平台可以按节点能力选择系统实现，但不能改变 core 已确定的布局坐标、可见性和事件所有权。
- 平台联动机制不进入 core；语义状态由谁持有必须逐项明确，不能 core 与平台同时可写。

## 4. 持久 ViewTree，而不是 Flutter 三棵树

### 4.1 一棵树的最小职责

ViewTree 是一棵保留节点树，用于表达：

- 稳定 `ViewId` 和唯一布局父节点。
- 有序兄弟节点及其绘制、层级和命中顺序。
- 布局属性、约束、bounds、clip、visible、interactive。
- Native 绘制属性、事件注册和 dirty。
- 需要平台处理的内容配置和组合标记。

它不复制 Flutter 的 Widget、Element、RenderObject 三棵树，不实现通用 diff、动态 DSL 或代码生成器。

普通 Container、Button、文本或图形节点不默认对应系统 View；它们优先绘制在共享的 Native
Surface 中。只有确实需要系统合成或平台能力的节点才创建平台对象。

### 4.2 统一视图身份，但保留类型明确的能力

所有节点共享身份、布局、可见性、事件和 dirty 契约，但平台能力不能完全抹平：

| 标记 | 含义 | 必要能力 |
| --- | --- | --- |
| Native 内容 | 由现有 Canvas/Vulkan 路径绘制 | 内容身份、绘制状态、帧更新 |
| Web 内容 | 系统 WebView/ArkWeb | 初始请求、导航、documentSession、加载事件 |
| LinkPage 组合 | 平台接管内部交互的组合子树 | 页面配置、选择意图、抽屉意图、组合状态 |

不使用“某个可空字段非空就是某种视图”并伴随大量散落的类型判断。公共 ViewNode 持有类型明确的
平台内容配置；无效能力调用返回明确错误，不设计通用字符串命令通道。

Web 普通布局或主题变化不得默认重新加载文档。只有请求或导航 revision 变化才创建新文档会话。

### 4.3 可复用 Native 自绘控件

控件系统是同一棵 ViewTree 上的一组可复用具体节点或组合子树，不另建 Widget/Control 树，也不把
每个控件映射为系统 View 或 Surface。Button 可以是单个节点；Dropdown 由触发节点与 Overlay 子树
组成；Table 由行列布局、滚动、数据快照和可见区域管理共同构成，不能只用一次 `drawTable()` 冒充
完整控件。

每个控件显式分离以下职责：

- 类型明确的 Props、Style 和内容配置，支持直接复用和受控修改，不使用通用字符串属性或命令通道。
- 必要的内部交互状态，例如 pressed；选中项、表格数据等语义状态由 App/控制器持有，同一状态不得
  由控件与业务双方同时写入。
- 布局、命中和定向输入行为；控件接收 ViewTree 投递的手势，并向 App 提供 `onClick`、
  `onSelectionChanged`、`onCellActivated` 等类型明确的语义回调。
- 纯绘制阶段，只根据当前属性、内部状态和不可变快照生成 Canvas，不处理输入、修改业务状态、
  请求数据或直接调用 Surface。

Native 控件的外观全部通过 `ui::Canvas` 绘制到共享 Native Surface，不混用平台 Button、菜单或表格
控件。控件变化只标记 dirty 并等待下一帧，不直接调用 `IRasterSurface.render()`。Native Dropdown 的
弹出子树默认仍共享原 Native Surface；Table 的行和单元格不创建系统 View，大量数据按可见窗口管理，
不无界构建全部行。

“自绘一切”是 Native 控件的渲染边界，不等于当前 Canvas 已具备文本、图片等全部能力，也不要求把
Web、LinkPage 等平台合成内容纹理化或代绘。控件按 Button、具备 Overlay 后的 Dropdown、具备滚动与
可见区域管理后的 Table 逐步落地，不一次性实现全量控件清单。

### 4.4 ViewTree 与导航树分离

两种“父子”必须分开：

- **布局父子**：决定约束、坐标、裁剪、可见性和命中。
- **导航父子/先后**：决定从哪里来、返回到哪里，以及转场期间哪些 Route 保留。

```text
App Route 栈：首页 Route → 详情 Route → Linked Demo Route
每个 Route：拥有自己的 ViewTree 根
TabContainer：其页面可以是同一 ViewTree 中的兄弟节点
```

push 一个 Route 不等于把新页面 add 为旧页面的布局子节点。`RouteId`、`ViewId`、Linked Demo 内部
`PageId`、`SurfaceRef` 和 `NativeContentId` 不得互相替代。

## 5. Flutter 式约束布局与 dirty 帧更新

### 5.1 最小布局模型

借鉴 Flutter 的“约束向下、尺寸向上、父节点决定位置”，但只实现真实页面需要的容器和规则：

- 坐标统一为物理像素、左上角原点；平台负责 px/dp/vp 转换。
- 第一版按首页和详情需要实现 Container、Stack 及必要的 Row/Column 能力。
- 根布局输入包含安全内容视口、窗口尺寸、density 和环境快照。
- 布局结果同时服务 Canvas、平台几何和命中区域，不能分别计算三套坐标。
- 不为假想需求提前实现完整 Flex、文本测量、任意变换或通用布局语言。

### 5.2 dirty 类型与传播

第一版至少区分：

| dirty | 触发示例 | 下一帧工作 |
| --- | --- | --- |
| LayoutDirty | 尺寸、约束、结构、语言、关爱模式、折叠状态变化 | 重排受影响子树及必要祖先/兄弟 |
| PaintDirty | 数据、颜色、主题、按压状态变化 | 重新生成受影响 Native 内容的 Canvas |
| PlatformDirty | Web 配置、平台几何、可见性、交互状态变化 | 更新对应平台对象，不默认重建 |

传播规则必须在 host 中固定：

- 节点尺寸变化可能使父节点和兄弟重新布局。
- bounds、clip 或 visible 变化会影响后代绘制和命中。
- 数据变化只使订阅该数据的节点失效，不全树广播 dirty。
- 同一帧多次修改合并；无 dirty、无平台动画和无待处理事件时不请求无意义的新帧。

第一版采用“脏节点 → 重绘其所在整个 Native Surface”。dirty rect、局部 Swapchain 更新、复杂缓存和
局部布局优化必须等待真实性能数据，不在第一阶段预设。

### 5.3 一帧的顺序

```text
输入 / 数据结果 / 环境变化 / 平台语义事件
  → 在 UI 线程更新对应状态并标记 dirty
  → 合并请求下一帧
  → 应用本轮树结构变化
  → 必要布局与绝对 bounds/clip 计算
  → 生成 Canvas、平台几何和命中区域
  → 以同一 frameId、revision、environmentRevision 提交
  → 平台统一应用几何并走现有 IRasterSurface Present
```

同一联动组合内不得各自推进动画或滚动状态。平台执行动画时，core 不维护第二个可写动画时钟。

### 5.4 数据与 UI 强制分离：架构红线

数据/UI 分离是正确性要求，不是留待后续优化的性能建议。必须通过 API、依赖方向和自动测试强制，
不能只靠注释或开发者自觉。

#### UI 帧链路只能读取快照

- `build`、`layout`、`draw`、`onFrame` 只读取不可变数据快照、环境快照和布局输入。
- 这些阶段只能生成 View 描述、Canvas、几何、命中区域和 dirty 信息，不得产生外部副作用。
- 禁止在这些阶段发起 HTTP/TCP、订阅、重连、认证或大响应解析；即使调用形式是异步也同样禁止。
- 禁止同步 DNS、同步连接、`future.get()`、睡眠、条件变量等待或等待网络线程持有的锁。
- 网络 I/O 永远不在 UI 线程。大响应解析、图片解码和行情批量处理不得默认在 UI 线程，具体数据线程
  方案在对应阶段独立评审，不以独立 UI/Raster 多线程为代价。

#### 缺数据只能是一种 UI 状态

- 页面只渲染 `NotLoaded / Loading / Content / Refreshing / Error / Stale` 等明确状态。
- 请求只能由 Route 激活、用户刷新、重试、订阅恢复等数据控制器行为触发。
- 列表预加载由页面/列表控制器根据明确的可见窗口提前计算，不能在 item 的 build 或 draw 中触发。
- 绘制发现缓存缺失时只能绘制占位或错误状态，不得顺手请求数据。

#### 依赖方向必须阻止绕行

- ViewTree、Canvas、布局模块不依赖 HTTP/TCP 传输实现，传输实现也不依赖 ViewTree、Canvas 或 Renderer。
- 不提供全局 ServiceLocator、全局 App 指针或全局网络单例供绘制函数反向取得服务。
- 网络回调不能持有 View 指针，不能直接调用 `onFrame`、Canvas、Renderer、WebView 或 Surface。
- 跨线程只传递有所有权的不可变结果、快照或批次，并通过有界队列进入明确的执行器。
- 不通过 UI 与网络线程共享一把锁来保护可变页面数据；UI 不得为了读取行情阻塞在数据线程锁上。

#### 数据和 View 的所有权分离

- View 只订阅数据状态，不拥有 socket、连接或传输线程。
- Route 被 pop 时销毁 ViewTree 并注销订阅；共享缓存和 TCP 连接按数据策略独立保留或关闭。
- 旧请求、旧连接和旧订阅结果只能丢弃，不能为了尽快显示而直接写入新 View。

#### 必须自动验证红线

- Fake Transport 统计 `build/layout/draw/onFrame` 期间的请求、订阅和重连次数，必须始终为 0。
- Debug 帧阶段守卫应拒绝并记录绘制链路中的副作用调用，不能只在文档中声明禁止。
- 人为制造慢网络、大响应和 TCP 洪峰，期间导航、返回、主题切换和手势仍须可响应。
- 依赖检查必须证明 UI 模块不引用网络实现，网络实现也不引用 UI/渲染模块。

## 6. 事件体系：定向输入、全局环境、组件语义和数据事件分离

### 6.1 定向输入事件

Native 自绘区域的输入归属抽象 View，不归 Surface：

```text
系统原始输入
  → 平台宿主 / 输入适配器只转换坐标、pointerId、动作和时间戳
  → core 按 Overlay / Route 层级和 ViewTree 顺序命中
  → 目标 ViewNode / 控件处理输入与手势
  → 发出语义回调，更新状态并标记 dirty
```

`IRasterSurface`、`IPlatformSurface` 不提供 `onTouch`、`onClick`、`onPointer` 等输入入口，不保存
命中目标或手势 owner，也不接收业务事件回调。平台系统对象收到原始触摸只是输入适配入口，不代表
Surface 承担交互职责。Surface 重建需要取消输入时，由宿主或输入协调器通知 ViewTree 取消会话，
Surface 自身不参与手势判决。

“点击只通知注册它的视图”作为第一版语义点击规则，不急于实现 DOM 式冒泡。命中链只用于找目标，
不等于所有祖先都收到回调。

一次输入会话至少包含 `pointerId`、目标 `ViewId`、`gestureGeneration`、动作、坐标和时间戳：

1. 从最上层可见 Overlay/Route 向下，按兄弟顺序、bounds、clip、visible、interactive 命中。
2. Down 后固定整段会话目标；Move/Up/Cancel 不因轻微移出而改投其他 View。
3. Up 仅在会话有效且满足触发区域条件时，向目标 View 回调一次点击。
4. 视图删除、隐藏、Route 移除、Surface 重建、多指冲突或系统 Cancel 必须取消会话。
5. 滑动与点击竞争时只有一个最终所有者，不能既滚动又点击。
6. 平台组合子树或 Web 已消费的输入，core 不再合成第二次点击。

第一版只在实际需要时增加未消费回退，不实现无约束事件冒泡和全局输入广播。

### 6.2 平台组件语义事件

Web 加载、LinkPage 分页落定、抽屉完成、Surface 重建等属于特定组件的语义事件：

- 只投递给该组件的所有者或明确订阅者。
- 携带 Host、View/Route/Surface 身份及相应 generation/session。
- 它们不是全局通知，也不直接修改其他页面。
- 迟到事件只记录丢弃原因，不得写入新对象。

### 6.3 数据事件

HTTP 单次结果、TCP 连接状态、行情快照和有序增量各自有明确数据协议：

- 只通知数据所有者或订阅者，不按 ViewTree 广播。
- View 可以订阅数据状态，但网络回调不直接调用 View。
- 可合并的最新快照与不可丢失的有序事件不能使用同一种“只保留最后一个”策略。

## 7. 全局环境通知：类型化状态与按需订阅

### 7.1 不是无差别 EventBus

主题、语言、折叠屏和关爱模式属于**全局环境状态**，不是 Pointer 事件，也不是某个 Leaf View 的点击。
全局性体现在“一个统一状态源可影响多个订阅者”，不体现在“每次变化都调用所有 View”。

```text
平台系统事件 + App 内设置变化
  → core 环境状态唯一写入者
  → 生成不可变 EnvironmentSnapshot + EnvironmentChangeSet
  → 按 changedAspects 找到有效订阅者
  → 更新数据/样式状态并标记相应 dirty
  → 下一帧统一布局、绘制和平台更新
```

平台回调只负责转换为明确类型并投递到宿主 UI 线程，不在回调中逐 View 重绘。

### 7.2 EnvironmentSnapshot 与 ChangeSet

环境快照至少按以下方面分组，具体字段随对应阶段评审：

| Aspect | 典型内容 | 默认失效范围 |
| --- | --- | --- |
| Theme | 暗黑/亮色、系统/App 主题来源、有效主题 revision | 订阅主题的 PaintDirty；尺寸变化时再 LayoutDirty |
| Locale | 当前语言区域、系统/App 语言来源、方向性预留 | 订阅语言的内容与 LayoutDirty |
| Display/Fold | 展开/折叠/未知、窗口尺寸、安全区、density | 根布局和相关子树 LayoutDirty、PlatformDirty |
| Accessibility | 关爱模式、系统字体缩放、必要的触控尺寸策略 | 相关 LayoutDirty、PaintDirty 和命中区域更新 |

每次有效变更携带：

- `environmentRevision` 和 `changedAspects`。
- 一份完整、不可变的新快照，避免同一帧一半读取旧主题、一半读取新主题。
- 来源、事件序号和必要的宿主代次。
- 对不支持或暂时无法确定的能力显式标记 Unknown/Unsupported，不伪造平台能力。

订阅记录至少包含所有者身份、订阅 generation 和关心的 Aspect 集合。View、Route 或数据所有者销毁后
订阅同步失效；只有关心对应 Aspect 的有效订阅者才被通知。

### 7.3 系统状态与 App 覆盖设置的唯一写入者

- 系统暗黑/亮色、系统语言、折叠状态和系统字体缩放由平台转换后输入。
- App 内主题、语言和关爱模式开关由 App 设置状态持有；持久化是独立数据职责。
- 最终有效环境由同一环境控制器根据“系统值 + App 覆盖策略”计算，平台和页面不能分别覆盖。
- 关爱模式不是简单把全局像素尺寸乘一个系数；文本、布局、命中区域和内容密度按实际样例评审。
- 折叠状态不能只凭宽高猜测；平台无法提供姿态时报告 Unknown，窗口尺寸变化仍正常触发布局。

### 7.4 全局变化的帧与生命周期规则

- 多次环境回调可在下一帧前合并，但必须保留最终完整快照和本轮所有 changedAspects。
- 隐藏 Route 可以更新自己的环境相关状态，不得直接 Present 或抢输入；恢复时先同步最新快照。
- 环境变化不得默认重建全部 WebView、TCP 连接或 Route。
- Web 内容是否响应主题/语言由明确的内容桥接协议决定，不以整页刷新代替。
- 转场中发生环境变化时，新旧 Route 使用同一 environmentRevision；平台转场进度仍只有一个执行源。

## 8. 平台组合子树与 Surface 联动

### 8.1 LinkPage 是平台交互所有者

LinkPage 的第一版边界为：

- core 负责其外层安全内容 bounds、内部内容描述、稳定身份和语义配置。
- 平台负责内部系统视图父子关系、Surface 生命周期、命中、滚动、手势、分页和抽屉执行。
- core 不对该平台已接管的区域再次分发原始触摸。
- 业务选中页和抽屉意图若由 core 持有，平台只执行并回报；若由平台持有，core 只观察。
- 纵向偏移、gesture owner、动画进度等平台执行状态不能在 core 再保存一份可写副本。

当前 `LinkedPageState` 可继续承担已验证的内部分页语义，但不能扩张成应用 Route 栈。

### 8.2 安全区、几何和帧一致性

- 页面外层宿主先生成安全内容视口，ViewTree 和各 Surface 不分别查询、叠加安全区。
- core 必须保留平台中立的 bounds、clip、visible、interactive，否则无法完成布局和事件命中。
- 同一 LinkPage 的一次更新必须使用同一状态 revision；平台负责让相关系统 Surface 一致应用。
- WebView 保持系统合成，不纹理化、不像素回读、不用原生位图代绘。
- Surface 重建使旧 generation 失效，Web 异步事件继续校验 documentSession。

### 8.3 前后台、隐藏和关闭

暂时隐藏与永久关闭分离：

- 隐藏：停止新输入，取消/落定手势和动画，保存必要状态，停止可见帧调度，按平台规则挂起。
- 恢复：同步最新环境、Route、数据 revision 和 Surface generation，生成一帧完整画面后恢复输入。
- 关闭：先失效事件、订阅、请求和组合引用，再在安全时点释放系统与 GPU 资源。
- `close()` 幂等且为终态，不能在同一对象上重新 open；重新进入需要新对象或明确的未关闭恢复路径。

## 9. 应用导航、Tab 与转场

### 9.1 导航事务

`push`、`pop`、`replace` 和底部栏目选择形成完整事务：

1. 校验目标、参数、当前状态和重复操作；无效输入不修改现有状态。
2. 分配 RouteId/transitionGeneration，决定离场、入场成员和输入归属。
3. 入场成员准备系统对象和首个有效尺寸，网络文档或行情首包不作为动画开始前提。
4. 转场期间新旧 Route 都保持有效，只有目标 Route 消费输入。
5. 完成后冻结被覆盖 Route 的可见绘制和交互，按策略保留其数据与滚动状态。
6. 被移除 Route 先失效输入、订阅和异步回调，转场结束后再释放成员。

快速重复操作必须明确拒绝、合并到最新目标或保留一个有界待处理意图，不能按点击次数无限压栈。

### 9.2 平台执行转场

- core/App 持有逻辑 Route 栈和导航结果，平台持有原生页面与转场对象，不各自决定导航结果。
- Android、鸿蒙分别使用合适的原生页面/容器转场能力，动画优先作用于整个公共父容器。
- 不为每个 Surface 单独启动转场，不在 core 增加并行动画时钟。
- 首次进入可显示普通占位，Native 首帧准备后展示；不等待远端 Web 或行情数据。
- 返回开始先停止输入和惯性，最终资源关闭等待退出完成；异常跳过动画也不能跳过释放。
- 共享元素、Surface 截图或纹理化动画必须单独评审，不能违反 Web 系统合成约束。

### 9.3 返回和 Overlay 优先级

系统返回按可见层级处理：

```text
模态 Overlay
  → 当前 Route 自己公开的可返回状态
  → App Route pop
  → 交还系统默认行为
```

被遮挡 Route 不响应返回、手势或数据驱动的可见动画。底部栏目第一版共享一个 Route 栈，不为每个
栏目提前建立独立历史栈。

### 9.4 SceneHost 场景组合与有界复用

导航单位是 Route 及其 SceneHost，不是单个 Surface。纯 Native、Native+Web、LinkPage 组合场景共用
场景配置、成员生命周期、输入归属和转场协议，但不要求相同数量的物理 Surface，也不把所有页面强行
塞入当前 `ILinkedLayout` 的分页、Tab 停靠和抽屉语义。

- core/App 持有逻辑 Route、ViewTree 和类型明确的场景内容配置；平台 SceneHost 拥有系统成员并维护
  `ViewId → Surface/组合成员` 映射。ViewTree 仍是唯一页面结构，不另建通用 Surface 树。
- 顶栏、底部导航、菜单和其他自绘控件属于场景内容，随 SceneHost 根容器一起转场；平台转场只作用于
  新旧 SceneHost 根容器，不为内部 Native、Web 或未来视频成员分别启动动画。
- push/pop 转场期间，离场与入场 SceneHost 必须同时有效。入场可以先显示占位，不等待 Web 文档、
  行情首包或视频首帧；转场未完成时不得把离场成员换绑给新 Route。
- Route 逻辑状态与可复用物理 Host 分离。每次换绑使用新的 `hostBindingGeneration`，旧输入、订阅、
  文档和异步回调同步失效。只有退出转场、解除 Route 绑定且不再承担保留现场职责的 Host 才能进入
  有界复用；复用不得改变导航结果或绕过资源释放。
- 被覆盖 Route 若要求保留 Web 文档、滚动位置等现场，不能将其成员交给其他 Route；若为降低资源占用
  而解绑，只保留逻辑状态，恢复时按明确策略重建或重新导航，不伪造文档仍然存在。

高频 K 线场景可以评审“最多保留一个兼容 KlineHost”的专项策略。同一 K 线 Route 内切换合约时复用
现有 Native/Web Surface，只更新 `InstrumentId`、行情订阅代次、不可变数据快照以及 Web 请求 revision/
`documentSession`，不因切约重建场景或 Surface。K 线跳转到资讯等其他场景时仍按新旧 SceneHost 转场，
不能提前复用仍在退场或保留返回现场的 K 线成员。

后续视频内容沿用同一场景模型，但必须先评审类型明确的 `VideoContent`、播放会话所有权和双端能力。
系统视频组件与解码纹理进入 Vulkan 是不同实现路径，需分别探针生命周期、合成、GPU 同步和资源释放；
SceneHost 只增加相应平台适配，不引入通用字符串命令通道，也不预设两端能力等价。

## 10. HTTP 请求与数据 UI 分离

### 10.1 职责分层

| 层级 | 责任 | 禁止事项 |
| --- | --- | --- |
| 平台 HTTP 适配 | 异步传输、超时、取消转发、状态码和传输错误 | 不直接修改 View、Canvas 或 Surface |
| 数据仓库 | 数据模型、缓存、刷新、错误归类和重试策略 | 不持有系统 View 或控制器 |
| 页面状态/控制器 | 发起加载意图、保存当前请求身份、管理订阅 | 不在绘制中触发副作用 |
| ViewTree/Canvas | 根据一致状态快照生成布局、图元和命中区域 | 不等待网络、不解析大响应、不接收传输线程回调 |

### 10.2 请求身份和迟到结果

```text
Route 激活 / 用户刷新
  → 数据所有者分配 requestId / generation
  → 平台异步 HTTP
  → 不可变结果投递到宿主 UI 线程
  → 校验所有者、订阅、requestId、generation、revision
  → 一次性更新数据状态并标记订阅节点 dirty
  → 下一帧统一更新
```

取消是尽力而为，不等于服务器停止处理。页面退出后即使传输取消失败，迟到结果也不能写入新页面或
另一代请求。刷新失败时按策略保留可用旧内容，不突然清空有效 UI。

真实 HTTP 实现前，先用 Fake Transport 验证延迟、乱序、失败、取消和销毁；网络库、JSON、TLS 和证书
方案必须单独确认，不默认引入新依赖。

## 11. TCP 长连接与行情数据

### 11.1 独立于 View 和 Route 的四层边界

```text
平台 TCP Transport
  字节收发、连接/关闭、超时、TLS/代理由对应阶段确认
        ↓
协议分帧与解码
  半包/粘包、长度校验、消息类型、序号、心跳、错误帧
        ↓
MarketDataStore / 数据所有者
  连接状态、订阅集合、快照、增量、重连恢复、数据 revision
        ↓
View 订阅与 dirty
  只读取有效数据状态，下一帧更新相关节点
```

TCP 连接默认属于 App 或明确的数据会话，不属于某个 View。Route 离开可以退订页面订阅，但是否保留
连接由数据策略决定；最后一个订阅者离开、App 隐藏和 App 销毁分别采用明确策略，不能偶然随 View 销毁。

通用 TCP 协议不内置行情业务语义。行情协议、 symbol 订阅和快照恢复放在 App 数据层，平台只负责传输。

### 11.2 连接状态机与身份

每个连接至少有：

```text
Idle → Connecting → Connected ↔ Reconnecting → Closing → Closed
                     └→ Degraded / Resyncing（协议需要恢复时）
```

每次连接和每次逻辑订阅分别携带：

- `connectionId`、`connectionGeneration`。
- `subscriptionId`、订阅代次和 symbol/主题集合。
- 消息来源序号、数据 revision、必要的行情交易日/会话身份。
- 心跳、最后接收时间和可诊断的连接状态原因。

旧连接、旧重连任务、旧认证结果和旧订阅回调必须失效。重连成功后使用新 generation，不允许旧 socket
的迟到数据继续写入 Store。

### 11.3 TCP 字节流分帧

TCP 没有消息边界，必须按实际协议处理：

- 一次读取只有半个包头、半个消息体。
- 一次读取包含多个消息或跨多次读取的拼接。
- 固定头、长度前缀或协议约定的其他帧边界。
- 长度溢出、负值/异常值、超过最大帧、未知消息类型和不可恢复协议错误。
- 明确的字节序、编码、压缩和版本协商；不凭经验默认。

读缓冲和单帧大小必须有上限。协议错误不能静默吞掉；无法继续确定帧边界时关闭并进入有日志的恢复流程。

### 11.4 心跳、超时和重连

- 心跳、ping/pong、读写空闲超时和登录/认证超时按服务端协议定义。
- 网络可用回调不等于连接健康，最终仍以连接状态、心跳和读错误综合判断。
- 自动重连采用有上限或有明确退避的策略，并加入抖动，避免多客户端同步重连。
- 页面切换不能为每个页面各建一条相同连接；共享连接由数据所有者统一管理。
- 非幂等业务命令不得因超时被自动重放；行情订阅恢复与订单/交易命令是不同策略。
- App 后台时是降低订阅、保持、主动断开还是等待系统回收，必须由产品策略明确，不默认无限保活。

### 11.5 快照、增量、顺序与缺口

行情数据至少区分两类：

| 数据 | 允许的合并 | 一致性要求 |
| --- | --- | --- |
| 某 symbol 最新快照/最新价 | 可按 symbol 合并到下一帧，只保留最新有效 revision | 旧 revision 不得覆盖新值，重连后先建立可信基准 |
| 逐笔、K 线增量、订单或其他有序事件 | 不得简单只保留最后一个 | 校验来源序号、重复、乱序和缺口，必要时重新同步 |

若协议采用“快照 + 增量”：

1. 重连或订阅建立后进入 Resyncing。
2. 获取带序号的基准快照，并按协议缓存或对齐后续增量。
3. 只应用连续且属于同一数据会话的增量。
4. 发现缺口、重复无法判定或快照过期时，将数据标为不可信并重新拉取，不能拼接出看似完整的错误行情。
5. 恢复可信前，UI 显示陈旧/恢复中状态，不把旧数据伪装成实时数据。

### 11.6 背压与 UI 合帧

网络速度可能远高于 UI 刷新率，不能为每个 TCP packet 或每个价格变化都投递一次 UI 回调：

- I/O 线程到数据层使用有界队列和不可变结果/批次，所有权清晰。
- 可合并快照按 symbol 合并；有序事件保持顺序，不能为降低队列长度随意丢弃。
- UI 按下一帧合并数据 revision，只有订阅相关数据的 View 被置脏。
- 隐藏 Route 可以按策略更新 Store，但不触发可见绘制。
- 有界队列达到高水位时，快照流按声明的 lossy 策略合并；有序流必须采取限读、断开重同步或其他明确
  背压策略，不能静默丢弃后继续标称实时。
- 记录合并、丢弃、重同步、队列高水位和 UI 落后原因，不记录无界行情负载。

### 11.7 线程和性能边界

- 平台网络库在自己的 I/O 线程执行，ViewTree、Canvas 和 Renderer 仍在宿主 UI 线程顺序执行。
- 普通页面数据 Store 默认由 UI 线程持有；高负载 MarketDataStore 可以在阶段 7 经独立设计后由数据线程
  封闭持有，UI 只接收不可变投影/快照，不共享需要加锁读取的可变行情状态。
- 跨线程只传递有所有权的字节缓冲、不可变帧或不可变数据批次，不捕获裸的已销毁 C++ 指针。
- 分帧和批量处理不得把每个字节或每个 packet 都无界投递到 UI 线程。
- 大响应解码和行情批量处理不得默认在 UI 线程；具体数据线程方案保持序号、所有权和有界队列，不在公共
  UI 方法中零散加锁。
- TCP I/O 和行情数据线程不等于引入独立 UI/Raster 多线程，现有 Vulkan 线程模型不变。

### 11.8 模拟行情与网络验收

首个看盘场景使用项目自建的确定性模拟行情。固定合约资料、交易日历、历史 K 线、报价、盘口快照和
增量，以及消息与故障的注入时序，使同一用例可在 host 和双端设备重复运行。看盘界面明确标注模拟行情。

- 实施前确认模拟协议的帧格式、序号作用域、快照恢复、心跳、超时、网络权限和重连策略。
- host 使用 Fake Transport 验证半包、粘包、乱序、重复、缺口、洪峰、慢 UI、断网、重连和销毁；
  双端设备通过实际 TCP 连接接入自建模拟服务，回放相同业务用例并验证网络和画面行为。
- 外部行情接入另行评审，届时确认服务协议、行情权限、TLS、证书、认证和代理。日志不得输出认证信息、
  完整消息、隐私数据或无界负载。

### 11.9 期货看盘的行情正确性约束

通用 TCP 正确性不等于期货业务正确性。期货看盘至少还要满足以下约束。

#### 合约和数据查询身份

- 使用稳定 `InstrumentId` 区分交易所、合约和必要的品种信息，不能只用可能重名的显示 symbol。
- `MarketDataQuery` 至少包含 InstrumentId、数据类型、周期/深度和来源；View 订阅查询，不订阅裸 socket。
- 主力连续、指数等合成合约与真实可交易合约是不同身份；换月映射使用独立 mappingRevision，不能把
  新映射的旧数据或另一合约的 tick 静默拼入当前序列。
- 多路行情源使用 `sourceId` 和各自序号。主备切换后不得把一个来源的序号直接套到另一个来源。

#### 价格、数量和时间

- 权威价格使用定点整数、十进制表示或协议原始精度及 `priceTick` 保存，不能用 `double` 做价格相等、
  涨跌停、排序或结算判断。绘制坐标转换可以使用浮点，但不能反写为权威行情。
- 数量、持仓量和成交量使用能满足协议范围的整数/十进制类型，明确单位和乘数。
- 同时保留来源时间、设备单调接收时间和必要的交易日，不用设备本地墙上时间给网络消息排序。
- 数据源须定义涨跌及涨跌幅基准、舍入口径和缺值处理。报价列表和详情使用同一口径；缺值与真实零值
  必须可区分。界面展示报价对应的行情时间。
- 夜盘跨自然日时仍按交易所交易日和 session 归属，不得按本地日期把同一交易时段拆成两天。
- 集合竞价、连续交易、休市、午间休息、夜盘、收盘和结算等状态由交易日历及协议共同判断；休市期间
  没有新 tick 不等于断线，不能仅因行情静默就盲目重连。

#### 实时、延时和陈旧状态

- TCP Connected 只表示传输连通，不代表已认证、已订阅、行情权限有效或数据实时。
- 数据状态至少区分 Connecting、Resyncing、Live、Stale、Delayed、Error 和 Unsupported；UI 不得把
  旧快照在断线或重同步期间伪装为 Live。
- 行情权限、延时等级和来源能力属于数据状态。仅有延时权限时展示并标明延时行情；不可用时明确标记。
- 休市、延时、陈旧和恢复中在界面上可区分。保留旧值时显示其行情时间和可信状态。可信状态按查询判定，
  例如盘口发生序号缺口时，仍可信的报价可以继续更新，盘口明确显示恢复中。
- Stale 判断结合交易时段、来源时间、最后有效消息和心跳，不使用单一固定超时误伤正常休市。

#### 盘口、逐笔和 K 线连续性

- 盘口快照与增量必须校验来源序号、深度、重置标记及协议提供的 checksum；缺口或校验失败后整个盘口
  进入 Resyncing，不能只补一笔后继续显示。
- 更正、撤销、重置和重复消息按实际协议处理，不能假设所有行情都是只追加的新 tick。
- K 线以 InstrumentId、周期、交易日/session 和 bar 开盘时间等形成稳定 BarId；实时未完结 bar 与历史
  已完结 bar 明确区分。
- 按交易日历和数据协议区分无成交时段与应到未到的数据；缺口须显式标记，避免生成看似完整的 K 线。
- HTTP 历史 K 线与 TCP 实时 K 线并行到达时，按 BarId、序号和 revision 对齐，不能出现重复 bar、缺失 bar
  或旧 HTTP 响应覆盖更新的实时 bar。
- 换月、复权/拼接策略和交易日历变化必须产生可追踪的数据 revision，UI 不得无提示混合不同口径。

### 11.10 期货看盘的多视图一致性与性能约束

#### 同一画面读取同一份行情快照

- 报价列表、详情顶部、盘口、K 线和 LinkPage 内相关 Surface 必须读取同一次 Store 提交的不可变快照，
  使用共同 `storeRevision` 和 `frameId`。
- 不允许每个 View/Surface 各自请求行情、各自推进最新价或各自保存一份互相竞争的 K 线状态。
- 一帧内可以只更新受影响 View，但不能让同一 frame 的两个 Surface 分别显示新旧两个价格。
- 数据可信状态也属于同一快照；同时展示同一查询的视图必须给出一致的状态。
- 双端设备检查系统实际合成的画面。对详情最新价与图上最新价标记等重复字段，验收前确定 Surface 间
  可见更新的最大帧差和超界时的显示策略。

#### 订阅、缓存和绘制必须有界

- 订阅管理器按 MarketDataQuery 引用计数和合并，列表 cell、详情和图表不能分别建立相同 TCP 连接。
- 可见列表及预取窗口驱动订阅变化；隐藏 View 可以取消 UI 订阅，但是否保留底层数据订阅由数据策略决定。
- tick、逐笔、盘口和 K 线历史使用有界缓存、分页或环形结构，不能随连接时长无限增长。
- 最新价按 symbol 合帧；列表、盘口和图表可以有不同的更新预算，但都必须遵守数据顺序和可信状态。
- 涨跌闪烁、颜色和过渡动画不能为每个 tick 排队；中间已不可见的闪烁应合并，动画使用统一时钟。
- K 线聚合、指标计算和大批量解码不得默认占用 UI 线程；需要数据线程时只发布不可变投影/快照。

#### 必须建立可测量的行情预算

首个模拟看盘场景实施前，按目标设备、合约数量、消息速率和刷新率确定：

- 设备单调接收时间到画面提交的延迟分布，至少观察 p50/p95/p99；来源到设备的延迟单独评估，不能把
  不同步的两台机器墙上时间差当作准确端到端延迟。
- UI 帧耗时、卡顿帧、每秒实际 Present、列表/盘口/K 线各自的更新频率和最大允许落后。
- 行情洪峰下切换合约、滚动列表和拖动图表的输入到画面响应时间。
- 各队列深度、高水位次数、快照合并率、有序流重同步次数和恢复耗时。
- 长时间连接下的内存、缓存条目、订阅数、Surface 数和连接数是否稳定。
- 断网、主备切换和缺口后的恢复时间，以及恢复前是否始终明确显示非 Live。

未达到事先约定的正确性与性能预算时，只能宣称链路可运行，不能宣称期货看盘高负载流畅或实时可靠。

### 11.11 首个看盘样例的页面验收

- 快速切换合约时，标题、报价、盘口和图表同步切到目标合约的当前数据或加载状态。源合约的迟到响应
  不得写入目标页面。返回报价列表时保留原滚动位置。
- 用户拖到历史区间或使用十字光标时，实时更新保留当前视窗和选中的 bar；用户可主动回到最新位置。
- 首个自绘报价页验证价格、涨跌符号和状态文字在双端正常和大字模式下的可读性。涨跌及状态使用符号或
  文字辅助颜色表达，读屏可访问合约名、报价、行情状态和基本操作。
- 回放覆盖夜盘跨日、无成交与数据缺口、历史回补遇到实时更新、快速切约和断线恢复。每个用例写明
  预期报价、盘口、K 线与可见状态，供 host 检查业务结果，双端设备检查实际画面与交互。

## 12. 身份、所有权、线程与日志

### 12.1 身份不能混用

| 身份 | 用途 |
| --- | --- |
| HostId | 一个平台 App 根宿主及运行时 |
| RouteId / transitionGeneration | 一个逻辑页面及一次导航事务 |
| hostBindingGeneration | Route 与可复用 SceneHost 的一次绑定，换绑后旧事件失效 |
| ViewId / viewGeneration | 一个稳定布局/事件节点及其实例 |
| OverlayId / generation | 一个浮层及打开、关闭过程 |
| SurfaceRef / surfaceGeneration | 一个逻辑系统成员及实际窗口代次 |
| PageId | LinkPage 内部稳定分页身份 |
| NativeContentId | App Native 内容映射，不是平台资源所有权 |
| documentSession | 某个 Web 对象的文档导航 |
| pointerId / gestureGeneration | 一段触摸及其归属 |
| environmentRevision / subscriptionGeneration | 一份全局环境快照及一个环境订阅 |
| requestId / requestGeneration / dataRevision | 一次 HTTP 请求及结果有效性 |
| connectionId / connectionGeneration / subscriptionId | 一条 TCP 连接及一个逻辑数据订阅 |
| InstrumentId / MarketDataQuery | 一个真实/合成合约及一类行情数据订阅，不是显示名称或 socket |
| sourceId / streamSequence / tradingDay | 一路行情来源、其消息序号及交易所交易日 |
| storeRevision | 一次行情 Store 一致提交，供同一画面的所有 View 读取 |
| frameId | 一次状态、Canvas、平台几何和环境 revision 的共同提交 |

成员销毁遵循“先失效关系和回调，再释放资源”。转场需要保留旧成员到退出完成，但保留绘制不等于继续
接收输入。GPU 资源继续遵守现有完成与释放顺序。

### 12.2 UI 线程与有界异步

- App、ViewTree、导航、环境状态、普通数据 Store、平台成员和 Vulkan 默认只在宿主 UI 线程顺序调用。
- 网络、系统和 Web 回调通过显式 UI 投递进入，不直接修改上述对象。
- 阶段 7 若采用数据线程封闭的 MarketDataStore，必须保持单一线程所有者，并只向 UI 发布不可变投影/快照；
  这不是共享可变 Store 加锁的许可。
- 事件队列、网络队列和待处理导航意图必须有界、可失效。
- 不使用全局 App 指针、无界 EventBus、无界任务队列或全局锁隐藏生命周期问题。

### 12.3 日志

沿用现有 `SVK_LOGV/D/I/W/E` 和结构化字段，按阶段补充：

```text
route_id, view_id, overlay_id, transition_generation, host_binding_generation,
navigation_result, pointer_id, owner_id, environment_revision, changed_aspects,
request_id, request_generation, data_revision,
connection_id, connection_generation, subscription_id,
stream_sequence, queue_size, resync_reason,
frame_id, surface_generation, lifecycle_state, result, reason
```

Info 只记录低频状态转换；动画逐帧、MOVE、行情更新和高频环境变化使用 Debug/Verbose 的限流、去重或
采样。日志必须能回答“哪个页面、哪个节点、哪次环境变化、哪条连接、哪一代 Surface、为何接受或丢弃”。

## 13. 目录演进方向

以下只表达依赖方向，不要求立即创建全部目录：

```text
samples/<app>/
  app.cpp、pages/、data/          # 页面、业务控制器、数据模型、行情协议和订阅

core/src/
  ui/                            # 后续按阶段审阅的 ViewTree、布局、输入和 dirty
  ui/controls/                   # 可复用 Native 自绘控件，不包含平台系统对象
  app/                           # 独立导航、Overlay、环境状态和帧协调
  services/                      # HTTP、TCP、系统事件等类型明确的协议
  platform_surface.h             # 现有系统成员控制
  web_surface.h                  # 现有 Web 成员协议
  linked_layout.h                # 现有 LinkPage 组合基础
  linked_page_state.*            # 只承担 LinkPage 内部分页
  raster_surface.h               # 保持 Native 绘制边界

platform/android/
  根宿主、SceneHost、帧调度、系统环境、网络适配、Surface、LinkPage、转场

platform/hongmeng/
  与 Android 同职责的鸿蒙实现，保留 ArkUI 的真实父子关系和线程要求
```

平台包不得出现具体首页 URL、业务标题、固定页面数量、行情 symbol 或认证配置。新增普通业务页面原则上
只修改 sample 的 C++ 业务文件和必要资源。

## 14. 分阶段实施路线

每一阶段都提供可运行样例、host 回归、Android 与鸿蒙构建，以及对应设备场景。阶段完成只表示该范围
验收通过，不自动获得下一阶段或提交授权。

### 阶段 0：收口当前多 Surface 基线

目标：确认当前 Linked Demo 可以作为独立 Route 嵌入。

- 完成安全区、独立页面入口、隐藏/恢复、最终关闭和反复进入退出收口。
- 保留网页内部横滑优先、普通区域切页、Tab 停靠、绿色页滚动和右菜单行为。
- 验证 Android、鸿蒙真实设备上的 Surface 重建、旧回调和资源释放。
- 明确 ViewTree 方案与已批准 `linked-view-architecture.md` 的迁移衔接，不提前重写现有场景。

出口：当前行为可对照，双端缺口有证据，后续 ViewTree 接口获得单独审阅。

### 阶段 1：纯 C++ 最小 ViewTree、布局、dirty 与环境模型

目标：不依赖平台视图建立可测试的核心结构。

- 实现稳定 ViewId、唯一父节点、兄弟顺序和最小节点配置。
- 实现 BoxConstraints、必要的 Container/Stack/Row/Column、物理像素布局。
- 实现 Layout/Paint/Platform dirty、传播规则和按需帧请求合并。
- 实现独立 Route 身份，不把导航父子写入 View.parent。
- 实现 EnvironmentSnapshot、ChangeSet、Aspect 订阅和失效规则。

Host 覆盖：约束布局、嵌套裁剪、dirty 传播、重复更新去重、节点删除，以及主题、语言、折叠、关爱模式
分别只使相关订阅者失效。

出口：核心纯逻辑稳定，不创建任何 Java/ArkTS 系统对象。

### 阶段 2：最小 Native 页面、绘制与定向输入

目标：完成一个根 Native Surface 的数据到画面闭环。

- 普通容器、可复用 Button 控件和图形共享根 Surface，不为每个 View 创建系统 View 或 Surface。
- 固化控件类型明确的 Props/Style、内部交互状态、语义回调和纯绘制阶段边界。
- 实现 Down/Move/Up/Cancel、pointerId、目标回调、按压和取消；Pointer 由 ViewTree 分发到 View，
  Surface 不提供输入或手势回调。
- 数据变化只置脏相关节点，下一帧重绘受影响 Native Surface。
- 静止时不持续请求无意义的新帧，Surface 重建不丢失逻辑状态。

先完成 Android 设备和 host 验收，再进行鸿蒙同场景验收；覆盖慢点、快点、移出、Cancel、多指、旋转、
安全区、前后台和重建。

出口：双端最小首页骨架可用，文本未进入前只宣称几何 UI。

### 阶段 3：双端全局环境通知落地

目标：让真实系统和 App 设置驱动同一环境快照。

- 接入暗黑/亮色、中英文、窗口/折叠状态和关爱模式的平台输入或 App 设置。
- 验证系统值与 App 覆盖策略的唯一写入者、revision 和订阅失效。
- 主题只重绘必要内容；语言、折叠和关爱模式触发布局及命中区域更新。
- 不重建无关 Web、Route 或网络连接。

出口：双端在首页、详情和转场中切换上述环境，画面、布局、触摸和订阅状态一致，无重复通知和旧订阅回调。

### 阶段 4：独立导航栈与平台转场

目标：跑通简单 Native 首页、底部栏目、详情和返回。

- 建立独立 Route 栈、底部选中状态、系统返回优先级和重复导航策略。
- 按 Route 创建或绑定 SceneHost，以整个场景根容器执行转场；顶栏、导航和其他成员不分别启动动画，
  core 不另行动画时钟，转场期间新旧 SceneHost 均保持有效。
- 被覆盖 Route 停止交互和可见绘制，按策略保留数据、ViewTree 和滚动状态。
- 转场中发生主题、语言、折叠或关爱模式变化时，新旧 Route 使用同一环境 revision。

出口：双端逐层进入、逐级返回；覆盖快速点击、进入中返回、退出中再进入、前后台和连续至少 20 次进出。

### 阶段 5：Web 节点、LinkPage 组合与 Overlay

目标：将当前联动 Demo 接入新 ViewTree/Route，而不重写平台联动机制。

- 先验证单 Web 节点，普通帧更新不重建或重载文档。
- 将当前 Demo 迁移为一个 LinkPage 平台组合子树，保留双端手势、滚动、分页和抽屉执行。
- 建立 Overlay 的遮罩、层级、输入门控、返回优先级和成员所有权。
- 基于 Overlay 实现 Native Dropdown 控件；弹出子树默认共享原 Native Surface，覆盖外部点击、返回、
  取消和选择回调。
- 自绘按钮可打开底部 Web 弹层，加载失败可提示、重试和关闭。

出口：完整回归慢拖、快滑、斜滑、惯性、取消、Tab 停靠、到顶回拉、抽屉、前后台、Surface 重建和反复进出；
不出现 core 与平台双滚动、双动画或重复点击。

### 阶段 6：HTTP 数据驱动首页

目标：让首页使用真实异步数据，同时保持导航和环境切换可响应。

- 先实现 Fake Transport、请求 token、缓存边界、UI 线程结果投递和数据订阅。
- 再接入一个真实只读 HTTP 接口，支持首次加载、刷新、保留旧内容、失败重试和迟到结果丢弃。
- 测量 UI 线程耗时后再决定大解析和图片解码的线程边界。

出口：host 覆盖成功、失败、乱序、重复、取消和销毁；双端覆盖慢网、断网、刷新中导航、请求中环境变化
和前后台，确认回调不直接绘制。

### 阶段 7：模拟期货看盘与行情 TCP 长连接

目标：以自建模拟行情验证首条看盘链路和长连接，保留已完成的普通页面行为。

- 先确定模拟协议、连接所有权、订阅集合、心跳、序号和快照恢复语义，以及样例所需的最小文本绘制、
  图表交互和读屏能力。
- 评审最多保留一个兼容 KlineHost 的有界复用策略，明确转场占用、覆盖保留、解绑和最终关闭条件。
- 同一 K 线 Route 内切合约复用 Native/Web Surface，只切换合约身份、订阅代次、数据快照和 Web 文档会话；
  旧行情与旧网页回调不得写入新合约。
- 使用 Fake Transport 验证半包、粘包、分帧上限、心跳、重连、订阅代次和旧连接失效。
- 验证快照合帧、有序事件、缺口重同步、有界队列、背压和慢 UI。
- 双端连接自建模拟服务，明确后台策略，不自动重放非幂等业务命令。

出口：Android、鸿蒙跑通“报价列表 → 切换合约 → 同屏查看盘口和一种周期的 K 线 → 断线并恢复 → 返回列表”，
达到第 11.9 至 11.11 节的行情正确性、实际画面、交互和性能要求。双端覆盖断网、弱网、洪峰、重复/乱序/缺口、
重连、页面切换、后台、关爱模式/主题切换和 App 销毁，无旧连接写入、无无界队列、无错误行情被标为实时。

### 阶段 8：按真实需求扩展与优化

目标：在完整链路上提高表达力，而不是回头重写已验证能力。

- 根据首页真实内容评审文本、图片、列表、主题资源和可访问性细节。
- 在滚动、文本和可见区域构建能力具备后实现可复用 Table；按可见窗口管理行列和单元格，不为
  单元格创建系统 View，不无界构建全部数据行。
- 根据性能数据选择局部布局、dirty rect、缓存和可见项构建。
- 根据产品设计增加共享元素、弹性、错峰和自定义绘制效果。
- 视频需求进入时先评审 `VideoContent`、播放会话和 SceneHost 复用结构，分别探针系统视频组件与
  Vulkan 纹理路径；复用通用场景生命周期，不另建 Surface 树，不预设双端能力等价。
- 每项复杂平台 Surface 效果先做双端探针；不支持的能力明确降级或拒绝。

出口：新增普通业务样例只修改 App 的 `.cpp/.h` 和必要业务资源，不需要修改平台壳。

## 15. 验证、性能与阶段审阅点

### 15.1 分层验证

- host：ViewTree、布局、dirty、命中、控件状态与语义回调、环境订阅、导航事务、HTTP 状态、TCP 分帧和恢复。
- 平台构建：Android assemble/lint，鸿蒙 HAR/HAP 与平台模块打包。
- 双端设备：真实命中、系统合成、滚动、转场、Web、环境变化、前后台和 Surface 重建。
- 数据测试：Fake Transport 使用确定性字节流、延迟和错误；设备 TCP 验收使用自建模拟服务和可回放行情。

host mock 只能证明核心状态正确，不能证明双端系统视图没有遮挡、接缝或命中问题，也不能证明真实网络
在洪峰和断线时表现正确。

### 15.2 性能原则

- 静止无状态时，不持续请求无意义的新帧。
- 数据与环境回调只更新状态并合并下一帧，不直接触发多次绘制。
- 转场只更新公共容器和必要内容，避免每帧重建 WebView、Route 或 TCP 连接。
- 记录 UI 线程各阶段耗时、帧间隔、资源数量、TCP 队列和合并/重同步指标，再依据设备刷新率评估。
- 布局缓存、解析卸载、复杂节点复用和局部绘制必须建立在测量结果上。

### 15.3 每阶段开始前必须确认

1. 当前样例所需的最小 View 类型、可复用控件和布局规则，不预先实现完整 Widget 体系或全量控件清单。
2. ViewTree 与 Route 栈的边界，以及 LinkPage 内部语义状态的唯一写入者。
3. 全局环境字段的系统来源、App 覆盖策略、关爱模式具体样式和折叠能力降级。
4. 平台转场执行位置、首个有效尺寸、可中断规则和最终资源释放时点；SceneHost 的转场占用、换绑代次、
   有界复用及被覆盖 Route 的现场保留策略。
5. HTTP/TCP 在当前阶段采用的协议、依赖、线程、证书要求、测试服务和后台策略。
6. TCP 行情哪些数据允许合并，哪些必须保序，以及缺口后的可信恢复方式。
7. 页面状态只保留在内存，还是需要进程被杀后的恢复；后者不默认纳入第一阶段。

## 16. 明确不做

当前规划不授权以下行为：

- 一次性实现完整 Flutter、Dart 运行时、三棵树、DisplayList 或 Layer Tree。
- 用通用脚本、动态 DSL、代码生成器或字符串命令通道描述所有页面和能力。
- 为每个自绘控件、下拉项或表格单元格创建 Android View、ArkUI 组件或独立 Surface。
- 在 Surface 接口中接收输入、监听手势或持有业务事件回调。
- 把控件绘制等同于控件行为，或在绘制阶段处理输入、修改业务状态、请求数据或直接调用 Surface。
- 让所有 Route 无边界共用或永久缓存同一套物理 Surface，或复用仍在转场、保留现场的有效 SceneHost。
- 为 Web、视频等平台成员另建通用 Surface 树，或绕过类型明确的适配协议扩散业务类型判断。
- 把导航父子直接当作布局 View.parent，或把 LinkPage 的 PageId 扩张成应用 RouteId。
- 用全局 EventBus 向所有 View 广播输入、环境、网络和组件事件。
- 让主题、语言、折叠或关爱模式变化默认重建全部页面、WebView 或网络连接。
- 让 HTTP/TCP 回调直接驱动 Canvas、WebView 或 Vulkan。
- 把 TCP 当成带消息边界的协议，或静默处理半包、粘包、乱序、缺口和队列溢出。
- 为可合并行情快照与不可合并有序事件使用同一种只保留最新值策略。
- 为 WebView 纹理化、任意像素特效或独立 UI/Raster 多线程预设结论。
- 用全局 App 指针、无界订阅、无界任务队列或全局锁隐藏生命周期问题。

## 17. 参考资料

- [Flutter architectural overview](https://docs.flutter.dev/resources/architectural-overview)
- [Inside Flutter](https://docs.flutter.dev/resources/inside-flutter)
- [Navigation and routing](https://docs.flutter.dev/ui/navigation)
- [Animations tutorial](https://docs.flutter.dev/ui/animations/tutorial)
- [Gestures](https://docs.flutter.dev/ui/interactivity/gestures)
- [Platform channels](https://docs.flutter.dev/platform-integration/platform-channels)
- [SafeArea 与 MediaQuery](https://docs.flutter.dev/ui/adaptive-responsive/safearea-mediaquery)
- [Fetch data from the internet](https://docs.flutter.dev/cookbook/networking/fetch-data)
- [Guide to app architecture](https://docs.flutter.dev/app-architecture/guide)

这些资料用于理解职责划分、状态流、帧调度和生命周期，不作为复制 Flutter API、源码或平台能力的依据。
