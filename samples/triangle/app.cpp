#include "app.h"
#include "linked_layout.h"
#include "surface_probe.h"

#include <string>

namespace svk {

namespace {

class SurfaceProbeApp final : public IApp {
public:
    void onFrame(ui::Canvas& canvas, const AppFrameContext& context) override {
        demo::buildSurfaceProbeFrame(canvas, context.width, context.height,
                                     demo::surfaceProbeColor(context.contentId));
    }
};

SurfaceRef reference(SurfaceId id) {
    return SurfaceRef{id, 0};
}

WebSurfaceConfig web(SurfaceId id, const std::string& path) {
    return WebSurfaceConfig{reference(id), WebRequest{
        "https://news.epolestar.xyz/" + path +
        "?isEstarX=1&contractNo=CFFEX%7CF%7CIF&theme=0&contractName=IF"}};
}

} // namespace

std::unique_ptr<IApp> createApp() {
    return std::make_unique<SurfaceProbeApp>();
}

std::unique_ptr<LinkedSceneSpec> createLinkedSceneSpec() {
    auto spec = std::make_unique<LinkedSceneSpec>();
    spec->nativeSurfaces = {
        NativeSurfaceConfig{reference(1), 0},
        NativeSurfaceConfig{reference(6), 1},
        NativeSurfaceConfig{reference(7), 2},
    };
    spec->webSurfaces = {
        web(2, "notice"), web(3, "relevance"), web(4, "rank"), web(5, "f10")};
    spec->layout.header = reference(1);
    spec->layout.drawer = reference(7);
    spec->layout.pages = {
        PageBinding{10, reference(2), "资讯"},
        PageBinding{11, reference(3), "关联"},
        PageBinding{12, reference(4), "排行"},
        PageBinding{13, reference(5), "F10"},
        PageBinding{14, reference(6), "绿色"},
    };
    spec->layout.initialPage = 10;
    return spec;
}

} // namespace svk
