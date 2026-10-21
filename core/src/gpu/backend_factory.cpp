/**
 * @file backend_factory.cpp
 * @brief 后端装配点实现：按编译目标选择 gpu::IGpuBackend 实现。
 *
 * iOS 工程定义 SVK_METAL_BACKEND 时链入 gpu/metal/ 的 Metal 后端；
 * 其余目标（Android / 鸿蒙 / host）默认走 gpu/vulkan/ 的 VulkanBackend。
 */
#include "gpu/rhi.h"

#if defined(SVK_METAL_BACKEND)
// 契约头由 Metal 后端任务提供，与本文件解耦：
namespace svk::gpu::metal {
std::unique_ptr<IGpuBackend> createMetalBackend(IPlatform* platform);
}
#else
#include "gpu/vulkan/vulkan_backend.h"
#endif

namespace svk::gpu {

const char* toString(BackendType type) {
    switch (type) {
        case BackendType::Vulkan: return "vulkan";
        case BackendType::Metal:  return "metal";
    }
    return "unknown";
}

std::unique_ptr<IGpuBackend> createBackend(IPlatform* platform) {
#if defined(SVK_METAL_BACKEND)
    return metal::createMetalBackend(platform);
#else
    return std::make_unique<VulkanBackend>(platform);
#endif
}

} // namespace svk::gpu
