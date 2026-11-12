#pragma once

#include "platform_surface.h"
#include "web_surface.h"

#include <optional>
#include <string>
#include <vector>

namespace svk {

struct PageBinding {
    PageId pageId = 0;
    SurfaceRef surface;
    std::string title;
};

struct LinkedViewport {
    float width = 0.0f;
    float height = 0.0f;
    float topBarHeight = 0.0f;
    float tabHeight = 0.0f;
};

struct LinkedLayoutSpec {
    SurfaceRef header;
    std::vector<PageBinding> pages;
    std::optional<SurfaceRef> drawer;
    PageId initialPage = 0;
};

struct LinkedLayoutState {
    PageId activePage = 0;
    PageId animationTarget = 0;
    PageId requestedPage = 0;
    uint64_t generation = 0;
    bool animating = false;
    size_t pageCount = 0;
};

struct LayoutResult {
    SurfaceError error = SurfaceError::None;
    bool ok() const { return error == SurfaceError::None; }
};

// 可选成员组合协议。布局只持有成员引用，不拥有成员生命周期；加入布局后，
// 组内几何与交互由布局统一写入，成员不能并行推进自己的场景进度。
class ILinkedLayout {
public:
    virtual ~ILinkedLayout() = default;
    virtual LayoutResult setViewport(const LinkedViewport& viewport) = 0;
    virtual LayoutResult replacePages(const std::vector<PageBinding>& pages) = 0;
    virtual LayoutResult selectPage(PageId pageId) = 0;
    virtual LayoutResult setDrawerOpen(bool open) = 0;
    virtual LinkedLayoutState state() const = 0;
    virtual void close() = 0;
};

struct LinkedSceneSpec {
    std::vector<WebSurfaceConfig> webSurfaces;
    std::vector<NativeSurfaceConfig> nativeSurfaces;
    LinkedLayoutSpec layout;
};

// 由具体 App 实现。平台宿主只在 UI 线程读取配置并创建成员，不据此推断页面
// 数量、顺序或业务类型。
std::unique_ptr<LinkedSceneSpec> createLinkedSceneSpec();

} // namespace svk
