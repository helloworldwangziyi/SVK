# estarx_vulkan_minimal

面向 SDK 绘制的 C++17/Vulkan 工程，只支持 Android 和鸿蒙。C++ App 提供
Web/Native 成员、页面组合和绘制内容；平台宿主提供系统视图、生命周期、手势、
滚动和动画执行，Native 绘制唯一输出仍是各端 Surface。

## 目录与边界

```text
core/src/
├─ app.h                       # IApp：帧回调公共接口
├─ platform_surface.h          # 单系统视图身份、布局与生命周期协议
├─ web_surface.h               # Web 成员、导航与成员工厂协议
├─ linked_layout.h             # 可选联动组合与 C++ 场景配置
├─ linked_page_state.*         # 公共分页状态机
├─ linked_scene.*              # 场景校验与 JNI/NAPI 配置传输
├─ raster_surface.h            # IRasterSurface：仅负责 Native 绘制
├─ renderer.*                  # Vulkan 帧执行与资源生命周期
├─ gpu/vulkan/                 # Context、Swapchain、Pipeline、TextureCache
├─ ui/canvas.*                 # CPU 顶点流与裁剪批次
└─ log/log.h                   # 终端与日期分卷文件日志

platform/
├─ android/
│  ├─ java/com/estarx/vulkan/  # 单成员、联动布局、通用宿主与 JNI 接口
│  └─ cpp/                     # 动态宿主/Surface 注册与 ANativeWindow
└─ hongmeng/
   ├─ src/main/ets/            # HAR：单成员、联动布局与通用宿主
   └─ cpp/                     # 动态宿主/Surface 注册与 OHNativeWindow

samples/triangle/              # C++ 场景配置、绘制和双端薄宿主
test/                          # host 日志、绘制、场景和分页状态测试
```

依赖方向为 `App → core ← platform`。平台桥接层只调用公共接口，不包含具体
App 业务；core 不依赖平台界面 API。渲染实现遵循 `estarx_vulkan` 的 Vulkan
路径，但不引入其网络、页面、平台视图、离屏渲染或其他业务模块。

## 渲染流程

```text
SurfaceView / XComponent
  → JNI / NAPI
  → IPlatform 创建 VkSurfaceKHR
  → Renderer 创建 Swapchain
  → IApp.onFrame 写 Canvas
  → acquire → 上传顶点 → 录制 → 提交 → vkQueuePresentKHR
```

`IRasterSurface` 只提供初始化、尺寸变化、Surface 渲染、挂起和恢复，没有离屏
绘制或像素回读入口。系统视图控制由独立的 `IPlatformSurface` / `IWebSurface`
承担，不与 Vulkan 绘制接口混用；联动布局只持有成员引用，不接管成员所有权。
所有引擎方法在同一个线程顺序调用；当前两端都由 UI 线程驱动。

## 日志

日志只保留以下能力：

- Android Logcat、鸿蒙 HiLog，以及 host 测试的 stdout。
- 应用沙箱内文件输出：Android 为 `filesDir/logs`，鸿蒙为 `filesDir/logs`。
- 按本地日期命名并跨天切换：`estarx-vulkan-YYYY-MM-DD.log`。
- 单文件默认达到 10 MiB 后分卷：`estarx-vulkan-YYYY-MM-DD-001.log`，依此类推。
- 同日重启追加到仍有空间的文件，不自动清理历史文件。

文件同步写入，不包含异步队列、审计通道、加密、脱敏、数据录制或崩溃钩子。
`SVK_LOGV/D/I/W/E` 是公共日志宏；Surface 挂起和引擎关闭时会显式 `flush()`。

## 构建

Android 需要 JDK 17+、Android SDK/NDK 和 `glslc`：

```bash
cd samples/triangle/android
export JAVA_HOME="$(/usr/libexec/java_home -v 17)"
./gradlew assembleDebug
```

鸿蒙需要 DevEco Studio 及其 SDK。`entry` 通过本地 `file:` 依赖引用
`platform/hongmeng` HAR，构建入口仍为 sample 工程：

```bash
cd samples/triangle/hongmeng
export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
export JAVA_HOME=/Applications/DevEco-Studio.app/Contents/jbr/Contents/Home
/Applications/DevEco-Studio.app/Contents/tools/hvigor/bin/hvigorw assembleHap --no-daemon
```

host 构建需要 CMake、Vulkan SDK 和 `glslc`：

```bash
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

架构目标、职责边界与实施顺序见 `docs/linked-view-architecture.md`。
