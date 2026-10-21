// ============================================================================
// 鸿蒙平台适配：持有 XComponent 下发的 OHNativeWindow 句柄与创建时刻尺寸。
//
// 与 Android 壳（android_vulkan_platform.cpp）的对应关系：
//   ANativeWindow（Java Surface 解包）→ OHNativeWindow（XComponent 独立 surface）
//
// 两处值得注意的差别：
//   - 窗口句柄所有权：Android 由 ANativeWindow_fromSurface 取得、析构时 release；
//     鸿蒙的 OHNativeWindow 由 XComponent 框架持有（OnSurfaceCreated 下发、
//     OnSurfaceDestroyed 回收），本类只借用、不释放。本对象随桥接层在
//     OnSurfaceDestroyed 回调里先销毁（见 hongmeng_napi_bridge.cpp），
//     生命周期天然安全。
//   - 尺寸获取：Android 可随时 ANativeWindow_getWidth 现查；鸿蒙要在
//     XComponent 回调里用 OH_NativeXComponent_GetXComponentSize 查，
//     因此尺寸随构造传入并缓存；之后的尺寸变化由 NAPI 桥通知 Renderer，
//     不经过本类。
//
// Vulkan surface 的创建已收进 core 的 Vulkan 后端
// （VK_USE_PLATFORM_OHOS，见 samples/<app>/hongmeng/entry/src/main/cpp/CMakeLists.txt）；
// 本类只暴露不透明原生窗口句柄，不感知 Vulkan。
// ============================================================================

#include "render_platform.h"

#include <cstdint>

#include <native_window/external_window.h>

#include "log/log.h"

namespace svk {

class HongmengPlatform : public IPlatform {
public:
    HongmengPlatform(OHNativeWindow* window, uint32_t width, uint32_t height)
        : window_(window), width_(width), height_(height) {}

    // 后端（core 的 Vulkan 后端）经本句柄创建 VkSurfaceKHR；本类不再感知 Vulkan。
    void* nativeWindowHandle() const override { return window_; }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        *width = width_;
        *height = height_;
    }

private:
    OHNativeWindow* window_ = nullptr; // 借用指针，XComponent 框架持有
    uint32_t width_ = 0;               // 创建时刻的物理像素尺寸
    uint32_t height_ = 0;
};

IPlatform* createHongmengPlatform(OHNativeWindow* window, uint32_t width, uint32_t height) {
    return new HongmengPlatform(window, width, height);
}

void destroyHongmengPlatform(IPlatform* platform) {
    delete platform;
}

} // namespace svk

extern "C" {
// 以 C 链接导出工厂函数，NAPI 桥接层无需感知 HongmengPlatform 类的内存布局
// （与 Android svkCreateAndroidPlatform 同款约定）。
svk::IPlatform* svkCreateHongmengPlatform(void* window, int32_t width, int32_t height) {
    return svk::createHongmengPlatform(static_cast<OHNativeWindow*>(window),
                                       static_cast<uint32_t>(width),
                                       static_cast<uint32_t>(height));
}

void svkDestroyHongmengPlatform(svk::IPlatform* platform) {
    svk::destroyHongmengPlatform(platform);
}
} // extern "C"
