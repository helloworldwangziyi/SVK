/**
 * @file ui_pipeline.cpp
 * @brief UI 图形管线实现：pipeline layout + graphics pipeline 的创建。
 */
#include "gpu/vulkan/ui_pipeline.h"

// VulkanContext：设备句柄与 shader module 创建。
#include "gpu/vulkan/vulkan_context.h"
// 预编译 SPIR-V 字节码（编译期内嵌进二进制，免运行时读文件）。
#include "assets/ui_shaders.h"
// SVK_LOGE 日志宏：spdlog 封装，全局统一的日志入口。
#include "log/log.h"
// ui::UiVertex：顶点输入布局的 stride 来源。
#include "ui/canvas.h"

namespace svk::gpu {

UiPipeline::UiPipeline(VulkanContext& context) : context_(context) {}

bool UiPipeline::create(VkRenderPass renderPass, VkExtent2D extent,
                        VkSampleCountFlagBits msaaSamples,
                        VkDescriptorSetLayout descriptorSetLayout) {
    const VkDevice device = context_.device();
    SVK_LOGI("pipeline", "create_start width={} height={} msaa={}", extent.width,
             extent.height, static_cast<int>(msaaSamples));

    // viewport 和 scissor 保持动态，所以缩放时只需重录命令缓冲。
    // 从内嵌字节码建 shader module；管线建完即可销毁（见函数尾）。
    VkShaderModule vert = context_.createShaderModule(
        assets::ui_vert_spv, sizeof(assets::ui_vert_spv));
    VkShaderModule frag = context_.createShaderModule(
        assets::ui_frag_spv, sizeof(assets::ui_frag_spv));
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return false;
    }
    SVK_LOGI("pipeline", "shader_modules_created vertex_bytes={} fragment_bytes={}",
             sizeof(assets::ui_vert_spv), sizeof(assets::ui_frag_spv));

    // ---- 固定状态 ----

    // 顶点数据是 UiVertex 交错布局：先像素坐标(vec2)，再 RGBA 颜色(vec4)，
    // 最后纹理坐标(vec2)。binding 0 对应 vkCmdBindVertexBuffers 的槽位 0。
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    // stride = 32 字节：UiVertex = vec2 位置 + vec4 颜色 + vec2 纹理坐标。
    bindingDesc.stride = sizeof(ui::UiVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // attribute 描述 shader 里每个输入变量在顶点数据里的位置与格式。
    VkVertexInputAttributeDescription attrs[3];
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = sizeof(float) * 2;
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = sizeof(float) * 6;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    // 输入装配：TRIANGLE_LIST，每 3 个顶点一个独立三角形。
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // viewport/scissor 占位：真正生效的是录制命令时动态设置的值。
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    // 光栅化器：实心填充、不剔除（UI 三角形顶点顺序不统一）。
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // 多重采样状态：rasterizationSamples 必须与 renderPass 附件声明的一致。
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = msaaSamples;

    // 颜色混合：标准 SRC_ALPHA srcOver 合成，UI 半透明需要。
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // 动态状态：viewport 和 scissor 留到录制命令时再设。
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // push constant：mat4 mvp（64B），像素坐标 → NDC 投影，顶点阶段可见。
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = 64;

    // 管线布局 = shader 的"接口签名"：set 0 组合图像采样 + 64B push constant。
    VkDescriptorSetLayout setLayouts[] = {descriptorSetLayout};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_) !=
        VK_SUCCESS) {
        SVK_LOGE("pipeline", "create_layout_failed");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }
    SVK_LOGI("pipeline", "layout_created descriptor_sets={} push_constant_bytes={}",
             layoutInfo.setLayoutCount, pushConstant.size);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    // 管线与 render pass 的兼容性在创建期就会被校验。
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    const bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                              &pipelineInfo, nullptr,
                                              &pipeline_) == VK_SUCCESS;
    if (!ok) {
        SVK_LOGE("pipeline", "create_graphics_pipeline_failed");
    } else {
        SVK_LOGI("pipeline",
                 "created vertex_stride={} attributes={} dynamic_states={} blend=src_alpha",
                 bindingDesc.stride, vertexInput.vertexAttributeDescriptionCount,
                 dynamicState.dynamicStateCount);
    }

    // 管线建完后 SPIR-V 已被"消化"，shader module 可即刻销毁。
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    return ok;
}

void UiPipeline::destroy() {
    const VkDevice device = context_.device();
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
}

} // namespace svk::gpu
