/**
 * @file main.cpp
 * @brief 桌面端薄壳（Win32）：创建窗口 → 提供 Vulkan surface → 每帧画旋转三角形。
 *
 * 这是"平台薄壳"的桌面形态：只负责窗口/surface 生命周期与帧驱动，
 * 不含任何业务绘制内容（三角形来自 samples/triangle.h）。
 * 用途：在没有手机的开发机上端到端验证引擎核心。
 */
// 必须定义在包含任何 vulkan 头文件之前：打开 Win32 surface 的类型与函数声明。
#define VK_USE_PLATFORM_WIN32_KHR

#include "log.h"
#include "render_platform.h"
#include "vulkan_renderer.h"

// 共享的示例内容：每帧一个三色渐变三角形。
#include "samples/triangle.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>

namespace {

/// Win32 平台适配：HWND → VkSurfaceKHR（vkCreateWin32SurfaceKHR）。
class Win32Platform : public evk::IPlatform {
public:
    explicit Win32Platform(HWND hwnd) : hwnd_(hwnd) {}

    const char* getSurfaceExtensionName() const override {
        return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override {
        VkWin32SurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hinstance = GetModuleHandle(nullptr);
        createInfo.hwnd = hwnd_;
        if (vkCreateWin32SurfaceKHR(instance, &createInfo, nullptr, surface) !=
            VK_SUCCESS) {
            EVK_LOGE("desktop", "surface_create_failed reason=vulkan_surface_failed");
            return false;
        }
        return true;
    }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        *width = static_cast<uint32_t>(rect.right - rect.left);
        *height = static_cast<uint32_t>(rect.bottom - rect.top);
    }

private:
    HWND hwnd_ = nullptr;
};

// 窗口过程：退出的唯一通道是 WM_DESTROY（PostQuitMessage）。
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE: {
        // 尺寸经 IPlatform::getSurfaceSize 由 swapchain 自查询，
        // 这里只通知 Renderer 置重建标志。
        auto* renderer = reinterpret_cast<evk::Renderer*>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (renderer) {
            uint32_t w = 0, h = 0;
            RECT rect{};
            GetClientRect(hwnd, &rect);
            w = static_cast<uint32_t>(rect.right - rect.left);
            h = static_cast<uint32_t>(rect.bottom - rect.top);
            renderer->setSize(w, h);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

} // namespace

int main() {
    const HINSTANCE instance = GetModuleHandle(nullptr);
    const int showCmd = SW_SHOWDEFAULT;
    // 引擎日志：桌面默认 stdout（evk/log.h 的便携默认）。
    evk::log::init("evk_desktop");

    // DPI 感知：让 GetClientRect 拿到真实像素尺寸（否则高 DPI 屏上被系统缩放）。
    SetProcessDPIAware();

    // ---- 窗口 ----
    WNDCLASS wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = TEXT("evk_triangle_window");
    RegisterClass(&wc);

    constexpr int kInitialWidth = 480;
    constexpr int kInitialHeight = 800;
    HWND hwnd = CreateWindow(wc.lpszClassName, TEXT("estarx_vulkan_minimal — 三角形"),
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             kInitialWidth, kInitialHeight, nullptr, nullptr,
                             instance, nullptr);
    if (!hwnd) {
        EVK_LOGE("desktop", "create_window_failed");
        return 1;
    }
    ShowWindow(hwnd, showCmd);

    // ---- 引擎：platform → Renderer → initialize ----
    Win32Platform platform(hwnd);
    auto renderer = evk::createRasterSurface(&platform);
    if (!renderer->initialize()) {
        EVK_LOGE("desktop", "renderer_initialize_failed");
        return 1;
    }
    uint32_t width = 0, height = 0;
    platform.getSurfaceSize(&width, &height);
    renderer->setSize(width, height);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer.get()));

    EVK_LOGI("desktop", "engine_started width={} height={}", width, height);

    // ---- 帧循环：消息泵 + 每帧构建三角形并提交 ----
    const auto startTime = std::chrono::steady_clock::now();
    evk::ui::Canvas canvas;
    bool running = true;
    while (running) {
        MSG message{};
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        if (!running) {
            break;
        }

        const double timeSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          startTime)
                .count();
        evk::demo::buildTriangleFrame(canvas, width, height, timeSeconds);
        renderer->render(canvas);
    }

    // 释放顺序：Renderer 先（析构内部 vkDeviceWaitIdle 并释放全部资源），窗口后。
    SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
    renderer.reset();
    EVK_LOGI("desktop", "engine_stopped");
    return 0;
}
