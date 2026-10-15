import displaySync from '@ohos.graphics.displaySync';

/**
 * native 模块句柄：函数由 platform/hongmeng/cpp/hongmeng_napi_bridge.cpp 导出。
 * 句柄经 XComponent 的 onLoad(context) 获得（context 即 NAPI 模块的 exports，
 * 等价于 import 'libestarx_vulkan.so' 的返回值；刻意不手动 import——手动 import
 * 会让模块先于 XComponent 加载，Init 解包不到 XComponent 实例对象）。
 */
interface NativeBridge {
  beginFrame(frameTimeNanos: number): void;
  onShutdown(): void;
}

let g_bridge: NativeBridge | null = null;

/** 取 native 桥句柄；XComponent onLoad 之前返回 null（调用方自行判空）。 */
export function nativeBridge(): NativeBridge | null {
  return g_bridge;
}

/**
 * 鸿蒙薄壳视图：XComponent(SURFACE) + DisplaySync，只负责把 surface 与
 * 帧节奏交给 native，自身不含任何业务逻辑（"薄壳"的含义）。
 * 对应 Android 的 VulkanSurfaceView（SurfaceView + Choreographer）。
 *
 * 线程模型：XComponent 回调、DisplaySync 帧回调、本文件的 NAPI 调用全部在
 * UI 线程，与 Android/iOS 壳一致，core 的无锁单线程假设原样成立。
 */
@Component
export struct VulkanShell {
  // 注意：字段不能叫 displaySync——会遮蔽上面导入的同名模块命名空间。
  private frameSync: displaySync.DisplaySync | null = null;

  aboutToDisappear(): void {
    this.stopFrames();
  }

  build() {
    XComponent({ id: 'estarx', type: XComponentType.SURFACE, libraryname: 'estarx_vulkan' })
      .width('100%')
      .height('100%')
      .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
        [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
      .onLoad((context?: object) => {
        if (!context) {
          return;
        }
        // object → NativeBridge 是向下转型（NativeBridge 可赋给 object），
        // 合法；ArkTS 禁 any/unknown，不能用 as unknown 中转。
        g_bridge = context as NativeBridge;
        this.startFrames();
      })
      .onDestroy(() => {
        this.stopFrames();
        g_bridge = null;
      })
  }

  // DisplaySync 是 ArkTS 侧的 VSync 通道（对应 Android 的 Choreographer），
  // 回调在 UI 线程；native 侧每帧构建三角形并提交渲染。
  private startFrames(): void {
    if (this.frameSync) {
      return;
    }
    const sync = displaySync.create();
    sync.on('frame', (intervalInfo: displaySync.IntervalInfo) => {
      g_bridge?.beginFrame(intervalInfo.timestamp);
    });
    sync.start();
    this.frameSync = sync;
  }

  private stopFrames(): void {
    this.frameSync?.stop();
    this.frameSync = null;
  }
}
