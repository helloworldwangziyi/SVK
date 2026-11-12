package com.estarx.vulkan;

/** 物理像素、宿主左上角原点。 */
public final class SurfacePlacement {
    public final float x;
    public final float y;
    public final float width;
    public final float height;
    public final boolean visible;
    public final boolean interactive;

    public SurfacePlacement(
            float x, float y, float width, float height, boolean visible, boolean interactive) {
        this.x = x;
        this.y = y;
        this.width = width;
        this.height = height;
        this.visible = visible;
        this.interactive = interactive;
    }
}
