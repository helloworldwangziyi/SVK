#pragma once

/**
 * @file texture_cache.h
 * @brief 纹理 GPU 设施：共享采样器、descriptor 与 1x1 白纹理。
 */
#include <vulkan/vulkan.h>

namespace svk::gpu {

class VulkanContext;

/**
 * @brief 纯色批次走的 1x1 白纹理与采样设施。
 *
 * 白纹理让着色公式统一为"顶点色 × 纹理 r 通道"：采样到 1 即直出顶点色，
 * 渐变靠顶点插值。本模块是 TextureCache 的最小形态——只建白纹理，
 * 不含数据源纹理与脏区上传（原工程的完整版见 texture_source.h 路线）。
 */
class TextureCache {
public:
    explicit TextureCache(VulkanContext& context);

    /**
     * @brief 创建采样器、descriptor 布局与池、1x1 白纹理。
     * @return true 表示成功
     */
    bool initialize();

    /**
     * @brief 释放全部 Vulkan 对象；幂等。
     */
    void shutdown();

    /**
     * @brief 把待上传的白纹理写进命令缓冲（首帧一次，4 字节）。
     * 必须在 vkCmdBeginRenderPass 之前调用（布局转换不是 render pass 内合法操作）。
     * @param cmd 本帧命令缓冲
     */
    void uploadPendingTextures(VkCommandBuffer cmd);

    /// 白纹理的 descriptor set（纯色批次绑它）。
    VkDescriptorSet whiteDescriptorSet() const { return descriptorSet_; }

    /// set 0 的纹理布局（管线布局的输入，见 UiPipeline::create）。
    VkDescriptorSetLayout descriptorSetLayout() const { return descriptorSetLayout_; }

private:
    bool createWhiteTexture();

    VulkanContext& context_;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;

    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    bool uploaded_ = false; ///< true = 已转为 SHADER_READ_ONLY_OPTIMAL 布局
};

} // namespace svk::gpu
