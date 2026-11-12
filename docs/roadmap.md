# SVK 自有跨端 App 能力 Roadmap

> 状态：**待审阅，不构成实施授权**。
>
> 本文件维护产品目标、总体边界和阶段方向。当前普通联动视图的已批准实施范围仍以
> `linked-view-architecture.md` 为准；本 Roadmap 不扩大该范围，也不要求立即创建文中提到的
> 全部对象。各阶段实施前先审阅对应的最小接口和双端适配方案。

## 1. 产品目标：从联动 Demo 走向完整 App

SVK 的目标不是给 Android、鸿蒙分别写两套页面，也不是把 Flutter 源码换成另一套名字，而是：

- 使用 C++ 描述 App 的页面、状态、业务意图和 Native 绘制内容。
- 使用同一套核心逻辑驱动 Android、鸿蒙，平台差异留在适配层。
- 普通 UI 由自己的 Vulkan 路径绘制；WebView 等系统能力以独立 Surface 接入。
- 页面导航、转场、浮层、系统事件和异步数据具有明确的所有权与生命周期。
- 用一个真实 App 流程逐步验证能力，而不是先构造庞大的通用 UI 框架。

第一条完整产品链路确定为：

```text
启动 App
  → 首页（Native 自绘，底部导航）
  → 切换底部导航
  → 点击首页条目，带入场转场进入详情页
  → 点击“多 Surface 联动”，进入当前 Linked Demo
  → 点击 Native 自绘按钮，以浮层形式打开 WebView
  → 关闭浮层、返回详情、返回首页，原页面状态仍正确
```

首页随后接入真实数据。网络慢、失败、页面离开、App 进入后台时，UI 仍然能够响应导航和输入，
不能被一次请求阻塞。

这里的“首页”首先是验证导航、输入、生命周期和数据闭环的纵向样例，不代表在第一阶段就实现完整
商业首页。文字、图片、复杂列表等能力按实际需要单独评审，不用 WebView 代替自己的普通 UI。

## 2. 当前基础与真实缺口

当前工程已经提供以下基础：

- `IApp.onFrame` 和 `ui::Canvas`：生成最小 Native 绘制内容。
- `IRasterSurface`：沿用 Vulkan Swapchain Present，不开放绕过现有路径的输出。
- `IPlatformSurface`、`IWebSurface`：描述系统视图控制和 Web 导航，不等于 Vulkan 绘制接口。
- `ILinkedLayout`、`LinkedSceneSpec`：描述 Header、页面列表和右侧菜单的组合。
- `LinkedPageState`：处理当前联动布局内部的分页决策、最新意图和动画 generation。
- Android、鸿蒙宿主：创建系统对象并执行各自已经验证的手势、滚动和动画。

这些是**单个场景及其成员的能力**，还不是应用级运行框架。当前主要缺口是：

| 缺口 | 当前不能直接承担的事情 |
| --- | --- |
| App 根场景 | 首页、详情和 Linked Demo 之间没有统一的场景所有者 |
| 应用导航 | 没有 Route 栈、返回规则、跨页面状态保留和页面移除契约 |
| 普通 UI 组合 | `Canvas` 只有图元，没有最小布局、稳定内容节点和状态到界面的映射 |
| 自绘输入 | 没有公共 Pointer 输入、命中区域、按压状态和整段手势归属 |
| 通用转场 | 现有动画服务于固定联动关系，不能直接表示 Route 进退场 |
| Overlay | 没有统一遮罩、层级、返回优先级和 Web 弹层所有权 |
| 系统事件 | 尚未形成前后台、窗口、系统返回、Deep Link 等统一输入 |
| 数据层 | 没有请求会话、数据状态、取消、缓存和迟到结果处理 |
| 帧调度 | 尚未把数据变化、布局、动画和 Surface 更新收敛到同一帧协议 |

已存在代码不等于已经完成双端设备验收。后续能力必须在保留现有联动行为的前提下增量建设。

## 3. 借鉴 Flutter 的原则，而不是复制 Flutter

参考 [Flutter 分层架构](https://docs.flutter.dev/resources/architectural-overview)，SVK 应保留
App、核心逻辑、平台嵌入层之间的单向依赖。平台宿主负责系统入口、Surface、输入和事件循环，
核心层负责可测试的 App 行为，Vulkan 只负责 Native 光栅化。

| Flutter 机制 | 值得借鉴的性质 | SVK 的选择 |
| --- | --- | --- |
| Widget 描述 UI | `UI = f(state)`，构建不产生外部副作用 | 页面由状态生成本地场景描述，绘制函数不请求网络、不操作系统对象 |
| Element / RenderObject | 状态复用、布局与绘制分工 | 先保留简单页面状态和即时 `Canvas`，只有实际场景证明需要时才引入保留节点或局部更新 |
| Navigator / Router | Route 栈、页面身份、声明式配置与外部路由协作 | 第一版实现最小应用导航栈；Deep Link 到来时转换到同一导航状态，不另建平行导航 |
| AnimationController / Tween | 时间进度、插值和绘制分离 | 所有相关成员共享一次时间采样，效果使用明确插值，不各自启动计时器 |
| Pointer / Gesture Arena | 先命中，再决定整段手势归属 | 以 pointer ID、方向、session 和平台命中结果确定唯一所有者 |
| Overlay | 浮层独立于普通页面层级 | Web 弹层、菜单和遮罩由 App 根宿主统一管理，不能由某个按钮私下持有原生 View |
| Platform Channels | 平台服务异步调用，结果回到正确的执行上下文 | 使用类型明确的 C++ 服务协议和请求 token，不让 JNI/NAPI 类型泄漏到 App |
| Future / Stream | 异步结果与 UI 状态分离 | 请求只更新页面数据状态并请求新帧，不直接调用绘制或 Vulkan |
| Repository / Service | 数据来源、业务状态和界面绘制解耦 | 传输实现、数据仓库、页面状态和绘制分别测试 |

参考 [Flutter 内部机制](https://docs.flutter.dev/resources/inside-flutter)，长期优化方向是只处理变化
内容、复用稳定身份、避免无意义的全树布局。第一阶段不复制其三棵树，也不预先实现复杂 diff；
先用小规模、可测量的页面证明状态复用和帧更新是否正确。

明确不引入 Dart、Skia、Impeller、Flutter Engine 或 Flutter Widget 源码，不修改只读参考工程。

## 4. 目标分层与依赖方向

```text
samples/<app>/
  页面内容、底部导航配置、业务状态、页面意图、数据模型和场景组合
                         │
                         ▼
App 核心逻辑（C++）
  页面状态 → 场景描述
  导航栈、Overlay 状态、转场决策、请求状态、系统事件处理
                         │
             ┌───────────┴────────────┐
             ▼                        ▼
系统视图与平台服务协议             Native 绘制协议
Surface / Web / 帧调度             Canvas → IRasterSurface
系统事件 / 网络 / 环境              仍然是唯一 Vulkan 输出
             │                        │
             └───────────┬────────────┘
                         ▼
Android / 鸿蒙平台适配
  根宿主、系统生命周期、VSync、输入、Surface、WebView、异步 I/O
```

建议每个平台根宿主持有一个 App 运行时。运行时拥有应用级导航、Overlay 和共享数据对象；
各 Route 拥有自己的页面状态和成员引用，不再为每个页面创建一份进程级全局 App。

普通首页和详情优先绘制在根 Native Surface 中。进入 Linked Demo 时，由该 Route 的平台宿主挂载
现有联动组合；离开并完成退场后，再释放只属于该 Route 的成员。这样不需要把普通按钮、底部导航
强行拆成许多系统 View，也不需要为每个页面重复实现一套平台壳。

`IPlatformSurface` 与 `IRasterSurface` 继续分离。App 不直接持有 Java WebView、ArkWeb、
XComponent、ANativeWindow、OHNativeWindow 或 Vulkan 对象。

## 5. 应用导航：区分四种“页面切换”

参考 [Flutter 导航与路由](https://docs.flutter.dev/ui/navigation)，应用导航应是显式的 Route
集合，而不是把新页面直接 `addView` 到旧页面上。第一版采用简单栈和明确命令，暂不做路由 DSL。

| 层级 | 示例 | 状态所有者 |
| --- | --- | --- |
| 底部导航选中项 | 首页、第二栏目 | App Shell |
| 应用 Route 栈 | 首页 → 详情 → Linked Demo | App 导航状态 |
| Linked Demo 内部分页 | 资讯、关联、排行、F10、绿色 | 该 Route 内的 `LinkedPageState` |
| Web 文档会话 | 浮层初次加载、后续导航、关闭 | 对应 Web Surface |

`PageId` 继续表示联动布局内部的稳定分页身份，不能拿来代替 `RouteId`；一次应用导航也不能被错误
翻译成 Linked Demo 的 `selectPage`。

### 5.1 第一版导航结构

```text
App Shell
├─ 根 Native 内容
│  ├─ 首页 / 第二栏目
│  └─ 底部导航
├─ Route 栈
│  ├─ 详情（覆盖 Shell，保留 Shell 状态）
│  └─ Linked Demo（全屏 Route，拥有自己的 Surface 组合）
└─ Overlay 层
   ├─ 点击外部关闭的遮罩
   └─ Web 弹层或 Native 菜单
```

第一版采用“一个根 Route 栈 + 底部选中状态”。底部栏目保留页面数据与滚动状态，但暂不实现每个
栏目各自一套历史栈；产品确实需要独立栏目历史时再增加，避免第一版就出现多套返回语义。

### 5.2 导航事务与生命周期

`push`、`pop`、`replace`、底部栏目选择都应形成完整导航事务：

1. 校验目标、参数、当前状态和重复操作；无效输入不修改现有栈。
2. 为本次事务分配 generation，决定离场、入场成员及输入归属。
3. 入场成员准备系统对象和首个有效尺寸，网络文档加载完成不作为开始动画的前提。
4. 同一次转场中，旧 Route 和新 Route 都保持有效，只有转场目标消费输入。
5. 完成后冻结被覆盖 Route 的绘制和交互，但按保留策略保存其页面状态。
6. 被移除 Route 先失效输入、请求订阅和异步回调，再在安全的资源时点销毁成员。

转场期间的快速重复点击不能无限制压栈或并发创建多条转场。每种导航操作必须明确是拒绝、合并到
最新目标，还是在当前事务后执行一个待处理意图；不能由按钮点击次数偶然决定。

系统返回按可见层级处理：优先关闭模态 Overlay，其次处理当前 Route 自己公开的可返回状态，再执行
应用 `pop`，最后才交还系统。被遮挡的 Route 不响应返回、手势或数据驱动的可见动画。

外部 Deep Link、通知点击和系统返回最终都进入同一导航决策。等支持 Deep Link 时，再增加声明式
路由配置和参数校验，不另建一套绕过 Route 栈的跳转入口。

## 6. 普通 UI、按钮与输入闭环

### 6.1 先建立最小状态到画面的映射

```text
页面状态 + 环境快照
  → 本地场景描述
  → 布局（物理像素、左上角原点）
  ├─ Canvas 图元
  ├─ 命中区域
  └─ Surface 几何与可见性
  → 同一帧提交
```

第一阶段只用少量明确页面结构，不定义通用 Widget 基类。页面构建和绘制不能发起网络请求、创建
WebView、切换 Route 或直接写平台状态；它们只返回描述。用户操作先形成业务意图，由状态所有者处理
后再触发一次界面更新。

普通 Native 页面优先复用根 Surface。布局尺寸或数据变化时重新生成必要内容；纯动画尽量只更新变换
和重绘所需部分。是否引入局部布局、节点复用和缓存，必须由后续真实页面的性能数据决定。

### 6.2 自绘按钮不是系统 Button

自绘按钮由两部分组成：

- 绘制：矩形、颜色、按压反馈以及后续文本或图标。
- 行为：稳定内容 ID、布局后得到的命中区域、可用状态和动作 ID。

输入链至少支持 Down、Move、Up、Cancel 和 pointer ID：

1. 从最上层 Overlay 到下层 Route 依次命中，禁用和被遮挡区域不能获得输入。
2. Down 后记录命中对象和会话；同一 pointer 的后续事件不因轻微移出而改投其他按钮。
3. Up 时仅在动作仍有效、未取消且满足触发区域条件时发送一次点击意图。
4. Cancel、多指冲突、Route 移除、Overlay 关闭和 Surface 重建必须清理按压状态。
5. 滑动容器与按钮发生竞争时，整段手势只能有一个最终所有者，不能既滚动又点击。

这与 [Flutter Pointer 和手势竞争](https://docs.flutter.dev/ui/interactivity/gestures) 的思想一致，
但第一版只实现当前场景需要的 Tap、纵向滚动、横向分页和网页优先规则，不复制完整手势竞技场。

### 6.3 自绘按钮打开 WebView

按钮只提交“展示某个 Web 内容”的 App 意图，不直接执行 `new WebView`、ArkWeb controller 或
JavaScript。运行时创建带稳定身份的 Overlay 描述，平台工厂据此创建 `IWebSurface`，宿主拥有成员，
Overlay 只持有引用和控制关系。

第一版建议验证底部 Web 弹层：Native 遮罩、明确高度、系统 WebView 内容、共享转场进度、外部点击
关闭和系统返回关闭。后续锚点菜单、全屏 Web Route 可以复用同一所有权规则，但不必共用所有布局。

## 7. 转场、菜单与自定义动效

参考 [Flutter 动画教程](https://docs.flutter.dev/ui/animations/tutorial)，动画数值本身不负责渲染；
它只驱动界面属性。SVK 应将以下四件事分开：

- 导航或 Overlay 的状态转换。
- 平台帧时钟和动画调度。
- easing、插值、分段和弹簧等纯计算。
- Canvas 与平台 Surface 对同一进度的消费。

现有 Linked Demo 内部的 Android `ValueAnimator`、鸿蒙原生动画路径在迁移验收前保持。未来普通
Route/Overlay 的目标方案是：平台提供帧时间戳，核心状态按时间计算共享进度，同一帧更新相关成员；
不增加与之并行的 C++ 定时器。若某段动画仍由平台执行器接管，则只能由该执行器驱动对应进度，
核心不能再维护第二个可写时钟。

每个转场至少有开始、进行中、完成、取消和反向退出的明确语义，并带 generation：

- 打开中再次关闭，从当前可见进度反向，而不是先跳到终点。
- 返回手势取消时恢复原状态；提交时完成退出。
- Route 被替换、App 隐藏或 Surface 失效时，旧完成事件不得再落定新页面。
- 入场 Surface 的系统对象和尺寸先准备，首个网络请求耗时不计入转场时长。
- 转场结束才清理离场成员；异常关闭可以跳过动画，但不能跳过资源释放。

动效能力分两步验收：

| 层级 | 首选能力 | 约束 |
| --- | --- | --- |
| 基础转场 | 平移、尺寸、轴对齐裁剪、Native 颜色/透明度、共享进度 | 先覆盖首页、详情、Linked Demo 和 Web 弹层 |
| 个性化效果 | 缩放、回弹、错峰、旋转、透视、复杂遮罩、自定义绘制 | 逐项验证 SurfaceView、XComponent、WebView 的层级、裁剪和命中 |

对包含系统 WebView 或 Native Surface 的场景，优先动画它们的公共父容器。像素扭曲、网格变形和
网页内容 Shader 化会改变当前系统合成边界，不能作为普通 Surface 动效默认承诺。

## 8. 系统事件与环境快照

参考 [Flutter 生命周期监听](https://api.flutter.dev/flutter/widgets/AppLifecycleListener-class.html)
和 [平台通道](https://docs.flutter.dev/platform-integration/platform-channels)，系统事件需要统一入口，
但不能把两端不同系统回调机械地一一同名映射。

平台事件先转换为有限的 C++ 类型，并关联宿主、对象身份、事件序号和必要的 generation。禁止业务
页面注册一个永不注销的全局回调，也禁止任意后台线程直接修改页面状态。

| 事件类别 | 核心语义 | 必须处理的关系 |
| --- | --- | --- |
| 前后台、显示、隐藏、焦点 | 是否可见、是否可交互、是否继续调度帧 | 与 Surface 销毁分开，不能把 `onPause` 当成资源已销毁 |
| Surface 创建、尺寸变化、销毁 | 某一代系统窗口是否有效 | 旧 generation 不得挂起或绘制新窗口 |
| 窗口、旋转、density、安全区 | 更新环境快照和布局 | core 只接收物理像素及明确单位，平台负责 dp/vp 转换 |
| 系统返回、退出请求 | 交给可见 Overlay / Route / 导航栈 | 没有消费者时才交还系统默认行为 |
| Deep Link、通知点击 | 产生经过校验的导航意图 | 冷启动与热启动去重，宿主未就绪时有界等待，不直接操作 View |
| 语言、主题、字体缩放 | 更新环境并失效相关布局或资源 | 文本与主题支持进入对应阶段后验收 |
| 内存压力 | 清理可重建缓存 | 不能释放仍在 GPU 或系统合成中使用的资源 |
| 网络状态、权限结果 | 更新服务状态或触发明确业务策略 | 不自动重放非幂等请求，不把权限回调交给已销毁页面 |

### 8.1 前后台顺序

隐藏或后台化时：

```text
停止新输入
→ 取消/落定当前手势与转场
→ 保存必要页面状态
→ 失效相关业务订阅
→ 停止帧调度
→ 按平台顺序挂起 Surface 与 GPU 资源
```

恢复时：

```text
确认宿主和系统 Surface 身份
→ 更新窗口、density、安全区等环境快照
→ 重建必要的 Surface generation
→ 同步当前 Route、Overlay 和滚动状态
→ 生成一帧完整画面
→ 恢复帧调度与输入
```

页面请求是否允许在后台完成并写入缓存，是数据层策略；它不能直接触发后台 Vulkan Present。
网络回调、通知点击和 Web 加载结果在恢复后也必须通过原身份校验。

### 8.2 不是再做一个无边界 EventBus

系统事件应采用明确类型和所有者订阅。页面只接收自己关心且仍有效的事件，宿主的底层资源事件由
平台成员处理，不广播给所有业务页面。事件队列必须有界、可失效，销毁后释放订阅和回调负载。

## 9. 网络请求与 UI 绘制的平衡

参考 [Flutter 获取网络数据](https://docs.flutter.dev/cookbook/networking/fetch-data) 和
[Flutter App 分层建议](https://docs.flutter.dev/app-architecture/guide)：不要在构建或绘制中发起请求，
异步结果先成为状态，再由 UI 反映 loading、内容或错误。

### 9.1 数据与 UI 分层

| 层级 | 责任 | 禁止事项 |
| --- | --- | --- |
| 平台网络适配 | 异步传输、超时、取消转发、状态码和传输错误 | 不直接修改页面、Canvas 或 Surface |
| 数据仓库 | 业务数据模型、缓存、刷新、错误归类和重试策略 | 不持有 Java/ArkTS 系统视图 |
| 页面状态/控制器 | 发起明确加载意图，保存当前数据状态和最新请求身份 | 不在绘制函数中触发副作用 |
| 场景构建与绘制 | 根据一份状态快照生成图元和命中区域 | 不等待网络、不解析大响应、不处理传输回调 |

简单页面不需要为了形式拆出很多类，但“传输、数据状态、绘制”三个职责必须分开。

### 9.2 一次请求的完整链路

```text
Route 激活 / 用户刷新
  → 页面状态生成 Load 意图
  → 数据仓库分配 requestId / generation
  → 平台异步 HTTP
  → 获得不可变结果或明确错误
  → 通过平台 UI 执行器回到宿主 UI 线程
  → 校验宿主、所有者、订阅、requestId 和 generation
  → 一次性更新页面数据状态，标记脏并请求一帧
  → 下一帧统一布局、绘制和 Surface 更新
```

请求身份至少表达：数据所有者、请求序号、代次和数据 revision。页面退出后应取消可取消的传输并失效
订阅；取消是尽力而为，不代表服务器停止处理。即使取消失败，迟到结果也不能写入新页面或另一代请求。

重新加载时递增 generation，只接受当前策略允许的最新结果。页面被覆盖但保留时，可以允许结果写入
自己的数据状态，但不能让隐藏页面抢输入、播放可见动画或直接 Present。

### 9.3 UI 状态而不是裸 Future

每个数据源至少需要表达：

- 尚未加载。
- 首次加载中。
- 有有效内容。
- 正在刷新，同时保留旧内容。
- 加载失败，是否可重试，是否仍有旧内容。

刷新失败不应把可用旧列表突然清空；新数据应作为一个一致快照替换，避免同一帧一半来自旧列表、
一半来自新列表。快速连续的独立结果可以合并为一次帧请求，但有顺序要求的分页或流事件不能简单
只保留最后一个。

### 9.4 线程与耗时边界

第一阶段继续保持 App、场景、Surface 和 Vulkan 在宿主 UI 线程顺序执行：

- 平台网络库可以在自己的异步线程执行 I/O，但回调只负责把结果投递到 UI 执行器。
- 工作线程不得直接调用 App、Canvas、Renderer、WebView 或 XComponent。
- 不为网络结果在现有公共方法中零散加锁；跨线程只传递有所有权的不可变结果。
- 大 JSON 解析、图片解码等是否移出 UI 线程，依据测量和独立线程设计决定，不在第一阶段默认增加。
- 网络 I/O 不等于引入独立 UI/Raster 多线程，现有渲染线程模型保持不变。

### 9.5 真实网络接入前的约束

先使用可控的 Fake Transport 验证延迟、乱序、失败、取消和页面销毁，再接入一个真实只读接口。
真实实现需要单独确认：

- 优先复用 Android、鸿蒙系统异步网络能力，还是确有理由引入统一 C++ 网络/TLS 依赖。
- JSON 解析库、版本、构建方式和包体积；不得因为常用就默认引入。
- 网络权限、证书、TLS、超时、代理和错误分类。
- 缓存和重试范围；指数退避只用于明确可重试且幂等的操作。
- 认证信息、完整响应和隐私数据不进入日志，测试不依赖不稳定的公共第三方接口。

后续 WebSocket/推送等持续数据需要独立的 stream generation、序号和背压策略。可合并的行情快照与
不可丢失的订单事件不能使用同一种“只留最新值”处理。

## 10. 身份、所有权与线程的统一约束

后续每类对象都要有稳定身份，但不能把所有 ID 混成一个整数：

| 身份 | 用途 |
| --- | --- |
| Host 身份 | 一个平台 App 根宿主及其运行时 |
| RouteId / generation | 一个逻辑页面及其导航事务 |
| OverlayId / generation | 一个浮层及打开、关闭过程 |
| SurfaceRef / surfaceGeneration | 逻辑系统成员及其实际窗口代次 |
| PageId | Linked Demo 内部的稳定分页身份 |
| NativeContentId | App Native 内容映射，不是平台资源所有权 |
| documentSession | 某个 Web 对象的文档导航 |
| pointerId / gestureGeneration | 一段触摸及其归属 |
| requestId / generation / revision | 一次数据请求及其结果有效性 |
| frameId | 一次状态、Canvas 和 Surface 几何的共同提交 |

成员销毁顺序遵循“先失效关系和回调，再释放资源”。转场需要保留旧成员到退出完成，但保留绘制不
等于继续接收输入或业务事件。GPU 资源继续遵守现有完成与释放顺序，不能为了快速关闭页面提前销毁。

所有公共 App、导航、动画、数据状态和平台成员方法默认只在宿主 UI 线程顺序调用。跨线程平台回调
通过显式 UI 投递进入，不捕获裸的已销毁 C++ 指针，不以全局锁掩盖所有权问题。

## 11. 分阶段实施路线

每一阶段都提供可运行样例、host 回归、Android 与鸿蒙构建，以及对应设备场景。阶段完成只表示该
范围验收通过，不自动获得下一阶段或提交授权。

### 阶段 0：收口现有多 Surface 基线

目标：确认当前 `linked-view-architecture.md` 已批准迁移的实际可用范围。

- 保留网页内部横滑优先、普通区域切页、Tab 停靠、绿色页滚动和右菜单行为。
- 完成鸿蒙 HAR/HAP 构建与设备验证，补齐 Android 尚未覆盖的生命周期场景。
- 验证动态成员、混排、独立 Surface 和运行时页面替换的真实平台行为，而不只验证 C++ 数组。
- 明确当前 Linked Demo 作为一个可嵌入 Route 所需的挂载、隐藏、恢复和关闭行为。

出口：现有场景没有因后续 App 规划被提前重写，双端缺口有明确证据。

### 阶段 1：根 Native 场景、最小布局与自绘输入

目标：做一个不依赖网页的确定性首页骨架。

- 一个根 Native Surface 绘制本地几何内容、底部导航占位和自绘按钮。
- 支持明确尺寸、安全区、命中区域、按压/取消和一次点击。
- 引入最小环境快照与按需帧请求；静止时不保持无意义的高频重绘。
- 建立 App 根宿主与页面状态所有权，Surface 重建不丢失逻辑页面状态。

样例：本地首页，点击按钮能够产生可追踪的 App 意图，连续点击和多指不会重复误触发。

出口：双端完成慢点、快点、移出、Cancel、旋转/尺寸变化、前后台和 Surface 重建验证。文本尚未
进入时只宣称几何首页骨架，不宣称完整商业首页。

### 阶段 2：App Shell 与应用导航

目标：跑通“首页 → 底部栏目 → 详情 → Linked Demo → 返回”的完整链路。

- 建立稳定 Route 身份、最小导航栈、底部选中状态和系统返回优先级。
- 普通 Route 复用根 Native 内容；Linked Demo 作为全屏 Route 挂载现有组合。
- 被覆盖页面停止交互和可见绘制，按策略保留数据与滚动状态。
- 快速重复导航只产生定义过的行为，不重复创建宿主或无限压栈。

出口：双端逐层进入、逐级返回，验证当前页、底栏状态、Linked Demo 内分页、Web 状态和成员数量；
反复进入退出无旧回调和明显资源增长。

### 阶段 3：通用 Route 转场与 Overlay/Web 弹层

目标：将现有“共享一个进度”的能力推广到应用导航和弹出场景。

- 支持详情入场/退场、Linked Demo 入场/退场，以及反向和中途取消。
- 建立 Overlay 的遮罩、层级、输入门控、返回优先级和成员所有权。
- 自绘按钮能够打开底部 Web 弹层，网页加载失败可提示、重试和关闭。
- 转场中 Surface 重建、前后台和重复打开关闭均能使旧 generation 失效。
- 先实现平移、裁剪和 Native 淡入淡出，再评审个性化缩放、回弹与错峰。

出口：双端设备覆盖慢速返回手势、快速反向、连续点击、Web 加载中关闭、返回键和外部点击；
不出现两个转场争写同一位置。

### 阶段 4：系统事件与环境能力

目标：让导航和资源行为由真实系统状态驱动，而不是只在理想前台路径工作。

- 统一前后台、显示、焦点、窗口、安全区、系统返回和 Surface 事件。
- 接入 Deep Link/通知点击的导航意图，处理冷启动、热启动、未就绪和重复投递。
- 根据实际产品需要接入主题、语言、内存压力和网络状态。
- 验证恢复时 Route、Overlay、请求状态和 Surface generation 的一致性。

出口：双端在导航、弹层、请求和加载过程中分别执行前后台、返回、尺寸变化及外部通知进入；
不存在后台直接绘制或隐藏页面响应输入。

### 阶段 5：异步数据与真实网络闭环

目标：让首页使用数据驱动 UI，同时保持导航和动画可响应。

- 先实现页面数据状态、Fake Transport、缓存边界、请求 token 和 UI 线程结果投递。
- 再选择一个真实只读接口，实现平台异步网络适配和明确错误处理。
- 支持首次加载、刷新、保留旧内容、失败重试、离开页面和迟到结果丢弃。
- 测量 UI 线程耗时后再决定大解析和图片解码的线程边界，不预先建设通用任务框架。

出口：host 覆盖成功、失败、乱序、重复、取消和销毁；双端设备覆盖慢网、断网、恢复、刷新中导航、
请求中前后台，确认请求回调不直接绘制且不阻塞手势。

### 阶段 6：按真实需求扩展 UI 与动效能力

目标：在已有完整 App 流程上提高表达力，而不是回头重写已验证能力。

- 根据首页真实内容评审文本、图片、列表、主题和可访问性。
- 根据性能数据选择局部失效、稳定节点复用、可见项构建和缓存，不默认复制 Flutter 三棵树。
- 根据产品设计增加共享元素、弹性、错峰和自定义绘制效果。
- 每项复杂平台 Surface 效果先做双端探针；不支持的能力明确降级或拒绝，不伪造统一接口。

出口：新增业务样例只修改 App 的 `.cpp/.h` 和必要业务资源；不需要修改平台壳来完成普通页面。

## 12. 目录演进方向

以下只表达依赖方向，不要求立即创建全部目录：

```text
samples/<app>/
  app.cpp、pages/、data/          # 页面、业务意图、数据模型和场景配置

core/src/
  app/                            # 后续按阶段审阅的导航、Overlay、状态与帧逻辑
  services/                       # 系统事件、网络等类型明确的协议，不含平台类型
  platform_surface.h              # 现有系统成员控制
  web_surface.h                   # 现有 Web 成员协议
  linked_layout.h                 # 现有联动组合
  linked_page_state.*             # 现有内部分页，不承担应用 Route 栈
  raster_surface.h                # 保持 Native 绘制边界

platform/android/
  根宿主、帧调度、系统事件、网络适配、现有 Surface 与 LinkedLayout

platform/hongmeng/
  与 Android 同职责的鸿蒙实现，保留 ArkUI 的真实父子关系和线程要求
```

平台包不得出现具体首页 URL、业务标题或固定页面数量；业务状态也不能藏在 JNI/NAPI 单例里。
新增业务页面原则上只修改 sample 的 C++ 业务文件。

## 13. 验证、性能与可诊断性

### 13.1 分层验证

- host：导航事务、返回优先级、转场中断、Overlay 归属、手势取消、请求乱序和生命周期。
- 平台构建：Android assemble/lint，鸿蒙 HAR/HAP 与平台模块打包。
- 双端设备：真实命中、系统合成、滚动、转场、Web 加载、前后台和 Surface 重建。
- 数据测试：Fake Transport 使用确定性延迟与错误；真实网络验收使用可控服务，不依赖公共网站稳定性。

host mock 只能证明核心状态正确，不能证明双端系统视图没有遮挡、接缝或命中问题。

### 13.2 性能原则

- 静止无状态时，不持续请求无意义的新帧。
- 数据回调只标记状态变化并合并请求下一帧，不直接触发多次绘制。
- 转场只更新需要的变换和重绘内容，避免每帧重复创建 WebView 或重建全部 Route。
- 记录 UI 线程各阶段耗时、帧间隔和资源数量，再依据设备刷新率评估，不预设所有设备都是 60 Hz。
- 布局缓存、解析卸载和复杂节点复用必须建立在测量结果上，不能以增加框架复杂度代替性能证据。

### 13.3 日志

沿用现有 `SVK_LOGV/D/I/W/E` 和结构化字段，按阶段补充：

```text
route_id, overlay_id, transition_generation, navigation_result,
request_id, request_generation, data_revision, lifecycle_state,
pointer_id, owner_id, frame_id, surface_generation, result, reason
```

Info 只记录低频状态转换；动画逐帧、MOVE 和高频数据使用 Debug/Verbose 的限流或采样。网络日志
不记录认证信息、完整响应或无界负载。必须能回答“哪个页面、哪次转场、哪个请求、哪一代 Surface、
为何接受或丢弃”。

## 14. 明确不做与阶段审阅点

当前 Roadmap 不授权以下行为：

- 一次性实现完整 Flutter、Dart 运行时、三棵树、DisplayList 或 Layer Tree。
- 用通用脚本、动态 DSL 或代码生成器描述所有页面。
- 为每个自绘按钮创建一个 Android View 或 ArkUI 组件。
- 让网络库回调直接驱动 Canvas、WebView 或 Vulkan。
- 让 App 业务对象直接持有系统 View、控制器或原生窗口。
- 为 WebView 纹理化、任意像素特效或独立 UI/Raster 多线程预设结论。
- 把 Linked Demo 内部 `PageId` 扩张成整个 App 的导航系统。
- 用全局 EventBus、全局 App 指针或无界任务队列隐藏生命周期问题。

阶段开始前需要分别确认的最小决策：

1. 首页真实内容所需的文本、图片和本地数据边界。
2. 第一版底部栏目是否只需共享一个 Route 栈；默认先不为每个栏目建立独立历史。
3. Linked Demo 的全屏 Route 外壳、返回入口和状态保留策略。
4. 第一种 Web 展示形态；默认先做底部弹层，再验证锚点菜单和全屏 Web Route。
5. 普通 Route 转场的帧时间来源、平台执行位置和可中断规则。
6. 系统网络适配与统一 C++ 网络依赖的选择，以及首个真实接口和证书要求。
7. 页面状态只保留在内存，还是需要支持进程被杀后的恢复；后者不默认纳入第一阶段。

## 15. 参考资料

- [Flutter architectural overview](https://docs.flutter.dev/resources/architectural-overview)
- [Inside Flutter](https://docs.flutter.dev/resources/inside-flutter)
- [Navigation and routing](https://docs.flutter.dev/ui/navigation)
- [Animations tutorial](https://docs.flutter.dev/ui/animations/tutorial)
- [Gestures](https://docs.flutter.dev/ui/interactivity/gestures)
- [Platform channels](https://docs.flutter.dev/platform-integration/platform-channels)
- [AppLifecycleListener](https://api.flutter.dev/flutter/widgets/AppLifecycleListener-class.html)
- [Fetch data from the internet](https://docs.flutter.dev/cookbook/networking/fetch-data)
- [Guide to app architecture](https://docs.flutter.dev/app-architecture/guide)

这些资料用于理解职责划分、状态流、帧调度和生命周期，不作为复制 Flutter API 或源码的要求。
