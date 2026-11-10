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
    /// 调用方分配的 Surface 标识；App 只用于占位绘制与日志，不据此持有平台资源。
    uint32_t surfaceId = 0;
};

class IApp {
public:
    virtual ~IApp() = default;
    virtual void onFrame(ui::Canvas& canvas, const AppFrameContext& context) = 0;
};

std::unique_ptr<IApp> createApp();

} // namespace svk
