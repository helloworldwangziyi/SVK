package com.estarx.vulkan;

import android.content.Context;
import android.view.Choreographer;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * 把单个系统 Surface 转交给 native Vulkan 的薄壳。
 * generation 标识本次系统实例，contentId 仅用于 App 内容映射；平移由宿主统一驱动。
 */
public class VulkanSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
    private final long hostId;
    private final long generation;
    private final int contentId;
    private final boolean continuousFrames;
    private boolean surfaceReady;

    private final Choreographer.FrameCallback frameCallback =
            new Choreographer.FrameCallback() {
                @Override
                public void doFrame(long frameTimeNanos) {
                    if (!surfaceReady) return;
                    NativeBridge.nativeBeginFrame(hostId, generation, frameTimeNanos);
                    if (continuousFrames) {
                        Choreographer.getInstance().postFrameCallback(this);
                    }
                }
            };

    public VulkanSurfaceView(
            Context context, long hostId, long generation, int contentId,
            boolean continuousFrames) {
        super(context);
        this.hostId = hostId;
        this.generation = generation;
        this.contentId = contentId;
        this.continuousFrames = continuousFrames;
        getHolder().addCallback(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeBridge.nativeInit(hostId, generation, contentId, holder.getSurface());
        surfaceReady = true;
        NativeBridge.nativeBeginFrame(hostId, generation, System.nanoTime());
        if (continuousFrames) Choreographer.getInstance().postFrameCallback(frameCallback);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.nativeResize(hostId, generation, width, height);
        if (!continuousFrames && surfaceReady) {
            NativeBridge.nativeBeginFrame(hostId, generation, System.nanoTime());
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        NativeBridge.nativeDestroy(hostId, generation);
    }
}
