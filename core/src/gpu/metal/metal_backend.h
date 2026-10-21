#pragma once

/**
 * @file metal_backend.h
 * @brief iOS 原生 Metal 绘制后端：gpu::IGpuBackend 的 Metal 实现。
 *
 * 渲染语义与 gpu/vulkan/ 版完全对齐：清屏色 (0.06,0.06,0.09,1.0)、
 * 左上角原点 Y 向下像素坐标、顶点色 × 纹理着色、线性 + clamp 采样、
 * 双帧在飞、16ms 帧等待超时返回 Retry（Metal 无 OUT_OF_DATE 概念，
 * 尺寸自愈在 renderFrame 内完成，不重试不重建造帧循环）。
 *
 * ObjC 对象（MTLDevice/纹理/信号量等）全部藏在 .mm 的 Impl 里，
 * 本头文件保持纯 C++，不暴露任何 ObjC 类型。
 */
#include "gpu/rhi.h"

#include <memory>

namespace svk {
class IPlatform;

namespace gpu::metal {

/**
 * @brief Metal 绘制后端。一帧流程：nextDrawable → 上传顶点到当前帧槽 →
 * 编码（清屏 + 逐批次 scissor/draw）→ present + 完成信号量。
 * 线程/生命周期契约与 IGpuBackend 一致：全部方法同线程顺序调用。
 */
class MetalBackend : public IGpuBackend {
public:
    /**
     * @param platform 平台抽象层（不持有所有权）
     */
    explicit MetalBackend(IPlatform* platform);
    ~MetalBackend() override;

    BackendType backendType() const override { return BackendType::Metal; }

    bool initializeDevice() override;
    void shutdownDevice() override;
    void setPlatform(IPlatform* platform) override;
    bool createSurface() override;
    void destroySurface() override;
    void setTargetSize(uint32_t width, uint32_t height) override;
    RenderResult renderFrame(const ui::Canvas& canvas) override;
    void waitIdle() override;

private:
    struct Impl;                 ///< ObjC 资源句柄（.mm 内定义，ARC strong 管理）
    std::unique_ptr<Impl> impl_;
};

} // namespace gpu::metal
} // namespace svk
