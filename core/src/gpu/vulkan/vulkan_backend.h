#pragma once

/**
 * @file vulkan_backend.h
 * @brief Vulkan 渲染后端：IGpuBackend 的 Vulkan 实现，承接一帧的完整执行。
 *
 * GPU 设施拆给同目录子模块：instance/设备归 VulkanContext，swapchain/MSAA/
 * render pass 归 Swapchain，管线归 UiPipeline，纹理归 TextureCache；本类只做
 * 帧执行（acquire → 录制 → 提交 → present）并持有命令池、动态顶点缓冲与同步
 * 原语。surface 级资源（surface/swapchain/管线/帧缓冲）随 createSurface()/
 * destroySurface() 建销，设备级资源随 initializeDevice()/shutdownDevice() 建销。
 * 全部方法仅在同一个线程顺序调用（平台壳为 UI 线程；上层引入 Raster 线程后
 * 改为 Raster 线程）。
 */
// glm 深度映射约定（Vulkan NDC z∈[0,1]）必须在任何 glm 头之前定义；
// 本头经成员函数签名把 glm 暴露给包含者，宏因此收敛在此处。
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "../rhi.h"
#include "render_platform.h"
#include "vulkan_context.h"
#include "swapchain.h"
#include "ui_pipeline.h"
#include "texture_cache.h"
#include "ui/canvas.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace svk::gpu {

/**
 * @brief Vulkan 绘制后端：实现 gpu::IGpuBackend（Android / 鸿蒙 / host）。
 *
 * 帧执行语义与重试策略沿用旧 Renderer::render：有限次重建重试、
 * fence/acquire 16ms 超时返回 Retry，OUT_OF_DATE/SUBOPTIMAL 本帧内重建重画。
 */
class VulkanBackend : public IGpuBackend {
public:
    /**
     * @param platform 平台抽象层（原生窗口句柄与尺寸查询）
     */
    explicit VulkanBackend(IPlatform* platform);
    ~VulkanBackend() override;

    BackendType backendType() const override { return BackendType::Vulkan; }

    /**
     * @brief 创建设备级资源：instance/物理与逻辑设备（含 surface，因设备选取
     *        依赖 surface 的 present 支持查询）、纹理设施、命令池/顶点缓冲/
     *        命令缓冲/同步原语。与 surface 视觉尺寸无关。
     * @return true 表示成功；任一步失败即中止
     */
    bool initializeDevice() override;

    /**
     * @brief 释放全部设备级与 surface 级资源（waitIdle 后按相反顺序销毁）。
     */
    void shutdownDevice() override;

    /**
     * @brief 更换平台抽象层指针（保活恢复时指向新原生窗口；不持有所有权）。
     *        仅 surface 销毁期间调用，之后须紧跟 createSurface()。
     */
    void setPlatform(IPlatform* platform) override;

    /**
     * @brief 创建 surface 级资源（冷启动与热恢复共用同一路径）：
     *        surface → swapchain → 管线 → 帧缓冲。
     * @return true 表示成功；失败返回 false，由调用方决定是否冷启动兜底
     */
    bool createSurface() override;

    /**
     * @brief 销毁 surface 级资源（保活挂起），保留设备级资源；幂等。
     */
    void destroySurface() override;

    /**
     * @brief 记录 surface 新视觉尺寸（像素）。只记状态不重建，
     *        真正的资源调整延迟到 renderFrame() 的安全点。
     */
    void setTargetSize(uint32_t width, uint32_t height) override;

    /**
     * @brief 绘制一帧：获取呈现目标 → 上传顶点 → 编码 → 提交/呈现，
     *        内部消化交换资源重建重试（kMaxFrameAttempts 次）。
     * @param canvas 本帧收集的 UI 几何
     */
    RenderResult renderFrame(const ui::Canvas& canvas) override;

    /**
     * @brief 等待 GPU 在飞工作全部完成（挂起/重建/销毁前的安全点）。
     */
    void waitIdle() override;

private:
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createVertexBuffers();
    bool createVertexBuffer(uint32_t frameSlot, uint32_t capacity);
    void destroyVertexBuffers();

    /**
     * @brief swapchain 重建编排：管线与 swapchain 资源同批销毁后按原序重建。
     */
    bool recreateSwapchain();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                             const ui::Canvas& canvas);

    /**
     * @brief 把 UI 顶点上传到当前帧独立的动态顶点缓冲。
     * @param data 顶点数组首地址
     * @param count 顶点个数；缓冲不足时按需扩容
     * @param frameSlot 当前 in-flight 帧槽
     */
    bool uploadVertices(const ui::UiVertex* data, uint32_t count, uint32_t frameSlot);

    // ---- GPU 子模块（声明顺序即构造顺序：context 最先，其余引用它） ----
    VulkanContext context_;
    TextureCache textureCache_;
    Swapchain swapchain_;
    UiPipeline uiPipeline_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;

    uint32_t currentFrame_ = 0;

    std::vector<VkBuffer> vertexBuffers_; ///< 每个 in-flight 帧独占，避免 CPU 覆盖在途顶点
    std::vector<VkDeviceMemory> vertexBufferMemorys_;
    std::vector<uint32_t> vertexBufferCapacities_;
    static constexpr uint32_t kInitialVertexCapacity = 8192;
};

} // namespace svk::gpu
