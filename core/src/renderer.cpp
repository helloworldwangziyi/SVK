/**
 * @file renderer.cpp
 * @brief 渲染编排层实现：生命周期编排、帧重试语义与日志；帧执行委托 GPU 后端。
 *
 * 本类只做策略层工作：按装配点创建后端（gpu::createBackend）、按序驱动
 * 设备级 / surface 级初始化、挂起/恢复编排与计时日志；一帧的完整执行
 * （获取 → 上传 → 编码 → 提交/呈现）全部下沉到 gpu::IGpuBackend。
 */
#include "renderer.h"
// SVK_LOGI/W/E 日志宏：spdlog 封装，全局统一的日志入口。
#include "log/log.h"
// SVK_TRACE_SCOPE：帧耗时追踪（Android ATrace，其余平台暂空实现）。
#include "log/trace.h"

// std::chrono::steady_clock：initialize 计时（duration_ms 日志）。
#include <chrono>

namespace svk {

std::unique_ptr<IRasterSurface> createRasterSurface(IPlatform* platform) {
    return std::make_unique<Renderer>(platform);
}

Renderer::Renderer(IPlatform* platform) : platform_(platform) {}

Renderer::~Renderer() {
    // 析构兜底释放后端；backend 析构内部对空句柄有判断，重复清理也安全。
    backend_.reset();
}

bool Renderer::initialize() {
    // 按装配点创建后端 → 设备级资源 → surface 级资源；任何一步失败都中止启动。
    const auto startedAt = std::chrono::steady_clock::now();
    SVK_LOGI("renderer", "initialize_start");
    backend_ = gpu::createBackend(platform_);
    SVK_LOGI("renderer", "initialize_stage_start stage=backend backend={}",
             gpu::toString(backend_->backendType()));
    if (!backend_->initializeDevice()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=backend");
    SVK_LOGI("renderer", "initialize_stage_start stage=surface");
    if (!backend_->createSurface()) return false;
    SVK_LOGI("renderer", "initialize_stage_done stage=surface");
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - startedAt)
                                .count();
    SVK_LOGI("renderer", "initialize_complete duration_ms={}", durationMs);
    return true;
}

RenderResult Renderer::render(const ui::Canvas& canvas) {
    // 未初始化（backend 缺失）视为失败；正常路径直接委托后端执行一帧。
    if (!backend_) return RenderResult::Failed;
    // 帧号进 thread_local，本帧内引擎与业务的日志自动带 frame= 字段。
    svk::log::setFrameSeq(++frameSeq_);
    SVK_TRACE_SCOPE("svk.renderFrame");
    return backend_->renderFrame(canvas);
}

void Renderer::setSize(uint32_t width, uint32_t height) {
    if (!backend_) return;
    backend_->setTargetSize(width, height);
}

bool Renderer::suspendSurface() {
    // 挂起幂等：已挂起时后端的销毁调用对空句柄安全（backend 内判断），
    // waitIdle 对已空闲的设备是即刻返回，重复挂起恒成功。
    if (!backend_) return true;
    SVK_LOGI("renderer", "surface_suspend_start");
    backend_->waitIdle();
    backend_->destroySurface();
    svk::log::clearFrameSeq();  // 挂起期间日志不再携带过期帧号
    SVK_LOGI("renderer", "surface_suspended");
    return true;
}

bool Renderer::resumeSurface(IPlatform* platform) {
    // 恢复路径：换平台指针 → 后端按新原生窗口重建 surface 级资源。
    if (!backend_) return false;
    SVK_LOGI("renderer", "surface_resume_start");
    platform_ = platform;
    backend_->setPlatform(platform);
    SVK_LOGI("renderer", "surface_resume_stage_done stage=platform");
    if (!backend_->createSurface()) return false;
    SVK_LOGI("renderer", "surface_resumed");
    return true;
}

} // namespace svk
