#include "app.h"
#include "surface_probe.h"

namespace svk {

namespace {

class SurfaceProbeApp final : public IApp {
public:
    void onFrame(ui::Canvas& canvas, const AppFrameContext& context) override {
        demo::buildSurfaceProbeFrame(canvas, context.width, context.height,
                                     demo::surfaceProbeColor(context.surfaceId));
    }
};

} // namespace

std::unique_ptr<IApp> createApp() {
    return std::make_unique<SurfaceProbeApp>();
}

} // namespace svk
