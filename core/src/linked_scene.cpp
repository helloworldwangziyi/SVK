#include "linked_scene.h"

#include <unordered_set>

namespace svk {
namespace {

std::string escapeJson(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    result += ' ';
                } else {
                    result += static_cast<char>(ch);
                }
        }
    }
    return result;
}

const NativeSurfaceConfig* findNative(const LinkedSceneSpec& spec, SurfaceId id) {
    for (const auto& config : spec.nativeSurfaces) {
        if (config.surface.id == id) return &config;
    }
    return nullptr;
}

const WebSurfaceConfig* findWeb(const LinkedSceneSpec& spec, SurfaceId id) {
    for (const auto& config : spec.webSurfaces) {
        if (config.surface.id == id) return &config;
    }
    return nullptr;
}

} // namespace

SurfaceResult validateLinkedSceneSpec(const LinkedSceneSpec& spec) {
    if (!spec.layout.header.valid() || spec.layout.pages.empty()) {
        return {SurfaceError::InvalidLayout};
    }
    std::unordered_set<SurfaceId> surfaces;
    std::unordered_set<PageId> pages;
    for (const auto& config : spec.webSurfaces) {
        if (!config.surface.valid()) return {SurfaceError::InvalidArgument};
        if (!surfaces.insert(config.surface.id).second) {
            return {SurfaceError::OwnershipConflict};
        }
    }
    for (const auto& config : spec.nativeSurfaces) {
        if (!config.surface.valid()) return {SurfaceError::InvalidArgument};
        if (!surfaces.insert(config.surface.id).second) {
            return {SurfaceError::OwnershipConflict};
        }
    }
    if (!surfaces.count(spec.layout.header.id)) return {SurfaceError::InvalidLayout};
    if (findNative(spec, spec.layout.header.id) == nullptr) {
        return {SurfaceError::OwnershipConflict};
    }
    if (spec.layout.drawer &&
        (!surfaces.count(spec.layout.drawer->id) ||
         findNative(spec, spec.layout.drawer->id) == nullptr)) {
        return {SurfaceError::OwnershipConflict};
    }
    std::unordered_set<SurfaceId> boundSurfaces{spec.layout.header.id};
    if (spec.layout.drawer && !boundSurfaces.insert(spec.layout.drawer->id).second) {
        return {SurfaceError::OwnershipConflict};
    }
    bool initialFound = false;
    for (const auto& page : spec.layout.pages) {
        if (page.pageId == 0 || !page.surface.valid() || !surfaces.count(page.surface.id)) {
            return {SurfaceError::InvalidLayout};
        }
        if (!boundSurfaces.insert(page.surface.id).second) {
            return {SurfaceError::OwnershipConflict};
        }
        if (!pages.insert(page.pageId).second) return {SurfaceError::InvalidLayout};
        if (findWeb(spec, page.surface.id) == nullptr &&
            findNative(spec, page.surface.id) == nullptr) {
            return {SurfaceError::InvalidLayout};
        }
        if (page.pageId == spec.layout.initialPage) initialFound = true;
    }
    if (!initialFound) return {SurfaceError::PageNotFound};
    return {};
}

std::string linkedSceneSpecToJson(const LinkedSceneSpec& spec) {
    std::string json = "{\"header\":{\"id\":" + std::to_string(spec.layout.header.id) +
        ",\"contentId\":" + std::to_string(findNative(spec, spec.layout.header.id)->contentId) + "}";
    if (spec.layout.drawer) {
        json += ",\"drawer\":{\"id\":" + std::to_string(spec.layout.drawer->id) +
            ",\"contentId\":" +
            std::to_string(findNative(spec, spec.layout.drawer->id)->contentId) + "}";
    }
    json += ",\"initialPage\":" + std::to_string(spec.layout.initialPage) + ",\"pages\":[";
    for (size_t index = 0; index < spec.layout.pages.size(); ++index) {
        const auto& page = spec.layout.pages[index];
        if (index != 0) json += ',';
        json += "{\"pageId\":" + std::to_string(page.pageId) + ",\"surfaceId\":" +
            std::to_string(page.surface.id) + ",\"title\":\"" + escapeJson(page.title) + "\"";
        if (const auto* web = findWeb(spec, page.surface.id)) {
            json += ",\"url\":\"" + escapeJson(web->initialRequest.url) + "\"";
        } else {
            json += ",\"contentId\":" + std::to_string(findNative(spec, page.surface.id)->contentId);
        }
        json += "}";
    }
    json += "]}";
    return json;
}

} // namespace svk
