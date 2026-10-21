/**
 * @file texture_cache.cpp
 * @brief 纹理 GPU 设施实现：采样器 + descriptor + 1x1 白纹理。
 */
#include "gpu/texture_cache.h"

// VulkanContext：设备句柄与显存类型查找。
#include "gpu/vulkan_context.h"
// SVK_LOGE 日志宏：spdlog 封装，全局统一的日志入口。
#include "log.h"

// std::memset：白纹理像素写入。
#include <cstring>

namespace svk::gpu {

TextureCache::TextureCache(VulkanContext& context) : context_(context) {}

bool TextureCache::initialize() {
    const VkDevice device = context_.device();
    SVK_LOGI("texture", "initialize_start");

    // 采样器：线性过滤 + 边缘钳制。白纹理只有一个纹素，参数实际上无所谓，
    // 保留完整配置以便后续接入真实纹理时无需改动。
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        SVK_LOGE("texture", "create_sampler_failed");
        return false;
    }
    SVK_LOGI("texture", "sampler_created filter=linear address=clamp_to_edge");

    // descriptor 布局：binding 0 = 组合图像采样器，片元阶段可读。
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                    &descriptorSetLayout_) != VK_SUCCESS) {
        SVK_LOGE("texture", "create_descriptor_set_layout_failed");
        return false;
    }
    SVK_LOGI("texture", "descriptor_layout_created bindings={}", layoutInfo.bindingCount);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) !=
        VK_SUCCESS) {
        SVK_LOGE("texture", "create_descriptor_pool_failed");
        return false;
    }
    SVK_LOGI("texture", "descriptor_pool_created max_sets={}", poolInfo.maxSets);

    if (!createWhiteTexture()) {
        return false;
    }
    SVK_LOGI("texture", "initialize_complete");
    return true;
}

bool TextureCache::createWhiteTexture() {
    const VkDevice device = context_.device();
    SVK_LOGI("texture", "white_texture_create_start size=1x1 format=rgba8");

    // 白纹理：RGBA8 1x1，四个通道都是 255（采样 = vec4(1)）。
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {1, 1, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
        SVK_LOGE("texture", "create_white_image_failed");
        return false;
    }
    SVK_LOGI("texture", "white_image_created");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, image_, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = context_.findMemoryType(
        memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory_) != VK_SUCCESS) {
        SVK_LOGE("texture", "allocate_white_image_memory_failed");
        return false;
    }
    if (vkBindImageMemory(device, image_, imageMemory_, 0) != VK_SUCCESS) {
        SVK_LOGE("texture", "bind_white_image_memory_failed");
        return false;
    }
    SVK_LOGI("texture", "white_image_memory_bound bytes={} memory_type={}",
             memReq.size, allocInfo.memoryTypeIndex);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView_) != VK_SUCCESS) {
        SVK_LOGE("texture", "create_white_image_view_failed");
        return false;
    }
    SVK_LOGI("texture", "white_image_view_created");

    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(device, &setInfo, &descriptorSet_) != VK_SUCCESS) {
        SVK_LOGE("texture", "allocate_white_descriptor_set_failed");
        return false;
    }
    VkDescriptorImageInfo descImage{};
    descImage.sampler = sampler_;
    descImage.imageView = imageView_;
    descImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &descImage;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    SVK_LOGI("texture", "white_texture_ready binding=0");
    return true;
}

void TextureCache::uploadPendingTextures(VkCommandBuffer cmd) {
    if (uploaded_) {
        return;
    }
    const VkDevice device = context_.device();

    // 4 字节像素走命令缓冲内联上传：UNDEFINED → TRANSFER_DST → 拷入 → SHADER_READ。
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image_;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toDst);

    // 像素内容在 CPU 侧先写好（255,255,255,255），经 vkCmdUpdateBuffer 式的
    // 内联拷贝进图像：vkCmdCopyBufferToImage 需要源缓冲，这里只有 4 字节，
    // 直接用 staging 式的小缓冲不划算——改为 TRANSFER_DST 布局后
    // 用 vkCmdClearColorImage 清成不透明白，效果等价（RGBA 全 1）。
    VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white,
                         1, &range);

    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = image_;
    toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRead.subresourceRange.levelCount = 1;
    toRead.subresourceRange.layerCount = 1;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toRead);

    uploaded_ = true;
    SVK_LOGI("texture", "white_texture_uploaded layout=shader_read_only");
    (void)device;
}

void TextureCache::shutdown() {
    const VkDevice device = context_.device();
    if (imageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, imageView_, nullptr);
        imageView_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (imageMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, imageMemory_, nullptr);
        imageMemory_ = VK_NULL_HANDLE;
    }
    // descriptor set 归池所有，销毁池即释放，无需单独删。
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    uploaded_ = false;
}

} // namespace svk::gpu
