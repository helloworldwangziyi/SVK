#pragma once

/**
 * @file metal_shaders.h
 * @brief iOS Metal 后端内嵌 MSL 着色器源码：运行时 newLibraryWithSource 编译，
 *        构建期无 metallib 步骤（iOS 不再走 SPIR-V/glslc 管线）。
 *
 * 与 shaders/ui_vertex.vert + ui_fragment.frag（Vulkan 版 GLSL）逐语义对应：
 *   - 顶点：像素坐标 × mvp（左上角原点、y 向下）→ NDC，透传顶点色与 uv；
 *   - 片元：纹理 × 顶点色（RGBA 逐通道相乘），白纹理下直出顶点色。
 */
namespace svk::gpu::metal::shaders {

/// UI 顶点 + 片元着色器（单一翻译单元，入口 ui_vertex / ui_fragment）。
inline constexpr const char kUiShadersMsl[] = R"msl(
#include <metal_stdlib>
using namespace metal;

// 顶点输入：与 svk::ui::UiVertex 交错布局一一对应（stride 32 字节）。
struct UiVertexIn {
    float2 position [[attribute(0)]]; // 屏幕像素坐标
    float4 color [[attribute(1)]];    // 顶点色（RGB + A）
    float2 uv [[attribute(2)]];       // 纹理坐标（纯色几何恒为 0）
};

struct UiVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
};

// 顶点着色器：mvp 由 CPU 侧 setVertexBytes 以 64B 传入（buffer(1)，
// 列主序 float4x4，与 glm::mat4 内存布局一致）。
vertex UiVertexOut ui_vertex(UiVertexIn in [[stage_in]],
                             constant float4x4& mvp [[buffer(1)]]) {
    UiVertexOut out;
    out.position = mvp * float4(in.position, 0.0, 1.0);
    out.color = in.color;
    out.uv = in.uv;
    return out;
}

// 片元着色器：texture(0) + sampler(0)（恒为 1x1 白纹理 + linear/clamp），
// 输出 = 顶点色 × 纹理采样，对齐 Vulkan 版 GLSL 的着色公式。
fragment float4 ui_fragment(UiVertexOut in [[stage_in]],
                            texture2d<float> uTexture [[texture(0)]],
                            sampler uSampler [[sampler(0)]]) {
    const float4 sampled = uTexture.sample(uSampler, in.uv);
    return float4(in.color.rgb * sampled.rgb, in.color.a * sampled.a);
}
)msl";

} // namespace svk::gpu::metal::shaders
