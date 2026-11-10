#include "surface_probe.h"

#include <cassert>
#include <cstdio>

namespace {

void assertColor(const svk::ui::Canvas& canvas, const svk::ui::Color& expected) {
    assert(canvas.vertices().size() == 6);
    assert(canvas.batches().size() == 1);
    assert(canvas.batches().front().vertexCount == 6);
    for (const auto& vertex : canvas.vertices()) {
        assert(vertex.r == expected.r);
        assert(vertex.g == expected.g);
        assert(vertex.b == expected.b);
        assert(vertex.a == expected.a);
    }
}

} // namespace

int main() {
    svk::ui::Canvas canvas;
    svk::demo::buildSurfaceProbeFrame(canvas, 320, 240);
    assert(canvas.batches().front().clip.w == 320.0f);
    assert(canvas.batches().front().clip.h == 240.0f);
    assertColor(canvas, svk::ui::Color{1.0f, 0.0f, 0.0f, 1.0f});

    svk::demo::buildSurfaceProbeFrame(canvas, 320, 240,
                                      svk::demo::surfaceProbeColor(1));
    assertColor(canvas, svk::ui::Color{0.0f, 1.0f, 0.0f, 1.0f});

    svk::demo::buildSurfaceProbeFrame(canvas, 320, 240,
                                      svk::demo::surfaceProbeColor(2));
    assertColor(canvas, svk::ui::Color{1.0f, 1.0f, 0.0f, 1.0f});

    svk::demo::buildSurfaceProbeFrame(canvas, 0, 240);
    assert(canvas.vertices().empty());
    assert(canvas.batches().empty());

    std::puts("test_surface_probe passed");
    return 0;
}
