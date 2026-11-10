#pragma once

/**
 * @file raster_surface.h
 * @brief 渲染接口抽象：平台壳只依赖本接口，不感知 Vulkan。
 */
#include <cstdint>
#include <memory>

namespace svk {
class IPlatform;
namespace ui { class Canvas; }

// Rendered means the submission was accepted, not physical presentation.
enum class RenderResult { Rendered, Retry, Failed };

// 唯一输出是平台 Surface。所有方法均在同一个线程顺序调用
// （Android/鸿蒙壳为 UI 线程直调；上层引入 Raster 线程后改为 Raster 线程）。
class IRasterSurface {
public:
    virtual ~IRasterSurface() = default;
    virtual bool initialize() = 0;
    virtual void setSize(uint32_t width, uint32_t height) = 0;
    virtual RenderResult render(const ui::Canvas& canvas) = 0;
    virtual bool suspendSurface() = 0;
    virtual bool resumeSurface(IPlatform* platform) = 0;
};

std::unique_ptr<IRasterSurface> createRasterSurface(IPlatform* platform);
} // namespace svk
