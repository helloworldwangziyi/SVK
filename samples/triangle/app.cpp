#include "app.h"
#include "surface_probe.h"

namespace svk {

namespace {

class SurfaceProbeApp final : public IApp {
public:
    void onFrame(ui::Canvas& canvas, const AppFrameContext& context) override {
        const ui::Color color = context.surfaceId == 1
                                    ? ui::Color{0.0f, 1.0f, 0.0f, 1.0f}
                                    : ui::Color{1.0f, 0.0f, 0.0f, 1.0f};
        demo::buildSurfaceProbeFrame(canvas, context.width, context.height, color);
    }
};

} // namespace

std::unique_ptr<IApp> createApp() {
    return std::make_unique<SurfaceProbeApp>();
}

} // namespace svk
