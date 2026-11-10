// Android JNI 薄桥：按 Surface ID 管理当前探针需要的固定槽位。
// 这里不抽象通用 Surface 框架；槽位只隔离各系统 Surface 的 Renderer/Platform，
// 场景状态、手势与动画全部留在 samples 的原生页面中。

#include <jni.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <spdlog/sinks/android_sink.h>

#include "app.h"
#include "log/log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

extern "C" svk::IPlatform* svkCreateAndroidPlatform(JNIEnv* env, jobject surface);
extern "C" void svkDestroyAndroidPlatform(svk::IPlatform* platform);

namespace {

constexpr jint kSurfaceCount = 3;

struct SurfaceSlot {
    std::unique_ptr<svk::IRasterSurface> renderer;
    svk::IPlatform* platform = nullptr;
};

std::array<SurfaceSlot, kSurfaceCount> g_surfaces;
std::unique_ptr<svk::IApp> g_app;
std::string g_logDirectory;
std::string g_reportedLogFile;

bool validSurfaceId(jint id) {
    return id >= 0 && id < kSurfaceCount;
}

void ensureLogger() {
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::android_logger_mt("estarx-vulkan", "estarx-vulkan");
    }
    if (g_logDirectory.empty()) {
        svk::log::init(logger);
        return;
    }

    svk::log::LogFileConfig config;
    config.directory = g_logDirectory;
    std::string filePath;
    if (!svk::log::initWithFile(logger, config, &filePath)) {
        return;
    }
    if (g_reportedLogFile != filePath) {
        g_reportedLogFile = filePath;
        SVK_LOGI("log", "file_started platform=android path={}", filePath);
    }
}

void suspendSlot(SurfaceSlot& slot, jint id) {
    if (slot.renderer) {
        slot.renderer->suspendSurface();
    }
    if (slot.platform) {
        svkDestroyAndroidPlatform(slot.platform);
        slot.platform = nullptr;
    }
    SVK_LOGI("android", "surface_suspended surface_id={}", id);
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeSetLogDirectory(
    JNIEnv* env, jclass, jstring directory) {
    if (!directory) {
        g_logDirectory.clear();
        return;
    }
    const char* chars = env->GetStringUTFChars(directory, nullptr);
    if (!chars) {
        return;
    }
    g_logDirectory = chars;
    env->ReleaseStringUTFChars(directory, chars);
    ensureLogger();
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeInit(JNIEnv* env, jclass, jint surfaceId,
                                              jobject surface) {
    ensureLogger();
    if (!validSurfaceId(surfaceId) || !surface) {
        SVK_LOGE("android", "surface_start_failed surface_id={} reason=invalid_args", surfaceId);
        return;
    }

    SurfaceSlot& slot = g_surfaces[static_cast<size_t>(surfaceId)];
    if (slot.renderer) {
        svk::IPlatform* resumed = svkCreateAndroidPlatform(env, surface);
        if (resumed && slot.renderer->resumeSurface(resumed)) {
            svkDestroyAndroidPlatform(slot.platform);
            slot.platform = resumed;
            SVK_LOGI("android", "surface_resumed surface_id={}", surfaceId);
            return;
        }
        SVK_LOGE("android", "surface_resume_failed surface_id={} action=cold_start", surfaceId);
        if (resumed) {
            svkDestroyAndroidPlatform(resumed);
        }
        slot.renderer.reset();
        svkDestroyAndroidPlatform(slot.platform);
        slot.platform = nullptr;
    }

    slot.platform = svkCreateAndroidPlatform(env, surface);
    if (!slot.platform) {
        SVK_LOGE("android", "surface_start_failed surface_id={} phase=create_platform", surfaceId);
        return;
    }

    slot.renderer = svk::createRasterSurface(slot.platform);
    if (!slot.renderer->initialize()) {
        SVK_LOGE("android", "surface_start_failed surface_id={} phase=initialize_renderer",
                 surfaceId);
        slot.renderer.reset();
        svkDestroyAndroidPlatform(slot.platform);
        slot.platform = nullptr;
        return;
    }

    if (!g_app) {
        g_app = svk::createApp();
    }

    uint32_t width = 0;
    uint32_t height = 0;
    slot.platform->getSurfaceSize(&width, &height);
    slot.renderer->setSize(width, height);
    SVK_LOGI("android", "surface_started surface_id={} width={} height={}", surfaceId, width,
             height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeResize(JNIEnv*, jclass, jint surfaceId, jint width,
                                                jint height) {
    if (!validSurfaceId(surfaceId)) {
        return;
    }
    SurfaceSlot& slot = g_surfaces[static_cast<size_t>(surfaceId)];
    if (slot.renderer) {
        slot.renderer->setSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        SVK_LOGD("android", "surface_resized surface_id={} width={} height={}", surfaceId, width,
                 height);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeBeginFrame(JNIEnv*, jclass, jint surfaceId,
                                                    jlong frameTimeNanos) {
    if (!validSurfaceId(surfaceId)) {
        return;
    }
    SurfaceSlot& slot = g_surfaces[static_cast<size_t>(surfaceId)];
    if (!g_app || !slot.renderer || !slot.platform) {
        SVK_LOGW("android", "frame_dropped surface_id={} reason=surface_not_ready", surfaceId);
        return;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    slot.platform->getSurfaceSize(&width, &height);
    const double timeSeconds = static_cast<double>(frameTimeNanos) * 1e-9;
    svk::ui::Canvas canvas;
    const svk::AppFrameContext context{width, height, timeSeconds,
                                       static_cast<uint32_t>(surfaceId)};
    g_app->onFrame(canvas, context);
    const svk::RenderResult result = slot.renderer->render(canvas);
    if (result == svk::RenderResult::Failed) {
        SVK_LOGE("android", "frame_failed surface_id={} width={} height={}", surfaceId, width,
                 height);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroy(JNIEnv*, jclass, jint surfaceId) {
    if (!validSurfaceId(surfaceId)) {
        return;
    }
    suspendSlot(g_surfaces[static_cast<size_t>(surfaceId)], surfaceId);
    svk::log::flush();
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
    g_app.reset();
    for (jint id = 0; id < kSurfaceCount; ++id) {
        SurfaceSlot& slot = g_surfaces[static_cast<size_t>(id)];
        slot.renderer.reset();
        svkDestroyAndroidPlatform(slot.platform);
        slot.platform = nullptr;
    }
    SVK_LOGI("android", "engine_stopped surface_count={}", kSurfaceCount);
    svk::log::flush();
}
