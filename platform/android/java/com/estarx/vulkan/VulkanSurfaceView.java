package com.estarx.vulkan;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Choreographer;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * 把单个系统 Surface 转交给 native Vulkan 的薄壳。
 *
 * surfaceId 仅用于区分同进程中的多个 Surface。continuousFrames=false 时只在创建和
 * 尺寸变化后绘制，适用于静态红色探针；平移动画由原生场景统一驱动，避免每块
 * Surface 各自推进动画。
 */
public class VulkanSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
    private final int surfaceId;
    private final boolean continuousFrames;
    private boolean surfaceReady;

    private final Choreographer.FrameCallback frameCallback =
            new Choreographer.FrameCallback() {
                @Override
                public void doFrame(long frameTimeNanos) {
                    if (!surfaceReady) {
                        return;
                    }
                    NativeBridge.nativeBeginFrame(surfaceId, frameTimeNanos);
                    if (continuousFrames) {
                        Choreographer.getInstance().postFrameCallback(this);
                    }
                }
            };

    public VulkanSurfaceView(Context context) {
        this(context, 0, true);
    }

    public VulkanSurfaceView(Context context, AttributeSet attrs) {
        this(context, 0, true);
    }

    public VulkanSurfaceView(Context context, int surfaceId, boolean continuousFrames) {
        super(context);
        this.surfaceId = surfaceId;
        this.continuousFrames = continuousFrames;
        getHolder().addCallback(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeBridge.nativeInit(surfaceId, holder.getSurface());
        surfaceReady = true;
        NativeBridge.nativeBeginFrame(surfaceId, System.nanoTime());
        if (continuousFrames) {
            Choreographer.getInstance().postFrameCallback(frameCallback);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.nativeResize(surfaceId, width, height);
        if (!continuousFrames && surfaceReady) {
            NativeBridge.nativeBeginFrame(surfaceId, System.nanoTime());
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        NativeBridge.nativeDestroy(surfaceId);
    }
}
