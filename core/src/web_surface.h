#pragma once

#include "platform_surface.h"

#include <memory>
#include <string>

namespace svk {

struct WebRequest {
    std::string url;
};

struct WebSurfaceConfig {
    SurfaceRef surface;
    WebRequest initialRequest;
};

struct WebSurfaceResult {
    std::unique_ptr<class IWebSurface> surface;
    SurfaceResult result;
};

struct NativeSurfaceResult {
    std::unique_ptr<IPlatformSurface> surface;
    SurfaceResult result;
};

class IWebSurface : public IPlatformSurface {
public:
    ~IWebSurface() override = default;
    virtual SurfaceResult navigate(const WebRequest& request) = 0;
};

// 工厂由宿主注入并拥有平台上下文。返回的对象具有唯一所有权；失败时 surface
// 必须为空，不能返回半初始化对象。
class IPlatformSurfaceFactory {
public:
    virtual ~IPlatformSurfaceFactory() = default;
    virtual WebSurfaceResult createWeb(const WebSurfaceConfig& config) = 0;
    virtual NativeSurfaceResult createNative(const NativeSurfaceConfig& config) = 0;
};

} // namespace svk
