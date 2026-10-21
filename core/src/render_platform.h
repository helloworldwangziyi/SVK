#pragma once

/**
 * @file render_platform.h
 * @brief 平台抽象层：渲染后端经它获取原生绘制窗口句柄与绘制区尺寸。
 *
 * 接口刻意不暴露任何图形 API 类型：原生窗口以不透明 void* 传递，
 * 由当前编译的后端（Vulkan / Metal）自行解释：
 *   - Android：ANativeWindow*
 *   - iOS：    CAMetalLayer*
 *   - 鸿蒙：   OHNativeWindow*
 *
 * 多后端拆分后，本接口不再承担 Vulkan surface 创建职责；
 * 日志不经过本接口，统一走 svk/log.h（SVK_LOG* 宏）。
 */
#include <cstdint>

namespace svk {

/**
 * @brief 平台抽象接口。渲染后端经它拿原生窗口、查绘制区尺寸。
 */
class IPlatform {
public:
    virtual ~IPlatform() = default;

    /**
     * @brief 返回平台原生绘制窗口句柄（后端按编译目标解释）：
     * Android = ANativeWindow*，iOS = CAMetalLayer*，鸿蒙 = OHNativeWindow*。
     * @return 原生窗口指针；无效时为 nullptr
     */
    virtual void* nativeWindowHandle() const = 0;

    /**
     * @brief 返回 surface 当前的像素尺寸。
     * @param width 输出：surface 宽度（像素）
     * @param height 输出：surface 高度（像素）
     */
    virtual void getSurfaceSize(uint32_t* width, uint32_t* height) = 0;
};

} // namespace svk
