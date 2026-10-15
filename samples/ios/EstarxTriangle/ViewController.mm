#import "ViewController.h"

#import <QuartzCore/CAMetalLayer.h>

#include <chrono>

#include "ios_shell_bridge.h"

// ----------------------------------------------------------------------------
// iOS 示例壳：EVKMetalView（CAMetalLayer 宿主）+ CADisplayLink 帧驱动。
//
// 薄壳视图只负责三件事：
//   1. backing layer 换成 CAMetalLayer（MoltenVK 的 surface 来源）；
//   2. 布局时同步 drawableSize（像素 = 点 × scale）并上报 resize；
//   3. CADisplayLink（iOS 的 VSync 通道，对应 Android 的 Choreographer）
//      每帧把时间戳交给 native 引擎。
// 绘制内容完全由引擎决定（本示例为每帧一个旋转的三色渐变三角形）。
// ----------------------------------------------------------------------------

// UIView 的 backing layer 类型挂钩：系统建 layer 时改建成 CAMetalLayer。
@interface EVKMetalView : UIView
@end

@implementation EVKMetalView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    // contentsScale 必须在主线程、布局阶段设置；Retina 上 scale=2/3。
    const CGFloat scale =
        self.window.screen.nativeScale ?: UIScreen.mainScreen.nativeScale;
    self.layer.contentsScale = scale;
    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    const CGSize points = self.bounds.size;
    layer.drawableSize = CGSizeMake(points.width * scale, points.height * scale);
    if (layer.drawableSize.width > 0 && layer.drawableSize.height > 0) {
        evkIosResize((int32_t)layer.drawableSize.width,
                     (int32_t)layer.drawableSize.height);
    }
}

@end

// ----------------------------------------------------------------------------
@interface ViewController ()
@property(nonatomic, strong) CADisplayLink* displayLink;
@property(nonatomic, strong) EVKMetalView* metalView;
@end

@implementation ViewController

- (void)loadView {
    self.metalView = [[EVKMetalView alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.metalView.backgroundColor = UIColor.blackColor;
    // 对 WindowServer 声明不透明：合成时无需与下层混合，降低每帧合成开销。
    self.metalView.layer.opaque = YES;
    self.view = self.metalView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // CADisplayLink 以 common runloop mode 注册：滚动等场景帧节奏不中断。
    self.displayLink = [CADisplayLink displayLinkWithTarget:self
                                                   selector:@selector(onDisplayLink:)];
    [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop
                           forMode:NSRunLoopCommonModes];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // surface 就绪（layer 已布局出有效 drawableSize）：冷启动或热恢复引擎。
    evkIosInit(self.metalView.layer);
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    // 保活挂起：只释放 surface 级资源，引擎级资源保留，回前台由
    // viewDidAppear → evkIosInit 热恢复。
    evkIosSuspend();
}

- (void)dealloc {
    [self.displayLink invalidate];
    evkIosShutdown(); // 真正销毁：全量释放引擎资源。
}

// CADisplayLink 帧回调（主线程）：把单调时钟的帧时间戳交给 native。
// 注：target timestamp 为系统建议的呈现时刻，本壳取 steady_clock 本地
// 单调秒数，与 Android/HarmonyOS 壳的时间基准语义一致（动画时间源）。
- (void)onDisplayLink:(CADisplayLink*)link {
    (void)link;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const int64_t nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    evkIosBeginFrame(nanos);
}

@end
