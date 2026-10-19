// ============================================================================
// JNI 实现层：Java 薄壳(NativeBridge)与 core 之间的翻译层。
//
// 命名约定（静态注册）：函数名 = Java_<包名>_<类名>_<方法名>，
// 包名中的 . 换成 _。extern "C" 保持符号不被 name mangling，
// JVM 才能按名字在 .so 里查到。本层只做"解包参数 → 转发 core"，
// 不含任何业务逻辑；所有函数都运行在 Android UI 线程。
// ============================================================================

#include <jni.h>

#include <spdlog/sinks/android_sink.h>

#include "app.h"
#include "log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

// android_vulkan_platform.cpp 以 C 链接导出的工厂函数：
// bridge 只需声明原型即可调用，无需知道 AndroidPlatform 类的布局。
extern "C" evk::IPlatform* evkCreateAndroidPlatform(JNIEnv* env, jobject surface);
extern "C" void evkDestroyAndroidPlatform(evk::IPlatform* platform);

// App 与渲染器随引擎冷启动创建、nativeShutdown 销毁；g_platform 与 surface
// 一致（nativeInit 建或热恢复 / nativeDestroy 拆）。全部在 UI 线程访问，无锁。
static std::unique_ptr<evk::IApp> g_app;
static evk::IRasterSurface* g_renderer = nullptr;
static evk::IPlatform* g_platform = nullptr;

// Java: NativeBridge.nativeInit(Surface)
// surface 创建就绪时调用（可能多次：退后台重建）。已有引擎时走保活热恢复，
// 否则冷启动；失败时清理现场，保证下次 surfaceCreated 能干净重试。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeInit(JNIEnv* env, jclass, jobject surface) {
    // logcat sink 由平台壳注入；core 默认只提供 stdout（见 evk/log.h）。
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::android_logger_mt("estarx-vulkan", "estarx-vulkan");
    }
    evk::log::init(logger);

    if (g_renderer) {
        // 保活热恢复：引擎级资源（设备/纹理/命令设施）仍在，只按新 surface
        // 重建 surface 级资源；失败则全量拆除，落到下面的冷启动兜底。
        evk::IPlatform* resumed = evkCreateAndroidPlatform(env, surface);
        if (resumed && g_renderer->resumeSurface(resumed)) {
            evkDestroyAndroidPlatform(g_platform);
            g_platform = resumed;
            EVK_LOGI("android", "engine_resumed renderer=vulkan");
            return;
        }
        EVK_LOGE("android", "engine_resume_failed action=cold_start");
        if (resumed) {
            evkDestroyAndroidPlatform(resumed);
        }
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }

    g_platform = evkCreateAndroidPlatform(env, surface);
    if (!g_platform) {
        EVK_LOGE("android", "engine_start_failed phase=create_platform");
        return;
    }

    g_renderer = evk::createRasterSurface(g_platform).release();
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 surfaceCreated 能干净重试。
        EVK_LOGE("android", "engine_start_failed phase=initialize_renderer");
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    if (!g_app) {
        g_app = evk::createApp();
    }

    uint32_t width = 0, height = 0;
    g_platform->getSurfaceSize(&width, &height);
    g_renderer->setSize(width, height);
    EVK_LOGI("android", "engine_started renderer=vulkan width={} height={}", width,
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
    evk::ui::Canvas canvas;
    const evk::AppFrameContext context{width, height, timeSeconds};
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
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("android", "engine_suspended renderer=vulkan");
}

// Java: NativeBridge.nativeShutdown()
// Activity.onDestroy 语义 = 真正销毁：全量释放，之后若再启动走完整冷启动。
extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
    g_app.reset();
    delete g_renderer;
    g_renderer = nullptr;
    if (g_platform) {
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("android", "engine_stopped renderer=vulkan");
}
