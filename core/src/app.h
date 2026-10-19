#pragma once

#include <cstdint>
#include <memory>

namespace evk {

namespace ui {
class Canvas;
}

struct AppFrameContext {
    uint32_t width = 0;
    uint32_t height = 0;
    double timeSeconds = 0.0;
};

class IApp {
public:
    virtual ~IApp() = default;
    virtual void onFrame(ui::Canvas& canvas, const AppFrameContext& context) = 0;
};

std::unique_ptr<IApp> createApp();

} // namespace evk
