#include "render_platform.h"

#include <cstdint>
#include <native_window/external_window.h>

#include "log/log.h"

namespace svk {

class HongmengPlatform final : public IPlatform {
public:
    HongmengPlatform(OHNativeWindow* window, uint32_t width, uint32_t height)
        : window_(window), width_(width), height_(height) {}

    const char* getSurfaceExtensionName() const override {
        return VK_OHOS_SURFACE_EXTENSION_NAME;
    }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override {
        if (!window_) {
            SVK_LOGE("harmony", "surface_create_failed reason=native_window_null");
            return false;
        }
        VkSurfaceCreateInfoOHOS createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
        createInfo.window = window_;
        if (vkCreateSurfaceOHOS(instance, &createInfo, nullptr, surface) != VK_SUCCESS) {
            SVK_LOGE("harmony", "surface_create_failed reason=vulkan_surface_failed");
            return false;
        }
        return true;
    }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        *width = width_;
        *height = height_;
    }

private:
    OHNativeWindow* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace svk

extern "C" svk::IPlatform* svkCreateHongmengPlatform(void* window, int32_t width,
                                                     int32_t height) {
    return new svk::HongmengPlatform(static_cast<OHNativeWindow*>(window),
                                     static_cast<uint32_t>(width),
                                     static_cast<uint32_t>(height));
}

extern "C" void svkDestroyHongmengPlatform(svk::IPlatform* platform) {
    delete platform;
}
