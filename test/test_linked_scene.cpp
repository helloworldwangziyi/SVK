#include "linked_scene.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

svk::LinkedSceneSpec makeSpec() {
    svk::LinkedSceneSpec spec;
    spec.nativeSurfaces = {{{1, 0}, 0}, {{3, 0}, 1}, {{4, 0}, 2}};
    spec.webSurfaces = {{{2, 0}, {"https://example.test/page?a=1&b=2"}}};
    spec.layout.header = {1, 0};
    spec.layout.drawer = svk::SurfaceRef{4, 0};
    spec.layout.pages = {{10, {2, 0}, "资讯"}, {11, {3, 0}, "绿色"}};
    spec.layout.initialPage = 11;
    return spec;
}

} // namespace

int main() {
    auto spec = makeSpec();
    assert(svk::validateLinkedSceneSpec(spec).ok());
    const std::string json = svk::linkedSceneSpecToJson(spec);
    assert(json.find("\"pageId\":10") != std::string::npos);
    assert(json.find("\"contentId\":1") != std::string::npos);
    assert(json.find("example.test/page?a=1&b=2") != std::string::npos);

    auto duplicate = makeSpec();
    duplicate.layout.pages[1].pageId = 10;
    assert(svk::validateLinkedSceneSpec(duplicate).error == svk::SurfaceError::InvalidLayout);

    auto shared = makeSpec();
    shared.layout.pages[1].surface.id = 2;
    assert(svk::validateLinkedSceneSpec(shared).error == svk::SurfaceError::OwnershipConflict);

    auto missing = makeSpec();
    missing.layout.initialPage = 99;
    assert(svk::validateLinkedSceneSpec(missing).error == svk::SurfaceError::PageNotFound);

    auto mixed = makeSpec();
    mixed.layout.pages[0].surface = {3, 0};
    mixed.layout.pages[1].surface = {2, 0};
    assert(svk::validateLinkedSceneSpec(mixed).ok());

    const auto appSpec = svk::createLinkedSceneSpec();
    assert(appSpec && svk::validateLinkedSceneSpec(*appSpec).ok());
    assert(appSpec->layout.pages.size() == 5);
    assert(appSpec->webSurfaces.size() == 4);
    assert(appSpec->nativeSurfaces.size() == 3);
    assert(appSpec->layout.pages[4].surface.id == 6);

    std::puts("test_linked_scene passed");
    return 0;
}
