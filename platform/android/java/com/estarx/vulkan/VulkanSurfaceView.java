package com.estarx.vulkan;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Choreographer;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * A SurfaceView that forwards surface lifecycle events to the native Vulkan renderer.
 *
 * 薄壳视图：只负责向系统拿 surface，然后原样转发给 JNI，并用 Choreographer
 * 驱动每帧。自身不含任何业务逻辑（"薄壳"的含义）；绘制内容由 C++ App
 * 通过引擎的公共接口决定。
 */
public class VulkanSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

    private boolean surfaceReady;

    // Choreographer 是 Android 的 VSync 通道：每个显示帧回调一次 doFrame，
    // 我们把帧时间戳原样转给 native（旋转动画的时间基准）。
    private final Choreographer.FrameCallback frameCallback =
            new Choreographer.FrameCallback() {
                @Override
                public void doFrame(long frameTimeNanos) {
                    if (!surfaceReady) {
                        return;
                    }
                    NativeBridge.nativeBeginFrame(frameTimeNanos);
                    Choreographer.getInstance().postFrameCallback(this);
                }
            };

    // 构造函数①：代码里 new VulkanSurfaceView(context) 时走这个。
    public VulkanSurfaceView(Context context) {
        super(context);
        init();
    }

    // 构造函数②：XML 布局反射创建时走这个。
    // attrs 装着 XML 标签上写的属性（android:id、layout_width 等）。
    // 两个构造都必须提供，否则反射或手动 new 总有一边会崩。
    public VulkanSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        // getHolder() 拿到这块 View 底层画板的管理员(SurfaceHolder)，
        // addCallback(this) 向他登记："画板创建/变化/销毁时请通知我"，
        // 通知会送到下面三个 surfaceXxx 方法（this 就是本对象）。
        getHolder().addCallback(this);
    }

    // 画板被系统创建好时回调（View 显示到屏幕、或退后台再回来时重建）。
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // holder.getSurface() 就是 Java 层的画板句柄，
        // 传给 native，C++ 用它创建 Vulkan 渲染表面。
        NativeBridge.nativeInit(holder.getSurface());
        surfaceReady = true;
        Choreographer.getInstance().postFrameCallback(frameCallback);
    }

    // 画板尺寸确定或变化时回调（首次创建后紧跟一次；旋转屏幕等也会触发）。
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.nativeResize(width, height); // 告诉 native 画板新尺寸
    }

    // 画板被销毁时回调（退后台、Activity 销毁），通知 native 释放渲染资源。
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        NativeBridge.nativeDestroy();
    }
}
