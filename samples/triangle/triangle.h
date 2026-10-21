#pragma once

/**
 * @file triangle.h
 * @brief Triangle App 业务：每帧往 ui::Canvas 里画一个三色渐变三角形。
 *
 * 由 samples/triangle/app.cpp 装配到 svk::IApp，平台层不包含本头。
 * 三角形绕屏幕中心缓慢旋转、顶点色随时间循环，用来验证三端 VSync 帧通道。
 */
#include "ui/canvas.h"

#include <algorithm>
#include <cmath>

namespace svk::demo {

/// 每帧构建三角形演示帧。timeSeconds 为单调时钟秒数（各端自行提供）。
inline void buildTriangleFrame(ui::Canvas& canvas, uint32_t width, uint32_t height,
                               double timeSeconds) {
    canvas.clear();
    if (width == 0 || height == 0) {
        return;
    }
    const ui::Rect clip{0.0f, 0.0f, static_cast<float>(width),
                        static_cast<float>(height)};
    const float cx = static_cast<float>(width) * 0.5f;
    const float cy = static_cast<float>(height) * 0.5f;
    const float radius = static_cast<float>(std::min(width, height)) * 0.3f;

    // 三个顶点绕中心缓慢旋转（角度 = 基角 + 120° 间隔）。
    constexpr double kTwoPi = 6.28318530717958647692;
    float px[3], py[3];
    for (int i = 0; i < 3; ++i) {
        const double angle = timeSeconds * 0.5 + i * (kTwoPi / 3.0);
        px[i] = cx + radius * static_cast<float>(std::cos(angle));
        py[i] = cy + radius * static_cast<float>(std::sin(angle));
    }

    // 顶点色随时间循环（三相错位正弦），GPU 插值出渐变。
    ui::Color color[3];
    for (int i = 0; i < 3; ++i) {
        const double t = timeSeconds * 0.8 + i * (kTwoPi / 3.0);
        color[i].r = 0.5f + 0.5f * static_cast<float>(std::sin(t));
        color[i].g = 0.5f + 0.5f * static_cast<float>(std::sin(t + kTwoPi / 3.0));
        color[i].b = 0.5f + 0.5f * static_cast<float>(std::sin(t + 2.0 * kTwoPi / 3.0));
        color[i].a = 1.0f;
    }

    canvas.drawTriangle(px[0], py[0], px[1], py[1], px[2], py[2], clip, color[0],
                        color[1], color[2]);
}

} // namespace svk::demo
