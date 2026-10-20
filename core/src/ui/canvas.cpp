/**
 * @file canvas.cpp
 * @brief Canvas 实现：顶点发射与合批。
 */
#include "ui/canvas.h"

namespace svp::ui {

void Canvas::clear() {
    vertices_.clear();
    batches_.clear();
}

void Canvas::drawTriangle(float x1, float y1, float x2, float y2, float x3,
                          float y3, const Rect& clip, Color c1, Color c2, Color c3) {
    const UiVertex verts[3] = {
        {x1, y1, c1.r, c1.g, c1.b, c1.a, 0.0f, 0.0f},
        {x2, y2, c2.r, c2.g, c2.b, c2.a, 0.0f, 0.0f},
        {x3, y3, c3.r, c3.g, c3.b, c3.a, 0.0f, 0.0f},
    };
    append(clip, verts, 3);
}

void Canvas::drawRect(const Rect& r, const Rect& clip, Color c) {
    const UiVertex verts[6] = {
        {r.x,       r.y,       c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x + r.w, r.y,       c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x + r.w, r.y + r.h, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x,       r.y,       c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x + r.w, r.y + r.h, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x,       r.y + r.h, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
    };
    append(clip, verts, 6);
}

void Canvas::append(const Rect& clip, const UiVertex* verts, size_t count) {
    // 与上一批 clip 相同则原地合并（顶点连续追加），否则开新 Batch。
    if (!batches_.empty()) {
        const Batch& last = batches_.back();
        const bool sameClip = last.clip.x == clip.x && last.clip.y == clip.y &&
                              last.clip.w == clip.w && last.clip.h == clip.h;
        if (sameClip) {
            vertices_.insert(vertices_.end(), verts, verts + count);
            batches_.back().vertexCount += static_cast<uint32_t>(count);
            return;
        }
    }
    Batch batch;
    batch.clip = clip;
    batch.firstVertex = static_cast<uint32_t>(vertices_.size());
    batch.vertexCount = static_cast<uint32_t>(count);
    batches_.push_back(batch);
    vertices_.insert(vertices_.end(), verts, verts + count);
}

} // namespace svp::ui
