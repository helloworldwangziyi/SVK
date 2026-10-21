/**
 * @file vulkan_backend.cpp
 * @brief Vulkan 渲染后端实现：帧执行设施（命令池/顶点缓冲/同步原语）与
 *        acquire → 录制 → 提交 → present 的 renderFrame 主流程。
 *
 * GPU 设施分属同目录子模块（VulkanContext / Swapchain / UiPipeline /
 * TextureCache）；本文件只保留帧执行原语与一帧的完整执行，重试语义与
 * 日志消息沿用旧 vulkan_renderer.cpp。
 */
// 后端主头文件：声明本文件要实现的所有方法与句柄成员
// （GLM_FORCE_DEPTH_ZERO_TO_ONE 已在 vulkan_backend.h 顶部统一定义）。
#include "gpu/vulkan/vulkan_backend.h"
// SVK_LOGI/W/E 日志宏：spdlog 封装，全局统一的日志入口。
#include "log/log.h"

// std::max / std::min：裁剪矩形的边界 clamp 要用。
#include <algorithm>
// std::memcpy：把顶点数据拷进映射内存。
#include <cstring>
#include <limits>
// std::vector：承接 Vulkan 枚举惯用法返回的列表。
#include <vector>

// glm 核心类型（mat4 等）与 glm::ortho：投影矩阵构建。
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace svk::gpu {

/// 单次 renderFrame() 内"重建交换链 → 重画"的最大重试次数（见 renderFrame 注释）。
constexpr uint32_t kMaxFrameAttempts = 3;

VulkanBackend::VulkanBackend(IPlatform* platform)
    : context_(platform), textureCache_(context_), swapchain_(context_),
      uiPipeline_(context_) {}

VulkanBackend::~VulkanBackend() {
    // 析构兜底调 shutdownDevice()；内部对空句柄有判断，重复调用也安全。
    shutdownDevice();
}

bool VulkanBackend::initializeDevice() {
    // 按依赖顺序搭建设备级 Vulkan 栈（与 surface 视觉尺寸无关，挂起/恢复期间保留）。
    // 每一步都依赖前一步，所以任何一环失败都会立刻停止启动。
    // ① 地基：instance / surface / 物理与逻辑设备（gpu::VulkanContext）。
    //    surface 在设备选取前创建：pickPhysicalDevice / createLogicalDevice 都要
    //    查询 surface 的 present 支持，这是 Vulkan 的标准顺序。
    SVK_LOGI("renderer", "initialize_stage_start stage=vulkan_context");
    if (!context_.initialize()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=vulkan_context");
    // ② 纹理设施：descriptor 布局是管线布局的输入（shader 的 set=0），必须先于管线。
    SVK_LOGI("renderer", "initialize_stage_start stage=texture_cache");
    if (!textureCache_.initialize()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=texture_cache");
    // ④ 执行设施：命令池、顶点缓冲、命令缓冲、同步原语，与 swapchain 尺寸无关。
    SVK_LOGI("renderer", "initialize_stage_start stage=command_pool");
    if (!createCommandPool()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=command_pool");
    SVK_LOGI("renderer", "initialize_stage_start stage=vertex_buffers");
    if (!createVertexBuffers()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=vertex_buffers");
    SVK_LOGI("renderer", "initialize_stage_start stage=command_buffers");
    if (!createCommandBuffers()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=command_buffers");
    SVK_LOGI("renderer", "initialize_stage_start stage=sync_objects");
    if (!createSyncObjects()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=sync_objects");
    return true;
}

bool VulkanBackend::createSurface() {
    // surface 与交换资源俱在视为已就绪（幂等）：冷启动时 ① 已为设备选取建过
    // surface，但 swapchain/管线/帧缓冲（③ 段）还没走，不能只凭 surface 判就绪。
    if (context_.surface() != VK_NULL_HANDLE &&
        swapchain_.handle() != VK_NULL_HANDLE) {
        return true;
    }
    // 重建顺序固定：surface → swapchain → 管线 → 帧缓冲（同旧 resumeSurface ③ 段）。
    if (!context_.recreateSurface()) return false;
    if (!swapchain_.create()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=swapchain");
    if (!uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                            swapchain_.msaaSamples(),
                            textureCache_.descriptorSetLayout())) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=ui_pipeline");
    if (!swapchain_.createFramebuffers()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=framebuffers");
    // 新 surface 的尺寸已直接用于建链，不必在首帧重复重建。
    swapchain_.takeRebuildRequest();
    return true;
}

void VulkanBackend::destroySurface() {
    // 与 recreateSwapchain 同序销毁"依赖 swapchain 的一段"，再销毁 surface 本体。
    // 句柄级判空在各自模块内，重复调用安全（Renderer 层已先 waitIdle）。
    uiPipeline_.destroy();
    swapchain_.cleanup();
    context_.destroySurface();
}

void VulkanBackend::shutdownDevice() {
    // 按相反顺序释放资源。设备必须先空闲，才能销毁任何仍在飞行中的对象。
    // 初始化可能中途失败退出，device 仍是空句柄，先判一下。
    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) {
        // 先等 GPU 跑完所有在飞工作：销毁仍在被 GPU 使用的对象是未定义行为。
        vkDeviceWaitIdle(device);

        // 同步原语：fence 与两类 semaphore，每帧一套。
        for (auto fence : inFlightFences_) vkDestroyFence(device, fence, nullptr);
        for (auto sem : renderFinishedSemaphores_)
            vkDestroySemaphore(device, sem, nullptr);
        for (auto sem : imageAvailableSemaphores_)
            vkDestroySemaphore(device, sem, nullptr);

        // 每个 in-flight 帧有自己的动态顶点缓冲。
        destroyVertexBuffers();

        // 销毁命令池会连带释放从中分配的所有命令缓冲。
        vkDestroyCommandPool(device, commandPool_, nullptr);

        // 纹理设施（白纹理 / 采样器 / descriptor 池与布局）。
        textureCache_.shutdown();

        // 管线与 swapchain 相关资源各归其模块集中清理。
        uiPipeline_.destroy();
        swapchain_.cleanup();
    }

    // device / surface / debug messenger / instance 归 VulkanContext 收尾（幂等）。
    context_.shutdown();

    // 清空所有持有句柄的 vector，避免残留已销毁的句柄值。
    inFlightFences_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableSemaphores_.clear();
    commandBuffers_.clear();
}

void VulkanBackend::setPlatform(IPlatform* platform) {
    context_.setPlatform(platform);
}

void VulkanBackend::setTargetSize(uint32_t width, uint32_t height) {
    swapchain_.setSize(width, height);
}

void VulkanBackend::waitIdle() {
    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
}

bool VulkanBackend::createCommandPool() {
    const VkDevice device = context_.device();

    // command pool 允许命令缓冲每帧重置并重新录制。
    // 命令池要从某个队列族分配命令缓冲：先再查一次图形族索引。
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &qCount,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &qCount,
                                             qf.data());

    // 找第一个带 GRAPHICS 位的族。
    uint32_t graphicsFamily = 0;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // RESET_COMMAND_BUFFER_BIT：允许单独 vkResetCommandBuffer 重录某条缓冲；
    // 我们每帧都要重录命令，必须开这个位（否则只能整池重置）。
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    // 池绑定队列族：从这里分配的命令缓冲只能提交到该族的队列。
    poolInfo.queueFamilyIndex = graphicsFamily;

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool_) !=
        VK_SUCCESS) {
        SVK_LOGE("renderer", "create_command_pool_failed");
        return false;
    }
    SVK_LOGI("renderer", "command_pool_created queue_family={} flags=reset_command_buffer",
             graphicsFamily);
    return true;
}

bool VulkanBackend::createVertexBuffers() {
    vertexBuffers_.assign(kMaxFramesInFlight, VK_NULL_HANDLE);
    vertexBufferMemorys_.assign(kMaxFramesInFlight, VK_NULL_HANDLE);
    vertexBufferCapacities_.assign(kMaxFramesInFlight, 0);
    for (uint32_t frameSlot = 0; frameSlot < kMaxFramesInFlight; ++frameSlot) {
        if (!createVertexBuffer(frameSlot, kInitialVertexCapacity)) {
            destroyVertexBuffers();
            return false;
        }
    }
    SVK_LOGI("renderer", "vertex_buffers_created count={} initial_capacity={} bytes_each={}",
             vertexBuffers_.size(), kInitialVertexCapacity,
             sizeof(ui::UiVertex) * static_cast<size_t>(kInitialVertexCapacity));
    return true;
}

bool VulkanBackend::createVertexBuffer(uint32_t frameSlot, uint32_t capacity) {
    const VkDevice device = context_.device();

    if (frameSlot >= vertexBuffers_.size() || capacity == 0) {
        return false;
    }
    // 每个帧槽独占一块 HOST_VISIBLE|HOST_COHERENT 缓冲，容量按需扩展。
    const VkDeviceSize bufferSize =
        sizeof(ui::UiVertex) * static_cast<VkDeviceSize>(capacity);
    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;

    // VkBuffer 只是"一块多大、干什么用"的描述对象，本身不带内存。
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    // usage = VERTEX_BUFFER：稍后要用 vkCmdBindVertexBuffers 绑它。
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    // 只有图形队列用它，EXCLUSIVE 独占即可。
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &newBuffer) != VK_SUCCESS) {
        SVK_LOGE("renderer", "create_vertex_buffer_failed operation=create_buffer");
        return false;
    }

    // 创建后问驱动：这块 buffer 需要多大、什么对齐、允许哪些内存类型。
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, newBuffer, &memReq);

    // Vulkan 不自动配内存：要显式 vkAllocateMemory 再 bind；
    // 内存类型必须同时满足 buffer 的要求（memoryTypeBits）与我们的属性要求。
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    // HOST_VISIBLE：CPU 可直接 map 写入；HOST_COHERENT：写完 GPU 立即可见、免手动 flush，
    // 每帧上传少量顶点时最省事，可以省掉 staging buffer。
    allocInfo.memoryTypeIndex = context_.findMemoryType(
        memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // 按需求尺寸和选定类型分配设备内存。
    if (vkAllocateMemory(device, &allocInfo, nullptr, &newMemory) != VK_SUCCESS) {
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
    SVK_LOGI("renderer",
             "vertex_buffer_ready frame_slot={} capacity={} bytes={} memory_type={}",
             frameSlot, capacity, bufferSize, allocInfo.memoryTypeIndex);
    return true;
}

void VulkanBackend::destroyVertexBuffers() {
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

bool VulkanBackend::uploadVertices(const ui::UiVertex* data, uint32_t count,
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

bool VulkanBackend::createCommandBuffers() {
    // 每个 in-flight 帧配一个主命令缓冲，提交逻辑更简单。
    // kMaxFramesInFlight = 2：CPU 最多领先 GPU 两帧，每帧一条缓冲互不干扰。
    commandBuffers_.resize(kMaxFramesInFlight);

    // 从命令池一次性批量分配。
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    // PRIMARY：可直接提交到队列（SECONDARY 只能被 primary 缓冲调用）。
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(context_.device(), &allocInfo,
                                 commandBuffers_.data()) != VK_SUCCESS) {
        SVK_LOGE("renderer", "allocate_command_buffers_failed");
        return false;
    }
    SVK_LOGI("renderer", "command_buffers_created count={} level=primary",
             commandBuffers_.size());
    return true;
}

bool VulkanBackend::createSyncObjects() {
    const VkDevice device = context_.device();

    // 双缓冲能让 CPU 和 GPU 有一点重叠，但不会无限堆积帧。
    // 两类同步原语：semaphore 做 GPU 内部阶段间等待（acquire → 渲染 → 呈现），
    // fence 做 CPU 等 GPU；每帧各一套，互不打架。
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);

    // semaphore 创建无需标志位，初始为未触发状态。
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    // fence 建成 SIGNALED 初始态：第一帧 renderFrame() 的 vkWaitForFences 能立即通过，
    // 否则会永久等在一个从没人 signal 过的 fence 上。
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    // 逐帧创建两个 semaphore + 一个 fence。
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semInfo, nullptr,
                              &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semInfo, nullptr,
                              &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences_[i]) !=
                VK_SUCCESS) {
            SVK_LOGE("renderer", "create_sync_objects_failed frame_slot={}", i);
            return false;
        }
    }
    SVK_LOGI("renderer", "sync_objects_created frames={} semaphores={} fences={}",
             kMaxFramesInFlight,
             imageAvailableSemaphores_.size() + renderFinishedSemaphores_.size(),
             inFlightFences_.size());
    return true;
}

bool VulkanBackend::recreateSwapchain() {
    // 先等 GPU 空闲，释放依赖 swapchain 的状态，再从头重建。
    // 重建期间不能有任何帧在飞：先 vkDeviceWaitIdle 让 GPU 完全空闲。
    SVK_LOGI("renderer", "swapchain_recreate_start");
    vkDeviceWaitIdle(context_.device());
    // 然后按 initialize 的 ③ 段原序重建；
    // 命令池、顶点缓冲、同步对象与尺寸无关，不用动。
    // 管线跟随 swapchain 格式/尺寸/采样数，同批销毁重建。
    uiPipeline_.destroy();
    swapchain_.cleanup();
    if (!swapchain_.create()) return false;
    SVK_LOGI("renderer", "swapchain_recreate_stage_done stage=swapchain");
    if (!uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                            swapchain_.msaaSamples(),
                            textureCache_.descriptorSetLayout())) return false;
    SVK_LOGI("renderer", "swapchain_recreate_stage_done stage=ui_pipeline");
    if (!swapchain_.createFramebuffers()) return false;
    SVK_LOGI("renderer", "swapchain_recreate_complete");
    return true;
}

RenderResult VulkanBackend::renderFrame(const ui::Canvas& canvas) {
    // 尺寸变化（旋转等）时先重建 swapchain 再画帧：否则本帧会按旧 swapchain 尺寸
    // 投影、画进旧尺寸的图像，呈现出去的是变形/裁剪的画面；本渲染器是按需模型，
    // 之后没有新帧覆盖，错误会一直挂到下次事件。
    //
    // 整个画帧流程包在有限次重试里：surface 刚变化时驱动状态尚未落定——
    // 重建查到的 surface 能力可能还是旧尺寸（滞后一拍），acquire/present 也会报
    // OUT_OF_DATE / SUBOPTIMAL。按需渲染没有"下一帧"来自我修正（持续渲染的引擎
    // 靠下一帧自然收敛），所以本帧内立刻重建重画，直到交换链与 surface 匹配。
    bool extentMismatchWarned = false; // 尺寸不匹配告警每次 renderFrame 只打一条，避免刷屏
    for (uint32_t attempt = 0; attempt < kMaxFrameAttempts; ++attempt) {
        if (swapchain_.takeRebuildRequest()) {
            if (!recreateSwapchain()) return RenderResult::Failed;
        }
        // 尺寸自愈：surface 变化后驱动可能仍按旧能力建链，且 acquire/present
        // 不报 OUT_OF_DATE/SUBOPTIMAL（折叠屏展开时观察到画面被拉伸），呈现前
        // 核对交换链尺寸与平台请求尺寸（视觉像素）一致，不一致重建重试。
        if (!swapchain_.matchesRequestedExtent()) {
            if (!extentMismatchWarned) {
                extentMismatchWarned = true;
                const VkExtent2D requested = swapchain_.requestedExtent();
                const VkExtent2D actual = swapchain_.extent();
                SVK_LOGW("renderer",
                         "swapchain_extent_mismatch requested={}x{} actual={}x{} "
                         "transform={} action=rebuild",
                         requested.width, requested.height, actual.width,
                         actual.height,
                         static_cast<int>(swapchain_.surfaceTransform()));
            }
            swapchain_.requestRebuild();
            continue;
        }
        // 每帧流程：等待 fence、获取图像、录制命令、提交执行、最后呈现。
        // ① 等本帧槽位的 fence：确保它上一轮的渲染已完成，
        // 这把 CPU 领先 GPU 的帧数限制在 kMaxFramesInFlight 以内，防止无限堆积。
        constexpr uint64_t waitNanos = 16000000;
        const VkResult fenceResult = vkWaitForFences(
            context_.device(), 1, &inFlightFences_[currentFrame_], VK_TRUE,
            waitNanos);
        if (fenceResult != VK_SUCCESS && fenceResult != VK_TIMEOUT) {
            SVK_LOGE("renderer", "wait_fence_failed result={}",
                     static_cast<int>(fenceResult));
            return RenderResult::Failed;
        }
        if (fenceResult == VK_TIMEOUT) return RenderResult::Retry;

        // ② acquire：向交换链申请下一张可写图像；图像就绪时 GPU 会 signal
        // imageAvailableSemaphore。
        uint32_t imageIndex = 0;
        VkResult result =
            vkAcquireNextImageKHR(context_.device(), swapchain_.handle(), waitNanos,
                                  imageAvailableSemaphores_[currentFrame_],
                                  VK_NULL_HANDLE, &imageIndex);
        if (result == VK_TIMEOUT || result == VK_NOT_READY) return RenderResult::Retry;

        // 这一帧还没来得及渲染，swapchain 就已经失效了。
        // OUT_OF_DATE 说明交换链与 surface 已不匹配（尺寸/旋转变化）：
        // 立刻重建并重试本帧（否则按需模型下画面会整帧丢失）。
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!recreateSwapchain()) return RenderResult::Failed;
            continue;
            // SUBOPTIMAL 也算拿到图像（只是与 surface 不再完全匹配），照常渲染，
            // present 后再重建。
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            SVK_LOGE("renderer", "acquire_image_failed result={}",
                     static_cast<int>(result));
            return RenderResult::Failed;
        }

        // acquire 成功后先把本帧顶点写进动态缓冲，再录命令。空 canvas 跳过上传，
        // 命令录制阶段也不会有 draw，只剩清屏帧。
        // ③ 上传顶点：赶在命令录制前把数据写进 HOST_VISIBLE 缓冲。
        if (!canvas.vertices().empty()) {
            if (!uploadVertices(canvas.vertices().data(),
                                static_cast<uint32_t>(canvas.vertices().size()),
                                currentFrame_)) {
                SVK_LOGE("renderer", "vertex_upload_failed operation=upload");
                return RenderResult::Failed;
            }
        }

        // signalSemaphore：渲染完成后 signal renderFinished，交给 present 等。
        // 声明在提交作用域外：⑥ present 的 waitSemaphores 还要引用。
        VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};

        // ④⑤ 录命令 + 提交。
        {
            // ④ reset fence 必须在确认本帧一定会提交之后做：若提前 reset 又中途
            // return，下一帧 vkWaitForFences 会永远等不到 signal（死锁）。
            vkResetFences(context_.device(), 1, &inFlightFences_[currentFrame_]);

            // 命令缓冲不能"局部修改"，每帧 reset 后整段重录。
            vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
            recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, canvas);

            // ⑤ 组装提交信息：等待条件 + 命令缓冲 + 完成信号。
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            // waitSemaphore：等 acquire 完成（图像真归我们了）才允许写它。
            VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
            // waitStages = COLOR_ATTACHMENT_OUTPUT：只在"写颜色"这个管线阶段前等待，
            // 之前的阶段（如顶点着色）可以提前跑，提高并行度。
            VkPipelineStageFlags waitStages[] = {
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = waitSemaphores;
            submitInfo.pWaitDstStageMask = waitStages;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];

            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = signalSemaphores;

            // 提交到图形队列；末尾的 fence 在这批命令全部执行完后由 GPU 置位，
            // 正是步骤①等待的那个信号。
            if (vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo,
                              inFlightFences_[currentFrame_]) != VK_SUCCESS) {
                SVK_LOGE("renderer", "queue_submit_failed");
                return RenderResult::Failed;
            }
        }

        // ⑥ present：把渲染好的图像交回交换链排队上屏。
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        // present 要等 renderFinished：渲染没完成不能显示，顺序由 semaphore 在
        // GPU 上保证。
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        VkSwapchainKHR swapchainHandle = swapchain_.handle();
        presentInfo.pSwapchains = &swapchainHandle;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);
        // present 阶段也可能提示 surface 和 swapchain 已经不匹配。
        // OUT_OF_DATE / SUBOPTIMAL 说明本帧是按不匹配的交换链画的（典型的如旋转后
        // 第一次重建拿到了旧尺寸）：置标志位并重试，下一趟循环开头重建、用同一
        // canvas 重画，用户看到的直接就是修正后的画面。
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_.requestRebuild();
            continue;
        } else if (result != VK_SUCCESS) {
            SVK_LOGE("renderer", "queue_present_failed result={}",
                     static_cast<int>(result));
            return RenderResult::Failed;
        }

        // ⑦ 轮转帧槽位 0→1→0→1：下一帧换用另一套 semaphore / fence / 命令缓冲。
        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
        return RenderResult::Rendered;
    }

    // 重试耗尽（surface 持续变化中，如快速连续旋转）：放弃本帧不算失败，
    // 调用方保留下一帧请求，保证静止 UI 也会继续收敛。
    SVK_LOGW("renderer", "frame_skipped reason=swapchain_out_of_date attempts={}",
             kMaxFrameAttempts);
    return RenderResult::Retry;
}

void VulkanBackend::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                                        const ui::Canvas& canvas) {
    const VkExtent2D extent = swapchain_.extent();
    const VkSampleCountFlagBits msaaSamples = swapchain_.msaaSamples();
    const VkSurfaceTransformFlagBitsKHR surfaceTransform =
        swapchain_.surfaceTransform();

    // 把一帧的绘制命令录进可复用的主命令缓冲。
    // begin 开始录制：缓冲进入"录制态"，之后的 vkCmd* 调用都被录进去。
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // 白纹理首帧上传：布局转换不是 render pass 内合法操作，必须在其外完成。
    textureCache_.uploadPendingTextures(cmd);

    // 布局/视觉尺寸（app 坐标空间 = 用户实际看到的方向）。
    const float bufferW = static_cast<float>(extent.width);
    const float bufferH = static_cast<float>(extent.height);
    // preTransform 为 90/270 度时，createSwapchain() 已保证 buffer 使用旋转前
    // 尺寸；这里把宽高换回用户实际看到的布局尺寸，并补偿呈现变换。
    const bool rotate90or270 =
        (surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
         surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    const bool compensate = surfaceTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    const float visualW = rotate90or270 ? bufferH : bufferW;
    const float visualH = rotate90or270 ? bufferW : bufferH;

    // 像素坐标 → NDC 的正交投影（原点在左上角，y 向下，基于视觉尺寸）。
    // 注意 glm::ortho 第三/四参数是 bottom/top：Vulkan 正 viewport 高度下
    // NDC +y 朝 framebuffer 下方，要 y 向下需传 bottom=0、top=height。
    glm::mat4 mvp = glm::ortho(0.0f, visualW, 0.0f, visualH, -1.0f, 1.0f);

    // 呈现旋转补偿：系统把 buffer 按 currentTransform 旋转上屏，这里预先把
    // NDC 反向旋转，上屏后内容回到正立方向（90/180/270 是精确换轴）。
    if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        // (x, y) -> (-y, x)
        const glm::mat4 rot(0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    } else if (compensate &&
               surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        // (x, y) -> (-x, -y)
        const glm::mat4 rot(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    } else if (compensate &&
               surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        // (x, y) -> (y, -x)
        const glm::mat4 rot(0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    }

    // 主 render pass 开始。
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = swapchain_.renderPass();
    renderPassInfo.framebuffer = swapchain_.framebuffer(imageIndex);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;
    // 清屏色：深蓝灰；MSAA 时两个附件（呈现图 + 多采样图）各一份。
    VkClearValue clearColors[2] = {
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
    };
    renderPassInfo.clearValueCount = msaaSamples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1;
    renderPassInfo.pClearValues = clearColors;
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 绑管线 + 设 viewport（NDC 到像素的映射）。
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline_.pipeline());
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // 绑定顶点缓冲到槽位 0（对应管线 vertex input 的 binding 0），偏移 0。
    VkBuffer vertexBuffers[] = {vertexBuffers_[currentFrame_]};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // mvp 整帧恒定，一次 push constant 写完。
    vkCmdPushConstants(cmd, uiPipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(mvp), &mvp);

    // 逐批次：设 scissor + 绑白纹理 descriptor + draw。
    const auto& batches = canvas.batches();
    for (const auto& batch : batches) {
        if (batch.vertexCount == 0) {
            continue;
        }

        // clip 与可视矩形求交（在视觉空间进行，与 app 布局坐标一致）。
        ui::Rect clip =
            ui::Rect::intersect(batch.clip, {0.0f, 0.0f, visualW, visualH});

        // 需要补偿时，把视觉空间的 clip 旋转到 buffer 像素空间（与 NDC 补偿同向）：
        // 90°: (x,y)->(H-y-h, x)，宽高互换；180°: 两轴各自翻转；270° 为 90° 逆。
        ui::Rect bclip;
        if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
            bclip = {visualH - clip.y - clip.h, clip.x, clip.h, clip.w};
        } else if (compensate &&
                   surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
            bclip = {visualW - clip.x - clip.w, visualH - clip.y - clip.h, clip.w,
                     clip.h};
        } else if (compensate &&
                   surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
            bclip = {clip.y, visualW - clip.x - clip.w, clip.h, clip.w};
        } else {
            bclip = clip;
        }

        // 转 int32 时 clamp：offset 不小于 0，且 offset+extent 不超出渲染目标。
        int32_t ox = std::max(0, static_cast<int32_t>(bclip.x));
        int32_t oy = std::max(0, static_cast<int32_t>(bclip.y));
        int32_t ex =
            std::min(static_cast<int32_t>(bclip.x + bclip.w),
                     static_cast<int32_t>(extent.width)) - ox;
        int32_t ey =
            std::min(static_cast<int32_t>(bclip.y + bclip.h),
                     static_cast<int32_t>(extent.height)) - oy;
        if (ex <= 0 || ey <= 0) {
            continue;
        }

        VkRect2D scissor{};
        scissor.offset = {ox, oy};
        scissor.extent = {static_cast<uint32_t>(ex), static_cast<uint32_t>(ey)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // 绑本批纹理：最小形态恒为白纹理（纯色/渐变）。
        VkDescriptorSet set = textureCache_.whiteDescriptorSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                uiPipeline_.layout(), 0, 1, &set, 0, nullptr);

        // 画这一批：firstVertex 定位到该批在总顶点数组里的起始偏移。
        vkCmdDraw(cmd, batch.vertexCount, 1, batch.firstVertex, 0);
    }

    // 结束 render pass：同时触发附件布局转换到 PRESENT_SRC_KHR。
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace svk::gpu
