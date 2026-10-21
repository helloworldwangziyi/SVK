#pragma once

/**
 * @file renderer.h
 * @brief 渲染编排层：平台壳只依赖本接口，不感知具体 GPU 后端。
 *
 * 本类是多后端架构的策略层（对照 Impeller 的 frame 编排 + Context 装配）：
 * 只负责生命周期编排（初始化/挂起/恢复/尺寸）、帧重试语义与日志；
 * 帧执行原语全部委托给 gpu::IGpuBackend（Vulkan 或 Metal）。
 */
#include "gpu/rhi.h"
#include "render_platform.h"
#include "raster_surface.h"
#include <memory>

namespace svk {

/**
 * @brief 渲染编排器：持有并按装配点选出的 GPU 后端驱动一帧渲染。
 *
 * 线程契约与旧渲染器一致：全部方法仅在同一个线程顺序调用，无内部锁。
 */
class Renderer : public IRasterSurface {
public:
    /**
     * @param platform 平台抽象层（原生窗口句柄与尺寸查询）
     */
    explicit Renderer(IPlatform* platform);
    ~Renderer() override;

    /**
     * @brief 初始化：按装配点创建后端 → 设备级资源 → surface 级资源。
     * @return true 表示成功；任一步失败即中止启动
     */
    bool initialize() override;

    /**
     * @brief 把 Canvas 收集的本帧几何绘制成一帧（委托后端执行）。
     * @param canvas 本帧收集的 UI 几何
     * @return Rendered 提交已接受；Retry 等待超时/尺寸尚未稳定，需下一帧重试。
     */
    RenderResult render(const ui::Canvas& canvas) override;

    /**
     * @brief 通知渲染器 surface 尺寸已变化（只记状态，渲染时消费）。
     */
    void setSize(uint32_t width, uint32_t height) override;

    /**
     * @brief 保活挂起：等待 GPU 空闲后释放 surface 级资源，保留设备级资源。
     * @return true 表示已挂起（surface 本为空也视为成功，幂等）
     */
    bool suspendSurface() override;

    /**
     * @brief 保活恢复：按新 platform 重建 surface 级资源。
     * @param platform 新的平台抽象层指针（不持有所有权）
     * @return true 表示恢复成功；false 由调用方决定是否冷启动兜底
     */
    bool resumeSurface(IPlatform* platform) override;

private:
    IPlatform* platform_ = nullptr;            ///< 平台抽象层（不持有所有权）
    std::unique_ptr<gpu::IGpuBackend> backend_; ///< 装配点选出的 GPU 后端
    uint64_t frameSeq_ = 0;                    ///< 帧号：经 svk::log 附加到日志
};

} // namespace svk
