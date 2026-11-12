#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/android_sink.h>

#include "app.h"
#include "linked_page_state.h"
#include "linked_scene.h"
#include "log/log.h"
#include "raster_surface.h"
#include "render_platform.h"
#include "ui/canvas.h"

extern "C" svk::IPlatform* svkCreateAndroidPlatform(JNIEnv* env, jobject surface);
extern "C" void svkDestroyAndroidPlatform(svk::IPlatform* platform);

namespace {

struct SurfaceSlot {
    std::unique_ptr<svk::IRasterSurface> renderer;
    svk::IPlatform* platform = nullptr;
    svk::NativeContentId contentId = 0;
};

struct HostSlot {
    std::unique_ptr<svk::IApp> app;
    std::unordered_map<uint64_t, SurfaceSlot> surfaces;
};

std::unordered_map<uint64_t, HostSlot> g_hosts;
uint64_t g_nextHostId = 1;
svk::LinkedPageStateStore g_pageStates;
std::string g_logDirectory;
std::string g_reportedLogFile;

void ensureLogger() {
    auto logger = spdlog::get("estarx-vulkan");
    if (!logger) {
        logger = spdlog::android_logger_mt("estarx-vulkan", "estarx-vulkan");
    }
    if (g_logDirectory.empty()) {
        svk::log::init(logger);
        return;
    }
    svk::log::LogFileConfig config;
    config.directory = g_logDirectory;
    std::string filePath;
    if (!svk::log::initWithFile(logger, config, &filePath)) return;
    if (g_reportedLogFile != filePath) {
        g_reportedLogFile = filePath;
        SVK_LOGI("log", "file_started platform=android path={}", filePath);
    }
}

HostSlot* findHost(uint64_t hostId) {
    const auto found = g_hosts.find(hostId);
    return found == g_hosts.end() ? nullptr : &found->second;
}

void destroyHostSlot(HostSlot& host) {
    for (auto& entry : host.surfaces) {
        entry.second.renderer.reset();
        svkDestroyAndroidPlatform(entry.second.platform);
        entry.second.platform = nullptr;
    }
    host.surfaces.clear();
    host.app.reset();
}

void suspendSurface(SurfaceSlot& slot, uint64_t hostId, uint64_t generation) {
    if (slot.renderer) slot.renderer->suspendSurface();
    if (slot.platform) {
        svkDestroyAndroidPlatform(slot.platform);
        slot.platform = nullptr;
    }
    SVK_LOGI("android", "surface_suspended host_id={} surface_generation={}", hostId, generation);
}

std::vector<svk::PageId> pageIds(JNIEnv* env, jlongArray values) {
    if (!values) return {};
    const jsize length = env->GetArrayLength(values);
    std::vector<jlong> raw(static_cast<size_t>(length));
    env->GetLongArrayRegion(values, 0, length, raw.data());
    std::vector<svk::PageId> result;
    result.reserve(raw.size());
    for (jlong value : raw) result.push_back(static_cast<svk::PageId>(value));
    return result;
}

jlongArray transitionArray(JNIEnv* env, const svk::PageTransition& transition) {
    const jlong values[] = {
        static_cast<jlong>(transition.type), transition.fromIndex, transition.targetIndex,
        transition.activeIndex, transition.requestedIndex,
        static_cast<jlong>(transition.generation), static_cast<jlong>(transition.pageCount),
        static_cast<jlong>(transition.fromPage), static_cast<jlong>(transition.targetPage),
        static_cast<jlong>(transition.activePage), static_cast<jlong>(transition.requestedPage),
    };
    jlongArray result = env->NewLongArray(sizeof(values) / sizeof(values[0]));
    if (result) env->SetLongArrayRegion(result, 0, sizeof(values) / sizeof(values[0]), values);
    return result;
}

jlongArray stateArray(JNIEnv* env, const svk::LinkedLayoutState& state) {
    const jlong values[] = {
        static_cast<jlong>(state.activePage), static_cast<jlong>(state.animationTarget),
        static_cast<jlong>(state.requestedPage), static_cast<jlong>(state.generation),
        state.animating ? 1L : 0L, static_cast<jlong>(state.pageCount),
    };
    jlongArray result = env->NewLongArray(sizeof(values) / sizeof(values[0]));
    if (result) env->SetLongArrayRegion(result, 0, sizeof(values) / sizeof(values[0]), values);
    return result;
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeSetLogDirectory(
    JNIEnv* env, jclass, jstring directory) {
    if (!directory) {
        g_logDirectory.clear();
        return;
    }
    const char* chars = env->GetStringUTFChars(directory, nullptr);
    if (!chars) return;
    g_logDirectory = chars;
    env->ReleaseStringUTFChars(directory, chars);
    ensureLogger();
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeCreateHost(JNIEnv*, jclass) {
    ensureLogger();
    const uint64_t hostId = g_nextHostId++;
    g_hosts.emplace(hostId, HostSlot{});
    return static_cast<jlong>(hostId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroyHost(JNIEnv*, jclass, jlong hostId) {
    const auto found = g_hosts.find(static_cast<uint64_t>(hostId));
    if (found != g_hosts.end()) {
        destroyHostSlot(found->second);
        g_hosts.erase(found);
    }
    SVK_LOGI("android", "host_stopped host_id={}", static_cast<uint64_t>(hostId));
    svk::log::flush();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeGetSceneSpec(JNIEnv* env, jclass) {
    ensureLogger();
    const auto spec = svk::createLinkedSceneSpec();
    if (!spec || !svk::validateLinkedSceneSpec(*spec).ok()) {
        SVK_LOGE("android", "scene_spec_invalid");
        return nullptr;
    }
    const std::string json = svk::linkedSceneSpecToJson(*spec);
    return env->NewStringUTF(json.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeInit(
    JNIEnv* env, jclass, jlong hostIdValue, jlong generationValue, jint contentId,
    jobject surface) {
    ensureLogger();
    const uint64_t hostId = static_cast<uint64_t>(hostIdValue);
    const uint64_t generation = static_cast<uint64_t>(generationValue);
    if (!surface || generation == 0) {
        SVK_LOGE("android", "surface_start_failed host_id={} surface_generation={} reason=invalid_args",
                 hostId, generation);
        return;
    }
    HostSlot* host = findHost(hostId);
    if (!host) {
        SVK_LOGE("android", "surface_start_failed host_id={} reason=stale_host", hostId);
        return;
    }
    SurfaceSlot& slot = host->surfaces[generation];
    slot.contentId = static_cast<svk::NativeContentId>(contentId);
    if (slot.renderer) {
        svk::IPlatform* resumed = svkCreateAndroidPlatform(env, surface);
        if (resumed && slot.renderer->resumeSurface(resumed)) {
            svkDestroyAndroidPlatform(slot.platform);
            slot.platform = resumed;
            SVK_LOGI("android", "surface_resumed host_id={} surface_generation={}", hostId,
                     generation);
            return;
        }
        SVK_LOGE("android", "surface_resume_failed host_id={} surface_generation={} action=cold_start",
                 hostId, generation);
        if (resumed) svkDestroyAndroidPlatform(resumed);
        slot.renderer.reset();
        svkDestroyAndroidPlatform(slot.platform);
        slot.platform = nullptr;
    }
    slot.platform = svkCreateAndroidPlatform(env, surface);
    if (!slot.platform) return;
    slot.renderer = svk::createRasterSurface(slot.platform);
    if (!slot.renderer->initialize()) {
        SVK_LOGE("android", "surface_start_failed host_id={} surface_generation={} phase=initialize",
                 hostId, generation);
        slot.renderer.reset();
        svkDestroyAndroidPlatform(slot.platform);
        slot.platform = nullptr;
        return;
    }
    if (!host->app) host->app = svk::createApp();
    uint32_t width = 0;
    uint32_t height = 0;
    slot.platform->getSurfaceSize(&width, &height);
    slot.renderer->setSize(width, height);
    SVK_LOGI("android", "surface_started host_id={} surface_generation={} content_id={} width={} height={}",
             hostId, generation, slot.contentId, width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeResize(
    JNIEnv*, jclass, jlong hostIdValue, jlong generationValue, jint width, jint height) {
    HostSlot* host = findHost(static_cast<uint64_t>(hostIdValue));
    if (!host) return;
    const auto found = host->surfaces.find(static_cast<uint64_t>(generationValue));
    if (found == host->surfaces.end()) return;
    if (found->second.renderer) {
        found->second.renderer->setSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        SVK_LOGD("android", "surface_resized host_id={} surface_generation={} width={} height={}",
                 hostIdValue, generationValue, width, height);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeBeginFrame(
    JNIEnv*, jclass, jlong hostIdValue, jlong generationValue, jlong frameTimeNanos) {
    HostSlot* host = findHost(static_cast<uint64_t>(hostIdValue));
    if (!host) return;
    const auto found = host->surfaces.find(static_cast<uint64_t>(generationValue));
    if (found == host->surfaces.end()) return;
    SurfaceSlot& slot = found->second;
    if (!host->app || !slot.renderer || !slot.platform) {
        SVK_LOGW("android", "frame_dropped host_id={} surface_generation={} reason=surface_not_ready",
                 hostIdValue, generationValue);
        return;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    slot.platform->getSurfaceSize(&width, &height);
    const double timeSeconds = static_cast<double>(frameTimeNanos) * 1e-9;
    svk::ui::Canvas canvas;
    const svk::AppFrameContext context{
        width, height, timeSeconds, slot.contentId, static_cast<uint64_t>(generationValue)};
    host->app->onFrame(canvas, context);
    if (slot.renderer->render(canvas) == svk::RenderResult::Failed) {
        SVK_LOGE("android", "frame_failed host_id={} surface_generation={} width={} height={}",
                 hostIdValue, generationValue, width, height);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroy(
    JNIEnv*, jclass, jlong hostIdValue, jlong generationValue) {
    HostSlot* host = findHost(static_cast<uint64_t>(hostIdValue));
    if (!host) return;
    const auto found = host->surfaces.find(static_cast<uint64_t>(generationValue));
    if (found != host->surfaces.end()) suspendSurface(found->second, hostIdValue, generationValue);
    svk::log::flush();
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeCreatePageState(
    JNIEnv* env, jclass, jlongArray pages, jlong initialPage) {
    return static_cast<jlong>(g_pageStates.create(pageIds(env, pages), initialPage));
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroyPageState(JNIEnv*, jclass, jlong handle) {
    g_pageStates.destroy(static_cast<uint64_t>(handle));
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_estarx_vulkan_NativeBridge_nativePageSelect(
    JNIEnv* env, jclass, jlong handle, jlong pageId) {
    return transitionArray(env, g_pageStates.select(handle, pageId));
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_estarx_vulkan_NativeBridge_nativePageComplete(
    JNIEnv* env, jclass, jlong handle, jlong generation) {
    return transitionArray(env, g_pageStates.complete(handle, generation));
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_estarx_vulkan_NativeBridge_nativePageSettle(
    JNIEnv* env, jclass, jlong handle, jlong pageId) {
    return stateArray(env, g_pageStates.settle(handle, pageId));
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_estarx_vulkan_NativeBridge_nativePageReplace(
    JNIEnv* env, jclass, jlong handle, jlongArray pages) {
    return stateArray(env, g_pageStates.replacePages(handle, pageIds(env, pages)));
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_estarx_vulkan_NativeBridge_nativePageState(JNIEnv* env, jclass, jlong handle) {
    return stateArray(env, g_pageStates.state(handle));
}
