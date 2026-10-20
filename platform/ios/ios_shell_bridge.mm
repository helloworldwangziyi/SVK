// ============================================================================
// iOS C 桥实现（最小形态）：UIKit 薄壳与 core 之间的翻译层。
// 与 Android 的 android_jni_bridge.cpp 同构：
//   CAMetalLayer → IosPlatform（vkCreateMetalSurfaceEXT，经 MoltenVK）
//   CADisplayLink 帧回调 → IApp 构建本帧内容并提交 Renderer。
//
// 线程规则：本文件所有函数都在 iOS 主线程调用，core 的无锁单线程假设成立。
// 日志：stdout（Xcode 控制台可见）+ Application Support/logs 完整文件。
// ============================================================================

#import <Foundation/Foundation.h>

#include <string>

#include "ios_shell_bridge.h"

#include "app.h"
#include "log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

// ios_vulkan_platform.mm 以 C 链接导出的工厂函数（与 Android 同款约定）：
// bridge 只需声明原型即可调用，无需知道 IosPlatform 类的布局。
extern "C" svp::IPlatform* svpCreateIosPlatform(const void* layer);
extern "C" void svpDestroyIosPlatform(svp::IPlatform* platform);

// App 与渲染器随引擎创建、svpIosShutdown 销毁；surface 挂起时保留。
// svpIosInit 建/热恢复、svpIosSuspend 挂起。全部在主线程访问，无锁。
static std::unique_ptr<svp::IApp> g_app;
static svp::IRasterSurface* g_renderer = nullptr;
static svp::IPlatform* g_platform = nullptr;
static uint32_t g_width = 0;
static uint32_t g_height = 0;
static std::string g_reportedLogFile;

static std::string logDirectory() {
    @autoreleasepool {
        NSArray<NSString*>* directories = NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES);
        NSString* base = directories.firstObject;
        if (!base) {
            return {};
        }
        NSString* directory = [base stringByAppendingPathComponent:@"logs"];
        return directory.UTF8String ? directory.UTF8String : "";
    }
}

static void ensureLogger() {
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("estarx-vulkan");
    }

    svp::log::LogFileConfig config;
    config.directory = logDirectory();
    if (config.directory.empty()) {
        svp::log::init(logger);
        return;
    }

    std::string filePath;
    if (!svp::log::initWithFile(logger, config, &filePath)) {
        return;
    }
    if (g_reportedLogFile != filePath) {
        g_reportedLogFile = filePath;
        SVP_LOGI("log", "file_started platform=ios path={}", filePath);
    }
}

void svpIosInit(const void* layer) {
    ensureLogger();

    if (g_renderer) {
        // 保活热恢复：引擎级资源（设备/纹理/命令设施）仍在，只按新 layer
        // 重建 surface 级资源；失败则全量拆除，落到下面的冷启动兜底。
        svp::IPlatform* resumed = svpCreateIosPlatform(layer);
        if (resumed && g_renderer->resumeSurface(resumed)) {
            svpDestroyIosPlatform(g_platform);
            g_platform = resumed;
            SVP_LOGI("ios", "engine_resumed renderer=vulkan");
            return;
        }
        SVP_LOGE("ios", "engine_resume_failed action=cold_start");
        if (resumed) {
            svpDestroyIosPlatform(resumed);
        }
        delete g_renderer;
        g_renderer = nullptr;
        svpDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }

    g_platform = svpCreateIosPlatform(layer);
    if (!g_platform) {
        SVP_LOGE("ios", "engine_start_failed phase=create_platform");
        return;
    }

    g_renderer = svp::createRasterSurface(g_platform).release();
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 svpIosInit 能干净重试。
        SVP_LOGE("ios", "engine_start_failed phase=initialize_renderer");
        delete g_renderer;
        g_renderer = nullptr;
        svpDestroyIosPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    if (!g_app) {
        g_app = svp::createApp();
    }

    g_platform->getSurfaceSize(&g_width, &g_height);
    g_renderer->setSize(g_width, g_height);
    SVP_LOGI("ios", "engine_started renderer=vulkan width={} height={}", g_width,
             g_height);
}

void svpIosResize(int32_t width, int32_t height) {
    g_width = static_cast<uint32_t>(width);
    g_height = static_cast<uint32_t>(height);
    if (g_renderer) {
        g_renderer->setSize(g_width, g_height);
    }
}

void svpIosBeginFrame(int64_t frameTimeNanos) {
    if (!g_app || !g_renderer) {
        return;
    }
    const double timeSeconds = static_cast<double>(frameTimeNanos) * 1e-9;
    svp::ui::Canvas canvas;
    const svp::AppFrameContext context{g_width, g_height, timeSeconds};
    g_app->onFrame(canvas, context);
    g_renderer->render(canvas);
}

void svpIosSuspend(void) {
    if (g_renderer) {
        g_renderer->suspendSurface();
    }
    if (g_platform) {
        svpDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    SVP_LOGI("ios", "engine_suspended renderer=vulkan");
    svp::log::flush();
}

void svpIosShutdown(void) {
    g_app.reset();
    delete g_renderer;
    g_renderer = nullptr;
    if (g_platform) {
        svpDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    g_width = g_height = 0;
    SVP_LOGI("ios", "engine_stopped renderer=vulkan");
    svp::log::flush();
}
