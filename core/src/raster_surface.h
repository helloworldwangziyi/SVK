#pragma once

/**
 * @file raster_surface.h
 * @brief 渲染接口抽象：平台壳只依赖本接口，不感知 Vulkan。
 */
#include <cstdint>
#include <memory>

namespace svp {
class IPlatform;
namespace ui { class Canvas; }

// Rendered means the submission was accepted, not physical presentation.
enum class RenderResult { Rendered, Retry, Failed };

// Raster-only surface backend. All backends obey the same thread/lifetime
// contract: initialize/render/setSize/suspend/resume 均在同一个线程调用
// （Android/iOS 壳为 UI 线程直调；上层引入 Raster 线程后改为 Raster 线程）。
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
} // namespace svp
