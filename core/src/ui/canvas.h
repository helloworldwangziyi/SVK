#pragma once

/**
 * @file canvas.h
 * @brief 每帧 2D 绘制命令收集器（立即模式顶点流 + scissor 合批）的最小形态。
 *
 * 图元在 CPU 侧三角化成顶点流，GPU 基础着色是"纹理 × 顶点色"：
 * 纯色/渐变图元走 1x1 白纹理（采样恒为 1，直出顶点色，渐变靠顶点插值）。
 * 本类不持有任何 GPU 资源，只产出 CPU 侧顶点数据，由渲染层消费。
 */
#include <cstdint>
#include <vector>

namespace svp::ui {

/// RGBA 颜色，分量 0~1。
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

/// 屏幕坐标矩形（像素，左上角原点）。
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    static Rect intersect(const Rect& a, const Rect& b) {
        const float x1 = a.x > b.x ? a.x : b.x;
        const float y1 = a.y > b.y ? a.y : b.y;
        const float x2 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
        const float y2 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
        return {x1, y1, x2 - x1, y2 - y1};
    }
};

/**
 * @brief UI 顶点：屏幕像素坐标 + 顶点色（分量 0~1）+ 纹理坐标。
 *
 * Canvas 产出的是"已在 CPU 侧算好屏幕坐标、可直接交给 GPU"的顶点流，
 * 顶点着色器不需要任何模型/视图变换。uv 配合批次绑定的纹理使用：
 * 纯色几何全 0（绑定 1x1 白纹理，采样恒为 1）。
 */
struct UiVertex {
    float x, y;       ///< 屏幕像素坐标
    float r, g, b, a; ///< 顶点色（分量 0~1）
    float u, v;       ///< 纹理坐标（纯色几何为 0）
};

/**
 * @brief 一个 GPU 绘制批次：共享同一裁剪矩形（scissor）的连续顶点区间。
 *
 * 顶点统一顺序存放在 Canvas::vertices_，Batch 只记区间不复制数据；
 * GPU 侧一个 Batch 一次 draw call（设一次 scissor + 绑一次纹理即可画完整批）。
 */
struct Batch {
    Rect clip;             ///< 本批次共享的裁剪矩形（屏幕坐标）
    uint32_t firstVertex;  ///< vertices_ 中的起始下标
    uint32_t vertexCount;  ///< 顶点数（注意不是三角形数）
};

/**
 * @brief 每帧的 2D 绘制命令收集器（立即模式）。
 *
 * 每帧开始时 clear()，随后用 drawTriangle / drawRect 发射图元；
 * 连续且 clip 相同的绘制自动合并成同一 Batch。
 */
class Canvas {
public:
    /// 每帧开始调用，清空上一帧的内容。
    void clear();

    /**
     * @brief 画三色渐变三角形（任意图元的底料）。
     */
    void drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                      const Rect& clip, Color c1, Color c2, Color c3);

    /**
     * @brief 画实心矩形（2 个三角形，6 顶点）。
     */
    void drawRect(const Rect& r, const Rect& clip, Color c);

    /// 本帧顶点流（顺序追加，供渲染层消费）。
    const std::vector<UiVertex>& vertices() const { return vertices_; }

    /// 本帧合批列表（共享同一 clip 的连续顶点区间）。
    const std::vector<Batch>& batches() const { return batches_; }

private:
    /// 连续且 clip 相同的绘制合并进同一个 Batch；clip 变化时开新 Batch。
    void append(const Rect& clip, const UiVertex* verts, size_t count);

    std::vector<UiVertex> vertices_; ///< 顶点流（帧内顺序追加）
    std::vector<Batch> batches_;     ///< 合批列表（只记顶点区间，不复制数据）
};

} // namespace svp::ui
