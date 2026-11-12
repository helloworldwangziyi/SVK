package com.estarx.vulkan;

import android.content.Context;

/** 平台成员工厂；稳定 ID 来自 App，系统 generation 由宿主递增分配。 */
public final class SurfaceFactory {
    private final Context context;
    private final long hostId;
    private long nextGeneration = 1;

    public SurfaceFactory(Context context, long hostId) {
        this.context = context.getApplicationContext();
        this.hostId = hostId;
    }

    public WebSurface createWeb(long surfaceId, String initialUrl) {
        return new WebSurface(new SurfaceRef(surfaceId, nextGeneration++), context, initialUrl);
    }

    public NativeSurface createNative(long surfaceId, int contentId) {
        return new NativeSurface(new SurfaceRef(surfaceId, nextGeneration++), context, hostId, contentId);
    }
}
