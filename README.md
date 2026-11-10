# estarx_vulkan_minimal

面向 SDK 绘制的 C++17/Vulkan 种子工程，只支持 Android 和鸿蒙。平台壳只提供
Surface、尺寸、生命周期和 VSync；具体绘制内容由平台无关的 C++ App 写入
`ui::Canvas`，引擎唯一输出是各端 Surface。

## 目录与边界

```text
core/src/
├─ app.h                       # IApp：帧回调公共接口
├─ render_platform.h           # IPlatform：Vulkan Surface 创建与尺寸查询
├─ raster_surface.h            # IRasterSurface：Surface 渲染公共接口
├─ renderer.*                  # Vulkan 帧执行与资源生命周期
├─ gpu/vulkan/                 # Context、Swapchain、Pipeline、TextureCache
├─ ui/canvas.*                 # CPU 顶点流与裁剪批次
└─ log/log.h                   # 终端与日期分卷文件日志

platform/
├─ android/                    # JNI、SurfaceView、ANativeWindow
└─ hongmeng/                   # NAPI、XComponent、OHNativeWindow

samples/triangle/              # 当前 App 与安卓/鸿蒙原生工程
test/                          # host 日志测试
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
绘制或像素回读入口。所有引擎方法在同一个线程顺序调用；当前两端都由 UI
线程驱动。

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

鸿蒙需要 DevEco Studio 及其 SDK：

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

架构目标与演进计划见 `docs/roadmap.md`。
