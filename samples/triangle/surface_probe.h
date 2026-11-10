#pragma once

#include "ui/canvas.h"

#include <cstdint>

namespace svk::demo {

/** 多 Surface 探针的纯色占位：默认红色，底部原生内容显式传绿色，不依赖文字或业务数据。 */
inline void buildSurfaceProbeFrame(
    ui::Canvas& canvas, uint32_t width, uint32_t height,
    ui::Color color = ui::Color{1.0f, 0.0f, 0.0f, 1.0f}) {
    canvas.clear();
    if (width == 0 || height == 0) {
        return;
    }
    const ui::Rect bounds{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    canvas.drawRect(bounds, bounds, color);
}

} // namespace svk::demo
