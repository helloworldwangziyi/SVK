// ============================================================================
// JNI 实现层：Java 薄壳(NativeBridge)与 core 之间的翻译层。
//
// 命名约定（静态注册）：函数名 = Java_<包名>_<类名>_<方法名>，
// 包名中的 . 换成 _。extern "C" 保持符号不被 name mangling，
// JVM 才能按名字在 .so 里查到。本层只做"解包参数 → 转发 core"，
// 不含任何业务逻辑；所有函数都运行在 Android UI 线程。
// ============================================================================

#include <jni.h>

#include <string>

#include <spdlog/sinks/android_sink.h>

#include "app.h"
#include "log/log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

// android_vulkan_platform.cpp 以 C 链接导出的工厂函数：
// bridge 只需声明原型即可调用，无需知道 AndroidPlatform 类的布局。
extern "C" svk::IPlatform* svkCreateAndroidPlatform(JNIEnv* env, jobject surface);
extern "C" void svkDestroyAndroidPlatform(svk::IPlatform* platform);

// App 与渲染器随引擎冷启动创建、nativeShutdown 销毁；g_platform 与 surface
// 一致（nativeInit 建或热恢复 / nativeDestroy 拆）。全部在 UI 线程访问，无锁。
static std::unique_ptr<svk::IApp> g_app;
static svk::IRasterSurface* g_renderer = nullptr;
static svk::IPlatform* g_platform = nullptr;
static std::string g_logDirectory;
static std::string g_reportedLogFile;

static void ensureLogger() {
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

// Java: NativeBridge.nativeSetLogDirectory(String)
// Activity 在 Surface 创建前传入应用私有日志目录；Surface 重建时 nativeInit
// 重复进入 ensureLogger，core 会复用同一个文件而不是重复建文件。
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

// Java: NativeBridge.nativeInit(Surface)
// surface 创建就绪时调用（可能多次：退后台重建）。已有引擎时走保活热恢复，
// 否则冷启动；失败时清理现场，保证下次 surfaceCreated 能干净重试。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeInit(JNIEnv* env, jclass, jobject surface) {
    ensureLogger();

    if (g_renderer) {
        // 保活热恢复：引擎级资源（设备/纹理/命令设施）仍在，只按新 surface
        // 重建 surface 级资源；失败则全量拆除，落到下面的冷启动兜底。
        svk::IPlatform* resumed = svkCreateAndroidPlatform(env, surface);
        if (resumed && g_renderer->resumeSurface(resumed)) {
            svkDestroyAndroidPlatform(g_platform);
            g_platform = resumed;
            SVK_LOGI("android", "engine_resumed renderer=vulkan");
            return;
        }
        SVK_LOGE("android", "engine_resume_failed action=cold_start");
        if (resumed) {
            svkDestroyAndroidPlatform(resumed);
        }
        delete g_renderer;
        g_renderer = nullptr;
        svkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }

    g_platform = svkCreateAndroidPlatform(env, surface);
    if (!g_platform) {
        SVK_LOGE("android", "engine_start_failed phase=create_platform");
        return;
    }

    g_renderer = svk::createRasterSurface(g_platform).release();
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 surfaceCreated 能干净重试。
        SVK_LOGE("android", "engine_start_failed phase=initialize_renderer");
        delete g_renderer;
        g_renderer = nullptr;
        svkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    if (!g_app) {
        g_app = svk::createApp();
    }

    uint32_t width = 0, height = 0;
    g_platform->getSurfaceSize(&width, &height);
    g_renderer->setSize(width, height);
    SVK_LOGI("android", "engine_started renderer=vulkan width={} height={}", width,
             height);
}

// Java: NativeBridge.nativeResize(int, int)
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeResize(JNIEnv*, jclass, jint width,
                                                 jint height) {
    if (g_renderer) {
        g_renderer->setSize(static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height));
    }
}

// Java Choreographer 的 VSync 回调：由 App 构建本帧内容并提交渲染。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeBeginFrame(JNIEnv*, jclass,
                                                     jlong frameTimeNanos) {
    if (!g_app || !g_renderer || !g_platform) {
        return;
    }
    uint32_t width = 0, height = 0;
    g_platform->getSurfaceSize(&width, &height);
    const double timeSeconds = static_cast<double>(frameTimeNanos) * 1e-9;
    svk::ui::Canvas canvas;
    const svk::AppFrameContext context{width, height, timeSeconds};
    g_app->onFrame(canvas, context);
    g_renderer->render(canvas);
}

// Java: NativeBridge.nativeDestroy()
// surfaceDestroyed 语义 = 保活挂起：只释放 surface 级资源，引擎级资源
// （设备/纹理/命令设施）保留，下次 surfaceCreated 由 nativeInit 热恢复。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroy(JNIEnv*, jclass) {
    if (g_renderer) {
        g_renderer->suspendSurface();
    }
    if (g_platform) {
        svkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }
    SVK_LOGI("android", "engine_suspended renderer=vulkan");
    svk::log::flush();
}

// Java: NativeBridge.nativeShutdown()
// Activity.onDestroy 语义 = 真正销毁：全量释放，之后若再启动走完整冷启动。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
    g_app.reset();
    delete g_renderer;
    g_renderer = nullptr;
    if (g_platform) {
        svkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }
    SVK_LOGI("android", "engine_stopped renderer=vulkan");
    svk::log::flush();
}
