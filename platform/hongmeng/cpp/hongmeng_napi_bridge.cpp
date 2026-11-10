// 鸿蒙 NAPI 薄桥：按 XComponent ID 隔离当前探针的三个固定 Surface。
// 不引入通用 Session 体系；ArkUI 场景负责滚动和动画，native 只在 Surface
// 创建/尺寸变化时提交静态红色帧，避免每块 Surface 各自推进时间线。

#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <AbilityKit/ability_runtime/application_context.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <spdlog/sinks/callback_sink.h>

#include "app.h"
#include "log/log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

extern "C" svk::IPlatform* svkCreateHongmengPlatform(void* window, int32_t width,
                                                     int32_t height);
extern "C" void svkDestroyHongmengPlatform(svk::IPlatform* platform);

namespace {

constexpr int kSurfaceCount = 3;

struct SurfaceSlot {
    std::unique_ptr<svk::IRasterSurface> renderer;
    svk::IPlatform* platform = nullptr;
    OH_NativeXComponent* component = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

std::array<SurfaceSlot, kSurfaceCount> g_surfaces;
std::unique_ptr<svk::IApp> g_app;
std::string g_reportedLogFile;

int surfaceIdForComponent(OH_NativeXComponent* component) {
    char id[OH_XCOMPONENT_ID_LEN_MAX + 1] = {};
    uint64_t size = sizeof(id);
    if (component && OH_NativeXComponent_GetXComponentId(component, id, &size) ==
                         OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        if (std::strcmp(id, "estarx-header") == 0) return 0;
        if (std::strcmp(id, "estarx-native") == 0) return 1;
        if (std::strcmp(id, "estarx-menu") == 0) return 2;
    }
    for (int index = 0; index < kSurfaceCount; ++index) {
        if (g_surfaces[static_cast<size_t>(index)].component == component) return index;
    }
    return -1;
}

std::string logDirectory() {
    char buffer[1024] = {};
    int32_t writeLength = 0;
    const AbilityRuntime_ErrorCode result = OH_AbilityRuntime_ApplicationContextGetFilesDir(
        buffer, static_cast<int32_t>(sizeof(buffer)), &writeLength);
    (void)writeLength;
    if (result != ABILITY_RUNTIME_ERROR_CODE_NO_ERROR || buffer[0] == '\0') return {};
    return std::string(buffer) + "/logs";
}

void ensureLogger() {
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::callback_logger_mt(
            "estarx-vulkan", [](const spdlog::details::log_msg& msg) {
                LogLevel level;
                switch (msg.level) {
                    case spdlog::level::trace:
                    case spdlog::level::debug: level = LOG_DEBUG; break;
                    case spdlog::level::info: level = LOG_INFO; break;
                    case spdlog::level::warn: level = LOG_WARN; break;
                    case spdlog::level::err: level = LOG_ERROR; break;
                    default: level = LOG_FATAL; break;
                }
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
    if (!svk::log::initWithFile(logger, config, &filePath)) return;
    if (g_reportedLogFile != filePath) {
        g_reportedLogFile = filePath;
        SVK_LOGI("log", "file_started platform=hongmeng path={}", filePath);
    }
}

void renderSlot(int id) {
    SurfaceSlot& slot = g_surfaces[static_cast<size_t>(id)];
    if (!g_app || !slot.renderer || !slot.platform || slot.width == 0 || slot.height == 0) {
        SVK_LOGW("harmony", "frame_dropped surface_id={} reason=surface_not_ready", id);
        return;
    }
    svk::ui::Canvas canvas;
    const svk::AppFrameContext context{slot.width, slot.height, 0.0,
                                       static_cast<uint32_t>(id)};
    g_app->onFrame(canvas, context);
    if (slot.renderer->render(canvas) == svk::RenderResult::Failed) {
        SVK_LOGE("harmony", "frame_failed surface_id={} width={} height={}", id, slot.width,
                 slot.height);
    }
}

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    ensureLogger();
    const int id = surfaceIdForComponent(component);
    if (id < 0) {
        SVK_LOGE("harmony", "surface_start_failed reason=unknown_component");
        return;
    }
    SurfaceSlot& slot = g_surfaces[static_cast<size_t>(id)];
    slot.component = component;

    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) !=
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS ||
        width == 0 || height == 0) {
        SVK_LOGE("harmony", "surface_start_failed surface_id={} phase=get_component_size", id);
        return;
    }
    slot.width = static_cast<uint32_t>(width);
    slot.height = static_cast<uint32_t>(height);

    if (slot.renderer) {
        svk::IPlatform* resumed = svkCreateHongmengPlatform(
            window, static_cast<int32_t>(slot.width), static_cast<int32_t>(slot.height));
        if (resumed && slot.renderer->resumeSurface(resumed)) {
            svkDestroyHongmengPlatform(slot.platform);
            slot.platform = resumed;
            SVK_LOGI("harmony", "surface_resumed surface_id={}", id);
            renderSlot(id);
            return;
        }
        SVK_LOGE("harmony", "surface_resume_failed surface_id={} action=cold_start", id);
        if (resumed) svkDestroyHongmengPlatform(resumed);
        slot.renderer.reset();
        svkDestroyHongmengPlatform(slot.platform);
        slot.platform = nullptr;
    }

    slot.platform = svkCreateHongmengPlatform(window, static_cast<int32_t>(slot.width),
                                              static_cast<int32_t>(slot.height));
    if (!slot.platform) {
        SVK_LOGE("harmony", "surface_start_failed surface_id={} phase=create_platform", id);
        return;
    }
    slot.renderer = svk::createRasterSurface(slot.platform);
    if (!slot.renderer->initialize()) {
        SVK_LOGE("harmony", "surface_start_failed surface_id={} phase=initialize_renderer", id);
        slot.renderer.reset();
        svkDestroyHongmengPlatform(slot.platform);
        slot.platform = nullptr;
        return;
    }
    if (!g_app) g_app = svk::createApp();
    slot.renderer->setSize(slot.width, slot.height);
    SVK_LOGI("harmony", "surface_started surface_id={} width={} height={}", id, slot.width,
             slot.height);
    renderSlot(id);
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    const int id = surfaceIdForComponent(component);
    if (id < 0) return;
    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) !=
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS ||
        width == 0 || height == 0) {
        return;
    }
    SurfaceSlot& slot = g_surfaces[static_cast<size_t>(id)];
    slot.width = static_cast<uint32_t>(width);
    slot.height = static_cast<uint32_t>(height);
    if (slot.renderer) {
        slot.renderer->setSize(slot.width, slot.height);
        renderSlot(id);
    }
    SVK_LOGD("harmony", "surface_resized surface_id={} width={} height={}", id, slot.width,
             slot.height);
}

void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* /*window*/) {
    const int id = surfaceIdForComponent(component);
    if (id < 0) return;
    SurfaceSlot& slot = g_surfaces[static_cast<size_t>(id)];
    if (slot.renderer) slot.renderer->suspendSurface();
    svkDestroyHongmengPlatform(slot.platform);
    slot.platform = nullptr;
    slot.component = nullptr;
    slot.width = slot.height = 0;
    SVK_LOGI("harmony", "surface_suspended surface_id={}", id);
    svk::log::flush();
}

napi_value OnShutdown(napi_env /*env*/, napi_callback_info /*info*/) {
    g_app.reset();
    for (SurfaceSlot& slot : g_surfaces) {
        slot.renderer.reset();
        svkDestroyHongmengPlatform(slot.platform);
        slot.platform = nullptr;
        slot.component = nullptr;
        slot.width = slot.height = 0;
    }
    SVK_LOGI("harmony", "engine_stopped surface_count={}", kSurfaceCount);
    svk::log::flush();
    return nullptr;
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    if (!env || !exports) return nullptr;
    static const napi_property_descriptor desc[] = {
        {"onShutdown", nullptr, OnShutdown, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) !=
            napi_ok ||
        !exportInstance) {
        return exports;
    }
    OH_NativeXComponent* component = nullptr;
    if (napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&component)) != napi_ok ||
        !component || surfaceIdForComponent(component) < 0) {
        return exports;
    }
    static OH_NativeXComponent_Callback callback = {
        OnSurfaceCreatedCB, OnSurfaceChangedCB, OnSurfaceDestroyedCB, nullptr,
    };
    OH_NativeXComponent_RegisterCallback(component, &callback);
    return exports;
}
EXTERN_C_END

static napi_module g_estarxModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "estarx_vulkan",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEstarxVulkanModule(void) {
    napi_module_register(&g_estarxModule);
}
