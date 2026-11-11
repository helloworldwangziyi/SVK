# 跨 Android、鸿蒙的细粒度 Surface SDK 方案

> 状态：**待审阅，未批准实施**。
>
> 公共单元是单个 WebView 或 Native Surface；上层决定数量、内容及组合关系。当前多 Surface 场景通过可选联动布局组合这些单元，后续由各自的 Widget 管理。

## 1. 目标与范围

从 sample 提取已经验证的平台能力，形成跨 Android、鸿蒙的 C++ SDK：

- 一个 WebView 对应一个独立 `IWebSurface`，一个 Native 绘制视图对应一个独立 `IPlatformSurface`。
- 上层可创建一个或多个 WebView，混合 Native 内容并决定顺序；公共 API 不固定为四个 WebView 或五个 Tab。
- 各 Surface 可单独挂载，也可加入联动布局，组织当前 Header、分页、Tab 停靠和右侧菜单。
- 平台实现移入 `platform/`；sample 保留 C++ 内容配置、组合描述、绘制和业务回调。
- Flutter 后续为每个 Surface 建立 Widget 生命周期映射；本轮不引入 Flutter SDK、Dart 或完整 Widget 引擎。

公共接口不暴露 Java View、ArkWeb controller、XComponent、JNI/NAPI 句柄或 Vulkan 类型。`App → core ← platform` 保持不变。

## 2. 抽象单元与组合边界

| 对象 | 管理范围 | 由谁拥有 |
| --- | --- | --- |
| `IPlatformSurface` | 单个系统合成视图的身份、挂载、布局、可见性及生命周期 | 对应的 App 控制对象；后续对应 Widget |
| `IWebSurface` | 单个 WebView，增加导航和加载事件 | 对应的 WebView Widget |
| Native Surface | 单个 Native 绘制视图，通过内容 ID 连接既有渲染路径 | 对应的 Native Surface Widget |
| `ILinkedLayout` | 引用成员，处理当前场景的外层滚动、分页、菜单及手势交接 | 组合层；后续对应父级联动 Widget |
| 平台宿主 | 系统父容器、合成、事件派发、平台上下文与模块装配 | SDK 的 Android/鸿蒙宿主 |

`ILinkedLayout` 不拥有成员 Surface，不负责偷偷创建固定数量的 WebView。成员的创建、内容和销毁由上层决定；布局只持有可校验的身份引用。

`IPlatformSurface` 是系统视图控制接口，`IRasterSurface` 继续是 Native 的绘制/Swapchain Present 接口，二者不继承。WebView 保持系统合成，不纹理化、不回读、不用 Canvas 代绘。

联动布局只表达当前场景已经验证过的关系，不扩展为任意布局语言、通用 Surface 树或完整 Widget/Element/RenderObject 体系。

## 3. Widget 与 C++ 的对应方式

```text
LinkedLayoutWidget                    ← 当前场景的父级组合
├─ NativeSurfaceWidget(header)        ← 独立 Native Surface
├─ 页面列表（由 App 决定数量及顺序）
│  ├─ WebSurfaceWidget(pageA)          ← 独立 WebView
│  ├─ WebSurfaceWidget(pageB)          ← 独立 WebView
│  └─ NativeSurfaceWidget(pageC)       ← 独立 Native Surface
└─ NativeSurfaceWidget(drawer)        ← 独立 Native Surface

Widget / App C++
       │  创建内容、组合成员、提交布局和用户意图
       ▼
core：Surface 协议 + Web 专属接口 + 可选联动布局协议
       ▲
       ├─ Android：SurfaceView / WebView / 原生布局与动画
       └─ 鸿蒙：XComponent / ArkWeb / 原生嵌套滚动与 Swiper
```

各 Surface 也可以离开这套组合独立使用。自绘 Tab 只消费分页状态并提交选择意图；选中外观不参与分页动画的完成判定。

“由 Widget 管理”在本轮指生命周期、配置和布局接口与 Widget 的职责匹配；本仓库当前还没有 Widget 框架，不以新增一个接口代替真实 Flutter 接入验收。

## 4. C++ 最小接口草案

以下只用于评审，不是已新增的公共头文件。辅助类型的字段和错误码在接口阶段收敛。

```cpp
using SurfaceId = uint64_t;
using PageId = uint64_t;
using NativeContentId = uint32_t;

// 稳定引用与系统窗口代次分离；同一 Widget 的 Surface 重建不改变该引用。
// 关闭后重建的新对象不能复用旧引用。
struct SurfaceRef {
    SurfaceId id;
    uint64_t instanceGeneration;
};

// 物理像素，宿主内容区左上角原点；clip 与 bounds 使用相同坐标系。
struct SurfacePlacement {
    PixelRect bounds;
    PixelRect clip;
    bool visible;
    bool interactive;
};

class IPlatformSurface {
public:
    virtual ~IPlatformSurface() = default;
    virtual SurfaceRef ref() const = 0;
    virtual SurfaceResult mount() = 0;
    virtual SurfaceResult setPlacement(const SurfacePlacement&) = 0;
    virtual SurfaceResult suspend() = 0;
    virtual SurfaceResult resume() = 0;
    virtual SurfaceState state() const = 0;
    virtual void close() = 0;
};

class IWebSurface : public IPlatformSurface {
public:
    virtual SurfaceResult navigate(const WebRequest&) = 0;
};

// 工厂由平台宿主注入，持有创建原生 View 所需的上下文；不依靠全局 Activity。
// 成功返回唯一所有权，失败返回明确错误，不返回半初始化的可用对象。
class IPlatformSurfaceFactory {
public:
    virtual ~IPlatformSurfaceFactory() = default;
    virtual WebSurfaceResult createWeb(const WebSurfaceConfig&) = 0;
    virtual NativeSurfaceResult createNative(const NativeSurfaceConfig&) = 0;
};

struct PageBinding {
    PageId pageId;
    SurfaceRef surface;
    std::string title;
};

struct LinkedLayoutSpec {
    SurfaceRef header;
    std::vector<PageBinding> pages;
    std::optional<SurfaceRef> drawer;
    PageId initialPage;
};

class ILinkedLayout {
public:
    virtual ~ILinkedLayout() = default;
    virtual LayoutResult setViewport(const LinkedViewport&) = 0;
    virtual LayoutResult replacePages(const std::vector<PageBinding>&) = 0;
    virtual LayoutResult selectPage(PageId) = 0;
    virtual LayoutResult setDrawerOpen(bool open) = 0;
    virtual LinkedLayoutState state() const = 0;
    virtual void close() = 0;
};
```

`createNative` 接受 `NativeContentId`，经既有 `IApp.onFrame` 和 `IRasterSurface` 绘制，不在公共接口添加直接持有 Renderer、Vulkan 或平台窗口的入口。App 绘制上下文中应明确区分内容身份和运行时 Surface 身份，不能继续用固定 0/1/2 槽位代替业务内容。

`WebSurfaceConfig` 描述初始导航，事件由创建时注册的接收方获取；`navigate` 发起新文档会话。第一版不开放任意脚本执行、无限页面缓存或浏览器通用配置集合。

联动布局由宿主提供的工厂创建并绑定 `LinkedLayoutSpec`。工厂负责原生父子结构与成员注册；C++ 接口不接收平台父 View 指针。创建布局不会转移成员的唯一所有权。

### 生命周期与失败语义

- 所有调用、回调、创建及销毁在宿主 UI 线程顺序执行。后续 Flutter 适配器负责线程切换。
- 单个 Surface 的生命周期为 `Created → Mounted ↔ Suspended → Closed`；创建失败不生成有效引用，运行时失败显式报告。
- 首次顺序为创建 Surface、声明组合关系、提交有效布局，再挂载。新增成员也先声明归属，再挂载到该组的原生父容器。
- 独立 Surface 的布局由其 Widget 写入；加入联动布局后，几何与有效交互状态改由该布局唯一写入，直接 `setPlacement` 返回 `ControlledByLayout`。Widget 通过父布局提交基础尺寸与可见性意图，不能与平台动画争写位置。
- 暂停/隐藏时取消未完成输入并按组合规则落定动画、停止惯性。单个成员暂停须先通知布局，不能让正在动画中的组继续访问失效资源；恢复只恢复自身会话，不自行推进组合动画。
- `close()` 幂等且为终态，先失效身份、异步任务与组合引用，再释放原生资源。返回后不得再回调该 Surface 的事件接收方。
- 正常移除顺序为更新组合成员、解除绑定，再关闭被移除的 Surface。即使调用方先关闭，注册表也必须同步撤销该成员并停止相关操作，不能留下悬空引用。
- 布局关闭先取消组级输入/动画、解除所有绑定，再释放容器；成员仍由各自所有者持有，进入已解绑且暂停的状态，不暗中销毁。重新挂载需要明确的宿主/布局绑定。
- 事件接收方不能在同步回调中销毁正在执行的对象；宿主可在当前派发结束后处理释放。迟到回调仅通过稳定身份查找资源，不捕获裸的已销毁 C++ 指针。
- 工厂与宿主晚于所创建的布局和 Surface 销毁；最终清理不再调用影响进程中所有视图的全局 `nativeShutdown`。

结果至少区分接受、状态不允许、引用失效、页面不存在、布局无效、归属冲突、能力不支持和平台失败。命令接受不等于文档加载、动画结束或已经显示。无效输入保留上一份有效状态并返回原因；事件附带所属实例、会话与 revision。

## 5. 数量、顺序与页面变化

页面配置采用 `vector<PageBinding>`，Web 与 Native 可以混排。`PageId` 是稳定身份，位置仅决定显示顺序；不假定 Web 页连续排在前面、Native 页永远在最后。

- 支持配置不同的 WebView 数量。零个 WebView 可以组成纯 Native 分页；联动分页至少需要一个有效页面。
- 每个成员都有独立控制对象、文档会话、偏移和销毁路径，不再有 `controllers[4]`、固定 `WEB_PAGE_COUNT` 或按固定字符串识别角色的公共约定。
- `replacePages` 按稳定 ID 更新成员关系，保留未变化对象的文档和滚动位置；不会因为增删一个页而重建全部 WebView。
- 同一 PageId 更换 Surface 引用表示内容替换，需要取消旧内容相关输入、检测和偏移绑定；旧文档状态不转移给新对象。
- 替换先完整校验：引用有效、同宿主、页 ID 唯一、Surface 不重复、不会同时属于两个布局或两个角色、页面列表非空。校验失败不修改当前组合。
- 应用有效变化前失效旧手势/动画 generation 并停止相关惯性；保留最新请求页（若仍存在），否则选择它在旧顺序中的后继，后继也被删除则选前驱。替换落定不额外播放跨结构动画。
- 未变化的 PageId 只改变位置时保留其内容；新增页完成原生准备后才可参与动画。准备失败时保留旧有效组合并报告失败，不能显示半套分页结构。

已准备的成员来自 App 创建的 Surface；布局仅验证并安排归属。初期不增加池化或无限预加载。平台的实际资源限制要返回明确结果，不能用固定四页的实现冒充数量可配置。

可配置数量与运行时增删分别验收：前者证明不写死 N，后者证明稳定身份、移除与状态保持。两者不能只靠修改数组声明或 host mock 宣称已经支持。

## 6. 状态、布局与动画的唯一写入者

细粒度对象不各自保存一份场景进度。单个对象保存自己的生命周期、文档与资源状态；每个联动布局只有一份外层偏移、分页状态和菜单进度。

| 状态或行为 | 负责方 | 契约 |
| --- | --- | --- |
| Surface 数量、内容和组合关系 | App / 上层 Widget | 创建成员并按稳定 ID 提交关系 |
| 独立 Surface 的位置、尺寸、可见性 | 对应 Widget | 未加入布局时唯一写入 |
| 组内成员的有效位置、裁剪、交互 | 平台联动布局执行 | 同一次更新统一应用，成员不能自行推进 |
| settled / animationTarget / requested 页 | 可提取的 C++ 分页状态机 | 平台只执行命令、回报带 generation 的结果 |
| 纵向滚动与惯性 | 对应平台布局 | Android 保留手动协调；鸿蒙保留原生嵌套滚动 |
| DOM 检测、方向锁定、Cancel 与多指 | 平台手势实现 | 明确整段归属，不二次合成另一套手势 |
| 分页及菜单动画执行 | 平台单一动画源 | 不增加与之并行的 C++ 定时器 |
| 单个 WebView 文档/加载状态 | 该 Web Surface | 不与其他页面串用文档 session |

布局更新关联同一 `layoutId/frameId`、时间戳及组状态，按成员引用应用。重复更新去重，旧 session/frame 拒绝；只在全部输入有效后应用本次结果。平台系统合成能否达到接缝与时序要求仍以设备验证为准。

跨平台协议中的坐标与偏移全部是物理像素、左上角原点；平台换算 dp/vp、DOM CSS 坐标和手势阈值。`pageProgress` 是显示顺序中的连续位置，`drawerProgress` 为 `[0,1]`。两者不等同于稳定 PageId。

Widget 提交布局约束和用户意图，不经异步状态事件逐帧追赶平台动画；状态回报用于观察和 Tab 展示。组内主场景与菜单直接消费同一动画更新。公共分页状态机接管后平台不能继续保存第二套可写决策状态；未接管的端维持完整旧路径。

## 7. 保留的交互行为与平台差异

1. TopBar 固定，Header 占满下方视口；Tab 覆盖 Header 底边，外层最大偏移为 Header 高减 Tab 高。
2. WebView 实际高度扣除 TopBar 和停靠后的 Tab，不能被分页包装拉成过大的视口。
3. 外层停靠后继续上滑进入当前页；当前内容到顶后下拉恢复外层，只有一个滚动和惯性写入者。
4. 普通区域抬手判决切页；Canvas、Chart、iframe、横向列表优先交网页，检测未知或失败也保留网页手势。
5. 保留 40 逻辑单位距离，或超过 12 单位且末段速度至少 800 单位/秒的判决。传入公共纯逻辑前换算为物理像素。
6. 相邻页使用当前 160 ms 动画，跨多页直接切换。连续有效意图合并到最新目标，动画结束后继续，不等待原生 Tab 外观、不加冷却时间。
7. UP 早于 DOM 回调也能完成同一次手势；新 DOWN、Cancel、导航、成员变化和生命周期失效旧结果。分页结束不清空下一段已开始的触摸。
8. 鸿蒙保留 `changeIndex` 延至 UP 派发结束的顺序；保留自然嵌套滚动、固定实际高度的 ArkWeb 和分页包装。
9. Android 保留当前 MotionEvent 拦截、多指取消及唯一竖向偏移写入者；不能让 WebView 与宿主同时竖向滚动。
10. 菜单与主场景共用 VSync、进度和 easing；菜单宽度保持当前组合的 `1/4`，参与动画的 Surface 提前准备。

Android 与鸿蒙绿色页内部滚动范围目前有实现差异，迁移先保持各自基线。平台布局与真实命中链不能因接口细化被平铺掉；尤其鸿蒙需要保留 Scroll/Swiper/Web 的有效祖先关系。

`ILinkedLayout` 绑定的是现有明确场景关系。任意跨父容器拖动、重新挂载或跨布局转移不能承诺无损保留系统对象；不支持的转移显式返回错误，需重建时递增会话并报告。

## 8. 身份、生命周期与资源安全

- `SurfaceRef`：逻辑对象身份，支持同时存在多个 Web 与 Native 对象，关闭后不复用旧引用。
- `layoutId` 与布局会话：组合实例身份；成员变更、恢复及关闭失效相关旧事件。
- `surfaceGeneration`：系统 Surface 重建代次，与稳定对象引用分离。
- `documentSession`：每个 Web 对象自己的导航代次。
- `gestureGeneration`、`animationGeneration`：分别拒绝旧检测与旧动画完成。
- `revision/frameId`：关联一次状态或布局更新，去重并拒绝旧结果。

JNI/NAPI 与 XComponent 回调在读取资源前校验所属对象、会话及代次；旧销毁回调不能挂起新 Surface。不能继续用进程全局三个槽位、固定 XComponent 名称、全局 App 指针承担组件所有权。

宿主创建 App，取得内容及组合配置，再创建原生视图。NativeContentId 映射到 App 绘制，平台代码不判断探针颜色。资源挂起、销毁仍遵循现有 GPU 完成与平台窗口释放顺序，不绕过 IRasterSurface。

前后台由宿主统一分发，各布局先取消组级输入/动画，再顺序挂起成员；恢复时重建必要会话并同步几何。Widget 自身可见性与系统前后台分别记录原因，不能因宿主恢复而显示仍被 Widget 隐藏的对象。

## 9. Flutter 后续接入边界

每个 Surface 对应独立 Widget 身份、创建/更新/释放；父级联动 Widget 声明关系并管理该组状态。接口设计不预设每个 Widget 必须一对一注册为一个彼此独立的 Flutter PlatformView。

Android 的 WebView/SurfaceView 可以由平台适配器提供真实系统视图，但当前组合的祖先关系、命中链、裁剪和联动更新必须保持。真实接入时需验证以下两种映射，不能先承诺二者等价：

- 独立使用的 Surface：对应独立 Flutter PlatformView。
- 联动组内的多个 Surface Widget：必要时共用一个原生合成宿主，由各 Widget 的稳定身份映射到其中的独立系统视图；Flutter 侧仍分别控制内容及生命周期，原生宿主不固定页面数量。

直接将多个独立 PlatformView 放进 Flutter 的分页/滚动容器，是否仍能满足当前连续快滑与无接缝要求，需要单独设备探针。细粒度 C++ 接口本身不能解决 Flutter 手势竞争、跨 View 合成与 SurfaceView 层级问题。

鸿蒙以将来实际使用的 Flutter 发行版及其平台视图能力为准，不从 ArkUI 的成功推导 Flutter 已可等价嵌入。本轮仅建立接入边界，不引入引擎头文件或实际插件。

第一版布局支持平移、尺寸、轴对齐裁剪、显隐与交互门控。旋转、任意透明度、复杂遮罩及 Flutter 内容插入多个 Native Surface 之间另行验证，不伪造接口能力。

## 10. 目录与迁移步骤

```text
core/src/
  platform_surface.h              # 单个对象的配置、身份、生命周期、布局、事件
  web_surface.h                   # 单个 Web 对象的导航和加载契约
  linked_layout.h                 # 可选成员组合、分页与菜单控制协议
  linked_page_state.*             # 已验证的公共分页转换
platform/android/
  java/com/estarx/vulkan/          # 单 Surface、联动父容器、手势与动画
  cpp/                            # JNI、实例注册与资源生命周期
  host/                           # 通用 Activity、资源与 Gradle 宿主
platform/hongmeng/
  ets/                            # 单 Web/Native 组件、联动布局、ArkUI 手势
  cpp/                            # NAPI、对象身份与 XComponent 生命周期
  host/                           # 通用 Ability、资源与 Hvigor 宿主
samples/<app>/
  app.cpp、*.h                    # 创建 Surface、页面列表、组合、绘制与业务事件
```

### 阶段 0：接口审阅与行为基线

以 `m3-surface-linkage-adaptation` 为当前行为基线。确认单个 Surface 为公共单元、页面数量由上层控制、联动布局持引用不拥有资源。对照相同操作序列补齐双端纵向接缝、菜单、Surface 重建等尚未验证项，不以构建或局部手感代替全部验收。

### 阶段 1：公共 C++ 协议与纯逻辑

定义独立 Web/Native 对象、稳定引用、所有权、成员关系、布局写入权和错误语义。增加公共分页状态机与 host 用例，旧双端实现继续可运行。不建立通用 Widget 树、布局语言或多线程框架。

### 阶段 2：Android 平台提取

拆分当前 `SurfaceLinkageLayout` 中的单 WebView、Native 视图、联动父容器和业务配置，迁入平台目录。C++ 创建任意页面列表，JNI 按对象身份路由。保留已验证的触摸/动画执行，消除固定三个 Surface 槽位与固定 Web 数量。

分别验证独立 Web 使用、不同页面数量、混排、运行时增删及原有多 Surface 场景；真机回归后提供阶段结果。

### 阶段 3：鸿蒙平台提取

提取单 ArkWeb/XComponent 组件及联动布局，通过平台模块复用。NAPI 接同一对象和分页协议；动态页面用稳定 key 生成，不能因数量变化重建所有控制器。

保留原生父子结构、自然嵌套滚动、Swiper 包装和 UP 派发后的执行顺序。`@Entry` 的页面显示/隐藏通过宿主显式转发给布局和成员，不能假设移走后页面回调仍自动触发。

验证 ArkTS 模块引用、HAP 打包及设备场景；不把源码临时复制回 sample 冒充 SDK 提取。

### 阶段 4：宿主收口与 C++ 复用验证

移动通用 Android/鸿蒙宿主和构建入口，按 App 参数选择 C++ 源码。平台包中移除 URL、合约参数、探针标题和配色；sample 只保留 C++ 组合与绘制。

新增确定性本地内容 sample，验证单 Web、多个 Web、Web/Native 混排。新增业务文件只有 `.cpp/.h`，不修改平台源码；同步更新 README 和 AGENTS 中实际受影响的构建/目录约定。

每阶段完成后提供修改、验证与未验证事项供审阅。提交授权独立，不因实现或阶段结束自动提交。

## 11. 验证与失败处理

| 层次 | 必须覆盖 |
| --- | --- |
| host | 引用失效、生命周期、单写入者门控、无效布局、成员重复/跨组冲突 |
| 公共分页 | 连续同向/反向、最新意图、跨页直达、重复/迟到完成、成员变化后旧结果 |
| 页面数量 | 1、2、4、6 个 Web，零 Web 的 Native 分页，Native/Web 交错顺序 |
| 动态变化 | 增删/重排，删除当前/目标页，更换同 ID 内容，未变化 Web 保留文档和偏移 |
| 独立对象 | 单个 Web 挂载/导航/关闭，关闭一个对象不关闭其他对象 |
| 平台构建 | Android assemble/lint，鸿蒙 HAP 与平台模块打包 |
| 双端设备 | 慢拖、快滑、斜滑、惯性、取消、多指、迟到 DOM 结果及网页交互优先 |
| 纵向联动 | Tab 停靠临界、内容到底、到顶回拉、各页偏移保持 |
| 组合生命周期 | 菜单开关、前后台、布局变化、关闭重建、成员销毁和旧回调 |

host 测试不证明系统合成与命中链；构建不等于设备验证。动态增删不能仅验证 C++ vector 变化，必须观察真实控制器、文档与平台节点是否正确复用。

日志沿用现有系统，关联 Surface/布局/session/generation/frame、手势归属、偏移、页面目标及失败处置。正常帧和 MOVE 不刷 Info。

若细分平台组件或公共状态机导致联动回退，优先撤销不合适的提取边界并保留已验证的实现，不降低无抖动标准，不改写阶段标签。

## 12. 审阅要点

1. 公共粒度为单个 WebView/Native Surface，数量由上层创建，支持独立 Widget 生命周期。
2. 联动布局是可选组合，不拥有成员、不固定页面数量；组内共享一次状态与动画更新。
3. 使用稳定身份更新页面列表，保留未变化对象，明确删除/替换当前页及旧回调的处理。
4. 两端保留各自手势、滚动和动画执行；Flutter 接入时单独验证独立 PlatformView 与共享原生宿主的映射。
5. 按“基线 → C++ → Android → 鸿蒙 → 宿主/sample 收口”推进，不以接口更通用为由降低原有行为验收要求。
