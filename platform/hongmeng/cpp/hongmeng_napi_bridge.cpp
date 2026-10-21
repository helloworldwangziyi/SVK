// ============================================================================
// NAPI 实现层：ArkTS 薄壳（VulkanShell.ets 的 XComponent）与 core 之间的翻译层。
// 与 Android 的 android_jni_bridge.cpp 同构：
//   - XComponent 回调（OnSurfaceCreated/Changed/Destroyed）≈ Android 的
//     surfaceCreated/Changed/Destroyed；
//   - ArkTS DisplaySync 帧回调 beginFrame ≈ Android 的 Choreographer。
//
// 线程规则：本文件所有函数都运行在 UI 线程——XComponent 四个回调、ArkTS
// 同步 NAPI 调用、DisplaySync 帧回调都在 UI 线程触发，core 的无锁单线程
// 假设原样成立。这正是帧节奏选 ArkTS DisplaySync 而非 OH_NativeVSync 的
// 原因：后者回调在独立 vsync 线程，会迫使 core 引入锁。
//
// 设计约定：本层只做"解包参数 → 转发 core"，不含任何业务逻辑。
// ============================================================================

#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <AbilityKit/ability_runtime/application_context.h>

#include <cstdint>
#include <cstring>
#include <string>

#include <spdlog/sinks/callback_sink.h>

#include "app.h"
#include "log/log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

// hongmeng_vulkan_platform.cpp 以 C 链接导出的工厂函数（与 Android 同款约定）：
// bridge 只需声明原型即可调用，无需知道 HongmengPlatform 类的布局。
extern "C" svk::IPlatform* svkCreateHongmengPlatform(void* window, int32_t width,
                                                     int32_t height);
extern "C" void svkDestroyHongmengPlatform(svk::IPlatform* platform);

// App 与渲染器随引擎创建、onShutdown 销毁；surface 挂起时保留。
// OnSurfaceCreated 建/热恢复、OnSurfaceDestroyed 挂起。全部在 UI 线程访问，无锁。
static std::unique_ptr<svk::IApp> g_app;
static svk::IRasterSurface* g_renderer = nullptr;
static svk::IPlatform* g_platform = nullptr;
static uint32_t g_width = 0;
static uint32_t g_height = 0;
static std::string g_reportedLogFile;

static std::string logDirectory() {
    char buffer[1024] = {};
    int32_t writeLength = 0;
    const AbilityRuntime_ErrorCode result =
        OH_AbilityRuntime_ApplicationContextGetFilesDir(
            buffer, static_cast<int32_t>(sizeof(buffer)), &writeLength);
    (void)writeLength;
    if (result != ABILITY_RUNTIME_ERROR_CODE_NO_ERROR || buffer[0] == '\0') {
        return {};
    }
    return std::string(buffer) + "/logs";
}

// hilog sink 由平台壳注入；spdlog 没有内置 hilog sink，用 callback_sink 转
// OH_LOG_Print。成功取得应用 filesDir 后再追加完整文件 sink。
static void ensureLogger() {
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::callback_logger_mt(
            "estarx-vulkan", [](const spdlog::details::log_msg& msg) {
                LogLevel level;
                switch (msg.level) {
                    case spdlog::level::trace:
                    case spdlog::level::debug: level = LOG_DEBUG; break;
                    case spdlog::level::info:  level = LOG_INFO;  break;
                    case spdlog::level::warn:  level = LOG_WARN;  break;
                    case spdlog::level::err:   level = LOG_ERROR; break;
                    default:                   level = LOG_FATAL; break;
                }
                // payload 不以 '\0' 结尾，拷贝成 C 字符串再交 hilog 的 %{public}s。
                const std::string text(msg.payload.data(), msg.payload.size());
                OH_LOG_Print(LOG_APP, level, 0x0000, "estarx-vulkan", "%{public}s",
                             text.c_str());
            });
    }

    svk::log::LogFileConfig config;
    config.directory = logDirectory();
    if (config.directory.empty()) {
        svk::log::init(logger);
        return;
    }

    std::string filePath;
    if (!svk::log::initWithFile(logger, config, &filePath)) {
        return;
    }
    if (g_reportedLogFile != filePath) {
        g_reportedLogFile = filePath;
        SVK_LOGI("log", "file_started platform=hongmeng path={}", filePath);
    }
}

// ---------------------------------------------------------------------------
// XComponent 回调（框架在 UI 线程触发）
// ---------------------------------------------------------------------------

// surface 创建就绪时调用（可能多次：页面重建/退后台恢复）。
// 已有引擎时走保活热恢复，否则冷启动；失败时清理现场可干净重试。
static void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    ensureLogger();

    // XComponent 的 surface 尺寸（物理像素），建平台适配器与帧布局都要用。
    uint64_t surfaceWidth = 0;
    uint64_t surfaceHeight = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &surfaceWidth,
                                              &surfaceHeight) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        SVK_LOGE("harmony", "engine_start_failed phase=get_component_size");
        return;
    }
    g_width = static_cast<uint32_t>(surfaceWidth);
    g_height = static_cast<uint32_t>(surfaceHeight);

    if (g_renderer) {
        // 保活热恢复：引擎级资源（设备/纹理/命令设施）仍在，只按新 surface
        // 重建 surface 级资源；失败则全量拆除，落到下面的冷启动兜底。
        svk::IPlatform* resumed = svkCreateHongmengPlatform(
            window, static_cast<int32_t>(g_width), static_cast<int32_t>(g_height));
        if (resumed && g_renderer->resumeSurface(resumed)) {
            svkDestroyHongmengPlatform(g_platform);
            g_platform = resumed;
            SVK_LOGI("harmony", "engine_resumed renderer=vulkan");
            return;
        }
        SVK_LOGE("harmony", "engine_resume_failed action=cold_start");
        if (resumed) {
            svkDestroyHongmengPlatform(resumed);
        }
        delete g_renderer;
        g_renderer = nullptr;
        svkDestroyHongmengPlatform(g_platform);
        g_platform = nullptr;
    }

    g_platform = svkCreateHongmengPlatform(window, static_cast<int32_t>(g_width),
                                           static_cast<int32_t>(g_height));
    if (!g_platform) {
        SVK_LOGE("harmony", "engine_start_failed phase=create_platform");
        return;
    }

    g_renderer = svk::createRasterSurface(g_platform).release();
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 OnSurfaceCreated 能干净重试。
        SVK_LOGE("harmony", "engine_start_failed phase=initialize_renderer");
        delete g_renderer;
        g_renderer = nullptr;
        svkDestroyHongmengPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    if (!g_app) {
        g_app = svk::createApp();
    }

    g_renderer->setSize(g_width, g_height);
    SVK_LOGI("harmony", "engine_started renderer=vulkan width={} height={}", g_width,
             g_height);
}

// surface 尺寸变化（旋转、分屏等）。与 Android nativeResize 同语义。
static void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    g_width = static_cast<uint32_t>(width);
    g_height = static_cast<uint32_t>(height);
    if (g_renderer) {
        g_renderer->setSize(g_width, g_height);
    }
}

// surface 销毁语义 = 保活挂起（对照 Android nativeDestroy）：只释放 surface 级
// 资源，引擎级资源（设备/纹理/命令设施）全部保留，下次 OnSurfaceCreated 热恢复。
static void OnSurfaceDestroyedCB(OH_NativeXComponent* /*component*/,
                                 void* /*window*/) {
    if (g_renderer) {
        g_renderer->suspendSurface();
    }
    if (g_platform) {
        svkDestroyHongmengPlatform(g_platform);
        g_platform = nullptr;
    }
    SVK_LOGI("harmony", "engine_suspended renderer=vulkan");
    svk::log::flush();
}

// ---------------------------------------------------------------------------
// ArkTS 经 onLoad(context) 调用的导出函数
// ---------------------------------------------------------------------------

// ArkTS: bridge.beginFrame(frameTimeNanos)
// ArkTS DisplaySync 的帧回调（UI 线程）：由 App 构建本帧内容并提交渲染。
static napi_value BeginFrame(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1 || !g_app || !g_renderer) {
        return nullptr;
    }
    int64_t frameTimeNanos = 0;
    napi_get_value_int64(env, argv[0], &frameTimeNanos);
    const double timeSeconds = static_cast<double>(frameTimeNanos) * 1e-9;
    svk::ui::Canvas canvas;
    const svk::AppFrameContext context{g_width, g_height, timeSeconds};
    g_app->onFrame(canvas, context);
    g_renderer->render(canvas);
    return nullptr;
}

// ArkTS: bridge.onShutdown()
// Ability.onDestroy 语义 = 真正销毁（对照 Android nativeShutdown）：全量释放，
// 之后若再启动走完整冷启动流程。
static napi_value OnShutdown(napi_env /*env*/, napi_callback_info /*info*/) {
    g_app.reset();
    delete g_renderer;
    g_renderer = nullptr;
    if (g_platform) {
        svkDestroyHongmengPlatform(g_platform);
        g_platform = nullptr;
    }
    g_width = g_height = 0;
    SVK_LOGI("harmony", "engine_stopped renderer=vulkan");
    svk::log::flush();
    return nullptr;
}

// ---------------------------------------------------------------------------
// 模块注册
// ---------------------------------------------------------------------------

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    if (!env || !exports) {
        return nullptr;
    }
    // 职责①：挂上 ArkTS 可见的函数表。
    napi_property_descriptor desc[] = {
        {"beginFrame", nullptr, BeginFrame, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"onShutdown", nullptr, OnShutdown, nullptr, nullptr, nullptr, napi_default,
         nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // 职责②：解包 XComponent 实例并注册回调。exports 由 XComponent 框架构造时
    // 才带 __NATIVE_XCOMPONENT_OBJ__；纯 import 加载没有它，跳过即可。
    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ,
                                &exportInstance) != napi_ok ||
        !exportInstance) {
        return exports;
    }
    OH_NativeXComponent* nativeXComponent = nullptr;
    if (napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&nativeXComponent)) !=
            napi_ok ||
        !nativeXComponent) {
        return exports;
    }
    // 静态存储期：RegisterCallback 只存指针，回调表必须比注册点活得久。
    static OH_NativeXComponent_Callback callback = {
        OnSurfaceCreatedCB, OnSurfaceChangedCB, OnSurfaceDestroyedCB,
        nullptr, // 触摸：最小形态不接入
    };
    OH_NativeXComponent_RegisterCallback(nativeXComponent, &callback);
    return exports;
}
EXTERN_C_END

static napi_module g_estarxModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,       // 模块加载回调：挂函数 + 注册 XComponent 回调
    .nm_modname = "estarx_vulkan",  // 必须等于 XComponent 的 libraryname
    .nm_priv = nullptr,
    .reserved = {0},
};

// so 加载（dlopen）时构造函数自动执行，把模块登记进 NAPI 注册表。
extern "C" __attribute__((constructor)) void RegisterEstarxVulkanModule(void) {
    napi_module_register(&g_estarxModule);
}
