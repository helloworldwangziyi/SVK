package com.estarx.vulkan;

import android.content.Context;

/** 独立 Native 绘制视图；contentId 不等于系统 Surface generation。 */
public final class NativeSurface extends PlatformSurface {
    NativeSurface(SurfaceRef ref, Context context, long hostId, int contentId) {
        super(ref, new VulkanSurfaceView(context, hostId, ref.instanceGeneration, contentId, false));
    }

    public VulkanSurfaceView nativeView() {
        return (VulkanSurfaceView) view();
    }

    @Override
    protected void onClose() {}
}
