// ============================================================================
// iOS 平台适配：CAMetalLayer → 渲染后端（iOS 走原生 Metal 后端，
// nativeWindowHandle 返回的 CAMetalLayer* 由 gpu::metal::MetalBackend 消费）。
// 壳层（ViewController）持有 layer 并保证比本对象活得久，本类不持有。
// ============================================================================

#include "render_platform.h"

#import <QuartzCore/CAMetalLayer.h>

namespace svk {

class IosPlatform : public IPlatform {
public:
    // layer 由壳层（ViewController）持有并保证比本对象活得久。
    explicit IosPlatform(CAMetalLayer* layer) : layer_(layer) {}

    void* nativeWindowHandle() const override {
        // iOS 上为 CAMetalLayer*（后端按编译目标解释；Metal 后端直接使用）。
        return (__bridge void*)layer_;
    }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        // drawableSize 是像素单位（壳层在布局时已乘 contentsScale）。
        const CGSize size = layer_ ? layer_.drawableSize : CGSizeZero;
        *width = static_cast<uint32_t>(size.width);
        *height = static_cast<uint32_t>(size.height);
    }

private:
    CAMetalLayer* layer_ = nullptr;
};

IPlatform* createIosPlatform(CAMetalLayer* layer) {
    return new IosPlatform(layer);
}

void destroyIosPlatform(IPlatform* platform) {
    delete platform;
}

} // namespace svk

extern "C" {

// C 链接工厂：桥接层只需声明原型，无需知道 IosPlatform 类布局。
svk::IPlatform* svkCreateIosPlatform(const void* layer) {
    return svk::createIosPlatform((__bridge CAMetalLayer*)layer);
}

void svkDestroyIosPlatform(svk::IPlatform* platform) {
    svk::destroyIosPlatform(platform);
}

} // extern "C"
