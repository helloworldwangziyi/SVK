// ============================================================================
// iOS C 桥实现（最小形态）：UIKit 薄壳与 core 之间的翻译层。
// 与 Android 的 android_jni_bridge.cpp 同构：
//   CAMetalLayer → IosPlatform（vkCreateMetalSurfaceEXT，经 MoltenVK）
//   CADisplayLink 帧回调 → 每帧构建三角形并提交 Renderer。
//
// 线程规则：本文件所有函数都在 iOS 主线程调用，core 的无锁单线程假设成立。
// 日志：core 默认 stdout（Xcode 控制台可见）；os_log sink 见母项目完整版。
// ============================================================================

#import <Foundation/Foundation.h>

#include "ios_shell_bridge.h"

#include "log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

// 共享的示例内容：每帧一个三色渐变三角形。
#include "samples/triangle.h"

// ios_vulkan_platform.mm 以 C 链接导出的工厂函数（与 Android 同款约定）：
// bridge 只需声明原型即可调用，无需知道 IosPlatform 类的布局。
extern "C" evk::IPlatform* evkCreateIosPlatform(const void* layer);
extern "C" void evkDestroyIosPlatform(evk::IPlatform* platform);

// 全局渲染器与平台适配器。保活语义（对照 Android）：evkIosInit 建/热恢复、
// evkIosSuspend 挂起（引擎级资源保留）、evkIosShutdown 全量拆。
// 全部在主线程访问，无锁。
static evk::IRasterSurface* g_renderer = nullptr;
static evk::IPlatform* g_platform = nullptr;
static uint32_t g_width = 0;
static uint32_t g_height = 0;

void evkIosInit(const void* layer) {
    if (g_renderer) {
        // 保活热恢复：引擎级资源（设备/纹理/命令设施）仍在，只按新 layer
        // 重建 surface 级资源；失败则全量拆除，落到下面的冷启动兜底。
        evk::IPlatform* resumed = evkCreateIosPlatform(layer);
        if (resumed && g_renderer->resumeSurface(resumed)) {
            evkDestroyIosPlatform(g_platform);
            g_platform = resumed;
            EVK_LOGI("ios", "engine_resumed renderer=vulkan");
            return;
        }
        EVK_LOGE("ios", "engine_resume_failed action=cold_start");
        if (resumed) {
            evkDestroyIosPlatform(resumed);
        }
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }

    g_platform = evkCreateIosPlatform(layer);
    if (!g_platform) {
        EVK_LOGE("ios", "engine_start_failed phase=create_platform");
        return;
    }

    g_renderer = evk::createRasterSurface(g_platform).release();
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次 evkIosInit 能干净重试。
        EVK_LOGE("ios", "engine_start_failed phase=initialize_renderer");
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
        return;
    }
    g_platform->getSurfaceSize(&g_width, &g_height);
    g_renderer->setSize(g_width, g_height);
    EVK_LOGI("ios", "engine_started renderer=vulkan width={} height={}", g_width,
             g_height);
}

void evkIosResize(int32_t width, int32_t height) {
    g_width = static_cast<uint32_t>(width);
    g_height = static_cast<uint32_t>(height);
    if (g_renderer) {
        g_renderer->setSize(g_width, g_height);
    }
}

void evkIosBeginFrame(int64_t frameTimeNanos) {
    if (!g_renderer) {
        return;
    }
    const double timeSeconds = static_cast<double>(frameTimeNanos) * 1e-9;
    evk::ui::Canvas canvas;
    evk::demo::buildTriangleFrame(canvas, g_width, g_height, timeSeconds);
    g_renderer->render(canvas);
}

void evkIosSuspend(void) {
    if (g_renderer) {
        g_renderer->suspendSurface();
    }
    if (g_platform) {
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("ios", "engine_suspended renderer=vulkan");
}

void evkIosShutdown(void) {
    delete g_renderer;
    g_renderer = nullptr;
    if (g_platform) {
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    g_width = g_height = 0;
    EVK_LOGI("ios", "engine_stopped renderer=vulkan");
}
