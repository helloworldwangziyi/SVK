#include "renderer.h"

#include "log/log.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace svk {

namespace {
constexpr uint32_t kMaxFrameAttempts = 3;
constexpr uint64_t kWaitNanos = 16000000;
}

std::unique_ptr<IRasterSurface> createRasterSurface(IPlatform* platform) {
    return std::make_unique<Renderer>(platform);
}

Renderer::Renderer(IPlatform* platform)
    : context_(platform),
      textureCache_(context_),
      swapchain_(context_),
      uiPipeline_(context_) {}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize() {
    if (!context_.initialize() || !textureCache_.initialize() ||
        !swapchain_.create()) {
        return false;
    }
    if (!uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                            swapchain_.msaaSamples(),
                            textureCache_.descriptorSetLayout()) ||
        !swapchain_.createFramebuffers()) {
        return false;
    }
    if (!createCommandPool() || !createVertexBuffers() ||
        !createCommandBuffers() || !createSyncObjects()) {
        return false;
    }
    swapchain_.takeRebuildRequest();
    initialized_ = true;
    return true;
}

void Renderer::shutdown() {
    initialized_ = false;
    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        for (VkFence fence : inFlightFences_) {
            vkDestroyFence(device, fence, nullptr);
        }
        for (VkSemaphore semaphore : renderFinishedSemaphores_) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        for (VkSemaphore semaphore : imageAvailableSemaphores_) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        destroyVertexBuffers();
        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }
        textureCache_.shutdown();
        uiPipeline_.destroy();
        swapchain_.cleanup();
    }
    context_.shutdown();
    inFlightFences_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableSemaphores_.clear();
    commandBuffers_.clear();
    currentFrame_ = 0;
}

bool Renderer::createCommandPool() {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &count,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &count,
                                             families.data());
    uint32_t graphicsFamily = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }

    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = graphicsFamily;
    if (vkCreateCommandPool(context_.device(), &createInfo, nullptr, &commandPool_) !=
        VK_SUCCESS) {
        SVK_LOGE("renderer", "create_command_pool_failed");
        return false;
    }
    return true;
}

bool Renderer::createVertexBuffers() {
    vertexBuffers_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);
    vertexBufferMemorys_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);
    vertexBufferCapacities_.assign(gpu::kMaxFramesInFlight, 0);
    for (uint32_t frameSlot = 0; frameSlot < gpu::kMaxFramesInFlight; ++frameSlot) {
        if (!createVertexBuffer(frameSlot, kInitialVertexCapacity)) {
            destroyVertexBuffers();
            return false;
        }
    }
    return true;
}

bool Renderer::createVertexBuffer(uint32_t frameSlot, uint32_t capacity) {
    const VkDevice device = context_.device();
    if (frameSlot >= vertexBuffers_.size() || capacity == 0) {
        return false;
    }

    const VkDeviceSize bufferSize =
        sizeof(ui::UiVertex) * static_cast<VkDeviceSize>(capacity);
    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &newBuffer) != VK_SUCCESS) {
        SVK_LOGE("renderer", "create_vertex_buffer_failed operation=create_buffer");
        return false;
    }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, newBuffer, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = context_.findMemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &allocateInfo, nullptr, &newMemory) != VK_SUCCESS) {
        SVK_LOGE("renderer", "create_vertex_buffer_failed operation=allocate_memory");
        vkDestroyBuffer(device, newBuffer, nullptr);
        return false;
    }
    if (vkBindBufferMemory(device, newBuffer, newMemory, 0) != VK_SUCCESS) {
        SVK_LOGE("renderer", "create_vertex_buffer_failed operation=bind_memory");
        vkFreeMemory(device, newMemory, nullptr);
        vkDestroyBuffer(device, newBuffer, nullptr);
        return false;
    }

    if (vertexBuffers_[frameSlot] != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vertexBuffers_[frameSlot], nullptr);
    }
    if (vertexBufferMemorys_[frameSlot] != VK_NULL_HANDLE) {
        vkFreeMemory(device, vertexBufferMemorys_[frameSlot], nullptr);
    }
    vertexBuffers_[frameSlot] = newBuffer;
    vertexBufferMemorys_[frameSlot] = newMemory;
    vertexBufferCapacities_[frameSlot] = capacity;
    return true;
}

void Renderer::destroyVertexBuffers() {
    const VkDevice device = context_.device();
    for (VkBuffer buffer : vertexBuffers_) {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
        }
    }
    for (VkDeviceMemory memory : vertexBufferMemorys_) {
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }
    vertexBuffers_.clear();
    vertexBufferMemorys_.clear();
    vertexBufferCapacities_.clear();
}

bool Renderer::uploadVertices(const ui::UiVertex* data, uint32_t count,
                              uint32_t frameSlot) {
    if (count == 0) {
        return true;
    }
    if (!data || frameSlot >= vertexBuffers_.size()) {
        return false;
    }
    if (count > vertexBufferCapacities_[frameSlot]) {
        uint32_t capacity =
            std::max(kInitialVertexCapacity, vertexBufferCapacities_[frameSlot]);
        while (capacity < count) {
            if (capacity > std::numeric_limits<uint32_t>::max() / 2) {
                capacity = count;
                break;
            }
            capacity *= 2;
        }
        if (!createVertexBuffer(frameSlot, capacity)) {
            return false;
        }
    }

    const VkDeviceSize byteSize =
        sizeof(ui::UiVertex) * static_cast<VkDeviceSize>(count);
    void* mapped = nullptr;
    if (vkMapMemory(context_.device(), vertexBufferMemorys_[frameSlot], 0, byteSize,
                    0, &mapped) != VK_SUCCESS) {
        SVK_LOGE("renderer", "vertex_upload_failed operation=map_memory");
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(byteSize));
    vkUnmapMemory(context_.device(), vertexBufferMemorys_[frameSlot]);
    return true;
}

bool Renderer::createCommandBuffers() {
    commandBuffers_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount =
        static_cast<uint32_t>(commandBuffers_.size());
    if (vkAllocateCommandBuffers(context_.device(), &allocateInfo,
                                 commandBuffers_.data()) != VK_SUCCESS) {
        SVK_LOGE("renderer", "allocate_command_buffers_failed");
        return false;
    }
    return true;
}

bool Renderer::createSyncObjects() {
    const VkDevice device = context_.device();
    imageAvailableSemaphores_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);
    renderFinishedSemaphores_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);
    inFlightFences_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < gpu::kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                              &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                              &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences_[i]) !=
                VK_SUCCESS) {
            SVK_LOGE("renderer", "create_sync_objects_failed frame_slot={}", i);
            return false;
        }
    }
    return true;
}

bool Renderer::recreateSwapchain() {
    vkDeviceWaitIdle(context_.device());
    uiPipeline_.destroy();
    swapchain_.cleanup();
    if (!swapchain_.create()) {
        return false;
    }
    if (!uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                            swapchain_.msaaSamples(),
                            textureCache_.descriptorSetLayout())) {
        return false;
    }
    return swapchain_.createFramebuffers();
}

void Renderer::setSize(uint32_t width, uint32_t height) {
    swapchain_.setSize(width, height);
}

bool Renderer::suspendSurface() {
    if (context_.surface() == VK_NULL_HANDLE) {
        return true;
    }
    if (context_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context_.device());
    }
    uiPipeline_.destroy();
    swapchain_.cleanup();
    context_.destroySurface();
    return true;
}

bool Renderer::resumeSurface(IPlatform* platform) {
    if (!initialized_ || !platform) {
        return false;
    }
    if (context_.surface() != VK_NULL_HANDLE) {
        return true;
    }
    context_.setPlatform(platform);
    if (!context_.recreateSurface() || !swapchain_.create()) {
        return false;
    }
    if (!uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                            swapchain_.msaaSamples(),
                            textureCache_.descriptorSetLayout()) ||
        !swapchain_.createFramebuffers()) {
        return false;
    }
    swapchain_.takeRebuildRequest();
    return true;
}

RenderResult Renderer::render(const ui::Canvas& canvas) {
    if (!initialized_) {
        return RenderResult::Failed;
    }
    if (context_.surface() == VK_NULL_HANDLE ||
        swapchain_.handle() == VK_NULL_HANDLE) {
        return RenderResult::Retry;
    }

    bool extentMismatchWarned = false;
    for (uint32_t attempt = 0; attempt < kMaxFrameAttempts; ++attempt) {
        if (swapchain_.takeRebuildRequest() && !recreateSwapchain()) {
            return RenderResult::Failed;
        }
        if (!swapchain_.matchesRequestedExtent()) {
            if (!extentMismatchWarned) {
                extentMismatchWarned = true;
                const VkExtent2D requested = swapchain_.requestedExtent();
                const VkExtent2D actual = swapchain_.extent();
                SVK_LOGW("renderer",
                         "swapchain_extent_mismatch requested={}x{} actual={}x{} "
                         "transform={} action=rebuild",
                         requested.width, requested.height, actual.width, actual.height,
                         static_cast<int>(swapchain_.surfaceTransform()));
            }
            swapchain_.requestRebuild();
            continue;
        }

        const VkResult fenceResult = vkWaitForFences(
            context_.device(), 1, &inFlightFences_[currentFrame_], VK_TRUE,
            kWaitNanos);
        if (fenceResult == VK_TIMEOUT) {
            return RenderResult::Retry;
        }
        if (fenceResult != VK_SUCCESS) {
            SVK_LOGE("renderer", "wait_fence_failed result={}",
                     static_cast<int>(fenceResult));
            return RenderResult::Failed;
        }

        uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(
            context_.device(), swapchain_.handle(), kWaitNanos,
            imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            return RenderResult::Retry;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!recreateSwapchain()) {
                return RenderResult::Failed;
            }
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            SVK_LOGE("renderer", "acquire_image_failed result={}",
                     static_cast<int>(result));
            return RenderResult::Failed;
        }

        if (!canvas.vertices().empty() &&
            !uploadVertices(canvas.vertices().data(),
                            static_cast<uint32_t>(canvas.vertices().size()),
                            currentFrame_)) {
            SVK_LOGE("renderer", "vertex_upload_failed operation=upload");
            return RenderResult::Failed;
        }

        VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};
        vkResetFences(context_.device(), 1, &inFlightFences_[currentFrame_]);
        vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, canvas);

        VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
        VkPipelineStageFlags waitStages[] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        if (vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo,
                          inFlightFences_[currentFrame_]) != VK_SUCCESS) {
            SVK_LOGE("renderer", "queue_submit_failed");
            return RenderResult::Failed;
        }

        VkSwapchainKHR swapchainHandle = swapchain_.handle();
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchainHandle;
        presentInfo.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_.requestRebuild();
            continue;
        }
        if (result != VK_SUCCESS) {
            SVK_LOGE("renderer", "queue_present_failed result={}",
                     static_cast<int>(result));
            return RenderResult::Failed;
        }

        currentFrame_ = (currentFrame_ + 1) % gpu::kMaxFramesInFlight;
        return RenderResult::Rendered;
    }

    SVK_LOGW("renderer", "frame_skipped reason=swapchain_out_of_date attempts={}",
             kMaxFrameAttempts);
    return RenderResult::Retry;
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                   uint32_t imageIndex,
                                   const ui::Canvas& canvas) {
    const VkExtent2D extent = swapchain_.extent();
    const VkSurfaceTransformFlagBitsKHR transform = swapchain_.surfaceTransform();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    textureCache_.uploadPendingTextures(commandBuffer);

    const float bufferWidth = static_cast<float>(extent.width);
    const float bufferHeight = static_cast<float>(extent.height);
    const bool rotate90or270 = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                               transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    const bool compensate = transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    const float visualWidth = rotate90or270 ? bufferHeight : bufferWidth;
    const float visualHeight = rotate90or270 ? bufferWidth : bufferHeight;
    glm::mat4 mvp = glm::ortho(0.0f, visualWidth, 0.0f, visualHeight, -1.0f, 1.0f);

    if (transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        const glm::mat4 rotation(0.0f, 1.0f, 0.0f, 0.0f,
                                -1.0f, 0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rotation * mvp;
    } else if (transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        const glm::mat4 rotation(-1.0f, 0.0f, 0.0f, 0.0f,
                                 0.0f, -1.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rotation * mvp;
    } else if (transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        const glm::mat4 rotation(0.0f, -1.0f, 0.0f, 0.0f,
                                 1.0f, 0.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rotation * mvp;
    }

    VkClearValue clearValues[2] = {
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
    };
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = swapchain_.renderPass();
    renderPassInfo.framebuffer = swapchain_.framebuffer(imageIndex);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;
    renderPassInfo.clearValueCount =
        swapchain_.msaaSamples() != VK_SAMPLE_COUNT_1_BIT ? 2 : 1;
    renderPassInfo.pClearValues = clearValues;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      uiPipeline_.pipeline());
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkBuffer vertexBuffers[] = {vertexBuffers_[currentFrame_]};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdPushConstants(commandBuffer, uiPipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(mvp), &mvp);

    for (const auto& batch : canvas.batches()) {
        if (batch.vertexCount == 0) {
            continue;
        }
        const ui::Rect clip = ui::Rect::intersect(
            batch.clip, {0.0f, 0.0f, visualWidth, visualHeight});
        ui::Rect bufferClip;
        if (compensate && transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
            bufferClip = {visualHeight - clip.y - clip.h, clip.x, clip.h, clip.w};
        } else if (compensate && transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
            bufferClip = {visualWidth - clip.x - clip.w,
                          visualHeight - clip.y - clip.h, clip.w, clip.h};
        } else if (compensate && transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
            bufferClip = {clip.y, visualWidth - clip.x - clip.w, clip.h, clip.w};
        } else {
            bufferClip = clip;
        }

        const int32_t offsetX = std::max(0, static_cast<int32_t>(bufferClip.x));
        const int32_t offsetY = std::max(0, static_cast<int32_t>(bufferClip.y));
        const int32_t width =
            std::min(static_cast<int32_t>(bufferClip.x + bufferClip.w),
                     static_cast<int32_t>(extent.width)) - offsetX;
        const int32_t height =
            std::min(static_cast<int32_t>(bufferClip.y + bufferClip.h),
                     static_cast<int32_t>(extent.height)) - offsetY;
        if (width <= 0 || height <= 0) {
            continue;
        }

        VkRect2D scissor{};
        scissor.offset = {offsetX, offsetY};
        scissor.extent = {static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height)};
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        VkDescriptorSet set = textureCache_.whiteDescriptorSet();
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                uiPipeline_.layout(), 0, 1, &set, 0, nullptr);
        vkCmdDraw(commandBuffer, batch.vertexCount, 1, batch.firstVertex, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
    vkEndCommandBuffer(commandBuffer);
}

} // namespace svk
