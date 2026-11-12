package com.estarx.vulkan;

/** 稳定对象 ID 与本次系统实例代次；关闭后不复用同一引用。 */
public final class SurfaceRef {
    public final long id;
    public final long instanceGeneration;

    public SurfaceRef(long id, long instanceGeneration) {
        this.id = id;
        this.instanceGeneration = instanceGeneration;
    }
}
