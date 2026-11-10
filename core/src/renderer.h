#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "gpu/vulkan/swapchain.h"
#include "gpu/vulkan/texture_cache.h"
#include "gpu/vulkan/ui_pipeline.h"
#include "gpu/vulkan/vulkan_context.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <vulkan/vulkan.h>

namespace svk {

class Renderer final : public IRasterSurface {
public:
    explicit Renderer(IPlatform* platform);
    ~Renderer() override;

    bool initialize() override;
    RenderResult render(const ui::Canvas& canvas) override;
    void setSize(uint32_t width, uint32_t height) override;
    bool suspendSurface() override;
    bool resumeSurface(IPlatform* platform) override;

private:
    void shutdown();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createVertexBuffers();
    bool createVertexBuffer(uint32_t frameSlot, uint32_t capacity);
    void destroyVertexBuffers();
    bool recreateSwapchain();
    bool uploadVertices(const ui::UiVertex* data, uint32_t count,
                        uint32_t frameSlot);
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                             const ui::Canvas& canvas);

    gpu::VulkanContext context_;
    gpu::TextureCache textureCache_;
    gpu::Swapchain swapchain_;
    gpu::UiPipeline uiPipeline_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
    uint32_t currentFrame_ = 0;
    bool initialized_ = false;

    std::vector<VkBuffer> vertexBuffers_;
    std::vector<VkDeviceMemory> vertexBufferMemorys_;
    std::vector<uint32_t> vertexBufferCapacities_;
    static constexpr uint32_t kInitialVertexCapacity = 8192;
};

} // namespace svk
