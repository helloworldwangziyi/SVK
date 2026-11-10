#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace svk {

class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual const char* getSurfaceExtensionName() const = 0;
    virtual bool createVulkanSurface(VkInstance instance,
                                     VkSurfaceKHR* surface) = 0;
    virtual void getSurfaceSize(uint32_t* width, uint32_t* height) = 0;
};

} // namespace svk
