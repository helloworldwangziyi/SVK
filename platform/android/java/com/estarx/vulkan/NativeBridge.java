package com.estarx.vulkan;

import android.view.Surface;

/**
 * JNI bridge to the native Vulkan renderer.
 *
 * 薄壳的 Java 侧：只声明 native 方法，实现全部在
 * platform/android/cpp/android_jni_bridge.cpp（静态注册命名约定）。
 */
public final class NativeBridge {
    static {
        System.loadLibrary("estarx_vulkan");
    }

    private NativeBridge() {}

    /** 初始化平台终端 + 文件日志；必须在 surface 创建前调用。 */
    public static native void nativeSetLogDirectory(String directory);

    /** surface 创建就绪时调用（可能多次：退后台重建，native 侧走保活热恢复）。 */
    public static native void nativeInit(Surface surface);

    /** surface 尺寸确定或变化时调用（首次创建后紧跟一次；旋转也会触发）。 */
    public static native void nativeResize(int width, int height);

    /** Choreographer 的 VSync 回调：由 C++ App 构建本帧内容并提交渲染。 */
    public static native void nativeBeginFrame(long frameTimeNanos);

    /** surfaceDestroyed 语义 = 保活挂起：只释放 surface 级资源，引擎级资源保留。 */
    public static native void nativeDestroy();

    /** Activity.onDestroy 语义 = 真正销毁，全量释放引擎资源。 */
    public static native void nativeShutdown();
}
