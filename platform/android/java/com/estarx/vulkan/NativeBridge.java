package com.estarx.vulkan;

import android.view.Surface;

/** JNI 薄桥：按调用方给出的 Surface ID 转发生命周期，不包含场景或绘制业务。 */
public final class NativeBridge {
    static {
        System.loadLibrary("estarx_vulkan");
    }

    private NativeBridge() {}

    public static native void nativeSetLogDirectory(String directory);

    /** surfaceId 由示例固定分配；同一 ID 重复调用表示挂起后的热恢复。 */
    public static native void nativeInit(int surfaceId, Surface surface);

    public static native void nativeResize(int surfaceId, int width, int height);

    /** 静态示例只在创建和尺寸变化时调用；连续动画场景可由 VSync 逐帧调用。 */
    public static native void nativeBeginFrame(int surfaceId, long frameTimeNanos);

    /** 只挂起指定 Surface，不影响同进程中的其他 Surface。 */
    public static native void nativeDestroy(int surfaceId);

    /** Activity 最终销毁时释放全部 Surface 与 App。 */
    public static native void nativeShutdown();
}
