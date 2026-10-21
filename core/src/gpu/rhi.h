#pragma once

/**
 * @file rhi.h
 * @brief 渲染硬件抽象层：后端无关的绘制后端接口与工厂。
 *
 * 分层对照 Impeller 的 renderer/context.h + backend/：
 *   Renderer（core/src/renderer.*）只做帧编排策略（生命周期、重试语义、日志），
 *   帧执行原语全部下沉到 gpu::IGpuBackend 的实现：
 *     - gpu/vulkan/vulkan_backend.*   Android / 鸿蒙 / host
 *     - gpu/metal/metal_backend.mm    iOS（仅定义 SVK_METAL_BACKEND 时编入）
 * 后端选择发生在装配点 createBackend()：iOS 工程定义 SVK_METAL_BACKEND，
 * 其余目标（Android / 鸿蒙 / host）默认走 Vulkan。
 */
#include <cstdint>
#include <memory>

#include "raster_surface.h" // RenderResult

namespace svk {
class IPlatform;
namespace ui { class Canvas; }

namespace gpu {

/// 渲染后端类型，用于日志与调试识别。
enum class BackendType { Vulkan, Metal };

/// BackendType 的日志字符串（"vulkan" / "metal"）。
const char* toString(BackendType type);

/**
 * @brief 绘制后端接口：一帧的完整执行（获取 → 上传 → 编码 → 提交/呈现）。
 *
 * 线程/生命周期契约与 IRasterSurface 相同：全部方法在同一个线程顺序调用。
 * 实现负责内部的交换资源重建重试；返回 RenderResult 的语义与
 * IRasterSurface::render 一致。
 */
class IGpuBackend {
public:
    virtual ~IGpuBackend() = default;

    virtual BackendType backendType() const = 0;

    /**
     * @brief 初始化设备级资源（instance/device 或 MTLDevice/命令队列等）。
     *        与 surface 无关，挂起/恢复期间保留。
     */
    virtual bool initializeDevice() = 0;

    /**
     * @brief 释放全部设备级与 surface 级资源（幂等）。
     */
    virtual void shutdownDevice() = 0;

    /**
     * @brief 更换平台抽象层指针（保活恢复时指向新原生窗口；不持有所有权）。
     *        仅 surface 销毁期间调用，之后须紧跟 createSurface()。
     */
    virtual void setPlatform(IPlatform* platform) = 0;

    /**
     * @brief 创建 surface 级资源（冷启动与热恢复共用同一路径）。
     * @return true 表示成功；失败返回 false，由调用方决定是否冷启动兜底
     */
    virtual bool createSurface() = 0;

    /**
     * @brief 销毁 surface 级资源（保活挂起），保留设备级资源；幂等。
     */
    virtual void destroySurface() = 0;

    /**
     * @brief 记录 surface 新视觉尺寸（像素）。只记状态不重建，
     *        真正的资源调整延迟到 renderFrame() 的安全点。
     */
    virtual void setTargetSize(uint32_t width, uint32_t height) = 0;

    /**
     * @brief 绘制一帧：获取呈现目标 → 上传顶点 → 编码 → 提交/呈现，
     *        内部消化交换资源重建重试（最多有限次，语义同旧 Renderer::render）。
     * @param canvas 本帧收集的 UI 几何
     */
    virtual RenderResult renderFrame(const ui::Canvas& canvas) = 0;

    /**
     * @brief 等待 GPU 在飞工作全部完成（挂起/重建/销毁前的安全点）。
     */
    virtual void waitIdle() = 0;
};

/**
 * @brief 后端装配点：按编译目标选择 IGpuBackend 实现。
 *        iOS（SVK_METAL_BACKEND）→ Metal；其余 → Vulkan。
 */
std::unique_ptr<IGpuBackend> createBackend(IPlatform* platform);

} // namespace gpu
} // namespace svk
