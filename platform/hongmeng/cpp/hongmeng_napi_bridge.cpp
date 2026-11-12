#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <AbilityKit/ability_runtime/application_context.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/callback_sink.h>

#include "app.h"
#include "linked_page_state.h"
#include "linked_scene.h"
#include "log/log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

extern "C" svk::IPlatform* svkCreateHongmengPlatform(void* window, int32_t width, int32_t height);
extern "C" void svkDestroyHongmengPlatform(svk::IPlatform* platform);

namespace {

struct ComponentKey {
    uint64_t hostId = 0;
    uint64_t generation = 0;
    svk::NativeContentId contentId = 0;
    bool valid() const { return hostId != 0 && generation != 0; }
};

struct SurfaceSlot {
    std::unique_ptr<svk::IRasterSurface> renderer;
    svk::IPlatform* platform = nullptr;
    svk::NativeContentId contentId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct HostSlot {
    std::unique_ptr<svk::IApp> app;
    std::unordered_map<uint64_t, SurfaceSlot> surfaces;
};

std::unordered_map<uint64_t, HostSlot> g_hosts;
uint64_t g_nextHostId = 1;
svk::LinkedPageStateStore g_pageStates;
std::string g_reportedLogFile;

ComponentKey componentKey(OH_NativeXComponent* component) {
    char id[OH_XCOMPONENT_ID_LEN_MAX + 1] = {};
    uint64_t size = sizeof(id);
    ComponentKey key;
    if (!component || OH_NativeXComponent_GetXComponentId(component, id, &size) !=
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS) return key;
    unsigned long long host = 0;
    unsigned long long generation = 0;
    unsigned int content = 0;
    if (std::sscanf(id, "svk:%llu:%llu:%u", &host, &generation, &content) == 3) {
        key.hostId = host;
        key.generation = generation;
        key.contentId = content;
    }
    return key;
}

HostSlot* findHost(uint64_t hostId) {
    const auto found = g_hosts.find(hostId);
    return found == g_hosts.end() ? nullptr : &found->second;
}

void destroyHostSlot(HostSlot& host) {
    for (auto& entry : host.surfaces) {
        entry.second.renderer.reset();
        svkDestroyHongmengPlatform(entry.second.platform);
        entry.second.platform = nullptr;
    }
    host.surfaces.clear();
    host.app.reset();
}

SurfaceSlot* findSurface(HostSlot* host, uint64_t generation) {
    if (!host) return nullptr;
    const auto found = host->surfaces.find(generation);
    return found == host->surfaces.end() ? nullptr : &found->second;
}

std::string logDirectory() {
    char buffer[1024] = {};
    int32_t writeLength = 0;
    const AbilityRuntime_ErrorCode result = OH_AbilityRuntime_ApplicationContextGetFilesDir(
        buffer, static_cast<int32_t>(sizeof(buffer)), &writeLength);
    (void)writeLength;
    if (result != ABILITY_RUNTIME_ERROR_CODE_NO_ERROR || buffer[0] == '\0') return {};
    return std::string(buffer) + "/logs";
}

void ensureLogger() {
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::callback_logger_mt(
            "estarx-vulkan", [](const spdlog::details::log_msg& msg) {
                LogLevel level;
                switch (msg.level) {
                    case spdlog::level::trace:
                    case spdlog::level::debug: level = LOG_DEBUG; break;
                    case spdlog::level::info: level = LOG_INFO; break;
                    case spdlog::level::warn: level = LOG_WARN; break;
                    case spdlog::level::err: level = LOG_ERROR; break;
                    default: level = LOG_FATAL; break;
                }
                const std::string text(msg.payload.data(), msg.payload.size());
                OH_LOG_Print(LOG_APP, level, 0x0000, "estarx-vulkan", "%{public}s", text.c_str());
            });
    }
    svk::log::LogFileConfig config;
    config.directory = logDirectory();
    if (config.directory.empty()) {
        svk::log::init(logger);
        return;
    }
    std::string filePath;
    if (!svk::log::initWithFile(logger, config, &filePath)) return;
    if (g_reportedLogFile != filePath) {
        g_reportedLogFile = filePath;
        SVK_LOGI("log", "file_started platform=hongmeng path={}", filePath);
    }
}

void renderSurface(HostSlot* host, uint64_t hostId, uint64_t generation) {
    SurfaceSlot* slot = findSurface(host, generation);
    if (!slot || !host->app || !slot->renderer || !slot->platform ||
        slot->width == 0 || slot->height == 0) {
        SVK_LOGW("harmony", "frame_dropped host_id={} surface_generation={} reason=surface_not_ready",
                 hostId, generation);
        return;
    }
    svk::ui::Canvas canvas;
    const svk::AppFrameContext context{slot->width, slot->height, 0.0, slot->contentId, generation};
    host->app->onFrame(canvas, context);
    if (slot->renderer->render(canvas) == svk::RenderResult::Failed) {
        SVK_LOGE("harmony", "frame_failed host_id={} surface_generation={}", hostId, generation);
    }
}

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    ensureLogger();
    const ComponentKey key = componentKey(component);
    if (!key.valid()) {
        SVK_LOGE("harmony", "surface_start_failed reason=unknown_component");
        return;
    }
    HostSlot* host = findHost(key.hostId);
    if (!host) {
        SVK_LOGE("harmony", "surface_start_failed host_id={} reason=stale_host", key.hostId);
        return;
    }
    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) !=
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS || width == 0 || height == 0) return;
    SurfaceSlot& slot = host->surfaces[key.generation];
    slot.contentId = key.contentId;
    slot.width = static_cast<uint32_t>(width);
    slot.height = static_cast<uint32_t>(height);
    if (slot.renderer) {
        svk::IPlatform* resumed = svkCreateHongmengPlatform(window, width, height);
        if (resumed && slot.renderer->resumeSurface(resumed)) {
            svkDestroyHongmengPlatform(slot.platform);
            slot.platform = resumed;
            renderSurface(host, key.hostId, key.generation);
            return;
        }
        if (resumed) svkDestroyHongmengPlatform(resumed);
        slot.renderer.reset();
        svkDestroyHongmengPlatform(slot.platform);
        slot.platform = nullptr;
    }
    slot.platform = svkCreateHongmengPlatform(window, width, height);
    if (!slot.platform) return;
    slot.renderer = svk::createRasterSurface(slot.platform);
    if (!slot.renderer->initialize()) {
        slot.renderer.reset();
        svkDestroyHongmengPlatform(slot.platform);
        slot.platform = nullptr;
        return;
    }
    if (!host->app) host->app = svk::createApp();
    slot.renderer->setSize(slot.width, slot.height);
    SVK_LOGI("harmony", "surface_started host_id={} surface_generation={} content_id={} width={} height={}",
             key.hostId, key.generation, key.contentId, slot.width, slot.height);
    renderSurface(host, key.hostId, key.generation);
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    const ComponentKey key = componentKey(component);
    HostSlot* host = findHost(key.hostId);
    SurfaceSlot* slot = findSurface(host, key.generation);
    if (!slot) return;
    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) !=
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS || width == 0 || height == 0) return;
    slot->width = static_cast<uint32_t>(width);
    slot->height = static_cast<uint32_t>(height);
    if (slot->renderer) {
        slot->renderer->setSize(slot->width, slot->height);
        renderSurface(host, key.hostId, key.generation);
    }
}

void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* /*window*/) {
    const ComponentKey key = componentKey(component);
    HostSlot* host = findHost(key.hostId);
    SurfaceSlot* slot = findSurface(host, key.generation);
    if (!slot) return;
    if (slot->renderer) slot->renderer->suspendSurface();
    svkDestroyHongmengPlatform(slot->platform);
    slot->platform = nullptr;
    slot->width = slot->height = 0;
    SVK_LOGI("harmony", "surface_suspended host_id={} surface_generation={}",
             key.hostId, key.generation);
    svk::log::flush();
}

napi_value number(napi_env env, uint64_t value) {
    napi_value result = nullptr;
    napi_create_double(env, static_cast<double>(value), &result);
    return result;
}

napi_value numberArray(napi_env env, const std::vector<uint64_t>& values) {
    napi_value result = nullptr;
    napi_create_array_with_length(env, values.size(), &result);
    for (size_t index = 0; index < values.size(); ++index) {
        napi_set_element(env, result, index, number(env, values[index]));
    }
    return result;
}

std::vector<svk::PageId> pageIds(napi_env env, napi_value value) {
    uint32_t length = 0;
    napi_get_array_length(env, value, &length);
    std::vector<svk::PageId> result;
    for (uint32_t index = 0; index < length; ++index) {
        napi_value element = nullptr;
        napi_get_element(env, value, index, &element);
        double id = 0;
        napi_get_value_double(env, element, &id);
        result.push_back(static_cast<svk::PageId>(id));
    }
    return result;
}

uint64_t argument(napi_env env, napi_callback_info info, size_t index) {
    size_t argc = index + 1;
    napi_value argv[index + 1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double value = 0;
    if (argc > index) napi_get_value_double(env, argv[index], &value);
    return static_cast<uint64_t>(value);
}

napi_value transition(napi_env env, const svk::PageTransition& value) {
    return numberArray(env, {
        static_cast<uint64_t>(value.type), static_cast<uint64_t>(value.fromIndex),
        static_cast<uint64_t>(value.targetIndex), static_cast<uint64_t>(value.activeIndex),
        static_cast<uint64_t>(value.requestedIndex), value.generation, value.pageCount,
        value.fromPage, value.targetPage, value.activePage, value.requestedPage});
}

napi_value state(napi_env env, const svk::LinkedLayoutState& value) {
    return numberArray(env, {value.activePage, value.animationTarget, value.requestedPage,
        value.generation, value.animating ? 1UL : 0UL, value.pageCount});
}

napi_value CreateHost(napi_env env, napi_callback_info) {
    ensureLogger();
    const uint64_t hostId = g_nextHostId++;
    g_hosts.emplace(hostId, HostSlot{});
    return number(env, hostId);
}

napi_value DestroyHost(napi_env env, napi_callback_info info) {
    const uint64_t hostId = argument(env, info, 0);
    const auto found = g_hosts.find(hostId);
    if (found != g_hosts.end()) {
        destroyHostSlot(found->second);
        g_hosts.erase(found);
    }
    SVK_LOGI("harmony", "host_stopped host_id={}", hostId);
    return number(env, 1);
}

napi_value GetSceneSpec(napi_env env, napi_callback_info) {
    ensureLogger();
    const auto spec = svk::createLinkedSceneSpec();
    if (!spec || !svk::validateLinkedSceneSpec(*spec).ok()) return nullptr;
    napi_value result = nullptr;
    const std::string json = svk::linkedSceneSpecToJson(*spec);
    napi_create_string_utf8(env, json.c_str(), json.size(), &result);
    return result;
}

napi_value CreatePageState(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) return number(env, 0);
    return number(env, g_pageStates.create(pageIds(env, argv[0]), argument(env, info, 1)));
}

napi_value DestroyPageState(napi_env env, napi_callback_info info) {
    return number(env, g_pageStates.destroy(argument(env, info, 0)) ? 1 : 0);
}

napi_value PageSelect(napi_env env, napi_callback_info info) {
    return transition(env, g_pageStates.select(argument(env, info, 0), argument(env, info, 1)));
}

napi_value PageComplete(napi_env env, napi_callback_info info) {
    return transition(env, g_pageStates.complete(argument(env, info, 0), argument(env, info, 1)));
}

napi_value PageSettle(napi_env env, napi_callback_info info) {
    return state(env, g_pageStates.settle(argument(env, info, 0), argument(env, info, 1)));
}

napi_value PageReplace(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) return state(env, {});
    return state(env, g_pageStates.replacePages(argument(env, info, 0), pageIds(env, argv[1])));
}

napi_value PageState(napi_env env, napi_callback_info info) {
    return state(env, g_pageStates.state(argument(env, info, 0)));
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    if (!env || !exports) return nullptr;
    static const napi_property_descriptor desc[] = {
        {"createHost", nullptr, CreateHost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyHost", nullptr, DestroyHost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getSceneSpec", nullptr, GetSceneSpec, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createPageState", nullptr, CreatePageState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyPageState", nullptr, DestroyPageState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pageSelect", nullptr, PageSelect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pageComplete", nullptr, PageComplete, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pageSettle", nullptr, PageSettle, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pageReplace", nullptr, PageReplace, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pageState", nullptr, PageState, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) == napi_ok &&
        exportInstance) {
        OH_NativeXComponent* component = nullptr;
        if (napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&component)) == napi_ok &&
            component && componentKey(component).valid()) {
            static OH_NativeXComponent_Callback callback = {
                OnSurfaceCreatedCB, OnSurfaceChangedCB, OnSurfaceDestroyedCB, nullptr};
            OH_NativeXComponent_RegisterCallback(component, &callback);
        }
    }
    return exports;
}
EXTERN_C_END

static napi_module g_estarxModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "estarx_vulkan",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEstarxVulkanModule(void) {
    napi_module_register(&g_estarxModule);
}
