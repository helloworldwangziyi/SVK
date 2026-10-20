#include "app.h"
#include "triangle.h"

namespace svp {

namespace {

class TriangleApp final : public IApp {
public:
    void onFrame(ui::Canvas& canvas, const AppFrameContext& context) override {
        demo::buildTriangleFrame(canvas, context.width, context.height,
                                 context.timeSeconds);
    }
};

} // namespace

std::unique_ptr<IApp> createApp() {
    return std::make_unique<TriangleApp>();
}

} // namespace svp
