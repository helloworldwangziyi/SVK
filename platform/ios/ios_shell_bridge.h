#pragma once

// ============================================================================
// iOS 壳层 ↔ core 的 C 桥接口（最小形态）。对照 Android 侧 NativeBridge 的
// JNI 方法表：
//
//   NativeBridge.nativeInit(Surface)     → evkIosInit(CAMetalLayer*)（含热恢复）
//   NativeBridge.nativeResize(w, h)      → evkIosResize(w, h)
//   NativeBridge.nativeBeginFrame(nanos) → evkIosBeginFrame(nanos)
//   NativeBridge.nativeDestroy()         → evkIosSuspend()（挂起）
//   NativeBridge.nativeShutdown()        → evkIosShutdown()（真正销毁）
//
// 约定与 JNI 层相同：本层只做"解包参数 → 转发 core"，不含业务逻辑；
// 所有函数都在 iOS 主线程（= UI 线程）调用。
// ============================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// layer 参数为 CAMetalLayer*（不透明指针，避免纯 C 头文件暴露 ObjC 类型）。
// 视图/surface 就绪（viewDidAppear / 回前台）时调用，可能多次。
// 引擎已存在时走保活热恢复（按新 layer 重建 surface 级资源），
// 否则冷启动；失败时清理现场，保证下次调用能干净重试。
void evkIosInit(const void* layer);
void evkIosResize(int32_t width, int32_t height);
// CADisplayLink 的帧回调：由 C++ App 构建本帧内容并提交渲染。
void evkIosBeginFrame(int64_t frameTimeNanos);
// 视图/surface 销毁（viewWillDisappear / 退后台）语义 = 保活挂起：
// 只释放 surface 级资源，引擎级资源（设备/纹理/命令设施）全部保留，
// 下次 evkIosInit 热恢复，不重走冷启动。
void evkIosSuspend(void);
// 真正销毁（壳层 dealloc / 明确的 terminate 路径）：全量释放，
// 之后若再启动走完整冷启动流程。
void evkIosShutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif
