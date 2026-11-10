#include "render_platform.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "log/log.h"

namespace svk {

class AndroidPlatform final : public IPlatform {
public:
    AndroidPlatform(JNIEnv* env, jobject surface) : surface_(surface) {
        env->GetJavaVM(&jvm_);
        if (surface_) {
            window_ = ANativeWindow_fromSurface(env, surface_);
        }
    }

    ~AndroidPlatform() override {
        if (window_) {
            ANativeWindow_release(window_);
        }
        if (surface_ && jvm_) {
            JNIEnv* env = nullptr;
            bool attached = false;
            if (jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK &&
                jvm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
            if (env) {
                env->DeleteGlobalRef(surface_);
            }
            if (attached) {
                jvm_->DetachCurrentThread();
            }
        }
    }

    const char* getSurfaceExtensionName() const override {
        return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override {
        if (!window_) {
            SVK_LOGE("android", "surface_create_failed reason=native_window_null");
            return false;
        }
        VkAndroidSurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        createInfo.window = window_;
        if (vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, surface) !=
            VK_SUCCESS) {
            SVK_LOGE("android", "surface_create_failed reason=vulkan_surface_failed");
            return false;
        }
        return true;
    }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        if (window_) {
            *width = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
            *height = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
        } else {
            *width = 0;
            *height = 0;
        }
    }

private:
    jobject surface_ = nullptr;
    JavaVM* jvm_ = nullptr;
    ANativeWindow* window_ = nullptr;
};

} // namespace svk

extern "C" svk::IPlatform* svkCreateAndroidPlatform(JNIEnv* env, jobject surface) {
    jobject globalSurface = surface ? env->NewGlobalRef(surface) : nullptr;
    return new svk::AndroidPlatform(env, globalSurface);
}

extern "C" void svkDestroyAndroidPlatform(svk::IPlatform* platform) {
    delete platform;
}
