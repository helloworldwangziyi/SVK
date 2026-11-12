#pragma once

#include <cstdint>
#include <memory>

namespace svk {

namespace ui {
class Canvas;
}

struct AppFrameContext {
    uint32_t width = 0;
    uint32_t height = 0;
    double timeSeconds = 0.0;
    /// App 内容身份，与运行时 Surface 身份分离；App 不据此持有平台资源。
    uint32_t contentId = 0;
    uint64_t surfaceGeneration = 0;
};

class IApp {
public:
    virtual ~IApp() = default;
    virtual void onFrame(ui::Canvas& canvas, const AppFrameContext& context) = 0;
};

std::unique_ptr<IApp> createApp();

} // namespace svk
