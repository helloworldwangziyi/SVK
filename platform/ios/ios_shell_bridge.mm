// ============================================================================
// iOS C 桥实现（最小形态）：UIKit 薄壳与 core 之间的翻译层。
// 与 Android 的 android_jni_bridge.cpp 同构：
//   CAMetalLayer → IosPlatform（原生 Metal 后端）
//   CADisplayLink 帧回调 → IApp 构建本帧内容并提交 Renderer。
//
// 线程规则：本文件所有函数都在 iOS 主线程调用，core 的无锁单线程假设成立。
// 日志：stdout（Xcode 控制台可见）+ Application Support/logs 完整文件。
// ============================================================================

#import <Foundation/Foundation.h>

#include <string>

#include "ios_shell_bridge.h"

#include "app.h"
#include "log/log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

// ios_platform.mm 以 C 链接导出的工厂函数（与 Android 同款约定）：
// bridge 只需声明原型即可调用，无需知道 IosPlatform 类的布局。
extern "C" svk::IPlatform* svkCreateIosPlatform(const void* layer);
extern "C" void svkDestroyIosPlatform(svk::IPlatform* platform);

// App 与渲染器随引擎创建、svkIosShutdown 销毁；surface 挂起时保留。
// svkIosInit 建/热恢复、svkIosSuspend 挂起。全部在主线程访问，无锁。
static std::unique_ptr<svk::IApp> g_app;
static svk::IRasterSurface* g_renderer = nullptr;
static svk::IPlatform* g_platform = nullptr;
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
        SVK_LOGI("log", "file_started platform=ios path={}", filePath);
    }
}

void svkIosInit(const void* layer) {
    ensureLogger();

    if (g_renderer) {
        // 保活热恢复：引擎级资源（设备/纹理/命令设施）仍在，只按新 layer
        // 重建 surface 级资源；失败则全量拆除，落到下面的冷启动兜底。
        svk::IPlatform* resumed = svkCreateIosPlatform(layer);
        if (resumed && g_renderer->resumeSurface(resumed)) {
            svkDestroyIosPlatform(g_platform);
            g_platform = resumed;
            SVK_LOGI("ios", "engine_resumed renderer=metal");
            return;
        }
        SVK_LOGE("ios", "engine_resume_failed action=cold_start");
        if (resumed) {
            svkDestroyIosPlatform(resumed);
        }
        delete g_renderer;
        g_renderer = nullptr;
        svkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }

    g_platform = svkCreateIosPlatform(layer);
    if (!g_platform) {
        SVK_LOGE("ios", "engine_start_failed phase=create_platform");
        return;
    }

    g_renderer = svk::createRasterSurface(g_platform).release();
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 svkIosInit 能干净重试。
        SVK_LOGE("ios", "engine_start_failed phase=initialize_renderer");
        delete g_renderer;
        g_renderer = nullptr;
        svkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    if (!g_app) {
        g_app = svk::createApp();
    }

    g_platform->getSurfaceSize(&g_width, &g_height);
    g_renderer->setSize(g_width, g_height);
    SVK_LOGI("ios", "engine_started renderer=metal width={} height={}", g_width,
             g_height);
}

void svkIosResize(int32_t width, int32_t height) {
    g_width = static_cast<uint32_t>(width);
    g_height = static_cast<uint32_t>(height);
    if (g_renderer) {
        g_renderer->setSize(g_width, g_height);
    }
}

void svkIosBeginFrame(int64_t frameTimeNanos) {
    if (!g_app || !g_renderer) {
        return;
    }
    const double timeSeconds = static_cast<double>(frameTimeNanos) * 1e-9;
    svk::ui::Canvas canvas;
    const svk::AppFrameContext context{g_width, g_height, timeSeconds};
    g_app->onFrame(canvas, context);
    g_renderer->render(canvas);
}

void svkIosSuspend(void) {
    if (g_renderer) {
        g_renderer->suspendSurface();
    }
    if (g_platform) {
        svkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    SVK_LOGI("ios", "engine_suspended renderer=metal");
    svk::log::flush();
}

void svkIosShutdown(void) {
    g_app.reset();
    delete g_renderer;
    g_renderer = nullptr;
    if (g_platform) {
        svkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    g_width = g_height = 0;
    SVK_LOGI("ios", "engine_stopped renderer=metal");
    svk::log::flush();
}
