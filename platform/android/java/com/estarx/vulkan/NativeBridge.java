package com.estarx.vulkan;

import android.view.Surface;

/** JNI 薄桥：按宿主和对象身份转发，不包含页面数量或场景业务。 */
public final class NativeBridge {
    static {
        System.loadLibrary("estarx_vulkan");
    }

    private NativeBridge() {}

    public static native void nativeSetLogDirectory(String directory);
    public static native long nativeCreateHost();
    public static native void nativeDestroyHost(long hostId);
    public static native String nativeGetSceneSpec();

    /** generation 标识一次系统 Surface 实例；contentId 只映射 App 绘制内容。 */
    public static native void nativeInit(
            long hostId, long generation, int contentId, Surface surface);
    public static native void nativeResize(long hostId, long generation, int width, int height);
    public static native void nativeBeginFrame(long hostId, long generation, long frameTimeNanos);
    public static native void nativeDestroy(long hostId, long generation);

    public static native long nativeCreatePageState(long[] pageIds, long initialPage);
    public static native void nativeDestroyPageState(long handle);
    /** 返回 11 个字段：type/from/target/active/requested/generation/count/fromPage/targetPage/activePage/requestedPage。 */
    public static native long[] nativePageSelect(long handle, long pageId);
    public static native long[] nativePageComplete(long handle, long generation);
    /** 返回 active/target/requested/generation/animating/pageCount。 */
    public static native long[] nativePageSettle(long handle, long pageId);
    public static native long[] nativePageReplace(long handle, long[] pageIds);
    public static native long[] nativePageState(long handle);
}
