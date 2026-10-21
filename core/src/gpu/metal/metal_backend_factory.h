#pragma once

/**
 * @file metal_backend_factory.h
 * @brief Metal 后端装配契约头（纯 C++）：backend_factory.cpp 按编译目标引用。
 *
 * 仅 iOS 工程定义 SVK_METAL_BACKEND 时编入；实现位于 metal_backend.mm。
 */
#include <memory>

namespace svk {
class IPlatform;
namespace gpu {
class IGpuBackend;
namespace metal {

/**
 * @brief 创建 iOS 原生 Metal 绘制后端。
 * @param platform 平台抽象层（原生窗口句柄与尺寸查询，不持有所有权）
 */
std::unique_ptr<IGpuBackend> createMetalBackend(IPlatform* platform);

} // namespace metal
} // namespace gpu
} // namespace svk
