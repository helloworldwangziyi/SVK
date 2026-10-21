/**
 * @file metal_backend.mm
 * @brief iOS 原生 Metal 绘制后端实现：渲染语义与 Vulkan 版逐条对齐。
 *
 * 帧流程（对照 Vulkan 版 acquire → 录制 → 提交 → present）：
 *   ensureSize（尺寸自愈，无 OUT_OF_DATE 概念）→ 帧信号量等待（16ms 超时
 *   → Retry，对应 vkWaitForFences TIMEOUT）→ nextDrawable（对应 acquire）
 *   → 上传顶点到当前帧槽独占缓冲 → 编码渲染通道（Clear + 逐批次 scissor/draw，
 *   对应 recordCommandBuffer）→ addCompletedHandler 归还信号量（对应 fence
 *   signal）+ presentDrawable + commit。
 *
 * ARC 说明（工程 CLANG_ENABLE_OBJC_ARC=YES）：
 *   - ObjC 句柄全部经 Impl 的 strong 成员管理（ARC 对 C++ struct 内单个
 *     strong 指针合成拷贝/析构；C 数组形式的不受管理，故顶点缓冲用两个独立成员）；
 *   - dispatch_semaphore_t 在现代 SDK 下同样是 ObjC 对象，ARC 托管，
 *     置 nil 即释放——dispatch_release 在 ARC 模式下不可用，不能调用。
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// glm 深度映射约定（Vulkan NDC z∈[0,1]）必须在任何 glm 头之前定义，
// 与 Vulkan 版（vulkan_renderer.h）保持一致。
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gpu/metal/metal_backend.h"

// 内嵌 MSL 着色器源码（运行时 newLibraryWithSource 编译）。
#include "gpu/metal/metal_shaders.h"

// SVK_LOG* 日志宏：spdlog 封装，全局统一的日志入口。
#include "log/log.h"
// IPlatform：原生窗口句柄（CAMetalLayer*）与绘制区尺寸查询。
#include "render_platform.h"
// ui::Canvas / UiVertex / Batch / Rect：每帧几何与顶点布局来源。
#include "ui/canvas.h"

// glm::ortho：像素坐标 → NDC 的正交投影。
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// std::max / std::min：裁剪矩形 clamp 与容量倍增要用。
#include <algorithm>
#include <cstring>
#include <limits>

namespace svk::gpu::metal {

namespace {

/// 双帧在飞：CPU 最多领先 GPU 两帧，与 Vulkan 版 kMaxFramesInFlight 一致。
constexpr uint32_t kMaxFramesInFlight = 2;
/// 顶点缓冲初始容量（顶点数），对齐 Vulkan 版 kInitialVertexCapacity。
constexpr uint32_t kInitialVertexCapacity = 8192;
/// MSAA 首选 4x，设备不支持时降 1x（Vulkan 版是 2x/4x 择一，Metal 侧定 4x）。
constexpr NSUInteger kPreferredMsaaSamples = 4;
/// 帧槽等待超时（纳秒）：16ms，与 Vulkan 版 vkWaitForFences 超时一致。
constexpr int64_t kFrameWaitTimeoutNanos = 16ll * 1000 * 1000;

} // namespace

/**
 * @brief Metal 后端资源句柄。ObjC 对象全部以 strong 成员持有（ARC 管理），
 *        layer_ 不持有（壳层保证生命周期，同 Vulkan 版对 ANativeWindow 的约定）。
 *        顶点缓冲双份（每帧槽独占）用两个独立成员而非数组：ARC 不管理
 *        C 数组形式的 strong 指针。
 */
struct MetalBackend::Impl {
    IPlatform* platform_ = nullptr;      ///< 平台抽象层（不持有所有权）
    CAMetalLayer* layer_ = nil;          ///< 呈现层（不持有）
    id<MTLDevice> device_ = nil;         ///< 默认 Metal 设备
    id<MTLCommandQueue> queue_ = nil;    ///< 命令队列（每帧 commandBuffer 来源）
    id<MTLLibrary> library_ = nil;       ///< 内嵌 MSL 编译产物
    id<MTLRenderPipelineState> pipeline_ = nil; ///< UI 管线
    id<MTLSamplerState> sampler_ = nil;  ///< 线性 + 边缘钳制采样器
    id<MTLTexture> whiteTexture_ = nil;  ///< 1x1 白纹理（采样恒为 1）
    id<MTLBuffer> vertexBuffer0_ = nil;  ///< 帧槽 0 独占顶点缓冲
    id<MTLBuffer> vertexBuffer1_ = nil;  ///< 帧槽 1 独占顶点缓冲
    uint32_t vertexCapacity0_ = 0;       ///< 帧槽 0 缓冲容量（顶点数）
    uint32_t vertexCapacity1_ = 0;       ///< 帧槽 1 缓冲容量（顶点数）
    dispatch_semaphore_t frameSemaphore_ = nil; ///< 帧槽信号量（初值 2，双帧在飞）
    id<MTLTexture> msaaTexture_ = nil;   ///< 多采样颜色图（memoryless，对应 Vulkan TRANSIENT）
    NSUInteger msaaSamples_ = 1;         ///< 采样数：4，设备不支持 4x 时降 1x
    uint32_t width_ = 0;                 ///< 目标视觉宽（像素，只记不重建）
    uint32_t height_ = 0;                ///< 目标视觉高（像素，只记不重建）
    uint32_t currentFrame_ = 0;          ///< 当前帧槽 0↔1 轮换

    // ---- 顶点缓冲（双帧槽） ----

    id<MTLBuffer> vertexBufferFor(uint32_t slot) const {
        return slot == 0 ? vertexBuffer0_ : vertexBuffer1_;
    }
    uint32_t vertexCapacityFor(uint32_t slot) const {
        return slot == 0 ? vertexCapacity0_ : vertexCapacity1_;
    }
    void setVertexBufferFor(uint32_t slot, id<MTLBuffer> buffer, uint32_t capacity) {
        if (slot == 0) {
            vertexBuffer0_ = buffer;
            vertexCapacity0_ = capacity;
        } else {
            vertexBuffer1_ = buffer;
            vertexCapacity1_ = capacity;
        }
    }

    /**
     * @brief 上传 UI 顶点到指定帧槽的独占缓冲。shared storage 下 CPU 写入
     *        对 GPU 立即可见（对应 Vulkan 版 HOST_VISIBLE|HOST_COHERENT）；
     *        容量不足时按倍增重建（初值 8192，对齐 Vulkan 版 uploadVertices）。
     */
    bool uploadVertices(const ui::UiVertex* data, uint32_t count, uint32_t slot) {
        if (count == 0) {
            return true;
        }
        if (!data || slot >= kMaxFramesInFlight) {
            return false;
        }
        if (count > vertexCapacityFor(slot)) {
            uint32_t capacity =
                std::max(kInitialVertexCapacity, vertexCapacityFor(slot));
            while (capacity < count) {
                if (capacity > std::numeric_limits<uint32_t>::max() / 2) {
                    capacity = count;
                    break;
                }
                capacity *= 2;
            }
            const NSUInteger bytes =
                sizeof(ui::UiVertex) * static_cast<NSUInteger>(capacity);
            id<MTLBuffer> buffer =
                [device_ newBufferWithLength:bytes
                                     options:MTLResourceStorageModeShared];
            if (!buffer) {
                SVK_LOGE("metal", "vertex_buffer_create_failed slot={} bytes={}",
                         slot, static_cast<unsigned long>(bytes));
                return false;
            }
            setVertexBufferFor(slot, buffer, capacity);
            SVK_LOGI("metal",
                     "vertex_buffer_ready frame_slot={} capacity={} bytes={}",
                     slot, capacity, static_cast<unsigned long>(bytes));
        }
        std::memcpy([vertexBufferFor(slot) contents], data,
                    sizeof(ui::UiVertex) * static_cast<size_t>(count));
        return true;
    }

    // ---- 帧槽信号量 ----

    /**
     * @brief 排空再补回全部帧槽令牌：与 Vulkan 版 vkDeviceWaitIdle 等价
     *        ——等两帧在飞工作全部完成（完成回调把令牌归还后才能取到）。
     */
    void drainFrameSlots() {
        if (!frameSemaphore_) {
            return;
        }
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            dispatch_semaphore_wait(frameSemaphore_, DISPATCH_TIME_FOREVER);
        }
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            dispatch_semaphore_signal(frameSemaphore_);
        }
    }

    // ---- 资源创建 ----

    /**
     * @brief UI 渲染管线：vertexDescriptor 对应 UiVertex（stride 32，
     *        float2@0 / float4@8 / float2@24，bufferIndex 0），颜色附件格式
     *        与 layer 一致，混合逐因子对齐 Vulkan 版 VkPipelineColorBlendAttachmentState：
     *        srcColor=SRC_ALPHA、dstColor=ONE_MINUS_SRC_ALPHA（源色加法），
     *        srcAlpha=ONE、dstAlpha=ONE_MINUS_SRC_ALPHA。
     */
    bool createPipeline() {
        pipeline_ = nil;
        if (!device_ || !library_) {
            SVK_LOGE("metal", "pipeline_create_failed reason=no_device_or_library");
            return false;
        }
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = [library_ newFunctionWithName:@"ui_vertex"];
        desc.fragmentFunction = [library_ newFunctionWithName:@"ui_fragment"];

        MTLVertexDescriptor* vertex = [[MTLVertexDescriptor alloc] init];
        vertex.attributes[0].format = MTLVertexFormatFloat2;
        vertex.attributes[0].offset = 0;
        vertex.attributes[0].bufferIndex = 0;
        vertex.attributes[1].format = MTLVertexFormatFloat4;
        vertex.attributes[1].offset = sizeof(float) * 2;
        vertex.attributes[1].bufferIndex = 0;
        vertex.attributes[2].format = MTLVertexFormatFloat2;
        vertex.attributes[2].offset = sizeof(float) * 6;
        vertex.attributes[2].bufferIndex = 0;
        vertex.layouts[0].stride = sizeof(ui::UiVertex);
        vertex.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        desc.vertexDescriptor = vertex;

        desc.rasterSampleCount = msaaSamples_; // 管线采样数必须与渲染目标一致
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = YES;
        desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        desc.colorAttachments[0].destinationRGBBlendFactor =
            MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationAlphaBlendFactor =
            MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;

        NSError* error = nil;
        pipeline_ = [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pipeline_) {
            SVK_LOGE("metal", "pipeline_create_failed error={}",
                     error ? (error.localizedDescription.UTF8String ?: "unknown")
                           : "unknown");
            return false;
        }
        SVK_LOGI("metal",
                 "pipeline_created vertex_stride={} attributes=3 blend=src_alpha "
                 "msaa={}",
                 static_cast<unsigned long>(sizeof(ui::UiVertex)),
                 static_cast<unsigned long>(msaaSamples_));
        return true;
    }

    /**
     * @brief 多采样颜色图：尺寸跟随 surface，storageModeMemoryless 对应
     *        Vulkan 版 TRANSIENT+LAZILY_ALLOCATED（tile 内存免真实分配），
     *        memoryless 失败回退 private；连 private 也失败则降级单采样
     *        （对齐 Vulkan 版 createColorResources 的失败降级语义）。
     */
    void createMsaaTexture(uint32_t width, uint32_t height) {
        msaaTexture_ = nil;
        if (msaaSamples_ <= 1) {
            SVK_LOGI("metal", "msaa_resources_skipped reason=single_sample");
            return;
        }
        SVK_LOGI("metal", "msaa_resources_create_start samples={} size={}x{}",
                 static_cast<unsigned long>(msaaSamples_), width, height);
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:width
                                        height:height
                                     mipmapped:NO];
        desc.textureType = MTLTextureType2DMultisample;
        desc.sampleCount = msaaSamples_;
        desc.usage = MTLTextureUsageRenderTarget;
        desc.storageMode = MTLStorageModeMemoryless;
        msaaTexture_ = [device_ newTextureWithDescriptor:desc];
        if (!msaaTexture_) {
            SVK_LOGI("metal", "msaa_memory_fallback reason=no_memoryless storage=private");
            desc.storageMode = MTLStorageModePrivate;
            msaaTexture_ = [device_ newTextureWithDescriptor:desc];
            if (!msaaTexture_) {
                SVK_LOGE("metal", "msaa_create_failed samples={} size={}x{}",
                         static_cast<unsigned long>(msaaSamples_), width, height);
                msaaSamples_ = 1;
                return;
            }
        }
        SVK_LOGI("metal", "msaa_resources_created samples={} size={}x{}",
                 static_cast<unsigned long>(msaaSamples_), width, height);
    }

    /**
     * @brief 1x1 白纹理（BGRA8，四通道全 255，采样恒为 vec4(1)）：纯色/渐变
     *        批次直出顶点色。shared storage 下 replaceRegion 立即生效，
     *        无需命令缓冲上传（对应 Vulkan 版 vkCmdClearColorImage 的简化）。
     */
    bool createWhiteTexture() {
        SVK_LOGI("metal", "white_texture_create_start size=1x1 format=bgra8");
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:1
                                        height:1
                                     mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        whiteTexture_ = [device_ newTextureWithDescriptor:desc];
        if (!whiteTexture_) {
            SVK_LOGE("metal", "white_texture_create_failed");
            return false;
        }
        uint8_t pixel[4] = {255, 255, 255, 255};
        [whiteTexture_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                         mipmapLevel:0
                           withBytes:pixel
                         bytesPerRow:4];
        SVK_LOGI("metal", "white_texture_ready");
        return true;
    }

    /**
     * @brief 尺寸自愈（renderFrame 的安全点）：平台上报尺寸与 layer 实际
     *        drawableSize 不一致时，先等 GPU 空闲，再更新 layer 尺寸并按
     *        新尺寸重建多采样目标。Metal 无 OUT_OF_DATE，本函数即全部
     *        "交换链重建"职责（CAMetalLayer 自动管理 drawable 池）。
     */
    void ensureSize() {
        if (!layer_) {
            return;
        }
        uint32_t w = 0;
        uint32_t h = 0;
        platform_->getSurfaceSize(&w, &h);
        const CGSize current = layer_.drawableSize;
        if (static_cast<uint32_t>(current.width) == w &&
            static_cast<uint32_t>(current.height) == h) {
            width_ = w;
            height_ = h;
            return;
        }
        SVK_LOGW_THROTTLE("metal", 1000,
                          "drawable_size_mismatch requested={}x{} actual={}x{} "
                          "action=rebuild",
                          w, h, static_cast<uint32_t>(current.width),
                          static_cast<uint32_t>(current.height));
        drainFrameSlots();
        layer_.drawableSize = CGSizeMake(w, h);
        width_ = w;
        height_ = h;
        createMsaaTexture(w, h);
    }
};

MetalBackend::MetalBackend(IPlatform* platform) : impl_(std::make_unique<Impl>()) {
    impl_->platform_ = platform;
}

MetalBackend::~MetalBackend() {
    shutdownDevice();
}

bool MetalBackend::initializeDevice() {
    // 设备级资源与 surface 无关，挂起/恢复期间保留；幂等。
    if (impl_->device_) {
        return true;
    }
    SVK_LOGI("metal", "initialize_start");
    impl_->device_ = MTLCreateSystemDefaultDevice();
    if (!impl_->device_) {
        SVK_LOGE("metal", "initialize_failed operation=create_device");
        return false;
    }
    impl_->queue_ = [impl_->device_ newCommandQueue];
    if (!impl_->queue_) {
        SVK_LOGE("metal", "initialize_failed operation=create_command_queue");
        impl_->device_ = nil;
        return false;
    }

    // MSAA 采样数只依赖设备属性（对齐 Vulkan 版 pickMsaaSampleCount 时机）。
    impl_->msaaSamples_ =
        [impl_->device_ supportsTextureSampleCount:kPreferredMsaaSamples]
            ? kPreferredMsaaSamples
            : 1;
    SVK_LOGI("metal", "msaa_selected samples={}",
             static_cast<unsigned long>(impl_->msaaSamples_));

    // 内嵌 MSL 源码编译（出错多为语法/版本问题，打错误日志并中止启动）。
    NSError* error = nil;
    impl_->library_ = [impl_->device_
        newLibraryWithSource:@(shaders::kUiShadersMsl)
                     options:nil
                       error:&error];
    if (!impl_->library_) {
        SVK_LOGE("metal", "library_compile_failed error={}",
                 error ? (error.localizedDescription.UTF8String ?: "unknown")
                       : "unknown");
        impl_->queue_ = nil;
        impl_->device_ = nil;
        return false;
    }
    SVK_LOGI("metal", "library_created");

    // 采样器：线性过滤 + 边缘钳制（对齐 Vulkan 版 TextureCache::initialize）。
    MTLSamplerDescriptor* sampler = [[MTLSamplerDescriptor alloc] init];
    sampler.minFilter = MTLSamplerMinMagFilterLinear;
    sampler.magFilter = MTLSamplerMinMagFilterLinear;
    sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl_->sampler_ = [impl_->device_ newSamplerStateWithDescriptor:sampler];
    if (!impl_->sampler_) {
        SVK_LOGE("metal", "create_sampler_failed");
        impl_->library_ = nil;
        impl_->queue_ = nil;
        impl_->device_ = nil;
        return false;
    }
    SVK_LOGI("metal", "sampler_created filter=linear address=clamp_to_edge");

    if (!impl_->createWhiteTexture()) {
        impl_->sampler_ = nil;
        impl_->library_ = nil;
        impl_->queue_ = nil;
        impl_->device_ = nil;
        return false;
    }

    // 双帧在飞信号量：初值 2 = 两个帧槽都空闲，对应 Vulkan 版
    // inFlightFences（SIGNALED 初始态）的等待语义。
    impl_->frameSemaphore_ = dispatch_semaphore_create(kMaxFramesInFlight);

    SVK_LOGI("metal", "initialize_complete frames_in_flight={}", kMaxFramesInFlight);
    return true;
}

void MetalBackend::shutdownDevice() {
    // 幂等：初始化可能中途失败退出，device 仍为空句柄，先判一下。
    if (!impl_->device_) {
        return;
    }
    SVK_LOGI("metal", "shutdown_start");
    // 先等 GPU 跑完所有在飞工作：释放仍在被 GPU 使用的对象是未定义行为。
    waitIdle();
    // 按依赖反序释放。layer_ 本就不持有；surface 级资源一并清掉（幂等）。
    impl_->msaaTexture_ = nil;
    impl_->pipeline_ = nil;
    impl_->layer_ = nil;
    impl_->whiteTexture_ = nil;
    impl_->sampler_ = nil;
    impl_->vertexBuffer0_ = nil;
    impl_->vertexBuffer1_ = nil;
    impl_->vertexCapacity0_ = 0;
    impl_->vertexCapacity1_ = 0;
    impl_->library_ = nil;
    impl_->queue_ = nil;
    impl_->device_ = nil;
    // dispatch_semaphore_t 在 ARC 下由 strong 成员管理，置 nil 即释放
    // （dispatch_release 在 ARC 模式下不可用）。
    impl_->frameSemaphore_ = nil;
    impl_->width_ = 0;
    impl_->height_ = 0;
    impl_->currentFrame_ = 0;
    SVK_LOGI("metal", "shutdown_complete");
}

void MetalBackend::setPlatform(IPlatform* platform) {
    // 仅换指针：保活恢复时指向新原生窗口（不持有所有权），
    // 之后由编排层紧跟 createSurface()。
    impl_->platform_ = platform;
}

bool MetalBackend::createSurface() {
    // 幂等：surface 仍在（未挂起的热恢复请求）直接成功。
    if (impl_->layer_) {
        return true;
    }
    CAMetalLayer* layer = (__bridge CAMetalLayer*)impl_->platform_
        ->nativeWindowHandle();
    if (!layer) {
        SVK_LOGE("metal", "surface_create_failed reason=metal_layer_null");
        return false;
    }
    SVK_LOGI("metal", "surface_create_start");
    impl_->layer_ = layer;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES; // 只渲染呈现，CAMetalLayer 免 CPU 可读开销
    uint32_t w = 0;
    uint32_t h = 0;
    impl_->platform_->getSurfaceSize(&w, &h);
    layer.drawableSize = CGSizeMake(w, h);
    impl_->width_ = w;
    impl_->height_ = h;

    // 渲染管线（热恢复路径下旧 pipeline 先置 nil 再建；library 是设备级保留）。
    if (!impl_->createPipeline()) {
        impl_->layer_ = nil;
        return false;
    }
    // 多采样目标：尺寸跟随 surface，随创建/热恢复同步建一次（renderFrame
    // 的 ensureSize 负责后续尺寸变化时的重建）。
    impl_->createMsaaTexture(w, h);

    SVK_LOGI("metal", "surface_created width={} height={} msaa={}", w, h,
             static_cast<unsigned long>(impl_->msaaSamples_));
    return true;
}

void MetalBackend::destroySurface() {
    // 幂等：挂起语义——等 GPU 空闲后释放 surface 级资源，保留设备级资源
    // （device/queue/library/sampler/白纹理/顶点缓冲，对齐 Vulkan 版
    // suspendSurface 的资源划分）。
    if (!impl_->layer_) {
        return;
    }
    SVK_LOGI("metal", "surface_destroy_start");
    waitIdle();
    impl_->msaaTexture_ = nil;
    impl_->pipeline_ = nil;
    impl_->layer_ = nil;
    SVK_LOGI("metal", "surface_destroyed");
}

void MetalBackend::setTargetSize(uint32_t width, uint32_t height) {
    // 只记状态不重建：真正的资源调整延迟到 renderFrame() 的 ensureSize()
    // 安全点（对齐 Vulkan 版 Swapchain::setSize 的"只置标志"语义）。
    impl_->width_ = width;
    impl_->height_ = height;
}

RenderResult MetalBackend::renderFrame(const ui::Canvas& canvas) {
    if (!impl_->layer_) {
        // 挂起/销毁后的帧请求：无呈现目标，交给下一帧（冷启动/热恢复后收敛）。
        return RenderResult::Retry;
    }

    // ① 尺寸自愈：平台上报尺寸与 layer drawableSize 不一致时在此消化
    //    （Metal 无 OUT_OF_DATE；不重试、不重建整个画帧流程）。
    impl_->ensureSize();

    // ② 帧槽等待（对应 Vulkan 版 vkWaitForFences 16ms 超时 → Retry）。
    if (dispatch_semaphore_wait(impl_->frameSemaphore_,
                                dispatch_time(DISPATCH_TIME_NOW,
                                              kFrameWaitTimeoutNanos)) != 0) {
        return RenderResult::Retry;
    }

    // ③ 获取呈现目标（对应 Vulkan 版 acquireNextImage）。
    id<CAMetalDrawable> drawable = [impl_->layer_ nextDrawable];
    if (!drawable) {
        // 取不到 drawable（极端竞争）：归还帧槽令牌，本帧重试。
        dispatch_semaphore_signal(impl_->frameSemaphore_);
        SVK_LOGE_THROTTLE("metal", 5000, "next_drawable_failed action=retry");
        return RenderResult::Retry;
    }

    // ④ 上传顶点到当前帧槽独占缓冲（空 canvas 跳过上传，仅清屏帧）。
    const auto& vertices = canvas.vertices();
    if (!vertices.empty()) {
        if (!impl_->uploadVertices(vertices.data(),
                                   static_cast<uint32_t>(vertices.size()),
                                   impl_->currentFrame_)) {
            dispatch_semaphore_signal(impl_->frameSemaphore_);
            SVK_LOGE("metal", "vertex_upload_failed operation=upload");
            return RenderResult::Failed;
        }
    }

    // ⑤ 渲染通道：清屏 + 多采样 resolve 配置（对应 Vulkan 版 renderPass）。
    id<MTLCommandBuffer> cmd = [impl_->queue_ commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
    if (impl_->msaaSamples_ > 1) {
        color.texture = impl_->msaaTexture_;
        color.resolveTexture = drawable.texture;
        color.storeAction = MTLStoreActionMultisampleResolve;
    } else {
        color.texture = drawable.texture;
        color.storeAction = MTLStoreActionStore;
    }
    color.loadAction = MTLLoadActionClear;
    color.clearColor = MTLClearColorMake(0.06, 0.06, 0.09, 1.0);

    const CGFloat drawableW = impl_->layer_.drawableSize.width;
    const CGFloat drawableH = impl_->layer_.drawableSize.height;
    const float visualW = static_cast<float>(drawableW);
    const float visualH = static_cast<float>(drawableH);

    id<MTLRenderCommandEncoder> encoder =
        [cmd renderCommandEncoderWithDescriptor:pass];
    MTLViewport viewport = {0.0, 0.0, drawableW, drawableH, 0.0, 1.0};
    [encoder setViewport:viewport];
    [encoder setRenderPipelineState:impl_->pipeline_];
    id<MTLBuffer> vertexBuffer = impl_->vertexBufferFor(impl_->currentFrame_);
    if (vertexBuffer) {
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    }

    // 像素坐标 → NDC 的正交投影（左上角原点，y 向下）。
    // 与 Vulkan 版相反：Vulkan NDC +y 朝 framebuffer 下方，Metal NDC +y 朝上，
    // 因此 glm::ortho 的 bottom/top 交换（top=0、bottom=height），
    // y=0 像素映射到 NDC +1（视口顶部）。
    const glm::mat4 mvp = glm::ortho(0.0f, visualW, visualH, 0.0f, -1.0f, 1.0f);
    [encoder setVertexBytes:&mvp length:sizeof(mvp) atIndex:1];

    // ⑦ 逐批次：scissor + 白纹理/采样器 + draw（对应 Vulkan 版逐批次循环）。
    // clip 与可视矩形求交（视觉空间；CAMetalLayer 已按视图方向呈现，
    // 无需 Vulkan 的旋转补偿），clamp 转整数；MTLScissorRect 原点左上、
    // y 向下，与 Vulkan framebuffer 坐标一致。
    for (const auto& batch : canvas.batches()) {
        if (batch.vertexCount == 0) {
            continue;
        }
        const ui::Rect clip =
            ui::Rect::intersect(batch.clip, {0.0f, 0.0f, visualW, visualH});
        const int32_t ox = std::max(0, static_cast<int32_t>(clip.x));
        const int32_t oy = std::max(0, static_cast<int32_t>(clip.y));
        const int32_t ex =
            std::min(static_cast<int32_t>(clip.x + clip.w),
                     static_cast<int32_t>(drawableW)) - ox;
        const int32_t ey =
            std::min(static_cast<int32_t>(clip.y + clip.h),
                     static_cast<int32_t>(drawableH)) - oy;
        if (ex <= 0 || ey <= 0) {
            continue;
        }
        MTLScissorRect scissor = {static_cast<NSUInteger>(ox),
                                  static_cast<NSUInteger>(oy),
                                  static_cast<NSUInteger>(ex),
                                  static_cast<NSUInteger>(ey)};
        [encoder setScissorRect:scissor];
        [encoder setFragmentTexture:impl_->whiteTexture_ atIndex:0];
        [encoder setFragmentSamplerState:impl_->sampler_ atIndex:0];
        // 画这一批：firstVertex 定位到该批在总顶点数组里的起始偏移。
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:batch.firstVertex
                    vertexCount:batch.vertexCount];
    }
    [encoder endEncoding];

    // ⑧ 完成回调归还帧槽令牌（对应 Vulkan 版 fence 在提交完成时 signal），
    //    再 present + commit。块内强引用信号量：后端先销毁也不会悬空。
    dispatch_semaphore_t frameSemaphore = impl_->frameSemaphore_;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        dispatch_semaphore_signal(frameSemaphore);
    }];
    [cmd presentDrawable:drawable];
    [cmd commit];

    impl_->currentFrame_ = (impl_->currentFrame_ + 1) % kMaxFramesInFlight;
    return RenderResult::Rendered;
}

void MetalBackend::waitIdle() {
    impl_->drainFrameSlots();
}

std::unique_ptr<IGpuBackend> createMetalBackend(IPlatform* platform) {
    return std::make_unique<MetalBackend>(platform);
}

} // namespace svk::gpu::metal
